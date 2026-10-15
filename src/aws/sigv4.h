// sigv4.h - AWS Signature Version 4 and the credentials it signs with.
//
// Every AWS call c_base makes (DynamoDB, the API Gateway management API,
// Lambda Invoke) is an HTTPS request signed here: canonical request, string to
// sign, the derived key HMAC-SHA256 chain (libcrypto), and the Authorization,
// X-Amz-Date and (with session credentials) X-Amz-Security-Token headers.
// Signed headers: host, x-amz-date, x-amz-security-token when there is one,
// and every header the caller passes (x-amz-target, content-type, ...).
// Test vectors: AWS's published SigV4 test suite (get-vanilla, post-vanilla,
// post-x-www-form-urlencoded, post-sts-header-before, ...) in test/unit_sigv4.c.
#ifndef CB_SIGV4_H
#define CB_SIGV4_H

#include "util/str.h"

#include <stddef.h>
#include <stdint.h>

typedef struct {
    char access_key[128];
    char secret_key[128];
    char session_token[2048];   // "" without one
    char region[32];
} cb_aws_creds;

// From AWS_ACCESS_KEY_ID, AWS_SECRET_ACCESS_KEY, AWS_SESSION_TOKEN and
// AWS_REGION (AWS_DEFAULT_REGION as a fallback; Lambda sets all of them).
// CB_OK, or CB_E_INVALID when a key is missing or too long.
int cb_aws_creds_from_env(cb_aws_creds *out);

// One request to sign. `headers` are "name:value" strings the caller sends
// and wants signed (host is added; do not pass it); `payload` is the exact body.
typedef struct {
    const char *method;     // "POST"
    const char *host;       // "dynamodb.us-west-1.amazonaws.com", with ":port" when not 443
    const char *path;       // the path exactly as sent on the wire (each segment
                            // percent-encoded once); the canonical form encodes it
                            // again, as SigV4 wants for every service but S3
    const char *query;      // "" or NULL, or the query as sent ("b=2&a=1"; sorted here)
    const char *service;    // "dynamodb", "execute-api", "lambda"
    const char *const *headers;
    int n_headers;
    const void *payload;
    size_t payload_len;
    int64_t now_ms;         // the signing time
} cb_sigv4_req;

// Appends the headers to send ("Name: value\r\n" lines: Authorization,
// X-Amz-Date and, with a session token, X-Amz-Security-Token) to `out`.
// CB_OK, or CB_E_INVALID (a header without ':', more than 32 headers).
int cb_sigv4_sign(const cb_aws_creds *c, const cb_sigv4_req *r, cb_buf *out);

// The pieces cb_sigv4_sign computes, exposed for the test suite vectors:
// the canonical request, the string to sign and the hex signature.
int cb_sigv4_debug(const cb_aws_creds *c, const cb_sigv4_req *r, cb_buf *canonical, cb_buf *string_to_sign,
                   char signature_hex[65]);

#endif
