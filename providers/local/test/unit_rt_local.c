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
#include "util/clock.h"
#include "util/log.h"
#include "util/pool.h"

#include "netwire.h"

#include <stdatomic.h>
#include <pthread.h>
#include <signal.h>
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

// A route that parks until the test sets the flag and wakes key "k" (http.h park_ms).
static atomic_int g_flag;
static int g_park_ms = 5000;
static void h_park(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    if (atomic_load(&g_flag)) { cb_res_text(res, 200, "go"); return; }
    if (req->park_expired) { cb_res_status(res, 204); return; }
    res->park_ms = g_park_ms;
    snprintf(res->park_key, sizeof res->park_key, "k");
}
static void h_ping(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) { cb_res_text(res, 200, "pong"); }

static void h_pub_on_loop(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res);
static void h_pub_twice(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res);
static void h_heard(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res);
static void h_act(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res);
static void h_would_wait(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res);
static void h_order(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res);

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
    T_EQ_INT(cb_router_add(g_ctx.router, "GET", "/t/park", CB_ROUTE_POOL, h_park), CB_OK);
    T_EQ_INT(cb_router_add(g_ctx.router, "GET", "/t/ping", CB_ROUTE_POOL, h_ping), CB_OK);
    T_EQ_INT(cb_router_add(g_ctx.router, "*", "/t/loop", CB_ROUTE_LOOP, h_who), CB_OK);
    T_EQ_INT(cb_router_add(g_ctx.router, "GET", "/t/publoop", CB_ROUTE_LOOP, h_pub_on_loop), CB_OK);
    T_EQ_INT(cb_router_add(g_ctx.router, "GET", "/t/pubtwice", CB_ROUTE_LOOP, h_pub_twice), CB_OK);
    T_EQ_INT(cb_router_add(g_ctx.router, "GET", "/t/heard", CB_ROUTE_LOOP, h_heard), CB_OK);
    T_EQ_INT(cb_router_add(g_ctx.router, "POST", "/t/act", CB_ROUTE_POOL | CB_ROUTE_INLINE, h_act), CB_OK);
    T_EQ_INT(cb_router_add(g_ctx.router, "POST", "/t/wait", CB_ROUTE_POOL | CB_ROUTE_INLINE, h_would_wait), CB_OK);
    T_EQ_INT(cb_router_add(g_ctx.router, "GET", "/t/order", CB_ROUTE_LOOP, h_order), CB_OK);
    T_EQ_INT(cb_router_add(g_ctx.router, "POST", "/v1/auth/t", CB_ROUTE_POOL | CB_ROUTE_INLINE, h_act), CB_OK);
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

// ---- the long poll's server half: a parked request holds no thread ----------------------------

static atomic_int g_parked_done;
static int g_parked_status[8];
static void *park_client(void *arg) {
    int i = (int)(intptr_t)arg;
    tc_response r;
    g_parked_status[i] = tc_http(g_port, "GET", "/t/park", NULL, NULL, 0, &r) == 0 ? r.status : -1;
    if (g_parked_status[i] >= 0) tc_response_free(&r);
    atomic_fetch_add(&g_parked_done, 1);
    return NULL;
}
static void wake_k(cb_loop *l, void *ud) { cb_server_wake(g_ctx.server, "k"); }

static void parked_requests_hold_no_thread(void) {
    enum { N = 6 };   // more than the pool's 3 workers
    pthread_t th[N];
    atomic_store(&g_flag, 0);
    atomic_store(&g_parked_done, 0);
    g_park_ms = 5000;
    for (int i = 0; i < N; i++) pthread_create(&th[i], NULL, park_client, (void *)(intptr_t)i);
    usleep(400000);
    T_EQ_INT(atomic_load(&g_parked_done), 0);   // all wait
    tc_response r;
    T_EQ_INT(tc_http(g_port, "GET", "/t/ping", NULL, NULL, 0, &r), 0);   // and the pool still serves
    T_EQ_INT(r.status, 200);
    tc_response_free(&r);
    atomic_store(&g_flag, 1);
    T_EQ_INT(cb_loop_post(g_ctx.loop, wake_k, NULL), CB_OK);
    for (int i = 0; i < N; i++) { pthread_join(th[i], NULL); T_EQ_INT(g_parked_status[i], 200); }
    // Time running out answers by itself: the handler runs once more with park_expired.
    atomic_store(&g_flag, 0);
    g_park_ms = 400;
    int64_t t0 = cb_now_ms();
    park_client((void *)(intptr_t)0);
    T_EQ_INT(g_parked_status[0], 204);
    T_CHECK(cb_now_ms() - t0 >= 350);
    // A wake with nothing to say parks it again until its time is up.
    atomic_store(&g_parked_done, 0);
    g_park_ms = 1500;
    pthread_create(&th[0], NULL, park_client, (void *)(intptr_t)0);
    usleep(300000);
    T_EQ_INT(cb_loop_post(g_ctx.loop, wake_k, NULL), CB_OK);
    usleep(300000);
    T_EQ_INT(atomic_load(&g_parked_done), 0);
    pthread_join(th[0], NULL);
    T_EQ_INT(g_parked_status[0], 204);
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

// A route on the loop thread that publishes U1's view: what an inline request's pushes do.
static uint64_t g_posts_before, g_posts_after;
static pthread_t g_loop_thr, g_act_thr;
static void h_pub_on_loop(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    g_loop_thr = pthread_self();
    g_posts_before = cb_loop_posts(ctx->loop);
    publish_view(9);
    g_posts_after = cb_loop_posts(ctx->loop);
    cb_res_text(res, 200, "ok");
}

// A push published ON the loop thread goes straight into its hearers' sockets: no copy posted to
// the loop (no post, no wake), and it reaches every connection of the user as one from another
// thread does (rt_local.c local_deliver).
static void a_push_on_the_loop_is_not_posted(void) {
    cb_buf m;
    cb_buf_init(&m);
    NwFrame pf;
    int a = open_as(U1, NULL, &m);
    int c = open_as(U1, NULL, &m);
    tc_response r;
    T_EQ_INT(tc_http(g_port, "GET", "/t/publoop", NULL, NULL, 0, &r), 0);
    T_EQ_INT(r.status, 200);
    tc_response_free(&r);
    T_CHECK(g_posts_after == g_posts_before);
    for (int fd = a; fd != -1; fd = fd == a ? c : -1) {
        T_EQ_INT(fr_recv(fd, 2000, &m), NW_WS_VIEW);
        T_EQ_INT(nw_frame_decode(m.p, (int)m.len, &pf), NW_OK);
        T_CHECK(pf.version == 9 && pf.blob.len == 2);
    }
    // From another thread it is still posted (this thread is not the loop's).
    uint64_t p0 = cb_loop_posts(g_ctx.loop);
    publish_view(10);
    T_CHECK(cb_loop_posts(g_ctx.loop) == p0 + 1);
    T_EQ_INT(fr_recv(a, 2000, &m), NW_WS_VIEW);
    close(a);
    close(c);
    cb_buf_free(&m);
}

// A commit's shape on the loop: one user's view and then its cast, one after the other.
static void h_pub_twice(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    publish_view(12);
    publish_view(13);
    cb_res_text(res, 200, "done");
}

// ONE WALK PER HEARER: a user's two pushes in one loop pass find the user's connections once
// (rt_local.c THE LAST USER'S HEARERS), and both reach every one of them; a push in a later pass
// walks again.
static void a_users_pushes_in_a_pass_walk_once(void) {
    cb_buf m;
    cb_buf_init(&m);
    NwFrame pf;
    int a = open_as(U1, NULL, &m);
    int c = open_as(U1, NULL, &m);
    const long w0 = cb_rt_local_user_walks(g_ctx.rt);
    tc_response r;
    T_EQ_INT(tc_http(g_port, "GET", "/t/pubtwice", NULL, NULL, 0, &r), 0);
    T_EQ_INT(r.status, 200);
    tc_response_free(&r);
    for (int fd = a; fd != -1; fd = fd == a ? c : -1)
        for (uint32_t v = 12; v <= 13; v++) {
            T_EQ_INT(fr_recv(fd, 2000, &m), NW_WS_VIEW);
            T_EQ_INT(nw_frame_decode(m.p, (int)m.len, &pf), NW_OK);
            T_EQ_INT((int)pf.version, (int)v);
        }
    T_EQ_INT(cb_rt_local_user_walks(g_ctx.rt) - w0, 1);
    T_EQ_INT(tc_http(g_port, "GET", "/t/publoop", NULL, NULL, 0, &r), 0);
    tc_response_free(&r);
    T_EQ_INT(fr_recv(a, 2000, &m), NW_WS_VIEW);
    T_EQ_INT(cb_rt_local_user_walks(g_ctx.rt) - w0, 2);
    close(a);
    close(c);
    cb_buf_free(&m);
}

// Whether the hub knows anyone watches G, asked on the loop: "1" or "0".
static void h_heard(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    cb_res_text(res, 200, cb_rt_room_heard(ctx->rt, G) ? "1" : "0");
}

static bool heard_on_loop(void) {
    tc_response r;
    T_EQ_INT(tc_http(g_port, "GET", "/t/heard", NULL, NULL, 0, &r), 0);
    T_EQ_INT(r.status, 200);
    const bool heard = r.body.len == 1 && r.body.p[0] == '1';
    tc_response_free(&r);
    return heard;
}

// WHO WATCHES A ROOM (realtime.h cb_rt_room_heard): on the loop the hub answers from its rooms, so
// a commit nobody watches builds no spectators' cast; off the loop it cannot tell and says yes.
static void a_room_is_heard_only_while_watched(void) {
    cb_buf m;
    cb_buf_init(&m);
    int b = open_as(U2, NULL, &m);
    T_CHECK(!heard_on_loop());                             // no one in G's room
    T_CHECK(cb_rt_room_heard(g_ctx.rt, G));                 // this thread is not the loop's
    sub(b, SF_SUB, 0x80000011u, NW_SUB_GAME, G, 200, &m);
    T_CHECK(heard_on_loop());
    sub(b, SF_UNSUB, 0x80000012u, NW_SUB_GAME, G, 200, &m);
    T_CHECK(!heard_on_loop());
    sub(b, SF_SUB, 0x80000013u, NW_SUB_GAME, G, 200, &m);
    T_CHECK(heard_on_loop());
    close(b);                                               // the watcher's socket goes
    int n = 0;
    while (heard_on_loop() && n++ < 200) usleep(5000);
    T_CHECK(!heard_on_loop());
    T_CHECK(cb_rt_room_heard(NULL, G));                     // no transport: every room may be heard
    cb_buf_free(&m);
}

// THE QUIET ROOM (realtime.h cb_rt_to.quiet_room): a game's view does not go to the user's
// connection in the game's room (the page, which follows the game by its casts), and still goes to
// the user's other connections; the seat's cast reaches both; a view of another game, and a seat's
// last view (gone), reach the room's connection too.
static void a_view_skips_the_users_connection_in_its_room(void) {
    cb_buf m;
    cb_buf_init(&m);
    NwFrame pf;
    int page = open_as(U1, NULL, &m), dash = open_as(U1, NULL, &m);
    sub(page, SF_SUB, 0x80000021u, NW_SUB_GAME, G, 200, &m);
    const long q0 = cb_rt_local_quiet_skips(g_ctx.rt);
    publish_view(31);
    T_EQ_INT(fr_recv(dash, 2000, &m), NW_WS_VIEW);
    T_EQ_INT(fr_recv(page, 300, &m), 0);                     // the page in G's room: no view of G
    T_EQ_INT(cb_rt_local_quiet_skips(g_ctx.rt) - q0, 1);
    tc_response r;
    T_EQ_INT(tc_http(g_port, "GET", "/t/publoop", NULL, NULL, 0, &r), 0);   // the same, published on the loop
    tc_response_free(&r);
    T_EQ_INT(fr_recv(dash, 2000, &m), NW_WS_VIEW);
    T_EQ_INT(fr_recv(page, 300, &m), 0);
    T_EQ_INT(cb_rt_local_quiet_skips(g_ctx.rt) - q0, 2);
    const uint8_t cast[] = { 7, 8, 9 };
    rt_publish_cast(g_ctx.rt, U1, G, 31, cast, 3);           // the seat's cast: every connection
    T_EQ_INT(fr_recv(page, 2000, &m), NW_WS_CAST);
    T_EQ_INT(fr_recv(dash, 2000, &m), NW_WS_CAST);
    cb_view_row v;
    memset(&v, 0, sizeof v);
    strcpy(v.game_id, "othergame001");
    uint8_t env[] = { 0xee };
    v.view = env;
    v.view_len = 1;
    v.version = 3;
    rt_publish_view(g_ctx.rt, U1, &v, false);                // another game's view: the page hears it
    T_EQ_INT(fr_recv(page, 2000, &m), NW_WS_VIEW);
    T_EQ_INT(fr_recv(dash, 2000, &m), NW_WS_VIEW);
    strcpy(v.game_id, G);
    rt_publish_view(g_ctx.rt, U1, &v, true);                 // the seat gone: every connection
    T_EQ_INT(fr_recv(page, 2000, &m), NW_WS_VIEW);
    T_EQ_INT(nw_frame_decode(m.p, (int)m.len, &pf), NW_OK);
    T_EQ_INT((int)pf.blob.len, 0);
    T_EQ_INT(fr_recv(dash, 2000, &m), NW_WS_VIEW);
    sub(page, SF_UNSUB, 0x80000022u, NW_SUB_GAME, G, 200, &m);   // out of the room: views again
    publish_view(32);
    T_EQ_INT(fr_recv(page, 2000, &m), NW_WS_VIEW);
    T_EQ_INT(fr_recv(dash, 2000, &m), NW_WS_VIEW);
    close(page);
    close(dash);
    cb_buf_free(&m);
}

// A move's shape: an inline route that pushes the caller's own view, then answers.
static void h_act(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    g_act_thr = pthread_self();
    publish_view(11);
    cb_res_text(res, 200, "done");
}

// A REQ to a CB_ROUTE_INLINE route runs on the loop thread (rt_local.c req_inline): no job posted
// back, and the handler's push to the caller and the REPLY leave in ONE send. A REQ whose session
// must be read again (sessions were ended since it was checked) goes to the pool, which may read
// the store.
static void an_inline_req_answers_with_its_pushes(void) {
    cb_buf m;
    cb_buf_init(&m);
    sf_frame f;
    NwFrame pf;
    int a = open_as(U1, NULL, &m);
    tc_response r;
    T_EQ_INT(tc_http(g_port, "GET", "/t/publoop", NULL, NULL, 0, &r), 0);   // the loop's thread, learned
    tc_response_free(&r);
    T_EQ_INT(fr_recv(a, 2000, &m), NW_WS_VIEW);
    usleep(20000);
    const uint64_t p0 = cb_loop_posts(g_ctx.loop);
    const int64_t s0 = cb_server_sends(g_ctx.server);
    uint8_t b[128];
    fr_send(a, b, sf_put_req(b, sizeof b, 7, SF_METH_POST, "/t/act", 6, (const uint8_t *)"x", 1));
    T_EQ_INT(fr_recv(a, 2000, &m), NW_WS_VIEW);   // the push the handler queued, then its answer
    T_EQ_INT(nw_frame_decode(m.p, (int)m.len, &pf), NW_OK);
    T_EQ_INT((int)pf.version, 11);
    T_EQ_INT(fr_recv(a, 2000, &m), SF_REPLY);
    T_EQ_INT(sf_parse(m.p, m.len, &f), 0);
    T_CHECK(f.id == 7 && f.status == 200);
    T_CHECK(pthread_equal(g_act_thr, g_loop_thr));
    T_CHECK(cb_loop_posts(g_ctx.loop) == p0);
    T_EQ_INT((int)(cb_server_sends(g_ctx.server) - s0), 1);
    // Sessions were ended (anybody's): the next REQ reads its own again, on the pool.
    cb_rt_end_sessions(g_ctx.rt, "00000000-0000-4000-8000-000000000000", NULL, NULL);
    T_EQ_INT(req(a, 8, SF_METH_POST, "/t/act", "x", &m, &f), -1000 - NW_WS_VIEW);
    T_EQ_INT(fr_recv(a, 2000, &m), SF_REPLY);
    T_CHECK(!pthread_equal(g_act_thr, g_loop_thr));
    // Checked again, the one after runs inline.
    T_EQ_INT(req(a, 9, SF_METH_POST, "/t/act", "x", &m, &f), -1000 - NW_WS_VIEW);
    T_EQ_INT(fr_recv(a, 2000, &m), SF_REPLY);
    T_CHECK(pthread_equal(g_act_thr, g_loop_thr));
    close(a);
    cb_buf_free(&m);
}

// An inline route whose next step would wait when it runs on the loop (http.h to_pool): there it
// queues a push and answers "to the pool", and on the pool it answers for real.
static atomic_int g_wait_runs;
static void h_would_wait(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    atomic_fetch_add(&g_wait_runs, 1);
    g_act_thr = pthread_self();
    if (cb_loop_on_thread(ctx->loop)) {
        cb_res_text(res, 200, "not this");
        res->to_pool = true;
        return;
    }
    cb_res_text(res, 201, "pooled");
}

// A REQ whose handler, run inline, would wait: nothing of that run is sent, and the same frame runs
// again on the pool, whose answer is the REPLY.
static void a_req_that_would_wait_runs_again_on_the_pool(void) {
    cb_buf m;
    cb_buf_init(&m);
    sf_frame f;
    int a = open_as(U1, NULL, &m);
    atomic_store(&g_wait_runs, 0);
    T_EQ_INT(req(a, 21, SF_METH_POST, "/t/wait", "x", &m, &f), 201);
    T_EQ_MEM(f.body, f.body_len, "pooled", 6);
    T_EQ_INT(atomic_load(&g_wait_runs), 2);
    T_CHECK(!pthread_equal(g_act_thr, g_loop_thr));
    T_EQ_INT(fr_recv(a, 200, &m), 0);   // and nothing else came
    // A route under /v1/auth/ is refused over the socket (sessions are made and ended over HTTP),
    // inline flag or not: its handler never runs.
    g_act_thr = pthread_self();
    T_EQ_INT(req(a, 22, SF_METH_POST, "/v1/auth/t", "x", &m, &f), 403);
    T_CHECK(pthread_equal(g_act_thr, pthread_self()));   // its handler never ran
    close(a);
    cb_buf_free(&m);
}

// A loop route that publishes U1's view 31 once the test has published view 30 from its own
// thread: that push is queued for the loop, which is in here and has not delivered it yet.
static atomic_int g_order_in, g_order_go;
static void h_order(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    atomic_store(&g_order_in, 1);
    for (int i = 0; i < 2000 && !atomic_load(&g_order_go); i++) usleep(1000);
    publish_view(31);
    cb_res_text(res, 200, "ok");
}
static void *get_order(void *ud) {
    tc_response r;
    if (tc_http(g_port, "GET", "/t/order", NULL, NULL, 0, &r) == 0) tc_response_free(&r);
    return NULL;
}

// A push from another thread, still queued for the loop, is heard before one the loop publishes
// after it: a game's pushes reach each hearer in its commit order, whichever thread committed.
static void pushes_keep_their_order_across_threads(void) {
    cb_buf m;
    cb_buf_init(&m);
    NwFrame pf;
    int a = open_as(U1, NULL, &m);
    atomic_store(&g_order_in, 0);
    atomic_store(&g_order_go, 0);
    pthread_t th;
    pthread_create(&th, NULL, get_order, NULL);
    for (int i = 0; i < 2000 && !atomic_load(&g_order_in); i++) usleep(1000);
    T_CHECK(atomic_load(&g_order_in));
    publish_view(30);   // queued: the loop is busy in /t/order
    atomic_store(&g_order_go, 1);
    pthread_join(th, NULL);
    T_EQ_INT(fr_recv(a, 2000, &m), NW_WS_VIEW);
    T_EQ_INT(nw_frame_decode(m.p, (int)m.len, &pf), NW_OK);
    T_EQ_INT((int)pf.version, 30);
    T_EQ_INT(fr_recv(a, 2000, &m), NW_WS_VIEW);
    T_EQ_INT(nw_frame_decode(m.p, (int)m.len, &pf), NW_OK);
    T_EQ_INT((int)pf.version, 31);
    close(a);
    cb_buf_free(&m);
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

// ---- the inbox (rt_local.h CB_RT_INBOX_MAX, CB_RT_INBOX_FRAMES_MAX) ---------------------------
//
// While a connection's REQ waits on the pool its next frames queue. An empty frame used to cost
// nothing against the cap (it counted payload bytes) yet a whole frame of heap, so a burst of them
// grew the heap without bound while a job held the one worker. Every pool worker is held here, so
// the REQ cannot run: the close comes from the inbox caps or not at all.

// Holds every pool worker and puts a REQ on the pool behind them: the connection is busy.
static void hold_and_req(int fd) {
    atomic_store(&g_hold, 1);
    for (int i = 0; i < 3; i++) T_EQ_INT(cb_pool_submit(g_ctx.req_pool, hold_worker, NULL), CB_OK);
    uint8_t b[128];
    fr_send(fd, b, sf_put_req(b, sizeof b, 1, SF_METH_POST, "/t/who", 6, (const uint8_t *)"x", 1));
}

static void a_flood_while_busy_ends_only_that_socket(void) {
    signal(SIGPIPE, SIG_IGN);
    cb_buf m;
    cb_buf_init(&m);
    const size_t empty_cost = 1024;   // an empty frame's room (rt_local.c SPARE_MSG_CAP), at the least
    // A few empty frames while busy are charged their heap, not their zero payload, and do not end it.
    int a = open_as(U1, NULL, &m);
    hold_and_req(a);
    T_EQ_INT(tc_ws_send_burst(a, NULL, 0, 100, 0), 0);
    usleep(100000);
    T_EQ_INT(fr_recv(a, 100, &m), 0);
    T_CHECK(cb_rt_local_inbox_peak(g_ctx.rt) >= 100 * empty_cost);
    atomic_store(&g_hold, 0);
    close(a);
    usleep(50000);

    // A burst of empty frames while busy: closed at CB_RT_INBOX_FRAMES_MAX, while the job still waits.
    a = open_as(U1, NULL, &m);
    int b = open_as(U2, NULL, &m);
    hold_and_req(a);
    tc_ws_send_burst(a, NULL, 0, 1000, 0);
    T_EQ_INT(fr_recv(a, 2000, &m), -1);
    T_EQ_INT(close_code(&m), 1008);
    T_CHECK(cb_rt_local_inbox_peak(g_ctx.rt) <= CB_RT_INBOX_FRAMES_MAX * (empty_cost + 64));
    // Everyone else is still served: b's PING is answered on the loop while the pool is held,
    // and its REQ once the pool is free.
    uint8_t pk[8];
    fr_send(b, pk, sf_put_bare(pk, sizeof pk, SF_PING));
    T_EQ_INT(fr_recv(b, 2000, &m), SF_PONG);
    atomic_store(&g_hold, 0);
    sf_frame f;
    T_EQ_INT(req(b, 2, SF_METH_POST, "/t/who", "y", &m, &f), 200);
    close(a);

    // Big frames while busy: closed once their heap passes CB_RT_INBOX_MAX, frame by frame, before the
    // frame count is anywhere near its cap.
    a = open_as(U1, NULL, &m);
    hold_and_req(a);
    const size_t big = 128 * 1024;
    int sent = 0;
    while (sent < 16) {
        tc_ws_send_burst(a, NULL, 0, 1, big);
        sent++;
        if (fr_recv(a, 100, &m) == -1) break;
    }
    T_EQ_INT(close_code(&m), 1008);
    T_EQ_INT(sent, (int)(CB_RT_INBOX_MAX / (big + 64)) + 1);
    T_CHECK(cb_rt_local_inbox_peak(g_ctx.rt) <= CB_RT_INBOX_MAX);
    atomic_store(&g_hold, 0);
    T_EQ_INT(req(b, 3, SF_METH_POST, "/t/who", "z", &m, &f), 200);
    close(a);
    close(b);
    cb_buf_free(&m);
}

static void drain_post(cb_loop *l, void *ud) { cb_server_drain(g_ctx.server, 2000); }

static void stop(void) {
    T_EQ_INT(cb_loop_post(g_ctx.loop, drain_post, NULL), CB_OK);
    pthread_join(g_thread, NULL);
    // A push from another thread once the loop has stopped: nothing will deliver it, and it says so
    // (counted a failure); the copy it queued is freed with the hub (local_free).
    cb_rt_failures_take();
    publish_view(99);
    T_EQ_INT(cb_rt_failures_take(), 1);
    cb_pool_free(g_ctx.req_pool);
    cb_server_free(g_ctx.server);
    cb_rt_free(g_ctx.rt);
    cb_router_free(g_ctx.router);
    cb_loop_free(g_ctx.loop);
}

// THE FAMILY RULE (realtime.h): a family is a user's own when its pattern is under u/{user}/, a
// room's under s/ naming {doc} alone; the check refuses a room's that names {user}, so the scope a
// push goes to is its prefix (cb_rt_family_user), never a search of the pattern.
static void the_family_rule(void) {
    static const cb_topic_family views = { .name = "views", .pattern = "u/{user}/v" }, seat = { .name = "seat", .pattern = "u/{user}/c/{doc}" },
                                 watch = { .name = "watch", .pattern = "s/{doc}" }, bad = { .name = "bad", .pattern = "s/{doc}/{user}" },
                                 out = { .name = "out", .pattern = "x/{user}" };
    const cb_topic_family *good[3] = { &views, &seat, &watch };
    char err[256];
    T_EQ_INT(cb_rt_families_check(good, 3, err, sizeof err), CB_OK);
    const cb_topic_family *one[1] = { &bad };
    T_EQ_INT(cb_rt_families_check(one, 1, err, sizeof err), CB_E_INVALID);
    one[0] = &out;
    T_EQ_INT(cb_rt_families_check(one, 1, err, sizeof err), CB_E_INVALID);
    T_CHECK(cb_rt_family_user(&views) && cb_rt_family_user(&seat));
    T_CHECK(!cb_rt_family_user(&watch));
    T_CHECK(!cb_rt_family_user(NULL));
    // A pattern shorter than the user prefix is read only to its end (ASan: a memcmp of the
    // prefix's length read past "s/{doc}" in pushes.c).
    static const char tiny_pattern[] = "s/x";
    static const cb_topic_family tiny = { .name = "tiny", .pattern = tiny_pattern };
    T_CHECK(!cb_rt_family_user(&tiny));
}

int main(void) {
    start();
    T_RUN(the_family_rule);
    T_RUN(ws_upgrade_route);
    T_RUN(parked_requests_hold_no_thread);
    T_RUN(pushes_reach_their_hearers);
    T_RUN(many_sockets_hear_once);
    T_RUN(a_push_on_the_loop_is_not_posted);
    T_RUN(a_users_pushes_in_a_pass_walk_once);
    T_RUN(a_room_is_heard_only_while_watched);
    T_RUN(a_view_skips_the_users_connection_in_its_room);
    T_RUN(an_inline_req_answers_with_its_pushes);
    T_RUN(a_req_that_would_wait_runs_again_on_the_pool);
    T_RUN(pushes_keep_their_order_across_threads);
    T_RUN(req_runs_a_route_as_the_user);
    T_RUN(ended_sessions_close_their_sockets);
    T_RUN(refusals_end_the_connection);
    T_RUN(a_flood_while_busy_ends_only_that_socket);
    stop();
    return T_DONE();
}
