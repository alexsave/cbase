// https.h - a minimal blocking HTTPS/1.1 client on OpenSSL, for AWS endpoints.
//
// One request per call over a kept-alive TLS connection per host (a warm
// Lambda container reuses it across invocations; reconnects on any error).
// Certificates are verified against the system store (Lambda's
// /etc/pki/tls/certs, macOS via SSL_CTX_set_default_verify_paths), hostname
// checked. Content-Length bodies only; that is all AWS sends here.
#ifndef CB_HTTPS_H
#define CB_HTTPS_H

#include "util/str.h"

#include <stddef.h>

typedef struct {
    int    status;
    cb_buf headers;   // the response head as text
    cb_buf body;
} cb_https_response;

void cb_https_response_free(cb_https_response *r);

// `headers`: "Name: value\r\n" lines (Host and Content-Length are added).
// CB_OK with *out filled (any status), or CB_E_IO for a connection or TLS
// failure, or a timeout after timeout_ms.
int cb_https_request(const char *host, int port, const char *method, const char *path, const char *headers,
                     const void *body, size_t body_len, int timeout_ms, cb_https_response *out);

#endif
