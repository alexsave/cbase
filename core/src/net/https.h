// https.h - a minimal blocking HTTPS/1.1 client on OpenSSL: a provider's cloud endpoints, and the
// platform's own outbound calls (an identity provider's keys, token and revocation endpoints).
//
// One request per call, over a kept-alive TLS connection per host and port
// (a warm one-shot container reuses it across invocations). A request that
// fails on a REUSED connection before any response byte arrives (the peer
// closed it while idle, or the container was frozen) is retried once on a
// fresh connection; nothing else is retried.
//
// TLS 1.2 or newer, the peer's certificate verified against a CA bundle and
// its name checked against `host`. The bundle is, in order: the file given to
// cb_https_set_ca_file, $CBASE_CA_FILE, and else the search tls.h describes
// (OpenSSL's own file, then a fixed list of system bundles).
//
// Bodies: Content-Length, chunked, or (with Connection: close) read to EOF.
#ifndef CB_HTTPS_H
#define CB_HTTPS_H

#include "util/str.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CB_HTTPS_MAX_BODY (32u << 20)

typedef struct {
    int    status;
    cb_buf headers;   // the response head as text (status line, headers, blank line)
    cb_buf body;      // de-chunked
} cb_https_response;

void cb_https_response_free(cb_https_response *r);

// `headers`: "Name: value\r\n" lines (Host and Content-Length are added).
// `host` may carry no port; `port` 443 is left out of the Host header.
// CB_OK with *out filled (any status), or CB_E_IO for a connection or TLS
// failure, a malformed response, or a timeout after timeout_ms (the whole
// exchange, connect included).
int cb_https_request(const char *host, int port, const char *method, const char *path, const char *headers,
                     const void *body, size_t body_len, int timeout_ms, cb_https_response *out);

// Whose call a request is, for the tally below.
typedef enum { CB_HTTPS_STORE, CB_HTTPS_PUSH, CB_HTTPS_OTHER } cb_https_role;

// The same exchange over plain TCP, no TLS, for a peer on the same machine or a network the
// caller trusts: a local stand-in for a cloud service (a store on http://127.0.0.1:8000, the
// instance metadata service), or a product's call to a process of its own on loopback (a
// companion app's local bridge). Nothing is encrypted or authenticated, so never for a cloud
// host or anything a stranger could stand in for. Everything above holds: the caller's header
// lines, a binary body either way (Content-Length or chunked), timeout_ms for the whole exchange,
// the kept-alive connection per host and port with its one retry, and a peer that closes after
// its answer. The tally has no host name to tell the role by, so the caller names it: `role`.
int cb_http_plain_request(cb_https_role role, const char *host, int port, const char *method, const char *path,
                          const char *headers, const void *body, size_t body_len, int timeout_ms,
                          cb_https_response *out);

// What this process's requests cost since the last take, by role: the store
// (a remote store, and a plain-HTTP stand-in for it), the pushes (a push
// broker), and anything else (an invoke, an edge invalidation, a secret, an
// identity provider, a product's own loopback call). `dials` counts new connections (TCP, plus TLS on https),
// the requests that found no warm one. Process-wide and lock-free; a one-shot
// provider's main takes it once per invocation for its timing line, where one
// invocation runs at a time.
typedef struct {
    int     calls;
    int     dials;
    int64_t us;      // wall time inside the requests, connects included
} cb_https_count;
typedef struct {
    cb_https_count store, push, other;
} cb_https_tally;
void cb_https_tally_take(cb_https_tally *out);
// Which TLS hosts are the store's and the pushes' (the tally knows no cloud by name): a host that
// starts with store_prefix is the store's; one that contains push_part is the pushes'; every other
// is other. Set once at start, before any request, by the provider whose main reads the tally;
// unset, every TLS host is other. A plain-HTTP request is the role its caller names
// (cb_http_plain_request).
void cb_https_tally_roles(const char *store_prefix, const char *push_part);

// Uses `path` (a PEM bundle) for verification from now on, or the default
// search above when NULL. Drops pooled connections. CB_OK, or CB_E_IO when
// the file does not load.
int cb_https_set_ca_file(const char *path);

// ---- the response parser (pure; exposed for the tests) ----------------------
//
// Reads one response from buf[0..len). `head_only` is a response to HEAD (no
// body whatever the headers say); `eof` says the peer has closed, which ends a
// body that has neither a length nor chunks. Returns:
//   > 0  a whole response: that many bytes consumed; out's status, headers and
//        body filled (out's buffers are reset first), *keep_alive set
//   0    not a whole response yet
//   < 0  CB_E_IO (malformed, or a body over CB_HTTPS_MAX_BODY)
// The times this process has set a socket's read and write timeouts (https.c
// bound(): once per connection while its requests carry one timeout). For the tests.
int64_t cb_https_timeout_sets(void);

int cb_https_parse_response(const uint8_t *buf, size_t len, bool head_only, bool eof, cb_https_response *out,
                            bool *keep_alive);

#endif
