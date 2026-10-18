// net.h - the bench's client side: HTTP/1.1 and WebSocket, over TCP or TLS.
//
// One client for both targets, so a number from `cbase serve` and a number
// from https://api.foolish.cards are the same measurement:
//   * http:// is a kept-alive TCP connection per bn_http (as a browser keeps
//     one), parsed by src/aws/https.c's own response parser;
//   * https:// goes through cb_https_request itself (src/aws/https.c: its
//     kept-alive TLS pool, its CA search, its certificate check), the client
//     the Lambda uses to reach AWS;
//   * a WebSocket is ws:// or wss:// beside the base URL's scheme, on its own
//     connection (https.c has no upgrade), TLS verified the same way.
// Each call reports the bytes it put on the wire and took off it (the request
// and response heads and bodies; TLS framing is not counted).
#ifndef CB_BENCH_NET_H
#define CB_BENCH_NET_H

#include "util/str.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    bool tls;
    char host[256];
    int  port;
} bn_url;

// "http://host[:port]" or "https://host[:port]", an optional trailing "/".
// false for anything else (a path is refused: the bench owns the paths).
bool bn_url_parse(const char *s, bn_url *out);

// Microseconds on the monotonic clock.
int64_t bn_now_us(void);

typedef struct {
    int    status;
    cb_buf head;     // the response head (status line, headers, blank line)
    cb_buf body;
    size_t sent;     // request bytes written (head + body)
    size_t recv;     // response bytes read (head + body)
} bn_resp;

void bn_resp_init(bn_resp *r);
void bn_resp_free(bn_resp *r);

// One HTTP client: its own kept-alive connection on http://; on https:// the
// shared pool of https.c (thread-safe; each request takes a connection out).
typedef struct {
    bn_url u;
    int    fd;       // http:// only, -1 when closed
} bn_http;

void bn_http_init(bn_http *h, const bn_url *u);
void bn_http_close(bn_http *h);
// `headers`: "Name: value\r\n" lines (Host and Content-Length are added).
// 0 with *out filled (any status), -1 on a connection or parse failure or a
// timeout. A request on a reused connection that fails before any response
// byte is sent once more on a fresh one, as https.c does.
int bn_http_request(bn_http *h, const char *method, const char *path, const char *headers, const void *body,
                    size_t body_len, int timeout_ms, bn_resp *out);

// A WebSocket client. Not shared between threads while in use; bn_ws_recv and
// bn_ws_send may be called from one thread after another.
typedef struct bn_ws bn_ws;

// Opens `path` (with its query) on the URL's host: ws:// for http://, wss://
// for https://. NULL on failure.
bn_ws *bn_ws_open(const bn_url *u, const char *path, int timeout_ms);
// The same with extra request headers ("Name: value\r\n" lines: a
// subprotocol, the edge's origin header), NULL for none.
bn_ws *bn_ws_open_with(const bn_url *u, const char *path, const char *headers, int timeout_ms);
int    bn_ws_send_bin(bn_ws *w, const void *data, size_t len);
void   bn_ws_close(bn_ws *w);
int    bn_ws_send_text(bn_ws *w, const char *text, size_t len);
// One frame's payload into out: its opcode, or -1 on a timeout, -2 when the
// peer closed or the stream broke. A ping is answered and skipped.
int    bn_ws_recv(bn_ws *w, int timeout_ms, cb_buf *out);


// One stream frame (netwire.h) sent as base64 text, as the clients send every
// frame (API Gateway refuses a client's binary frame). 0 on success.
int    bn_ws_send_frame(bn_ws *w, const void *nw_frame);
// A frame as it arrived into *nw_frame (an NwFrame): binary from `cbase serve`,
// base64 text from API Gateway. false for anything else.
bool   bn_ws_frame_of(int op, const cb_buf *in, void *nw_frame);
#endif
