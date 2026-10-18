// fz_http.c - the HTTP/1.1 request parser (net/http.h cb_http_parse) and
// what a handler then reads off a parsed request: headers, the bearer, the
// query string. Bytes a socket hands `cbase serve`.
#include "fz.h"

#include "net/http.h"

static bool each(const char *k, const char *v, void *ud) {
    (void)strlen(k);
    (void)strlen(v);
    (*(int *)ud)++;
    return true;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 1) return 0;
    // The first byte picks the body limit, so the 413 path is reachable.
    size_t max_body = (size_t)data[0] * 64;
    char *buf = (char *)fz_dup(data + 1, size - 1);
    size_t len = size - 1;
    cb_http_req r;
    memset(&r, 0, sizeof r);
    int n = cb_http_parse(buf, len, max_body, &r);
    FZ_CHECK(n <= (int)len);
    if (n == CB_HTTP_MORE) {
        // MORE leaves the buffer untouched: a prefix must stay a prefix.
        FZ_CHECK(len == 0 || memcmp(buf, data + 1, len) == 0);
    } else if (n > 0) {
        FZ_CHECK(r.n_headers >= 0 && r.n_headers <= CB_HTTP_MAX_HEADERS);
        FZ_CHECK(r.body_len <= max_body);
        FZ_CHECK(r.body_len == 0 || ((const char *)r.body >= buf && (const char *)r.body + r.body_len <= buf + n));
        FZ_CHECK(r.path && r.query);
        (void)strlen(r.path);
        for (int i = 0; i < r.n_headers; i++) { (void)strlen(r.headers[i].name); (void)strlen(r.headers[i].value); }
        (void)cb_http_header_get(&r, "content-length");
        const char *b = cb_http_bearer(&r);
        if (b) (void)strlen(b);
        char out[256];
        (void)cb_http_query_get(&r, "since", out, sizeof out);
        (void)cb_http_query_get(&r, "proto", out, 1);
        int visited = 0;
        (void)cb_http_query_each(r.query, each, &visited);
    }
    free(buf);
    return 0;
}
