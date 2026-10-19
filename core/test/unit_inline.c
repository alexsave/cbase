// unit_inline.c - hybrid inline dispatch (net/server.c inline_ok): a CB_ROUTE_INLINE request runs on
// the loop thread when the request pool is idle and nothing else is ready, goes to the pool when it
// is busy, and a request that blows the budget switches inline off until the window has passed.
#include "t.h"

#include "lib_client.h"
#include "net/loop.h"
#include "net/router.h"
#include "net/server.h"
#include "util/clock.h"
#include "util/err.h"
#include "util/log.h"
#include "util/pool.h"

#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

static cb_ctx g_ctx;
static cb_config g_cfg;
static int g_port;
static pthread_t g_thread;
static pthread_t g_loop_thread;
static pthread_t g_last;            // the thread the last /fast, /slow or /plain ran on
static atomic_int g_entered, g_release;

static void h_loopid(cb_ctx *c, const cb_http_req *q, cb_http_res *s) {
    g_loop_thread = pthread_self();
    cb_res_send(s, 200, "text/plain", "ok", 2);
}
static void h_fast(cb_ctx *c, const cb_http_req *q, cb_http_res *s) { g_last = pthread_self(); cb_res_send(s, 200, "text/plain", "ok", 2); }
static void h_slow(cb_ctx *c, const cb_http_req *q, cb_http_res *s) { g_last = pthread_self(); usleep(6000); cb_res_send(s, 200, "text/plain", "ok", 2); }
static void h_block(cb_ctx *c, const cb_http_req *q, cb_http_res *s) {
    atomic_store(&g_entered, 1);
    while (!atomic_load(&g_release)) usleep(1000);
    cb_res_send(s, 200, "text/plain", "ok", 2);
}
static void *run(void *arg) { cb_loop_run(g_ctx.loop); return NULL; }

static void start(void) {
    cb_log_init(CB_LOG_ERROR + 1, 0);
    memset(&g_cfg, 0, sizeof g_cfg);
    strcpy(g_cfg.allowed_origins, "*");
    g_ctx.cfg = &g_cfg;
    g_ctx.loop = cb_loop_new();
    g_ctx.req_pool = cb_pool_new(1, "tinl");
    g_ctx.router = cb_router_new();
    cb_router_add(g_ctx.router, "GET", "/loopid", CB_ROUTE_LOOP, h_loopid);
    cb_router_add(g_ctx.router, "POST", "/fast", CB_ROUTE_POOL | CB_ROUTE_INLINE, h_fast);
    cb_router_add(g_ctx.router, "POST", "/slow", CB_ROUTE_POOL | CB_ROUTE_INLINE, h_slow);
    cb_router_add(g_ctx.router, "POST", "/plain", CB_ROUTE_POOL, h_fast);
    cb_router_add(g_ctx.router, "POST", "/block", CB_ROUTE_POOL, h_block);
    cb_server_opts o;
    cb_server_opts_default(&o);
    o.inline_budget_us = 2000;
    o.inline_off_ms = 400;
    g_ctx.server = cb_server_new(&g_ctx, &o);
    T_EQ_INT(cb_server_listen(g_ctx.server, "127.0.0.1", 0, &g_port), CB_OK);
    pthread_create(&g_thread, NULL, run, NULL);
}

static int post(const char *path) {
    tc_response r;
    T_EQ_INT(tc_http(g_port, "POST", path, NULL, "x", 1, &r), 0);
    int st = r.status;
    tc_response_free(&r);
    return st;
}
static bool on_loop(void) { return pthread_equal(g_last, g_loop_thread); }

static void inline_dispatch(void) {
    tc_response r;
    T_EQ_INT(tc_http(g_port, "GET", "/loopid", NULL, NULL, 0, &r), 0);
    tc_response_free(&r);
    int64_t ran, tripped;

    // An idle pool and nothing else ready: the loop thread answers.
    T_EQ_INT(post("/fast"), 200);
    T_CHECK(on_loop());
    cb_server_inline_stats(g_ctx.server, &ran, &tripped);
    T_CHECK(ran == 1 && tripped == 0);
    // A route that is not marked stays on the pool.
    T_EQ_INT(post("/plain"), 200);
    T_CHECK(!on_loop());

    // A busy pool: the request is handed off, behind the running one.
    int a = tc_connect(g_port);
    const char *req = "POST /block HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\n\r\nx";
    T_CHECK(write(a, req, strlen(req)) == (ssize_t)strlen(req));
    for (int i = 0; i < 2000 && !atomic_load(&g_entered); i++) usleep(1000);
    T_CHECK(atomic_load(&g_entered));
    int b = tc_connect(g_port);
    const char *req2 = "POST /fast HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\n\r\nx";
    T_CHECK(write(b, req2, strlen(req2)) == (ssize_t)strlen(req2));
    usleep(50000);
    atomic_store(&g_release, 1);
    char buf[256];
    struct pollfd p = { b, POLLIN, 0 };
    T_CHECK(poll(&p, 1, 3000) == 1);
    T_CHECK(recv(b, buf, sizeof buf, 0) > 0);
    T_CHECK(!on_loop());
    close(a);
    close(b);
    cb_server_inline_stats(g_ctx.server, &ran, &tripped);
    T_CHECK(ran == 1);
    for (int i = 0; i < 2000 && !cb_pool_idle(g_ctx.req_pool); i++) usleep(1000);
}

static void valve(void) {
    int64_t ran, tripped;
    // Over budget: it still answers, the valve trips, and the next request goes to the pool.
    T_EQ_INT(post("/slow"), 200);
    T_CHECK(on_loop());
    cb_server_inline_stats(g_ctx.server, &ran, &tripped);
    T_EQ_INT((int)tripped, 1);
    T_EQ_INT(post("/fast"), 200);
    T_CHECK(!on_loop());
    cb_server_inline_stats(g_ctx.server, &ran, &tripped);
    T_EQ_INT((int)tripped, 1);
    // It re-arms once the window has passed.
    usleep(600000);
    T_EQ_INT(post("/fast"), 200);
    T_CHECK(on_loop());
}

static void drain_post(cb_loop *l, void *ud) { cb_server_drain(g_ctx.server, 2000); }

int main(void) {
    start();
    T_RUN(inline_dispatch);
    T_RUN(valve);
    cb_loop_post(g_ctx.loop, drain_post, NULL);
    pthread_join(g_thread, NULL);
    cb_pool_free(g_ctx.req_pool);
    cb_server_free(g_ctx.server);
    cb_router_free(g_ctx.router);
    cb_loop_free(g_ctx.loop);
    return T_DONE();
}
