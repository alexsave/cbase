// unit_inline.c - hybrid inline dispatch (net/server.c cb_server_inline_ok): a CB_ROUTE_INLINE request
// runs on the loop thread when the request pool is idle and the loop pass has inline time left, goes
// to the pool when it is busy or the pass's time is spent, and runs over their route's budget, enough
// of them close together, switch inline off until the window has passed, for every route but one
// whose home is the loop (CB_ROUTE_HOME). A run counts toward the valve only by the loop thread's
// CPU time: the slow handlers here burn CPU, and one that only waits (a descheduled vCPU's hole) is
// not slow.
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
static pthread_t g_tx, g_ty, g_tz;  // the threads /fx, /fy and /fz ran on
static pthread_t g_last;            // the thread the last /fast, /slow or /plain ran on
static atomic_int g_entered, g_release;

static void h_loopid(cb_ctx *c, const cb_http_req *q, cb_http_res *s) {
    g_loop_thread = pthread_self();
    cb_res_send(s, 200, "text/plain", "ok", 2);
}
static void h_fast(cb_ctx *c, const cb_http_req *q, cb_http_res *s) { g_last = pthread_self(); cb_res_send(s, 200, "text/plain", "ok", 2); }
static void h_fx(cb_ctx *c, const cb_http_req *q, cb_http_res *s) { g_tx = pthread_self(); cb_res_send(s, 200, "text/plain", "ok", 2); }
static void h_fy(cb_ctx *c, const cb_http_req *q, cb_http_res *s) { g_ty = pthread_self(); cb_res_send(s, 200, "text/plain", "ok", 2); }
static void h_fz(cb_ctx *c, const cb_http_req *q, cb_http_res *s) { g_tz = pthread_self(); cb_res_send(s, 200, "text/plain", "ok", 2); }
// us of this thread's CPU, spinning.
static void burn_us(int64_t us) {
    const int64_t t0 = cb_thread_cpu_us();
    while (cb_thread_cpu_us() - t0 < us) {}
}
static void h_f5(cb_ctx *c, const cb_http_req *q, cb_http_res *s) { burn_us(5000); h_fx(c, q, s); }
static void h_stall(cb_ctx *c, const cb_http_req *q, cb_http_res *s) { usleep(150000); cb_res_send(s, 200, "text/plain", "ok", 2); }
static void h_slow(cb_ctx *c, const cb_http_req *q, cb_http_res *s) { g_last = pthread_self(); burn_us(6000); cb_res_send(s, 200, "text/plain", "ok", 2); }
// 3 ms of wall clock and almost no CPU: what a 20 us handler looks like when the hypervisor parks
// its vCPU mid-run.
static void h_nap(cb_ctx *c, const cb_http_req *q, cb_http_res *s) { g_last = pthread_self(); usleep(3000); cb_res_send(s, 200, "text/plain", "ok", 2); }
static void h_nap6(cb_ctx *c, const cb_http_req *q, cb_http_res *s) { usleep(6000); h_fx(c, q, s); }
static void h_nap1(cb_ctx *c, const cb_http_req *q, cb_http_res *s) { usleep(1000); h_fx(c, q, s); }
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
    cb_router_add(g_ctx.router, "POST", "/fx", CB_ROUTE_POOL | CB_ROUTE_INLINE, h_fx);
    cb_router_add(g_ctx.router, "POST", "/fy", CB_ROUTE_POOL | CB_ROUTE_INLINE, h_fy);
    cb_router_add(g_ctx.router, "POST", "/stall", CB_ROUTE_LOOP, h_stall);
    cb_router_add(g_ctx.router, "POST", "/slow", CB_ROUTE_POOL | CB_ROUTE_INLINE, h_slow);
    cb_router_add(g_ctx.router, "POST", "/plain", CB_ROUTE_POOL, h_fast);
    cb_router_add(g_ctx.router, "POST", "/block", CB_ROUTE_POOL, h_block);
    cb_router_add(g_ctx.router, "POST", "/fz", CB_ROUTE_POOL | CB_ROUTE_INLINE, h_fz);
    cb_router_add(g_ctx.router, "POST", "/nap", CB_ROUTE_POOL | CB_ROUTE_INLINE, h_nap);
    cb_router_add(g_ctx.router, "POST", "/home", CB_ROUTE_POOL | CB_ROUTE_INLINE | CB_ROUTE_HOME, h_fast);
    cb_router_add(g_ctx.router, "POST", "/nap1", CB_ROUTE_POOL | CB_ROUTE_INLINE, h_nap1);
    // Routes with a budget of their own (router.h cb_route.inline_us): 5 and 6 ms handlers not over
    // theirs, however long a loaded runner oversleeps them (0.5 s).
    static const cb_route own[] = {
        { "POST", "/f5", CB_ROUTE_POOL | CB_ROUTE_INLINE, h_f5, 500000 },
        { "POST", "/slowok", CB_ROUTE_POOL | CB_ROUTE_INLINE, h_slow, 500000 },
        { "POST", "/nap6", CB_ROUTE_POOL | CB_ROUTE_INLINE, h_nap6, 5000 },
    };
    T_EQ_INT(cb_router_add_all(g_ctx.router, own, 3), CB_OK);
    cb_server_opts o;
    cb_server_opts_default(&o);
    o.inline_budget_us = 2000;
    o.inline_trips = 3;
    o.inline_trip_window_ms = 1000;
    o.inline_off_ms = 400;
    o.inline_pass_us = 4000;
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
    cb_inline_stats st;

    // An idle pool: the loop thread answers.
    T_EQ_INT(post("/fast"), 200);
    T_CHECK(on_loop());
    cb_server_inline_stats(g_ctx.server, &st);
    T_CHECK(st.ran == 1 && st.tripped == 0);
    // A route that is not marked stays on the pool.
    T_EQ_INT(post("/plain"), 200);
    T_CHECK(!on_loop());

    // A busy pool does not hold it back: with the only worker stuck in /block, /fast is answered on
    // the loop at once (a handler run there never waits on the pool's work).
    int a = tc_connect(g_port);
    const char *req = "POST /block HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\n\r\nx";
    T_CHECK(write(a, req, strlen(req)) == (ssize_t)strlen(req));
    for (int i = 0; i < 2000 && !atomic_load(&g_entered); i++) usleep(1000);
    T_CHECK(atomic_load(&g_entered));
    T_EQ_INT(post("/fast"), 200);
    T_CHECK(on_loop());
    atomic_store(&g_release, 1);
    char buf[256];
    struct pollfd p = { a, POLLIN, 0 };
    T_CHECK(poll(&p, 1, 3000) == 1);
    T_CHECK(recv(a, buf, sizeof buf, 0) > 0);
    close(a);
    cb_server_inline_stats(g_ctx.server, &st);
    T_CHECK(st.ran == 2);
    for (int i = 0; i < 2000 && !cb_pool_idle(g_ctx.req_pool); i++) usleep(1000);
}

// Requests that become ready in the same loop pass (the loop is held in a stalled handler while
// they arrive) all run inline while the pass has inline time left: a pass with several moves ready
// runs them there, not one of them (cb_server_inline_ok has no "last ready event" rule).
static void a_pass_runs_its_ready_requests_inline(void) {
    cb_inline_stats s0, s1;
    int a = tc_connect(g_port), b = tc_connect(g_port), c = tc_connect(g_port), d = tc_connect(g_port);
    usleep(50000);   // all accepted
    const char *rs = "POST /stall HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\n\r\nx";
    const char *rx = "POST /fx HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\n\r\nx";
    const char *ry = "POST /fy HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\n\r\nx";
    const char *rz = "POST /fz HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\n\r\nx";
    cb_server_inline_stats(g_ctx.server, &s0);
    T_CHECK(write(d, rs, strlen(rs)) == (ssize_t)strlen(rs));
    usleep(40000);   // the loop is inside /stall now
    T_CHECK(write(a, rx, strlen(rx)) == (ssize_t)strlen(rx));
    T_CHECK(write(b, ry, strlen(ry)) == (ssize_t)strlen(ry));
    T_CHECK(write(c, rz, strlen(rz)) == (ssize_t)strlen(rz));
    char buf[256];
    int fds[4] = { d, a, b, c };
    for (int i = 0; i < 4; i++) {
        struct pollfd p = { fds[i], POLLIN, 0 };
        T_CHECK(poll(&p, 1, 3000) == 1);
        T_CHECK(recv(fds[i], buf, sizeof buf, 0) > 0);
    }
    close(a); close(b); close(c); close(d);
    cb_server_inline_stats(g_ctx.server, &s1);
    T_CHECK(pthread_equal(g_tx, g_loop_thread) && pthread_equal(g_ty, g_loop_thread) && pthread_equal(g_tz, g_loop_thread));
    T_EQ_INT((int)(s1.ran - s0.ran), 3);
    for (int i = 0; i < 2000 && !cb_pool_idle(g_ctx.req_pool); i++) usleep(1000);
}

// A pass's inline time is bounded (inline_pass_us 4 ms here): a 5 ms handler ready first spends it
// (not over its route's own budget), and the two requests ready behind it in the same pass go
// to the pool instead of waiting for the loop.
static void the_pass_budget_sends_the_rest_to_the_pool(void) {
    cb_inline_stats s0, s1;
    int a = tc_connect(g_port), b = tc_connect(g_port), c = tc_connect(g_port), d = tc_connect(g_port);
    usleep(50000);
    const char *rs = "POST /stall HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\n\r\nx";
    const char *rx = "POST /f5 HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\n\r\nx";
    const char *ry = "POST /fy HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\n\r\nx";
    const char *rz = "POST /fz HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\n\r\nx";
    cb_server_inline_stats(g_ctx.server, &s0);
    T_CHECK(write(d, rs, strlen(rs)) == (ssize_t)strlen(rs));
    usleep(40000);   // the loop is inside /stall now: the three below are ready in its next pass, in this order
    T_CHECK(write(a, rx, strlen(rx)) == (ssize_t)strlen(rx));
    usleep(5000);
    T_CHECK(write(b, ry, strlen(ry)) == (ssize_t)strlen(ry));
    usleep(5000);
    T_CHECK(write(c, rz, strlen(rz)) == (ssize_t)strlen(rz));
    char buf[256];
    int fds[4] = { d, a, b, c };
    for (int i = 0; i < 4; i++) {
        struct pollfd p = { fds[i], POLLIN, 0 };
        T_CHECK(poll(&p, 1, 3000) == 1);
        T_CHECK(recv(fds[i], buf, sizeof buf, 0) > 0);
    }
    close(a); close(b); close(c); close(d);
    cb_server_inline_stats(g_ctx.server, &s1);
    T_CHECK(pthread_equal(g_tx, g_loop_thread));
    T_CHECK(!pthread_equal(g_ty, g_loop_thread) && !pthread_equal(g_tz, g_loop_thread));
    T_EQ_INT((int)(s1.ran - s0.ran), 1);
    T_EQ_INT((int)(s1.over - s0.over), 0);
    for (int i = 0; i < 2000 && !cb_pool_idle(g_ctx.req_pool); i++) usleep(1000);
}

// A run that only looks slow (6 ms of wall clock, its own budget 5 ms, no CPU: a hole) costs the
// pass nothing until a run measured by CPU says otherwise: the two requests ready behind it in the
// same pass run inline too, where a hole charged at its budget would have spent the pass's 4 ms.
static void a_hole_costs_the_pass_nothing(void) {
    cb_inline_stats s0, s1;
    int a = tc_connect(g_port), b = tc_connect(g_port), c = tc_connect(g_port), d = tc_connect(g_port);
    usleep(50000);
    const char *rs = "POST /stall HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\n\r\nx";
    const char *rx = "POST /nap6 HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\n\r\nx";
    const char *ry = "POST /fy HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\n\r\nx";
    const char *rz = "POST /fz HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\n\r\nx";
    cb_server_inline_stats(g_ctx.server, &s0);
    T_CHECK(write(d, rs, strlen(rs)) == (ssize_t)strlen(rs));
    usleep(40000);   // the loop is inside /stall now: the three below are ready in its next pass, in this order
    T_CHECK(write(a, rx, strlen(rx)) == (ssize_t)strlen(rx));
    usleep(5000);
    T_CHECK(write(b, ry, strlen(ry)) == (ssize_t)strlen(ry));
    usleep(5000);
    T_CHECK(write(c, rz, strlen(rz)) == (ssize_t)strlen(rz));
    char buf[256];
    int fds[4] = { d, a, b, c };
    for (int i = 0; i < 4; i++) {
        struct pollfd p = { fds[i], POLLIN, 0 };
        T_CHECK(poll(&p, 1, 3000) == 1);
        T_CHECK(recv(fds[i], buf, sizeof buf, 0) > 0);
    }
    close(a); close(b); close(c); close(d);
    cb_server_inline_stats(g_ctx.server, &s1);
    T_CHECK(pthread_equal(g_tx, g_loop_thread) && pthread_equal(g_ty, g_loop_thread) && pthread_equal(g_tz, g_loop_thread));
    T_EQ_INT((int)(s1.ran - s0.ran), 3);
    T_EQ_INT((int)(s1.over - s0.over), 0);
    T_EQ_INT(post("/fast"), 200);   // (the suspect it left measured away)
    for (int i = 0; i < 2000 && !cb_pool_idle(g_ctx.req_pool); i++) usleep(1000);
}

// The pass budget is CPU: six runs of a 1 ms nap each (within the 2 ms budget, so no suspect) and
// no CPU spend over 6 ms of the pass's wall but nothing of its 4 ms: the seventh, ready behind
// them in the same pass, runs inline too.
static void a_pass_counts_cpu_not_wall(void) {
    cb_inline_stats s0, s1;
    int fd[8];
    for (int i = 0; i < 8; i++) fd[i] = tc_connect(g_port);
    usleep(50000);
    const char *rs = "POST /stall HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\n\r\nx";
    const char *rn = "POST /nap1 HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\n\r\nx";
    const char *rz = "POST /fz HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\n\r\nx";
    cb_server_inline_stats(g_ctx.server, &s0);
    T_CHECK(write(fd[0], rs, strlen(rs)) == (ssize_t)strlen(rs));
    usleep(40000);   // the loop is inside /stall: the seven below are ready in its next pass
    for (int i = 1; i <= 6; i++) {
        T_CHECK(write(fd[i], rn, strlen(rn)) == (ssize_t)strlen(rn));
        usleep(2000);
    }
    T_CHECK(write(fd[7], rz, strlen(rz)) == (ssize_t)strlen(rz));
    char buf[256];
    for (int i = 0; i < 8; i++) {
        struct pollfd p = { fd[i], POLLIN, 0 };
        T_CHECK(poll(&p, 1, 3000) == 1);
        T_CHECK(recv(fd[i], buf, sizeof buf, 0) > 0);
        close(fd[i]);
    }
    cb_server_inline_stats(g_ctx.server, &s1);
    T_EQ_INT((int)(s1.ran - s0.ran), 7);
    T_CHECK(pthread_equal(g_tz, g_loop_thread));
    T_EQ_INT((int)(s1.cpu_timed - s0.cpu_timed), 5);   // the runs after the pass's second, and only they
    for (int i = 0; i < 2000 && !cb_pool_idle(g_ctx.req_pool); i++) usleep(1000);
}

static void valve(void) {
    cb_inline_stats st;
    usleep(1100000);   // nothing over budget in the window behind us
    // A route's own budget: 6 ms is not over its 0.5 s.
    T_EQ_INT(post("/slowok"), 200);
    T_CHECK(on_loop());
    cb_server_inline_stats(g_ctx.server, &st);
    T_EQ_INT((int)st.over, 0);
    // Three runs of 3 ms of wall clock each within the window, but microseconds of CPU: none is over
    // the 2 ms budget, and the valve stays shut.
    for (int i = 0; i < 3; i++) {
        T_EQ_INT(post("/nap"), 200);
        T_CHECK(on_loop());
    }
    cb_server_inline_stats(g_ctx.server, &st);
    T_EQ_INT((int)st.over, 0);
    T_EQ_INT((int)st.tripped, 0);
    // Over the server's 2 ms by its CPU. The first of a run of them is only a suspect (the wall
    // clock said so: net/server.c THE VALVE'S CLOCKS), each after it is measured by CPU and counts,
    // and only the third counted within the window trips the valve (inline_trips 3).
    T_EQ_INT(post("/fast"), 200);   // (no suspect pending from the naps)
    T_CHECK(on_loop());
    // A fast run after a fast one reads no CPU clock: a human-pace move costs no system call for the valve.
    cb_inline_stats f0;
    cb_server_inline_stats(g_ctx.server, &f0);
    for (int i = 0; i < 3; i++) {
        T_EQ_INT(post("/fast"), 200);
        T_CHECK(on_loop());
    }
    cb_server_inline_stats(g_ctx.server, &st);
    T_EQ_INT((int)(st.ran - f0.ran), 3);
    T_EQ_INT((int)(st.cpu_timed - f0.cpu_timed), 0);
    for (int i = 0; i < 3; i++) {
        T_EQ_INT(post("/slow"), 200);
        T_CHECK(on_loop());
    }
    cb_server_inline_stats(g_ctx.server, &st);
    T_CHECK(st.over == 2 && st.tripped == 0);
    T_EQ_INT(post("/fast"), 200);
    T_CHECK(on_loop());   // two are a hiccup: still inline
    T_EQ_INT(post("/slow"), 200);   // a suspect again, after the fast run
    T_CHECK(on_loop());
    T_EQ_INT(post("/slow"), 200);
    T_CHECK(on_loop());
    cb_server_inline_stats(g_ctx.server, &st);
    T_CHECK(st.over == 3 && st.tripped == 1);
    T_EQ_INT(post("/fast"), 200);
    T_CHECK(!on_loop());
    // A route whose home is the loop stays there while the valve is open.
    T_EQ_INT(post("/home"), 200);
    T_CHECK(on_loop());
    // It re-arms once the window has passed.
    usleep(600000);
    T_EQ_INT(post("/fast"), 200);
    T_CHECK(on_loop());
    // Runs over budget spread wider than the window never trip it.
    usleep(1100000);
    for (int i = 0; i < 4; i++) {
        T_EQ_INT(post("/slow"), 200);
        T_CHECK(on_loop());
        usleep(600000);
    }
    cb_server_inline_stats(g_ctx.server, &st);
    T_CHECK(st.over == 6 && st.tripped == 1);
}

// Two requests in one write, both inline: the first answer waits for the end of the loop pass
// (net/server.c finish), the pipelined second runs behind it, and both leave in ONE send, in order.
static void pipelined_answers_share_one_send(void) {
    int fd = tc_connect(g_port);
    usleep(20000);
    const char *two = "POST /fx HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\n\r\nx"
                      "POST /fy HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\n\r\ny";
    int64_t s0 = cb_server_sends(g_ctx.server);
    cb_inline_stats i0, i1;
    cb_server_inline_stats(g_ctx.server, &i0);
    T_CHECK(write(fd, two, strlen(two)) == (ssize_t)strlen(two));
    char buf[1024];
    size_t got = 0;
    int answers = 0;
    while (answers < 2) {
        struct pollfd p = { fd, POLLIN, 0 };
        if (poll(&p, 1, 3000) != 1) break;
        ssize_t r = recv(fd, buf + got, sizeof buf - 1 - got, 0);
        if (r <= 0) break;
        got += (size_t)r;
        buf[got] = 0;
        answers = 0;
        for (const char *q = buf; (q = strstr(q, "HTTP/1.1 200")) != NULL; q++) answers++;
    }
    T_EQ_INT(answers, 2);
    cb_server_inline_stats(g_ctx.server, &i1);
    T_EQ_INT((int)(i1.ran - i0.ran), 2);
    T_CHECK(pthread_equal(g_tx, g_loop_thread) && pthread_equal(g_ty, g_loop_thread));
    T_EQ_INT((int)(cb_server_sends(g_ctx.server) - s0), 1);
    close(fd);
}

static void drain_post(cb_loop *l, void *ud) { cb_server_drain(g_ctx.server, 2000); }

int main(void) {
    start();
    T_RUN(inline_dispatch);
    T_RUN(a_pass_runs_its_ready_requests_inline);
    T_RUN(the_pass_budget_sends_the_rest_to_the_pool);
    T_RUN(a_hole_costs_the_pass_nothing);
    T_RUN(a_pass_counts_cpu_not_wall);
    T_RUN(valve);
    T_RUN(pipelined_answers_share_one_send);
    cb_loop_post(g_ctx.loop, drain_post, NULL);
    pthread_join(g_thread, NULL);
    cb_pool_free(g_ctx.req_pool);
    cb_server_free(g_ctx.server);
    cb_router_free(g_ctx.router);
    cb_loop_free(g_ctx.loop);
    return T_DONE();
}
