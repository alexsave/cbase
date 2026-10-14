// https.h - a minimal blocking HTTPS/1.1 client on OpenSSL, for AWS endpoints.
//
// One request per call, over a kept-alive TLS connection per host and port
// (a warm Lambda container reuses it across invocations). A request that
// fails on a REUSED connection before any response byte arrives (the peer
// closed it while idle, or the container was frozen) is retried once on a
// fresh connection; nothing else is retried.
//
// TLS 1.2 or newer, the peer's certificate verified against a CA bundle and
// its name checked against `host`. The bundle is, in order: the file given to
// cb_https_set_ca_file, $CBASE_CA_FILE, the first that exists of
// /etc/pki/tls/certs/ca-bundle.crt (Amazon Linux, so the Lambda runtime),
// /etc/ssl/certs/ca-certificates.crt (Debian, Alpine) and /etc/ssl/cert.pem,
// and else OpenSSL's default paths (Homebrew's on a Mac). The static Alpine
// build's OpenSSL looks under /etc/ssl by default, which provided.al2023 does
// not populate the same way, hence the explicit list.
//
// Bodies: Content-Length, chunked, or (with Connection: close) read to EOF.
#ifndef CB_HTTPS_H
#define CB_HTTPS_H

#include "util/str.h"

#include <stdbool.h>
#include <stddef.h>

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

// The same exchange over plain TCP, no TLS: for a local stand-in only
// (DynamoDB Local on http://127.0.0.1:8000). cb_aws_call reaches it only for
// an endpoint URL that says http://, never for AWS's own hosts.
int cb_http_plain_request(const char *host, int port, const char *method, const char *path, const char *headers,
                          const void *body, size_t body_len, int timeout_ms, cb_https_response *out);

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
int cb_https_parse_response(const uint8_t *buf, size_t len, bool head_only, bool eof, cb_https_response *out,
                            bool *keep_alive);

#endif
