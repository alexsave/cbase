// http.h - HTTP/1.1 requests in, responses out.
//
// The parser is lifted from the native server's httpd.c (one pass over the
// header block, no sscanf, no strstr) and made incremental and pure: it reads
// a buffer, not a socket, so a test can hand it any bytes, and the server
// (server.c) can call it again as more arrive. Caddy terminates TLS in front
// and speaks plain HTTP/1.1 to cbase over loopback.
//
// WHAT IS NOT SUPPORTED, on purpose: chunked request bodies (411 Length
// Required; browsers and both SDKs send a Content-Length), pipelined requests
// beyond reading them one at a time, HTTP/2, and Expect: 100-continue.
//
// A request's strings point into the connection's buffer and live until the
// response has been written. Handlers copy what they keep.
#ifndef CB_HTTP_H
#define CB_HTTP_H

#include "util/str.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CB_HTTP_MAX_HEADERS   64
#define CB_HTTP_MAX_HEAD      16384   // request line + headers
#define CB_HTTP_DEFAULT_BODY  (1u << 20)

typedef struct {
    const char *name;    // as received; compare case-insensitively
    const char *value;   // leading and trailing blanks trimmed
} cb_http_header;

typedef struct cb_http_req {
    char        method[16];       // "GET", "POST", ...
    const char *path;             // "/rest/v1/player_views": raw (not percent-decoded), no query
    const char *query;            // "select=view&game_id=eq.abc": raw, "" when none
    int         minor;            // HTTP/1.<minor>
    cb_http_header headers[CB_HTTP_MAX_HEADERS];
    int         n_headers;
    const uint8_t *body;          // Content-Length bytes (NULL when 0)
    size_t      body_len;
    bool        keep_alive;       // what the client asked for (1.1 default, Connection: close)
    char        peer[64];         // remote address as the socket saw it (Caddy, in production)
    uint64_t    conn_id;          // the connection it came on (server.h)
} cb_http_req;

// The header's value, or NULL. First match, case-insensitive name.
const char *cb_http_header_get(const cb_http_req *r, const char *name);
// The token of "Authorization: Bearer <token>" (case-insensitive scheme), or NULL.
const char *cb_http_bearer(const cb_http_req *r);
// The first `key` in the query string, percent-decoded ('+' is a space) into
// out with a NUL. false when absent, malformed or longer than cap-1.
bool cb_http_query_get(const cb_http_req *r, const char *key, char *out, size_t cap);

// Every key=value of a query string, decoded, in order. fn returns false to
// stop. Returns the pairs visited, or CB_E_INVALID for a bad escape or a key or
// value longer than 1023 bytes (fn has seen the pairs before it).
typedef bool (*cb_query_fn)(const char *key, const char *value, void *ud);
int cb_http_query_each(const char *query, cb_query_fn fn, void *ud);

// ---- the parser (pure) ---------------------------------------------------------

#define CB_HTTP_MORE 0   // not a whole request yet: read more and call again

// Reads one request from buf[0..len). Returns:
//   > 0   a whole request: that many bytes are consumed, and *out points into
//         buf (which the parse has written NULs into: do not parse it again)
//   0     CB_HTTP_MORE (buf is untouched)
//   < 0   minus the HTTP status to answer before closing: -400 malformed,
//         -411 a chunked body, -413 a body over max_body, -431 headers over
//         CB_HTTP_MAX_HEAD or CB_HTTP_MAX_HEADERS, -505 not HTTP/1.x
int cb_http_parse(char *buf, size_t len, size_t max_body, cb_http_req *out);

// ---- the response --------------------------------------------------------------

typedef struct cb_http_res {
    int    status;     // 200 unless set
    cb_buf headers;    // "Name: value\r\n" lines (Content-Length is added on write)
    cb_buf body;
    bool   close;      // close the connection after this response
    // The answer to a HEAD (cb_handle sets it): serialized with the body's
    // Content-Length but without the body. The Lambda encoder still sends the
    // body; the function URL front end drops it (and says Content-Length: 0).
    bool   head;
    // A WebSocket upgrade (ws.h cb_ws_upgrade fills these; nothing else should).
    bool   upgrade;
    const struct cb_ws_handlers *ws_handlers;
    void  *ws_ud;
} cb_http_res;

void cb_res_init(cb_http_res *res);
void cb_res_free(cb_http_res *res);
void cb_res_status(cb_http_res *res, int status);
// Adds a header line. Names and values must not contain CR or LF (refused silently).
void cb_res_header(cb_http_res *res, const char *name, const char *value);
// true when a header of that name has been added (case-insensitive).
bool cb_res_has_header(const cb_http_res *res, const char *name);
void cb_res_body(cb_http_res *res, const void *p, size_t n);     // replaces
// status + Content-Type + body in one call.
void cb_res_send(cb_http_res *res, int status, const char *content_type, const void *body, size_t n);
// Content-Type: text/plain; charset=utf-8.
void cb_res_text(cb_http_res *res, int status, const char *text);

// The bytes on the wire: status line, the headers, Content-Length, Connection,
// a blank line and the body. `keep_alive` is the connection's decision.
void cb_res_serialize(const cb_http_res *res, bool keep_alive, cb_buf *out);
const char *cb_http_reason(int status);

#endif
