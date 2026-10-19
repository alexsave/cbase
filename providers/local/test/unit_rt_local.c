// unit_rt_local.c - realtime under `cbase serve` (realtime/rt_local.h), the
// socket hub, over a real loopback server: the upgrade route, frames handled on
// the request pool, publishes from another thread reaching only the user's own
// connections and a game's spectators, REQ running a route as the connection's
// user, sessions that end closing their sockets, and every refusal closing.
// unit_stream.c drives the same server with the kernel's client library.
#include "t.h"
#include "cbase/provider.h"

#include "auth/auth.h"
#include "handler.h"
#include "lib_client.h"
#include "lib_rt_token.h"
#include "net/loop.h"
#include "net/router.h"
#include "net/server.h"
#include "app/pushes.h"
#include "realtime/realtime.h"
#include "realtime/rt_local.h"
#include "../../../../shared/c/stream_frame.h"
#include "util/log.h"
#include "util/pool.h"

#include "netwire.h"

#include <stdatomic.h>
#include <pthread.h>
#include <unistd.h>

#define U1 "11111111-1111-4111-8111-111111111111"
#define U2 "22222222-2222-4222-8222-222222222222"
#define G  "ab12cd"

static cb_ctx g_ctx;
static cb_config g_cfg;
static int g_port;
static pthread_t g_thread;

static void *run(void *arg) {
    cb_loop_run(g_ctx.loop);
    return NULL;
}

// A route that answers "<the caller's user>:<session>:<body>": what a handler sees of a socket's request.
static void h_who(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    const cb_user *u = req->auth_user;
    char b[256];
    int n = u ? snprintf(b, sizeof b, "%s:%s:%.*s", u->id, u->session_id, (int)req->body_len, req->body ? (const char *)req->body : "") : snprintf(b, sizeof b, "anon");
    cb_res_send(res, 200, "application/octet-stream", b, (size_t)n);
}

static void start(void) {
    cb_log_init(CB_LOG_ERROR, 0);
    rtt_install();
    memset(&g_cfg, 0, sizeof g_cfg);
    strcpy(g_cfg.allowed_origins, "*");
    g_ctx.cfg = &g_cfg;
    g_ctx.prov = &cb_provider_local;
    g_ctx.loop = cb_loop_new();
    g_ctx.req_pool = cb_pool_new(3, "trt");
    g_ctx.router = cb_router_new();
    T_EQ_INT(cb_rt_local_new(&g_ctx, &g_ctx.rt), CB_OK);
    T_EQ_INT(cb_rt_local_routes(&g_ctx), CB_OK);
    T_EQ_INT(cb_router_add(g_ctx.router, "*", "/t/who", CB_ROUTE_POOL, h_who), CB_OK);
    T_EQ_INT(cb_router_add(g_ctx.router, "*", "/t/loop", CB_ROUTE_LOOP, h_who), CB_OK);
    cb_server_opts o;
    cb_server_opts_default(&o);
    g_ctx.server = cb_server_new(&g_ctx, &o);
    T_EQ_INT(cb_server_listen(g_ctx.server, "127.0.0.1", 0, &g_port), CB_OK);
    pthread_create(&g_thread, NULL, run, NULL);
}

// ---- the hub (stream_frame.h) -----------------------------------------------

static int open_ws(void) { return tc_ws_open(g_port, "/ws", NULL); }

static void fr_send(int fd, const uint8_t *b, int n) {
    T_CHECK(n > 0);
    T_EQ_INT(tc_ws_send(fd, CB_WS_OP_BIN, b, (size_t)n), 0);
}

// The next frame the hub sent, into m: its first-byte kind, 0 on a timeout, -1 when the socket closed (m holds the close code).
static int fr_recv(int fd, int timeout_ms, cb_buf *m) {
    cb_buf_reset(m);
    int op = tc_ws_recv(fd, timeout_ms, m);
    if (op == CB_WS_OP_CLOSE) return -1;
    if (op != CB_WS_OP_BIN) return 0;
    T_CHECK(m->len >= 2 && m->p[0] == SF_V);
    return m->p[1];
}

static int close_code(const cb_buf *m) { return m->len == 2 ? (m->p[0] << 8) | m->p[1] : -1; }

static void send_hello(int fd, const char *tok) {
    uint8_t b[512];
    fr_send(fd, b, sf_put_hello(b, sizeof b, tok, strlen(tok)));
}

// HELLO as `uid` in session `sess` (NULL: the default), and its READY.
static int open_as(const char *uid, const char *sess, cb_buf *m) {
    int fd = open_ws();
    T_CHECK(fd >= 0);
    char tok[96];
    if (sess) snprintf(tok, sizeof tok, "tok-%s@%s", uid, sess);
    else snprintf(tok, sizeof tok, "tok-%s", uid);
    send_hello(fd, tok);
    T_EQ_INT(fr_recv(fd, 2000, m), SF_READY);
    return fd;
}

// SUB or UNSUB `what` of `gid`, and its REPLY.
static void sub(int fd, uint8_t kind, uint32_t id, int what, const char *gid, int want_status, cb_buf *m) {
    uint8_t b[128];
    fr_send(fd, b, sf_put_sub(b, sizeof b, kind, id, (uint8_t)what, gid, strlen(gid)));
    T_EQ_INT(fr_recv(fd, 2000, m), SF_REPLY);
    sf_frame f;
    T_EQ_INT(sf_parse(m->p, m->len, &f), 0);
    T_EQ_INT(f.id, id);
    T_EQ_INT(f.status, want_status);
}

// A REQ and its REPLY: the status, the body into m.
static int req(int fd, uint32_t id, int meth, const char *path, const char *body, cb_buf *m, sf_frame *f) {
    uint8_t b[512];
    fr_send(fd, b, sf_put_req(b, sizeof b, id, (uint8_t)meth, path, strlen(path), (const uint8_t *)body, strlen(body)));
    int k = fr_recv(fd, 2000, m);
    if (k != SF_REPLY) return -k - 1000;
    T_EQ_INT(sf_parse(m->p, m->len, f), 0);
    T_EQ_INT(f->id, id);
    return f->status;
}

static void ws_upgrade_route(void) {
    tc_response r;
    T_EQ_INT(tc_http(g_port, "GET", "/ws", NULL, NULL, 0, &r), 0);
    T_EQ_INT(r.status, 426);   // not a WebSocket upgrade at all
    tc_response_free(&r);
    T_EQ_INT(tc_http(g_port, "GET", "/mqtt", NULL, NULL, 0, &r), 0);
    T_EQ_INT(r.status, 404);   // MQTT is gone from this path
    tc_response_free(&r);
    int fd = open_ws();
    T_CHECK(fd >= 0);
    close(fd);
}

static void publish_view(uint32_t version) {
    cb_view_row v;
    memset(&v, 0, sizeof v);
    strcpy(v.game_id, G);
    uint8_t env[] = { 0xee, 0xff };
    v.view = env;
    v.view_len = 2;
    v.version = version;
    rt_publish_view(g_ctx.rt, U1, &v, false);
}

static atomic_int g_hold;
static void hold_worker(void *ud) { while (atomic_load(&g_hold)) usleep(1000); }

static void pushes_reach_their_hearers(void) {
    cb_buf m;
    cb_buf_init(&m);
    NwFrame pf;
    // a, c: U1's two connections; b: U2, watching G.
    int a = open_as(U1, NULL, &m);
    int b = open_as(U2, NULL, &m);
    int c = open_as(U1, NULL, &m);
    sub(b, SF_SUB, 0x80000001u, NW_SUB_GAME, G, 200, &m);

    publish_view(4);
    for (int fd = a; fd != -1; fd = fd == a ? c : -1) {   // every connection of the user hears it
        T_EQ_INT(fr_recv(fd, 2000, &m), NW_WS_VIEW);
        T_EQ_INT(nw_frame_decode(m.p, (int)m.len, &pf), NW_OK);
        T_CHECK(pf.version == 4 && pf.blob.len == 2);
    }
    T_EQ_INT(fr_recv(b, 300, &m), 0);   // U2 hears nothing of U1's

    const uint8_t cast[] = { 7, 8, 9 };
    rt_publish_cast(g_ctx.rt, U1, G, 5, cast, 3);   // the seat's
    T_EQ_INT(fr_recv(a, 2000, &m), NW_WS_CAST);
    T_EQ_INT(fr_recv(c, 2000, &m), NW_WS_CAST);
    T_EQ_INT(fr_recv(b, 300, &m), 0);
    rt_publish_cast(g_ctx.rt, NULL, G, 5, cast, 1);   // the spectators'
    T_EQ_INT(fr_recv(b, 2000, &m), NW_WS_SPECT);
    T_EQ_INT(fr_recv(a, 300, &m), 0);   // a watches nothing
    // UNSUB: the game's spectators lose it; SUB again gets it back.
    sub(b, SF_UNSUB, 0x80000002u, NW_SUB_GAME, G, 200, &m);
    rt_publish_cast(g_ctx.rt, NULL, G, 6, cast, 1);
    T_EQ_INT(fr_recv(b, 300, &m), 0);
    sub(b, SF_SUB, 0x80000003u, NW_SUB_GAME, G, 200, &m);
    rt_publish_cast(g_ctx.rt, NULL, G, 7, cast, 1);
    T_EQ_INT(fr_recv(b, 2000, &m), NW_WS_SPECT);
    sub(b, SF_SUB, 0x80000004u, NW_SUB_GAME, "", 400, &m);   // a game with no id

    cb_chat_row ch;
    memset(&ch, 0, sizeof ch);
    strcpy(ch.id, "m1");
    strcpy(ch.game_id, G);
    strcpy(ch.user_id, U1);
    ch.message = "hi";
    cb_uuid members[1] = { U1 };
    rt_publish_chat(g_ctx.rt, G, &ch, members, 1);
    T_EQ_INT(fr_recv(a, 2000, &m), NW_WS_CHAT);
    T_EQ_INT(nw_frame_decode(m.p, (int)m.len, &pf), NW_OK);
    T_CHECK(pf.chat.message_len == 2);
    T_EQ_INT(fr_recv(c, 2000, &m), NW_WS_CHAT);
    T_EQ_INT(fr_recv(b, 300, &m), 0);

    // PING is answered; the account deleted ends both of U1's sockets and not U2's.
    uint8_t pk[8];
    fr_send(a, pk, sf_put_bare(pk, sizeof pk, SF_PING));
    T_EQ_INT(fr_recv(a, 2000, &m), SF_PONG);
    cb_rt_forget_user(g_ctx.rt, U1);
    T_EQ_INT(fr_recv(a, 2000, &m), -1);
    T_EQ_INT(close_code(&m), 1008);
    T_EQ_INT(fr_recv(c, 2000, &m), -1);
    fr_send(b, pk, sf_put_bare(pk, sizeof pk, SF_PING));
    T_EQ_INT(fr_recv(b, 2000, &m), SF_PONG);
    // The loop answers a PING itself: with every pool worker held, the PONG still comes.
    atomic_store(&g_hold, 1);
    for (int i = 0; i < 3; i++) T_EQ_INT(cb_pool_submit(g_ctx.req_pool, hold_worker, NULL), CB_OK);
    usleep(50000);
    fr_send(b, pk, sf_put_bare(pk, sizeof pk, SF_PING));
    T_EQ_INT(fr_recv(b, 500, &m), SF_PONG);
    atomic_store(&g_hold, 0);
    close(a);
    close(b);
    close(c);
    cb_buf_free(&m);
}

// 150 sockets of the user hear one view once each; the ones that hang up
// do not take the others down; a new socket is found by the user map.
static void many_sockets_hear_once(void) {
    cb_buf m;
    cb_buf_init(&m);
    enum { N = 150 };
    int on[N];
    for (int i = 0; i < N; i++) on[i] = open_as(i % 2 ? U1 : U2, NULL, &m);
    publish_view(9);
    for (int i = 1; i < N; i += 2) T_EQ_INT(fr_recv(on[i], 2000, &m), NW_WS_VIEW);
    for (int i = 0; i < N; i += 2) T_EQ_INT(fr_recv(on[i], 0, &m), 0);
    for (int i = 0; i < N; i += 4) { close(on[i]); on[i] = -1; }
    for (int i = 1; i < N; i += 4) { close(on[i]); on[i] = -1; }
    usleep(100000);
    publish_view(10);
    for (int i = 3; i < N; i += 4) T_EQ_INT(fr_recv(on[i], 2000, &m), NW_WS_VIEW);
    for (int i = 0; i < N; i++) if (on[i] >= 0) close(on[i]);
    cb_buf_free(&m);
}

static void req_runs_a_route_as_the_user(void) {
    cb_buf m;
    cb_buf_init(&m);
    sf_frame f;
    int a = open_as(U1, "sess-a", &m);
    T_EQ_INT(req(a, 7, SF_METH_POST, "/t/who", "hello", &m, &f), 200);
    T_EQ_MEM(f.body, f.body_len, U1 ":sess-a:hello", 36 + 1 + 6 + 1 + 5);
    T_EQ_INT(req(a, 8, SF_METH_GET, "/t/nothing", "", &m, &f), 404);
    T_EQ_INT(req(a, 9, SF_METH_GET, "/t/loop", "", &m, &f), 405);   // a loop route is not for a socket
    T_EQ_INT(req(a, 10, SF_METH_POST, "/v1/auth/signin", "", &m, &f), 403);
    // Replies come back in request order, ids echoed, even pipelined.
    uint8_t b[256];
    T_EQ_INT(tc_ws_send(a, CB_WS_OP_BIN, b, (size_t)sf_put_req(b, sizeof b, 11, SF_METH_POST, "/t/who", 6, (const uint8_t *)"x", 1)), 0);
    T_EQ_INT(tc_ws_send(a, CB_WS_OP_BIN, b, (size_t)sf_put_req(b, sizeof b, 12, SF_METH_POST, "/t/who", 6, (const uint8_t *)"y", 1)), 0);
    T_EQ_INT(fr_recv(a, 2000, &m), SF_REPLY);
    T_EQ_INT(sf_parse(m.p, m.len, &f), 0);
    T_EQ_INT(f.id, 11);
    T_EQ_INT(fr_recv(a, 2000, &m), SF_REPLY);
    T_EQ_INT(sf_parse(m.p, m.len, &f), 0);
    T_EQ_INT(f.id, 12);
    close(a);
    cb_buf_free(&m);
}

// A SESSION THAT ENDS CLOSES ITS SOCKETS (auth.c calls cb_rt_end_sessions).
static void ended_sessions_close_their_sockets(void) {
    cb_buf m;
    cb_buf_init(&m);
    sf_frame f;
    int s1 = open_as(U1, "s-one", &m), s2 = open_as(U1, "s-two", &m), o = open_as(U2, "s-one", &m);
    // Sign-out of one session: that socket closes with 1008, the user's other session and another user's stay.
    cb_rt_end_sessions(g_ctx.rt, U1, "s-one", NULL);
    T_EQ_INT(fr_recv(s1, 2000, &m), -1);
    T_EQ_INT(close_code(&m), 1008);
    T_EQ_INT(req(s2, 1, SF_METH_POST, "/t/who", "", &m, &f), 200);
    T_EQ_INT(req(o, 1, SF_METH_POST, "/t/who", "", &m, &f), 200);
    // A password change keeps the caller's session and ends every other.
    int s3 = open_as(U1, "s-three", &m);
    cb_rt_end_sessions(g_ctx.rt, U1, NULL, "s-two");
    T_EQ_INT(fr_recv(s3, 2000, &m), -1);
    T_EQ_INT(req(s2, 2, SF_METH_POST, "/t/who", "", &m, &f), 200);
    T_EQ_INT(fr_recv(o, 100, &m), 0);   // the other user's socket stayed
    // A global sign-out ends them all.
    cb_rt_end_sessions(g_ctx.rt, U1, NULL, NULL);
    T_EQ_INT(fr_recv(s2, 2000, &m), -1);
    // A socket cannot be opened on an ended session either: the token check refuses it (the real
    // check reads the session, auth.c).
    // A REQ that starts after the session ended but before the loop closed the socket is refused:
    // the hub's end counter moved, so the session is read again (rt_deps.h session_live).
    int s4 = open_as(U1, "s-four", &m);
    T_EQ_INT(req(s4, 1, SF_METH_POST, "/t/who", "", &m, &f), 200);
    strcpy(rtt_revoked, "s-four");
    cb_rt_end_sessions(g_ctx.rt, "00000000-0000-4000-8000-000000000000", NULL, NULL);   // anybody's: the counter moves
    uint8_t b[128];
    fr_send(s4, b, sf_put_req(b, sizeof b, 2, SF_METH_POST, "/t/who", 6, (const uint8_t *)"z", 1));
    T_EQ_INT(fr_recv(s4, 2000, &m), -1);   // closed, no reply
    rtt_revoked[0] = 0;
    close(o);
    cb_buf_free(&m);
}

// Every refusal ends the connection, with no READY or REPLY.
static void refusals_end_the_connection(void) {
    cb_buf m;
    cb_buf_init(&m);
    uint8_t b[512];
    int a = open_ws();
    send_hello(a, "forged");   // a token that does not verify
    T_EQ_INT(fr_recv(a, 2000, &m), -1);
    T_EQ_INT(close_code(&m), 1008);
    close(a);
    a = open_ws();
    fr_send(a, b, sf_put_bare(b, sizeof b, SF_PING));   // the first frame must be the HELLO
    T_EQ_INT(fr_recv(a, 2000, &m), -1);
    close(a);
    a = open_as(U1, NULL, &m);
    send_hello(a, "tok-" U1);   // a second HELLO
    T_EQ_INT(fr_recv(a, 2000, &m), -1);
    close(a);
    a = open_as(U1, NULL, &m);
    T_EQ_INT(tc_ws_send(a, CB_WS_OP_TEXT, "wAA=", 4), 0);   // frames are binary
    T_EQ_INT(fr_recv(a, 2000, &m), -1);
    T_EQ_INT(close_code(&m), 1003);
    close(a);
    a = open_as(U1, NULL, &m);
    T_EQ_INT(tc_ws_send(a, CB_WS_OP_BIN, "\x01\x05xx", 4), 0);   // not a socket frame
    T_EQ_INT(fr_recv(a, 2000, &m), -1);
    close(a);
    a = open_as(U1, NULL, &m);
    fr_send(a, b, sf_put_bare(b, sizeof b, SF_READY));   // a server's frame
    T_EQ_INT(fr_recv(a, 2000, &m), -1);
    close(a);
    // Sockets that close with their HELLO and REQ still out on the pool do not take the server down.
    for (int i = 0; i < 20; i++) {
        a = open_ws();
        T_EQ_INT(tc_ws_send(a, CB_WS_OP_BIN, b, (size_t)sf_put_hello(b, sizeof b, "tok-" U1, 40)), 0);
        close(a);
    }
    a = open_as(U1, NULL, &m);
    fr_send(a, b, sf_put_bare(b, sizeof b, SF_PING));
    T_EQ_INT(fr_recv(a, 2000, &m), SF_PONG);
    close(a);
    cb_buf_free(&m);
}

static void drain_post(cb_loop *l, void *ud) { cb_server_drain(g_ctx.server, 2000); }

static void stop(void) {
    T_EQ_INT(cb_loop_post(g_ctx.loop, drain_post, NULL), CB_OK);
    pthread_join(g_thread, NULL);
    cb_pool_free(g_ctx.req_pool);
    cb_server_free(g_ctx.server);
    cb_rt_free(g_ctx.rt);
    cb_router_free(g_ctx.router);
    cb_loop_free(g_ctx.loop);
}

int main(void) {
    start();
    T_RUN(ws_upgrade_route);
    T_RUN(pushes_reach_their_hearers);
    T_RUN(many_sockets_hear_once);
    T_RUN(req_runs_a_route_as_the_user);
    T_RUN(ended_sessions_close_their_sockets);
    T_RUN(refusals_end_the_connection);
    stop();
    return T_DONE();
}
