// unit_rt_anon.c - THE ANONYMOUS SOCKET (rt_local.c, GET /ws/public) over a real loopback server,
// as an authorization matrix: for each thing a socket that never signed in might try, what the
// hub does. An anonymous family (cbase/app.h) is the only way in; the route does not exist
// without one; a socket upgraded there says an empty HELLO and then may SUB and UNSUB rooms and
// PING, and nothing else; it never hears a user's push; a token, a REQ, a second HELLO, a text
// message or garbage ends it. A signed-in socket at /ws is unchanged: an empty HELLO there ends
// it. And the boot rule: anonymous is a room family's alone, and then every room family's.
#include "t.h"
#include "cbase/app.h"
#include "cbase/provider.h"

#include "lib_client.h"
#include "lib_rt_token.h"
#include "net/loop.h"
#include "net/router.h"
#include "net/server.h"
#include "realtime/realtime.h"
#include "realtime/rt_local.h"
#include "../../../../shared/c/stream_frame.h"
#include "util/err.h"
#include "util/log.h"
#include "util/pool.h"

#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <unistd.h>

#define U1 "11111111-1111-4111-8111-111111111111"

static const cb_topic_family fam_mine = { .name = "mine", .pattern = "u/{user}/v" };
static const cb_topic_family fam_feed = { .name = "feed", .pattern = "s/{doc}", .anonymous = true };
static const cb_topic_family *const TOPICS[2] = { &fam_mine, &fam_feed };
static const cb_app APP = { .name = "anon", .topics = TOPICS, .n_topics = 2 };

typedef struct {
    cb_ctx ctx;
    cb_config cfg;
    int port;
    pthread_t thread;
} srv;

static void *run(void *arg) {
    cb_loop_run(((srv *)arg)->ctx.loop);
    return NULL;
}

static void h_who(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) { cb_res_text(res, 200, req->auth_user ? "user" : "anon"); }
// On the loop: how many watch "feed" (realtime.h cb_rt_room_watchers), as text.
static void h_watchers(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    char b[16];
    snprintf(b, sizeof b, "%d", cb_rt_room_watchers(ctx->rt, "feed"));
    cb_res_text(res, 200, b);
}

static void start(srv *s, const cb_app *app) {
    memset(s, 0, sizeof *s);
    strcpy(s->cfg.allowed_origins, "*");
    s->ctx.cfg = &s->cfg;
    s->ctx.prov = &cb_provider_local;
    s->ctx.app = app;
    s->ctx.loop = cb_loop_new();
    s->ctx.req_pool = cb_pool_new(2, "tan");
    s->ctx.router = cb_router_new();
    T_EQ_INT(cb_rt_local_new(&s->ctx, &s->ctx.rt), CB_OK);
    T_EQ_INT(cb_rt_local_routes(&s->ctx), CB_OK);
    T_EQ_INT(cb_router_add(s->ctx.router, "*", "/t/who", CB_ROUTE_POOL, h_who), CB_OK);
    T_EQ_INT(cb_router_add(s->ctx.router, "GET", "/t/watchers", CB_ROUTE_LOOP, h_watchers), CB_OK);
    cb_server_opts o;
    cb_server_opts_default(&o);
    s->ctx.server = cb_server_new(&s->ctx, &o);
    T_EQ_INT(cb_server_listen(s->ctx.server, "127.0.0.1", 0, &s->port), CB_OK);
    pthread_create(&s->thread, NULL, run, s);
}

static void drain_post(cb_loop *l, void *ud) { cb_server_drain(ud, 2000); }

static void stop(srv *s) {
    T_EQ_INT(cb_loop_post(s->ctx.loop, drain_post, s->ctx.server), CB_OK);
    pthread_join(s->thread, NULL);
    cb_pool_free(s->ctx.req_pool);
    cb_server_free(s->ctx.server);
    cb_rt_free(s->ctx.rt);
    cb_router_free(s->ctx.router);
    cb_loop_free(s->ctx.loop);
}

static srv g;

static void fr_send(int fd, const uint8_t *b, int n) {
    T_CHECK(n > 0);
    T_EQ_INT(tc_ws_send(fd, CB_WS_OP_BIN, b, (size_t)n), 0);
}

// The next frame: its kind, 0 on a timeout, -1 when the socket closed (m holds the close code).
static int fr_recv(int fd, int timeout_ms, cb_buf *m) {
    cb_buf_reset(m);
    int op = tc_ws_recv(fd, timeout_ms, m);
    if (op == CB_WS_OP_CLOSE) return -1;
    if (op != CB_WS_OP_BIN) return 0;
    T_CHECK(m->len >= 2 && m->p[0] == SF_V);
    return m->p[1];
}

static int close_code(const cb_buf *m) { return m->len == 2 ? (m->p[0] << 8) | m->p[1] : -1; }

static void hello(int fd, const char *tok) {
    uint8_t b[512];
    fr_send(fd, b, sf_put_hello(b, sizeof b, tok, strlen(tok)));
}

// An anonymous socket past its HELLO.
static int open_anon(cb_buf *m) {
    int fd = tc_ws_open(g.port, "/ws/public", NULL);
    T_CHECK(fd >= 0);
    hello(fd, "");
    T_EQ_INT(fr_recv(fd, 2000, m), SF_READY);
    return fd;
}

// SUB or UNSUB and its REPLY's status.
static int sub(int fd, uint8_t kind, uint32_t id, int what, const char *gid, cb_buf *m) {
    uint8_t b[128];
    fr_send(fd, b, sf_put_sub(b, sizeof b, kind, id, (uint8_t)what, gid, strlen(gid)));
    if (fr_recv(fd, 2000, m) != SF_REPLY) return -1;
    sf_frame f;
    T_EQ_INT(sf_parse(m->p, m->len, &f), 0);
    T_EQ_INT(f.id, id);
    return f.status;
}

// A push from this (non-loop) thread: a room's, or U1's own.
static void push_room(const char *doc, uint8_t tag) {
    const cb_rt_to to = { &fam_feed, NULL, doc, NULL };
    const uint8_t frame[3] = { SF_V, 1, tag };
    cb_rt_publish(g.ctx.rt, &to, 1, frame, sizeof frame);
}
static void push_user(uint8_t tag) {
    const cb_rt_to to = { &fam_mine, U1, NULL, NULL };
    const uint8_t frame[3] = { SF_V, 1, tag };
    cb_rt_publish(g.ctx.rt, &to, 1, frame, sizeof frame);
}

// The frame `fd` must end on: sent, then the close and its code.
static void ends_on(int fd, const uint8_t *b, size_t n, int opcode, cb_buf *m) {
    T_EQ_INT(tc_ws_send(fd, opcode, b, n), 0);
    T_EQ_INT(fr_recv(fd, 2000, m), -1);
    T_EQ_INT(close_code(m), 1008);
    close(fd);
}

// ---- the matrix -------------------------------------------------------------------------

// Allowed: an empty HELLO, SUB of a room (heard), PING, UNSUB (not heard after).
static void anon_hears_a_room(void) {
    cb_buf m;
    cb_buf_init(&m);
    int a = open_anon(&m);
    T_EQ_INT(sub(a, SF_SUB, 1, SF_WHAT_ROOM, "feed", &m), 200);
    push_room("feed", 7);
    T_EQ_INT(fr_recv(a, 2000, &m), 1);
    T_CHECK(m.len == 3 && m.p[2] == 7);
    push_room("other", 8);   // a room it does not watch
    uint8_t b[8];
    fr_send(a, b, sf_put_bare(b, sizeof b, SF_PING));
    T_EQ_INT(fr_recv(a, 2000, &m), SF_PONG);   // the PONG came, and nothing of "other" before it
    T_EQ_INT(sub(a, SF_UNSUB, 2, SF_WHAT_ROOM, "feed", &m), 200);
    push_room("feed", 9);
    fr_send(a, b, sf_put_bare(b, sizeof b, SF_PING));
    T_EQ_INT(fr_recv(a, 2000, &m), SF_PONG);
    T_EQ_INT(fr_recv(a, 100, &m), 0);
    close(a);
    cb_buf_free(&m);
}

static int watchers(void) {
    tc_response r;
    T_EQ_INT(tc_http(g.port, "GET", "/t/watchers", NULL, NULL, 0, &r), 0);
    T_EQ_INT(r.status, 200);
    char b[16] = { 0 };
    memcpy(b, r.body.p, r.body.len < 15 ? r.body.len : 15);
    tc_response_free(&r);
    return atoi(b);
}

// A room's watchers are counted on the loop, anonymous ones too; elsewhere the hub cannot tell.
static void watchers_are_counted(void) {
    cb_buf m;
    cb_buf_init(&m);
    T_EQ_INT(watchers(), 0);
    int a = open_anon(&m), b = open_anon(&m);
    T_EQ_INT(sub(a, SF_SUB, 1, SF_WHAT_ROOM, "feed", &m), 200);
    T_EQ_INT(watchers(), 1);
    T_EQ_INT(sub(b, SF_SUB, 1, SF_WHAT_ROOM, "feed", &m), 200);
    T_EQ_INT(sub(b, SF_SUB, 2, SF_WHAT_ROOM, "feed", &m), 200);   // again: still one watch
    T_EQ_INT(watchers(), 2);
    T_EQ_INT(cb_rt_room_watchers(g.ctx.rt, "feed"), -1);   // this thread is not the loop's
    T_EQ_INT(cb_rt_room_watchers(NULL, "feed"), -1);
    T_EQ_INT(cb_rt_room_watchers(g.ctx.rt, NULL), -1);
    close(a);
    T_EQ_INT(sub(b, SF_UNSUB, 3, SF_WHAT_ROOM, "feed", &m), 200);
    for (int k = 0; k < 200 && watchers() != 0; k++) usleep(5000);   // a's close reaches the loop
    T_EQ_INT(watchers(), 0);
    close(b);
    cb_buf_free(&m);
}

// A signed-in spectator and an anonymous one hear the same room push, through the same path.
static void anon_and_signed_in_hear_alike(void) {
    cb_buf m;
    cb_buf_init(&m);
    int a = open_anon(&m);
    int s = tc_ws_open(g.port, "/ws", NULL);
    T_CHECK(s >= 0);
    hello(s, "tok-" U1);
    T_EQ_INT(fr_recv(s, 2000, &m), SF_READY);
    T_EQ_INT(sub(a, SF_SUB, 1, SF_WHAT_ROOM, "feed", &m), 200);
    T_EQ_INT(sub(s, SF_SUB, 1, SF_WHAT_ROOM, "feed", &m), 200);
    push_room("feed", 3);
    for (int fd = a; fd != -1; fd = fd == a ? s : -1) {
        T_EQ_INT(fr_recv(fd, 2000, &m), 1);
        T_CHECK(m.len == 3 && m.p[2] == 3);
    }
    // U1's own push: the signed-in socket hears it, the anonymous one never does.
    push_user(4);
    T_EQ_INT(fr_recv(s, 2000, &m), 1);
    T_CHECK(m.len == 3 && m.p[2] == 4);
    uint8_t b[8];
    fr_send(a, b, sf_put_bare(b, sizeof b, SF_PING));
    T_EQ_INT(fr_recv(a, 2000, &m), SF_PONG);
    T_EQ_INT(fr_recv(a, 100, &m), 0);
    close(a);
    close(s);
    cb_buf_free(&m);
}

// Refused but kept: a SUB of anything but a room (403, nothing registered); a SUB with no room
// id (400); more rooms than a connection may watch (429).
static void anon_refusals_that_answer(void) {
    cb_buf m;
    cb_buf_init(&m);
    int a = open_anon(&m);
    T_EQ_INT(sub(a, SF_SUB, 1, 1, U1, &m), 403);   // a user's own, by any `what` that is not a room
    T_EQ_INT(sub(a, SF_SUB, 2, 0, "feed", &m), 403);
    T_EQ_INT(sub(a, SF_SUB, 3, 255, "feed", &m), 403);
    T_EQ_INT(sub(a, SF_UNSUB, 4, 1, "feed", &m), 403);
    push_room("feed", 1);   // the 403s registered nothing
    push_user(2);
    uint8_t b[8];
    fr_send(a, b, sf_put_bare(b, sizeof b, SF_PING));
    T_EQ_INT(fr_recv(a, 2000, &m), SF_PONG);
    T_EQ_INT(fr_recv(a, 100, &m), 0);
    T_EQ_INT(sub(a, SF_SUB, 5, SF_WHAT_ROOM, "", &m), 400);
    char gid[16];
    for (int i = 0; i < 32; i++) {
        snprintf(gid, sizeof gid, "r%d", i);
        T_EQ_INT(sub(a, SF_SUB, 10 + (uint32_t)i, SF_WHAT_ROOM, gid, &m), 200);
    }
    T_EQ_INT(sub(a, SF_SUB, 99, SF_WHAT_ROOM, "one-more", &m), 429);
    fr_send(a, b, sf_put_bare(b, sizeof b, SF_PING));
    T_EQ_INT(fr_recv(a, 2000, &m), SF_PONG);   // still open
    close(a);
    cb_buf_free(&m);
}

// Refused and ended (1008): every other first frame, a token, a REQ (signed-in routes and
// /v1/auth alike), a second HELLO, a text message, a frame that does not parse, a server frame.
static void anon_refusals_that_end(void) {
    cb_buf m;
    cb_buf_init(&m);
    uint8_t b[512];
    int n;
    // Before the HELLO.
    int a = tc_ws_open(g.port, "/ws/public", NULL);
    n = sf_put_sub(b, sizeof b, SF_SUB, 1, SF_WHAT_ROOM, "feed", 4);
    ends_on(a, b, (size_t)n, CB_WS_OP_BIN, &m);
    a = tc_ws_open(g.port, "/ws/public", NULL);
    n = sf_put_bare(b, sizeof b, SF_PING);
    ends_on(a, b, (size_t)n, CB_WS_OP_BIN, &m);
    // A HELLO with a token, a valid one included: a client with a token signs in at /ws.
    a = tc_ws_open(g.port, "/ws/public", NULL);
    n = sf_put_hello(b, sizeof b, "tok-" U1, strlen("tok-" U1));
    ends_on(a, b, (size_t)n, CB_WS_OP_BIN, &m);
    a = tc_ws_open(g.port, "/ws/public", NULL);
    n = sf_put_hello(b, sizeof b, "x", 1);
    ends_on(a, b, (size_t)n, CB_WS_OP_BIN, &m);
    // After the HELLO.
    a = open_anon(&m);
    n = sf_put_req(b, sizeof b, 1, SF_METH_GET, "/t/who", 6, NULL, 0);
    ends_on(a, b, (size_t)n, CB_WS_OP_BIN, &m);
    a = open_anon(&m);
    n = sf_put_req(b, sizeof b, 1, SF_METH_POST, "/v1/auth/login", 14, (const uint8_t *)"x", 1);
    ends_on(a, b, (size_t)n, CB_WS_OP_BIN, &m);
    a = open_anon(&m);
    n = sf_put_hello(b, sizeof b, "", 0);
    ends_on(a, b, (size_t)n, CB_WS_OP_BIN, &m);
    a = open_anon(&m);
    ends_on(a, (const uint8_t *)"hi", 2, CB_WS_OP_TEXT, &m);
    a = open_anon(&m);
    n = sf_put_bare(b, sizeof b, SF_PING);   // a frame that parses, sent as text: still text
    ends_on(a, b, (size_t)n, CB_WS_OP_TEXT, &m);
    a = open_anon(&m);
    static const uint8_t junk[3] = { 9, 9, 9 };
    ends_on(a, junk, sizeof junk, CB_WS_OP_BIN, &m);
    a = open_anon(&m);
    n = sf_put_reply_head(b, sizeof b, 1, 200);
    ends_on(a, b, (size_t)n, CB_WS_OP_BIN, &m);
    a = open_anon(&m);
    static const uint8_t push[3] = { SF_V, 1, 0 };   // a push's kind, from a client
    ends_on(a, push, sizeof push, CB_WS_OP_BIN, &m);
    // The signed-in route is unchanged: an empty HELLO there is a token that does not verify.
    a = tc_ws_open(g.port, "/ws", NULL);
    n = sf_put_hello(b, sizeof b, "", 0);
    ends_on(a, b, (size_t)n, CB_WS_OP_BIN, &m);
    // And the server still serves.
    tc_response r;
    T_EQ_INT(tc_http(g.port, "GET", "/t/who", NULL, NULL, 0, &r), 0);
    T_EQ_INT(r.status, 200);
    tc_response_free(&r);
    cb_buf_free(&m);
}

// Without an anonymous family there is no /ws/public at all.
static void no_route_without_an_anonymous_family(void) {
    static const cb_topic_family watch = { .name = "watch", .pattern = "s/{doc}" };
    static const cb_topic_family *const topics[2] = { &fam_mine, &watch };
    static const cb_app closed = { .name = "closed", .topics = topics, .n_topics = 2 };
    srv s;
    start(&s, &closed);
    tc_response r;
    T_EQ_INT(tc_http(s.port, "GET", "/ws/public", NULL, NULL, 0, &r), 0);
    T_EQ_INT(r.status, 404);
    tc_response_free(&r);
    T_CHECK(tc_ws_open(s.port, "/ws/public", NULL) < 0);
    T_EQ_INT(tc_http(s.port, "GET", "/ws", NULL, NULL, 0, &r), 0);
    T_EQ_INT(r.status, 426);   // the signed-in route is there
    tc_response_free(&r);
    stop(&s);
    // No app at all: the same.
    start(&s, NULL);
    T_EQ_INT(tc_http(s.port, "GET", "/ws/public", NULL, NULL, 0, &r), 0);
    T_EQ_INT(r.status, 404);
    tc_response_free(&r);
    stop(&s);
}

// THE BOOT RULE (realtime.c cb_rt_families_check): anonymous is a room family's alone, and an app
// with one anonymous room family has no private one, since the hub knows a room by its doc.
static void the_anonymous_rule(void) {
    static const cb_topic_family mine_anon = { .name = "mine", .pattern = "u/{user}/v", .anonymous = true }, watch = { .name = "watch", .pattern = "s/{doc}" },
                                 feed2 = { .name = "feed2", .pattern = "s/x/{doc}", .anonymous = true };
    char err[256];
    const cb_topic_family *ok[3] = { &fam_mine, &fam_feed, &feed2 };
    T_EQ_INT(cb_rt_families_check(ok, 3, err, sizeof err), CB_OK);
    T_CHECK(cb_rt_families_anonymous(ok, 3));
    const cb_topic_family *closed[2] = { &fam_mine, &watch };
    T_EQ_INT(cb_rt_families_check(closed, 2, err, sizeof err), CB_OK);
    T_CHECK(!cb_rt_families_anonymous(closed, 2));
    T_CHECK(!cb_rt_families_anonymous(NULL, 0));
    const cb_topic_family *user_anon[1] = { &mine_anon };
    T_EQ_INT(cb_rt_families_check(user_anon, 1, err, sizeof err), CB_E_INVALID);
    T_CHECK(strstr(err, "cannot be anonymous") != NULL);
    const cb_topic_family *mixed[3] = { &fam_mine, &watch, &fam_feed };
    T_EQ_INT(cb_rt_families_check(mixed, 3, err, sizeof err), CB_E_INVALID);
    T_CHECK(strstr(err, "feed") && strstr(err, "watch"));
    const cb_topic_family *mixed2[2] = { &fam_feed, &watch };   // in either order
    T_EQ_INT(cb_rt_families_check(mixed2, 2, err, sizeof err), CB_E_INVALID);
}

// A CLIENT UNDER SIGNALS: lib_client's calls survive EINTR. Another thread signals this one
// every 200 us (a handler installed without SA_RESTART, as libFuzzer's -timeout alarm is) while it
// opens anonymous sockets, says HELLO and SUBs, and asks HTTP, for at least 200 rounds and 500
// signals; every call must succeed.
// fz_rt_anon failed at random on a loaded CI runner because poll and recv returned EINTR and the
// client took that for the server's refusal.
static volatile sig_atomic_t g_hits;
static void on_usr1(int sig) { (void)sig; g_hits++; }
static atomic_int g_storm;
static pthread_t g_target;
static void *storm(void *arg) {
    (void)arg;
    while (atomic_load(&g_storm)) {
        pthread_kill(g_target, SIGUSR1);
        usleep(200);
    }
    return NULL;
}

static void a_client_under_signals(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_usr1;   // no SA_RESTART: every blocking call the signal lands in returns EINTR
    sigemptyset(&sa.sa_mask);
    T_CHECK(sigaction(SIGUSR1, &sa, NULL) == 0);
    g_target = pthread_self();
    atomic_store(&g_storm, 1);
    pthread_t th;
    pthread_create(&th, NULL, storm, NULL);
    cb_buf m;
    cb_buf_init(&m);
    // At least 200 rounds and until 500 signals have landed (a fast machine finishes 200 rounds
    // before the storm gets going), at most 5000.
    int opened = 0, answered = 0, rounds = 0;
    for (; rounds < 5000 && (rounds < 200 || g_hits < 500); rounds++) {
        int a = tc_ws_open(g.port, "/ws/public", NULL);
        if (a < 0) continue;
        opened++;
        hello(a, "");
        if (fr_recv(a, 2000, &m) == SF_READY && sub(a, SF_SUB, 1, SF_WHAT_ROOM, "feed", &m) == 200) answered++;
        close(a);
        tc_response r;
        if (tc_http(g.port, "GET", "/t/who", NULL, NULL, 0, &r) == 0 && r.status == 200) answered++;
        tc_response_free(&r);
    }
    atomic_store(&g_storm, 0);
    pthread_join(th, NULL);
    signal(SIGUSR1, SIG_DFL);
    T_CHECK(g_hits >= 500);   // the storm really landed
    T_EQ_INT(opened, rounds);
    T_EQ_INT(answered, 2 * rounds);
    // And the server holds none of them now.
    for (int k = 0; k < 200 && atomic_load(&g.ctx.stats.ws_conns); k++) usleep(5000);
    T_EQ_INT(atomic_load(&g.ctx.stats.ws_conns), 0);
    cb_buf_free(&m);
}

int main(void) {
    cb_log_init(CB_LOG_ERROR, 0);
    rtt_install();
    T_RUN(the_anonymous_rule);
    start(&g, &APP);
    T_RUN(anon_hears_a_room);
    T_RUN(anon_and_signed_in_hear_alike);
    T_RUN(anon_refusals_that_answer);
    T_RUN(anon_refusals_that_end);
    T_RUN(watchers_are_counted);
    T_RUN(a_client_under_signals);
    stop(&g);
    T_RUN(no_route_without_an_anonymous_family);
    return T_DONE();
}
