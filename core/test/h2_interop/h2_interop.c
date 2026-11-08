// h2_interop.c - the HTTP/2 client and its gRPC calls (net/grpc.h) against Node's
// node:http2 (core/test/h2_interop/server.js), the stack amgi's Gemini client and its
// fake server run on. Over cleartext and TLS (ALPN h2, the certificate verified):
// echoes of many sizes on one connection, so Node's HPACK names its own earlier fields
// from its dynamic table and Huffman-codes its strings; the metadata as Node saw it
// (the API key never-indexed, grpc-timeout); a trailers-only RESOURCE_EXHAUSTED with
// its message and details; an unknown method; and calls from many threads at once.
//
// usage: h2_interop <cleartext port> <tls port> <ca file>   (core/test/h2_interop/run.sh)
// Exit 0 when every check passed.
#include "net/grpc.h"
#include "util/err.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks, g_failed;

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        g_checks++;                                                                       \
        if (!(cond)) { g_failed++; fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); } \
    } while (0)

static const uint8_t FAIL_DETAILS[] = { 0x08, 0x08, 0x12, 0x05, 'q', 'u', 'o', 't', 'a', 0x1a, 0x00 };

typedef struct {
    cb_h2_channel *ch;
    int            i;
    bool           ok;
} worker;

static void *work(void *arg) {
    worker *w = arg;
    char req[64];
    int n = snprintf(req, sizeof req, "thread %d says hello", w->i);
    cb_grpc_reply r;
    cb_grpc_reply_init(&r);
    for (int k = 0; k < 5; k++) {
        int rc = cb_grpc_unary(w->ch, "/interop.Echo/Echo", NULL, 0, req, (size_t)n, 20000, 0, &r);
        w->ok = rc == CB_OK && r.code == 0 && r.msg_len == (size_t)n && memcmp(r.msg, req, (size_t)n) == 0;
        if (!w->ok) { fprintf(stderr, "thread %d: rc %d code %d %s\n", w->i, rc, r.code, r.resp.err); break; }
    }
    cb_grpc_reply_free(&r);
    return NULL;
}

static void run(const char *origin, const char *ca) {
    cb_h2_opts o;
    memset(&o, 0, sizeof o);
    o.ca_file = ca;
    cb_h2_channel *ch = NULL;
    CHECK(cb_h2_channel_new(origin, &o, &ch) == CB_OK);
    if (!ch) return;
    cb_grpc_reply r;
    cb_grpc_reply_init(&r);

    // Echoes, small to past the 64 KB windows, every one on the first connection.
    static uint8_t big[300000];
    for (size_t i = 0; i < sizeof big; i++) big[i] = (uint8_t)(i * 31 + 7);
    static const size_t sizes[] = { 0, 1, 5, 127, 128, 4096, 16384, 16385, 65535, 65536, 100000, 300000 };
    for (int round = 0; round < 3; round++) {
        for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
            int rc = cb_grpc_unary(ch, "/interop.Echo/Echo", NULL, 0, big, sizes[i], 20000, 0, &r);
            CHECK(rc == CB_OK);
            CHECK(r.code == CB_GRPC_OK && r.http_status == 200);
            CHECK(r.has_msg && r.msg_len == sizes[i] && (!sizes[i] || memcmp(r.msg, big, sizes[i]) == 0));
            CHECK(rc == CB_OK && strcmp((const char *)r.message.p, "ok") == 0);
            if (rc != CB_OK) { fprintf(stderr, "echo %zu: %s (why %d)\n", sizes[i], r.resp.err, r.resp.why); return; }
        }
    }
    // Node's own fields came through its dynamic table and Huffman intact.
    cb_span v;
    CHECK(cb_h2_response_get(&r.resp, false, "x-interop-server", &v) && v.n > 5 && memcmp(v.p, "node v", 6) == 0);
    CHECK(cb_h2_response_get(&r.resp, false, "x-interop-long", &v) && v.n == 60 * 4);
    CHECK(cb_h2_response_get(&r.resp, false, "grpc-accept-encoding", &v) && v.n == 8 && memcmp(v.p, "identity", 8) == 0);

    // The metadata as Node saw it.
    cb_h2_field key = { "x-goog-api-key", "AIza-interop-key", 14, 16, true };
    CHECK(cb_grpc_unary(ch, "/interop.Echo/Meta", &key, 1, "m", 1, 1500, 0, &r) == CB_OK);
    CHECK(r.code == 0 && r.msg_len == 28 && memcmp(r.msg, "AIza-interop-key|1500m|never", 28) == 0);

    // A quota refusal, trailers-only, as Gemini sends it.
    CHECK(cb_grpc_unary(ch, "/interop.Echo/Fail", NULL, 0, "f", 1, 5000, 0, &r) == CB_OK);
    CHECK(r.code == CB_GRPC_RESOURCE_EXHAUSTED && r.http_status == 200 && !r.has_msg);
    CHECK(strcmp((const char *)r.message.p, "You exceeded your current quota: 100% used.") == 0);
    CHECK(r.details.len == sizeof FAIL_DETAILS && memcmp(r.details.p, FAIL_DETAILS, sizeof FAIL_DETAILS) == 0);
    CHECK(r.resp.n_trailers == 0);

    // An unknown method.
    CHECK(cb_grpc_unary(ch, "/interop.Echo/Nope", NULL, 0, "x", 1, 5000, 0, &r) == CB_OK);
    CHECK(r.code == 12);
    CHECK(strcmp((const char *)r.message.p, "unknown method /interop.Echo/Nope") == 0);

    // Many threads at once on the one connection.
    pthread_t th[16];
    worker w[16];
    for (int i = 0; i < 16; i++) {
        w[i] = (worker){ .ch = ch, .i = i };
        pthread_create(&th[i], NULL, work, &w[i]);
    }
    for (int i = 0; i < 16; i++) {
        pthread_join(th[i], NULL);
        CHECK(w[i].ok);
    }
    CHECK(cb_h2_channel_dials(ch) == 1);
    cb_grpc_reply_free(&r);
    cb_h2_channel_free(ch);
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s <cleartext port> <tls port> <ca file>\n", argv[0]);
        return 2;
    }
    char origin[128];
    snprintf(origin, sizeof origin, "http://127.0.0.1:%s", argv[1]);
    run(origin, NULL);
    int clear_failed = g_failed;
    printf("h2 interop, cleartext: %s\n", clear_failed ? "FAILED" : "ok");
    snprintf(origin, sizeof origin, "https://localhost:%s", argv[2]);
    run(origin, argv[3]);
    printf("h2 interop, TLS: %s\n", g_failed > clear_failed ? "FAILED" : "ok");
    printf("%s h2_interop: %d checks, %d failed\n", g_failed ? "FAIL" : "ok  ", g_checks, g_failed);
    return g_failed || !g_checks ? 1 : 0;
}
