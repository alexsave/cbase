// aws_call.h - one signed AWS request: credentials, SigV4, HTTPS.
//
// The shared step under apigw_mgmt.c and lambda_invoke.c, and the one
// store_dynamo.c can use for DynamoDB: read the Lambda credentials, sign the
// request for `service`, send it over https.h, hand back the response.
#ifndef CB_AWS_CALL_H
#define CB_AWS_CALL_H

#include "https.h"

#include <stdbool.h>
#include <stddef.h>

// An endpoint URL, "https://host[:port][/base/path]" (wss:// is read the same
// way, for an API Gateway WebSocket URL). base gets "" or "/base/path" with no
// trailing slash; *tls is true.
//
// "http://host[:port]" is a local stand-in (DynamoDB Local): *tls false, port
// 80 unless given. Plain HTTP is decided by the URL alone and is refused for
// an AWS host (*.amazonaws.com), so a typo cannot send signed requests to AWS
// in the clear. CB_OK, or CB_E_INVALID.
int cb_aws_url_parse(const char *url, char *host, size_t host_cap, int *port, bool *tls, char *base, size_t base_cap);

// `headers` are "Name: value" strings to send AND sign (x-amz-target,
// content-type, x-amz-invocation-type). `path` exactly as sent (percent-
// encoded once). CB_OK with *out filled (any status: the caller reads it), or
// CB_E_IO (no credentials, or the connection failed), CB_E_INVALID.
// `tls` false (an http:// endpoint from cb_aws_url_parse) sends it over plain
// TCP; every AWS host is reached with true.
int cb_aws_call(const char *service, const char *method, const char *host, int port, bool tls, const char *path,
                const char *const *headers, int n_headers, const void *body, size_t body_len, int timeout_ms,
                cb_https_response *out);

// Appends s percent-encoded (every byte but A-Z a-z 0-9 - _ . ~) to b: one
// path segment as AWS wants it on the wire.
void cb_aws_path_segment(cb_buf *b, const char *s);

#endif
