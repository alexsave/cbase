// unit_query_wire.c - cbc_query_add (cbase/client.h) against the server that reads its query: a
// path built by the client goes on the wire as a request line, core's own parser (net/http.h
// cb_http_parse) takes it apart, and cb_http_query_find gives back the very bytes the client was
// handed, for every byte a query could mistake for its own (& = + # % and a space) and for UTF-8.
// Linked from the client core and core's net/http.c with what it needs (client/Makefile), and
// nothing else of the server.
#include "t.h"

#include "cbase/client.h"
#include "net/http.h"
#include "util/err.h"

#include <string.h>

// Builds "/q" with each key and value through cbc_query_add, sends it as a request line, and reads
// every value back through the server's parser: each equal to the bytes it was built from.
static void round_trip(const char *const *keys, const char *const *vals, int n) {
    char path[CBC_PATH_MAX] = "/q";
    size_t len = 2;
    for (int i = 0; i < n; i++) T_EQ_INT(cbc_query_add(path, sizeof path, &len, keys[i], vals[i], strlen(vals[i])), 0);
    char req[512];
    size_t r = 0;
    memcpy(req, "GET ", 4); r = 4;
    memcpy(req + r, path, len); r += len;
    const char *tail = " HTTP/1.1\r\nHost: x\r\n\r\n";
    memcpy(req + r, tail, strlen(tail)); r += strlen(tail);
    cb_http_req q;
    T_EQ_INT(cb_http_parse(req, r, 0, &q), (long long)r);
    T_EQ_STR(q.path, "/q");
    for (int i = 0; i < n; i++) {
        char got[256];
        T_EQ_INT(cb_http_query_find(&q, keys[i], got, sizeof got), (long long)strlen(vals[i]));
        T_EQ_STR(got, vals[i]);
    }
}

static void every_reserved_byte_comes_back(void) {
    const char *keys[] = { "name", "sep", "plus", "hash", "pct", "space", "utf8" };
    const char *vals[] = { "Ada & Grace", "a=b&c=d", "1+1", "#top", "100%", " ", "Gr\303\242ce \342\234\223" };
    round_trip(keys, vals, 7);
}

static void keys_with_reserved_bytes_come_back(void) {
    const char *keys[] = { "a b", "x&y", "k=v", "+" };
    const char *vals[] = { "1", "2", "3", "4" };
    round_trip(keys, vals, 4);
}

static void an_empty_value_comes_back_empty(void) {
    const char *keys[] = { "name", "next" };
    const char *vals[] = { "", "x" };
    round_trip(keys, vals, 2);
}

int main(void) {
    T_RUN(every_reserved_byte_comes_back);
    T_RUN(keys_with_reserved_bytes_come_back);
    T_RUN(an_empty_value_comes_back_empty);
    return T_DONE();
}
