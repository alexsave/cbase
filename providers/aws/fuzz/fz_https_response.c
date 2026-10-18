// fz_https_response.c - the HTTP/1.1 response parser of the AWS client
// (aws/https.h cb_https_parse_response): what DynamoDB, Lambda, API
// Gateway's management API, CloudFront and the Lambda Runtime API answer,
// chunked or not. Fed in pieces as a socket would, every prefix must answer
// "more" or the same response.
#include "fz.h"

#include "aws/https.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 1) return 0;
    bool head_only = data[0] & 1, eof = data[0] & 2;
    size_t n = size - 1;
    uint8_t *buf = fz_dup(data + 1, n);
    cb_https_response r;
    memset(&r, 0, sizeof r);
    cb_buf_init(&r.headers);
    cb_buf_init(&r.body);
    bool ka = false;
    int k = cb_https_parse_response(buf, n, head_only, eof, &r, &ka);
    FZ_CHECK(k <= (int)n);
    if (k > 0) {
        FZ_CHECK(r.status >= 0);
        int status = r.status;
        size_t body = r.body.len;
        uint8_t *copy = fz_dup(r.body.p ? r.body.p : (const uint8_t *)"", body);
        // The same bytes with more after them read the same response.
        uint8_t *more = malloc(n + 7);
        memcpy(more, buf, n);
        memcpy(more + n, "HTTP/1.", 7);
        cb_https_response r2;
        memset(&r2, 0, sizeof r2);
        cb_buf_init(&r2.headers);
        cb_buf_init(&r2.body);
        int k2 = cb_https_parse_response(more, n + 7, head_only, false, &r2, &ka);
        // A body read to EOF has no end but the close, so only a delimited one
        // must come back the same.
        if (k2 > 0) FZ_CHECK(r2.status == status);
        if (k2 > 0 && !eof) FZ_CHECK(k2 == k && r2.body.len == body && (!body || memcmp(r2.body.p, copy, body) == 0));
        cb_https_response_free(&r2);
        free(more);
        free(copy);
    }
    cb_https_response_free(&r);
    free(buf);
    return 0;
}
