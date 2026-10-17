// subscribe.c - `cbase-bench subscribe`: how long a fresh socket takes to hear.
//
// Signs up one user, then --n times opens a fresh stream socket and subscribes
// to the user's games list, timing from the socket being open to the ACK that
// accepts the subscription, as a client's first push would wait. Two ways:
//
//   sub_after_auth  NW_WS_AUTH then NW_WS_SUB, and on a refusal for the token
//                   both again on the client's schedule (NC_RETRY_FIRST_MS,
//                   doubling). Behind API Gateway each frame is its own
//                   invocation, so the SUB can be read before AUTH is stored.
//   sub_as          NW_WS_SUB_AS: the subscription carries its token (--as,
//                   against a server that takes it).
//
// The account is deleted at the end. Every body is the kernel's (netwire.h).
#include "bench.h"
#include "net.h"

#include "net/ws.h"
#include "netclient.h"   // NC_RETRY_*: the client's retry schedule, one owner
#include "netwire.h"
#include "util/uuid.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TIMEOUT_MS 15000

static void sleep_ms(int ms) {
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static int signup(bn_http *h, char *token, size_t cap) {
    uint8_t rnd[4];
    cb_random_bytes(rnd, sizeof rnd);
    NwRequest q;
    memset(&q, 0, sizeof q);
    q.kind = NW_SIGNUP;
    q.name_len = (uint8_t)snprintf(q.name, sizeof q.name, "BENCHS%02X%02X%02X%02X", rnd[0], rnd[1], rnd[2], rnd[3]);
    q.password_len = (uint8_t)snprintf(q.password, sizeof q.password, "bench-pass-123");
    uint8_t body[256];
    int n = nw_request_encode(&q, body, sizeof body);
    bn_resp r;
    bn_resp_init(&r);
    int rc = n > 0 ? bn_http_request(h, "POST", "/v1/auth/signup", "Content-Type: application/octet-stream\r\n", body,
                                     (size_t)n, TIMEOUT_MS, &r) : -1;
    NwSession s;
    bool ok = rc == 0 && r.status == 200 && nw_session_decode(r.body.p, (int)r.body.len, &s) == NW_OK;
    if (ok) snprintf(token, cap, "%.*s", (int)s.access_len, s.access);
    else fprintf(stderr, "bench: signup answered %d\n", r.status);
    bn_resp_free(&r);
    return ok ? 0 : -1;
}

static void delete_account(bn_http *h, const char *token) {
    char headers[NW_ACCESS_MAX + 64];
    snprintf(headers, sizeof headers, "Authorization: Bearer %s\r\n", token);
    bn_resp r;
    bn_resp_init(&r);
    if (bn_http_request(h, "POST", "/v1/account/delete", headers, NULL, 0, TIMEOUT_MS, &r) != 0 || r.status >= 300)
        fprintf(stderr, "bench: account delete answered %d\n", r.status);
    bn_resp_free(&r);
}

static int send_sub(bn_ws *ws, const char *token, bool as, uint16_t ref) {
    NwFrame f;
    memset(&f, 0, sizeof f);
    if (!as) {
        f.kind = NW_WS_AUTH;
        f.token_len = (uint16_t)strlen(token);
        memcpy(f.token, token, f.token_len);
        if (bn_ws_send_frame(ws, &f) != 0) return -1;
        memset(&f, 0, sizeof f);
    }
    f.kind = as ? NW_WS_SUB_AS : NW_WS_SUB;
    if (as) {
        f.token_len = (uint16_t)strlen(token);
        memcpy(f.token, token, f.token_len);
    }
    f.ref = ref;
    f.what = NW_SUB_GAMES;
    return bn_ws_send_frame(ws, &f);
}

// One fresh socket: microseconds from open to the accepting ACK, -1 on failure;
// *refusals counts the refusals for the token on the way.
static int64_t one(const bn_url *u, const char *token, bool as, int *refusals) {
    bn_ws *ws = bn_ws_open(u, "/v1/stream?proto=nw1", TIMEOUT_MS);
    if (!ws) return -1;
    int64_t t0 = bn_now_us(), out = -1;
    cb_buf b;
    cb_buf_init(&b);
    for (int attempt = 0, backoff = NC_RETRY_FIRST_MS; attempt < 6; attempt++) {
        uint16_t ref = (uint16_t)(attempt + 1);
        if (send_sub(ws, token, as, ref) != 0) break;
        int why = -1;
        for (int i = 0; i < 20 && why < 0; i++) {
            int op = bn_ws_recv(ws, TIMEOUT_MS, &b);
            if (op < 0) break;
            NwFrame f;
            if (bn_ws_frame_of(op, &b, &f) && f.kind == NW_WS_ACK && f.ref == ref) why = f.why;
        }
        if (why == 0) { out = bn_now_us() - t0; break; }
        if (why != ASK_WHY_AUTH) break;
        (*refusals)++;
        sleep_ms(backoff);
        backoff = backoff * 2 > NC_RETRY_MAX_MS ? NC_RETRY_MAX_MS : backoff * 2;
    }
    cb_buf_free(&b);
    bn_ws_close(ws);
    return out;
}

int bench_subscribe(const bench_opts *o) {
    bn_url u;
    if (!bn_url_parse(o->url, &u)) { fprintf(stderr, "bench: not a base URL: %s\n", o->url); return 2; }
    bn_http h;
    bn_http_init(&h, &u);
    char token[NW_ACCESS_MAX + 1];
    if (signup(&h, token, sizeof token) != 0) { bn_http_close(&h); return 1; }
    int64_t *plain = calloc((size_t)o->n, sizeof *plain), *as = calloc((size_t)o->n, sizeof *as);
    int n_plain = 0, n_as = 0, refused_plain = 0, refused_as = 0, failed = 0;
    for (int i = 0; i < o->n && plain && as; i++) {
        int64_t t = one(&u, token, false, &refused_plain);
        if (t >= 0) plain[n_plain++] = t; else failed++;
        if (o->as) {
            t = one(&u, token, true, &refused_as);
            if (t >= 0) as[n_as++] = t; else failed++;
        }
    }
    delete_account(&h, token);
    bn_http_close(&h);

    char date[32], host[64], note[256];
    bench_date(date, sizeof date);
    bench_host(host, sizeof host);
    snprintf(note, sizeof note, "%s%srefused_after_auth=%d refused_as=%d failed=%d", o->note ? o->note : "",
             o->note ? " " : "", refused_plain, refused_as, failed);
    bench_note_load(note, sizeof note);
    bs_run run;
    bs_run_init(&run, "subscribe", date, host, o->url, note);
    bs_run_lat(&run, "sub_after_auth", plain, n_plain, 0, 0);
    if (o->as) bs_run_lat(&run, "sub_as", as, n_as, 0, 0);
    int rc = bench_save(&run, o);
    free(plain);
    free(as);
    return rc != 0 ? 1 : failed ? 1 : 0;
}
