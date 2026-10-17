// page.c - `cbase-bench page`: what a game page's stream costs, and how fast it hears.
//
// Sets up one throwaway account with one game it sits at (untimed), then:
//
//   1. --n page loads inside ONE wall-clock minute that starts on a boundary:
//      each opens a fresh stream socket, subscribes as a seated player's game
//      page does (the games list, the game, its chat), waits for every ACK and
//      the game's catch-up, holds the socket a second and closes it. The run
//      prints that minute, so on a quiet stack the table's
//      ConsumedWriteCapacityUnits for it (CloudWatch keeps one-minute sums) is
//      the page loads' alone. Two ways to subscribe:
//        --batch 0  three NW_WS_SUB_AS frames back to back, as the client sent
//                   them before NW_WS_BATCH (behind API Gateway each is its own
//                   invocation, and the three run at once)
//        --batch 1  one NW_WS_BATCH carrying the three (one invocation)
//      Timed: open to the last ACK (subscribed), open to the catch-up (caught_up).
//   2. --pushes chat messages on one subscribed socket, each timed from its
//      POST going out to its NW_WS_CHAT arriving (send_to_push), and from the
//      POST's answer to it (push_after_answer; negative when the push wins).
//
// The account is deleted at the end. Every body is the kernel's (netwire.h).
#include "bench.h"
#include "net.h"

#include "client_table.h"
#include "net/ws.h"
#include "netwire.h"
#include "util/uuid.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TIMEOUT_MS 15000
#define N_SUBS 3

typedef struct {
    bn_url  u;
    bn_http h;
    char    token[NW_ACCESS_MAX + 1];
    char    auth[NW_ACCESS_MAX + 64];
    char    gid[NW_GID_MAX + 1];
} page_ctx;

static void sleep_ms(int ms) {
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

// Sleeps until `extra_s` seconds past the next minute boundary.
static void next_minute(int extra_s) {
    time_t now = time(NULL);
    sleep_ms((int)(60 - now % 60 + extra_s) * 1000);
}

static int signup(page_ctx *c) {
    uint8_t rnd[4];
    cb_random_bytes(rnd, sizeof rnd);
    NwRequest q;
    memset(&q, 0, sizeof q);
    q.kind = NW_SIGNUP;
    q.name_len = (uint8_t)snprintf(q.name, sizeof q.name, "BENCHP%02X%02X%02X%02X", rnd[0], rnd[1], rnd[2], rnd[3]);
    q.password_len = (uint8_t)snprintf(q.password, sizeof q.password, "bench-pass-123");
    uint8_t body[256];
    int n = nw_request_encode(&q, body, sizeof body);
    bn_resp r;
    bn_resp_init(&r);
    int rc = n > 0 ? bn_http_request(&c->h, "POST", "/v1/auth/signup", "Content-Type: application/octet-stream\r\n", body,
                                     (size_t)n, TIMEOUT_MS, &r) : -1;
    NwSession s;
    bool ok = rc == 0 && r.status == 200 && nw_session_decode(r.body.p, (int)r.body.len, &s) == NW_OK;
    if (ok) {
        snprintf(c->token, sizeof c->token, "%.*s", (int)s.access_len, s.access);
        snprintf(c->auth, sizeof c->auth, "Authorization: Bearer %s\r\n", c->token);
    } else {
        fprintf(stderr, "bench: signup answered %d\n", r.status);
    }
    bn_resp_free(&r);
    return ok ? 0 : -1;
}

// A lobby the account sits at; its id read off the answer's envelope.
static int create_game(page_ctx *c) {
    char headers[NW_ACCESS_MAX + 128];
    snprintf(headers, sizeof headers, "%sContent-Type: application/octet-stream\r\n", c->auth);
    bn_resp r;
    bn_resp_init(&r);
    static ClientSlot slot;
    static ClientTable ct;
    client_init(&ct, &slot);
    bool ok = bn_http_request(&c->h, "POST", "/v1/create", headers, NULL, 0, TIMEOUT_MS, &r) == 0 && r.status == 200 &&
              client_adopt_envelope(&ct, r.body.p, (int)r.body.len) >= 0 && ct.gid_len > 0;
    if (ok) snprintf(c->gid, sizeof c->gid, "%.*s", (int)ct.gid_len, ct.gid);
    else fprintf(stderr, "bench: create answered %d\n", r.status);
    bn_resp_free(&r);
    return ok ? 0 : -1;
}

static void delete_account(page_ctx *c) {
    bn_resp r;
    bn_resp_init(&r);
    if (bn_http_request(&c->h, "POST", "/v1/account/delete", c->auth, NULL, 0, TIMEOUT_MS, &r) != 0 || r.status >= 300)
        fprintf(stderr, "bench: account delete answered %d\n", r.status);
    bn_resp_free(&r);
}

// The page's three subscriptions, refs 1..3.
static void sub_frame(const page_ctx *c, int i, NwFrame *f) {
    static const uint8_t what[N_SUBS] = { NW_SUB_GAMES, NW_SUB_GAME, NW_SUB_CHAT };
    memset(f, 0, sizeof *f);
    f->kind = NW_WS_SUB_AS;
    f->token_len = (uint16_t)strlen(c->token);
    memcpy(f->token, c->token, f->token_len);
    f->ref = (uint16_t)(i + 1);
    f->what = what[i];
    if (what[i] != NW_SUB_GAMES) {
        f->gid_len = (uint8_t)strlen(c->gid);
        memcpy(f->gid, c->gid, f->gid_len);
    }
}

static int send_subs(bn_ws *ws, const page_ctx *c, bool batch) {
    NwFrame f;
    if (!batch) {
        for (int i = 0; i < N_SUBS; i++) {
            sub_frame(c, i, &f);
            if (bn_ws_send_frame(ws, &f) != 0) return -1;
        }
        return 0;
    }
    static uint8_t inner[N_SUBS * (NW_ACCESS_MAX + 128)];
    static uint8_t raw[sizeof inner + 64];
    static char text[2 * sizeof raw];
    int at = 0;
    for (int i = 0; i < N_SUBS; i++) {
        uint8_t one[NW_ACCESS_MAX + 128];
        sub_frame(c, i, &f);
        int n = nw_frame_encode(&f, one, sizeof one);
        if (n < 0) return -1;
        n = nw_batch_add(one, n, inner + at, (int)sizeof inner - at);
        if (n < 0) return -1;
        at += n;
    }
    memset(&f, 0, sizeof f);
    f.kind = NW_WS_BATCH;
    f.blob.p = inner;
    f.blob.len = (uint32_t)at;
    int n = nw_frame_encode(&f, raw, sizeof raw);
    int t = n > 0 ? nw_text_encode(raw, n, text, sizeof text) : -1;
    return t > 0 ? bn_ws_send_text(ws, text, (size_t)t) : -1;
}

typedef struct {
    int acks, catchups, chats;
    int64_t t_acks, t_catchup, t_chat;
} seen;

static void take(const NwFrame *f, int64_t t, seen *s) {
    if (f->kind == NW_WS_ACK && f->why == 0 && ++s->acks == N_SUBS) s->t_acks = t;
    if (f->kind == NW_WS_CATCHUP && !s->catchups++) s->t_catchup = t;
    if (f->kind == NW_WS_CHAT && !s->chats++) s->t_chat = t;
}

// One received message: a frame, or a batch of them.
static void take_message(int op, const cb_buf *b, int64_t t, seen *s) {
    NwFrame f;
    if (!bn_ws_frame_of(op, b, &f)) return;
    if (f.kind != NW_WS_BATCH) { take(&f, t, s); return; }
    NwBlob all = f.blob, one;
    static _Thread_local NwFrame g;
    for (uint32_t pos = 0; nw_batch_next(all, &pos, &one) == 1;)
        if (nw_frame_decode(one.p, (int)one.len, &g) == NW_OK) take(&g, t, s);
}

// Waits until `done` holds or the timeout runs out. false on a timeout or a close.
static bool pump(bn_ws *ws, seen *s, bool (*done)(const seen *), cb_buf *b) {
    int64_t deadline = bn_now_us() + (int64_t)TIMEOUT_MS * 1000;
    while (!done(s)) {
        int left = (int)((deadline - bn_now_us()) / 1000);
        if (left <= 0) return false;
        int op = bn_ws_recv(ws, left, b);
        if (op == -2) return false;
        if (op >= 0) take_message(op, b, bn_now_us(), s);
    }
    return true;
}

static bool subscribed(const seen *s) { return s->acks >= N_SUBS && s->catchups >= 1; }
static bool chatted(const seen *s) { return s->chats >= 1; }

// One page load: microseconds from open to the last ACK and to the catch-up.
static bool page_load(const page_ctx *c, bool batch, int64_t *t_sub, int64_t *t_catch) {
    bn_ws *ws = bn_ws_open(&c->u, "/v1/stream?proto=nw1", TIMEOUT_MS);
    if (!ws) return false;
    int64_t t0 = bn_now_us();
    seen s = { 0 };
    cb_buf b;
    cb_buf_init(&b);
    bool ok = send_subs(ws, c, batch) == 0 && pump(ws, &s, subscribed, &b);
    if (ok) {
        *t_sub = s.t_acks - t0;
        *t_catch = s.t_catchup - t0;
        sleep_ms(1000);
    }
    cb_buf_free(&b);
    bn_ws_close(ws);
    return ok;
}

typedef struct {
    page_ctx *c;
    int       k;
    int64_t   t_answer;
    int       status;
} chat_post;

static void *post_chat(void *arg) {
    chat_post *p = arg;
    char msg[48];
    int mlen = snprintf(msg, sizeof msg, "bench push %d", p->k);
    NwRequest q;
    memset(&q, 0, sizeof q);
    q.kind = NW_CHAT_SEND;
    q.message_len = (uint16_t)mlen;
    memcpy(q.message, msg, (size_t)mlen);
    uint8_t body[256];
    int n = nw_request_encode(&q, body, sizeof body);
    char path[64], headers[NW_ACCESS_MAX + 128];
    snprintf(path, sizeof path, "/v1/chat/%s", p->c->gid);
    snprintf(headers, sizeof headers, "%sContent-Type: application/octet-stream\r\n", p->c->auth);
    bn_resp r;
    bn_resp_init(&r);
    p->status = n > 0 && bn_http_request(&p->c->h, "POST", path, headers, body, (size_t)n, TIMEOUT_MS, &r) == 0 ? r.status : -1;
    p->t_answer = bn_now_us();
    bn_resp_free(&r);
    return NULL;
}

int bench_page(const bench_opts *o) {
    page_ctx c;
    memset(&c, 0, sizeof c);
    if (!bn_url_parse(o->url, &c.u)) { fprintf(stderr, "bench: not a base URL: %s\n", o->url); return 2; }
    bn_http_init(&c.h, &c.u);
    if (signup(&c) != 0 || create_game(&c) != 0) { bn_http_close(&c.h); return 1; }
    int n = o->n, failed = 0;
    int64_t *sub = calloc((size_t)n, sizeof *sub), *cat = calloc((size_t)n, sizeof *cat);
    int pushes = o->pushes;
    int64_t *s2p = calloc((size_t)(pushes > 0 ? pushes : 1), sizeof *s2p);
    int64_t *a2p = calloc((size_t)(pushes > 0 ? pushes : 1), sizeof *a2p);
    int n_ok = 0, n_push = 0;

    // 1. The page loads, alone in their minute.
    next_minute(1);
    char from[32], to[32];
    bench_date(from, sizeof from);
    for (int i = 0; i < n && sub && cat; i++) {
        if (page_load(&c, o->batch, &sub[n_ok], &cat[n_ok])) n_ok++;
        else failed++;
    }
    bench_date(to, sizeof to);
    fprintf(stderr, "bench: page loads from %s to %s (batch %d)\n", from, to, o->batch ? 1 : 0);
    next_minute(1);

    // 2. Push latency on one socket.
    if (pushes > 0) {
        bn_ws *ws = bn_ws_open(&c.u, "/v1/stream?proto=nw1", TIMEOUT_MS);
        seen s = { 0 };
        cb_buf b;
        cb_buf_init(&b);
        bool ok = ws && send_subs(ws, &c, o->batch) == 0 && pump(ws, &s, subscribed, &b);
        for (int k = 0; ok && k < pushes; k++) {
            sleep_ms(o->gap_ms > 0 ? o->gap_ms : 300);
            chat_post p = { &c, k, 0, 0 };
            s.chats = 0;
            int64_t t_send = bn_now_us();
            pthread_t th;
            if (pthread_create(&th, NULL, post_chat, &p) != 0) break;
            bool heard = pump(ws, &s, chatted, &b);
            pthread_join(th, NULL);
            if (!heard || (p.status != 201 && p.status != 200)) { failed++; continue; }
            s2p[n_push] = s.t_chat - t_send;
            a2p[n_push] = s.t_chat - p.t_answer;
            n_push++;
        }
        cb_buf_free(&b);
        if (ws) bn_ws_close(ws);
        if (!ok) failed++;
    }
    delete_account(&c);
    bn_http_close(&c.h);

    char date[32], host[64], note[256];
    bench_date(date, sizeof date);
    bench_host(host, sizeof host);
    snprintf(note, sizeof note, "%s%sbatch=%d window=%s..%s failed=%d", o->note ? o->note : "", o->note && *o->note ? " " : "",
             o->batch ? 1 : 0, from, to, failed);
    bench_note_load(note, sizeof note);
    bs_run run;
    bs_run_init(&run, "page", date, host, o->url, note);
    bs_run_lat(&run, "subscribed", sub, n_ok, 0, 0);
    bs_run_lat(&run, "caught_up", cat, n_ok, 0, 0);
    if (pushes > 0) {
        bs_run_lat(&run, "send_to_push", s2p, n_push, 0, 0);
        bs_run_lat(&run, "push_after_answer", a2p, n_push, 0, 0);
    }
    int rc = bench_save(&run, o);
    free(sub);
    free(cat);
    free(s2p);
    free(a2p);
    return rc != 0 ? 1 : failed ? 1 : 0;
}
