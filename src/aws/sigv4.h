// sigv4.h - AWS Signature Version 4 and the credentials it signs with.
//
// Every AWS call c_base makes (DynamoDB, the API Gateway management API,
// Lambda Invoke) is an HTTPS request signed here: canonical request, string to
// sign, the derived key HMAC-SHA256 chain (libcrypto), and the Authorization,
// X-Amz-Date, X-Amz-Content-Sha256 and (with session credentials)
// X-Amz-Security-Token headers. Test vectors: AWS's published SigV4 test suite
// (get-vanilla, post-vanilla, ...) in test/unit_sigv4.c.
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
// AWS_REGION (all set by Lambda). CB_OK, or CB_E_INVALID when a key is missing.
int cb_aws_creds_from_env(cb_aws_creds *out);

// One request to sign. `headers` are "name:value" lines the caller sends and
// wants signed (host is added); `payload` is the exact body.
typedef struct {
    const char *method;     // "POST"
    const char *host;       // "dynamodb.us-west-1.amazonaws.com"
    const char *path;       // "/" (already URI-encoded as AWS wants it)
    const char *query;      // "" or the canonical query string
    const char *service;    // "dynamodb", "execute-api", "lambda"
    const char *const *headers;
    int n_headers;
    const void *payload;
    size_t payload_len;
    int64_t now_ms;         // the signing time
} cb_sigv4_req;

// Appends the headers to send ("Name: value\r\n" lines: Authorization,
// X-Amz-Date, X-Amz-Content-Sha256, X-Amz-Security-Token) to `out`.
int cb_sigv4_sign(const cb_aws_creds *c, const cb_sigv4_req *r, cb_buf *out);

#endif
