// fz_query.c - query strings: every key=value pair decoded
// (cb_http_query_each), one key looked up as a route does
// (cb_http_query_get), and the stream's protocol pick (cb_nw_proto_of_query),
// over the raw query a URL carries.
#include "fz.h"

#include "net/http.h"
#include "realtime/stream.h"

static bool each(const char *k, const char *v, void *ud) {
    FZ_CHECK(strlen(k) <= 1023 && strlen(v) <= 1023);
    return ++*(int *)ud < 1000000;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    char *q = fz_cstr(data, size);
    int visited = 0;
    int n = cb_http_query_each(q, each, &visited);
    FZ_CHECK(n < 0 || n == visited);
    cb_http_req r;
    memset(&r, 0, sizeof r);
    r.path = "/";
    r.query = q;
    char out[64];
    if (cb_http_query_get(&r, "proto", out, sizeof out)) FZ_CHECK(strlen(out) < sizeof out);
    (void)cb_http_query_get(&r, "", out, sizeof out);
    int p = cb_nw_proto_of_query(q);
    FZ_CHECK(p == CB_VSN_NW || p < 0);
    free(q);
    return 0;
}
