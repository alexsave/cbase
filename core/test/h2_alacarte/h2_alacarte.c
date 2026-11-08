// h2_alacarte.c - a consumer of the HTTP/2 and gRPC module alone: linked from net/h2*,
// net/hpack, net/grpc, net/tls and the util and lib pieces they need, and nothing else
// (the Makefile's H2_ALACARTE_SRC). That it links at all proves the module reaches no
// provider (providers/aws, providers/local) and no product code; core/test/h2_alacarte/check.sh
// then holds its symbols to that. Run, it touches each layer once: HPACK both ways, the
// session's preface, the gRPC pieces, and a channel's call to a port nobody listens on.
#include "net/grpc.h"
#include "net/hpack.h"
#include "util/err.h"

#include <stdio.h>
#include <string.h>

static int count(void *ctx, const char *n, size_t nl, const char *v, size_t vl, bool never) {
    (void)n; (void)v;
    *(int *)ctx += nl == 14 && vl == 1 && never;
    return CB_OK;
}

int main(void) {
    int bad = 0;
    cb_buf b;
    cb_buf_init(&b);
    cb_hpack_encode(&b, "x-goog-api-key", 14, "k", 1, true);
    cb_hpack_dec d;
    bad |= cb_hpack_dec_init(&d, CB_HPACK_TABLE_DEFAULT) != CB_OK;
    int fields = 0;
    bad |= cb_hpack_decode(&d, b.p, b.len, count, &fields) != CB_OK || fields != 1;
    cb_hpack_dec_free(&d);
    cb_buf_free(&b);

    static const cb_h2_callbacks cb = { NULL, NULL, NULL };
    cb_h2_sess *s = cb_h2_sess_new(NULL, &cb);
    cb_buf out;
    cb_buf_init(&out);
    bad |= s == NULL || cb_h2_sess_flush(s, &out) < CB_H2_PREFACE_LEN || memcmp(out.p, CB_H2_PREFACE, CB_H2_PREFACE_LEN) != 0;
    cb_h2_sess_free(s);
    cb_buf_free(&out);

    char t[16];
    cb_grpc_timeout(1500, t, sizeof t);
    bad |= strcmp(t, "1500m") != 0;

    // Port 1 on loopback: nobody listens, so the dial is refused at once.
    cb_h2_channel *ch = NULL;
    bad |= cb_h2_channel_new("http://127.0.0.1:1", NULL, &ch) != CB_OK;
    cb_grpc_reply r;
    cb_grpc_reply_init(&r);
    if (ch) {
        bad |= cb_grpc_unary(ch, "/a.B/C", NULL, 0, "x", 1, 2000, 0, &r) != CB_E_IO;
        bad |= r.resp.why != CB_H2_WHY_CONNECT;
        cb_h2_channel_free(ch);
    }
    cb_grpc_reply_free(&r);
    // The TLS context loads the system's CA bundle, as a real https:// channel would.
    bad |= cb_h2_channel_new("https://example.com", NULL, &ch) != CB_OK;
    cb_h2_channel_free(ch);
    printf("%s h2_alacarte: the HTTP/2 and gRPC module alone\n", bad ? "FAIL" : "ok  ");
    return bad;
}
