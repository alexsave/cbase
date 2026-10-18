// unit_net.c - the loop, the server and the router over real sockets.
//
// A server on a free loopback port, run on its own thread, with a test route
// table: a pool handler, a loop handler and a WebSocket echo.
// Each check talks to it through core/test/lib_client.c as a client would.
#include "t.h"

#include "lib_client.h"
#include "net/loop.h"
#include "net/router.h"
#include "net/server.h"
#include "util/clock.h"
#include "util/err.h"
#include "util/log.h"
#include "util/pool.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <unistd.h>

static cb_ctx g_ctx;
static cb_config g_cfg;
static int g_port;
static pthread_t g_thread;
static atomic_int g_closed;

static void h_echo(cb_ctx *c, const cb_http_req *q, cb_http_res *s) {
    // Runs on the pool: prove it by not being the loop thread.
    cb_res_header(s, "X-On-Loop", cb_loop_on_thread(c->loop) ? "yes" : "no");
    cb_res_send(s, 200, "application/octet-stream", q->body, q->body_len);
}

static void h_loop(cb_ctx *c, const cb_http_req *q, cb_http_res *s) {
    cb_res_header(s, "X-On-Loop", cb_loop_on_thread(c->loop) ? "yes" : "no");
    cb_res_text(s, 200, q->query);
}

static void h_slow(cb_ctx *c, const cb_http_req *q, cb_http_res *s) {
    usleep(300 * 1000);
    cb_res_text(s, 200, "slow");
}

static void ws_msg(cb_ws *ws, int op, const uint8_t *d, size_t n, void *ud) {
    if (n == 5 && memcmp(d, "close", 5) == 0) { cb_ws_close(ws, 1000); return; }
    cb_ws_send(ws, op, d, n);
}
static void ws_open(cb_ws *ws, void *ud) { cb_ws_send_text(ws, "hello", 5); }
static void ws_close(cb_ws *ws, void *ud) { atomic_fetch_add(&g_closed, 1); }
static const cb_ws_handlers WS = { ws_open, ws_msg, ws_close };

static void h_ws(cb_ctx *c, const cb_http_req *q, cb_http_res *s) { cb_ws_upgrade(q, s, &WS, NULL); }

// A post from another thread reaches every socket: what realtime's publish does.
static void broadcast(cb_ws *ws, void *ud) { cb_ws_send_text(ws, ud, strlen(ud)); }
static void post_broadcast(cb_loop *l, void *ud) { cb_server_ws_each(g_ctx.server, broadcast, ud); }

static void *run(void *arg) {
    cb_loop_run(g_ctx.loop);
    return NULL;
}

static void start(void) {
    cb_log_init(CB_LOG_WARN, 0);
    memset(&g_cfg, 0, sizeof g_cfg);
    strcpy(g_cfg.allowed_origins, "*");
    g_ctx.cfg = &g_cfg;
    g_ctx.loop = cb_loop_new();
    g_ctx.req_pool = cb_pool_new(2, "treq");
    g_ctx.router = cb_router_new();
    cb_router_add(g_ctx.router, "POST", "/echo", CB_ROUTE_POOL, h_echo);
    cb_router_add(g_ctx.router, "GET", "/loop/*", CB_ROUTE_LOOP, h_loop);
    cb_router_add(g_ctx.router, "GET", "/slow", CB_ROUTE_POOL, h_slow);
    cb_router_add(g_ctx.router, "GET", "/ws", CB_ROUTE_LOOP, h_ws);
    cb_server_opts o;
    cb_server_opts_default(&o);
    o.max_body = 4096;
    g_ctx.server = cb_server_new(&g_ctx, &o);
    T_EQ_INT(cb_server_listen(g_ctx.server, "127.0.0.1", 0, &g_port), CB_OK);
    T_CHECK(g_port > 0);
    pthread_create(&g_thread, NULL, run, NULL);
}

static void http_basics(void) {
    tc_response r;
    char v[64];
    T_EQ_INT(tc_http(g_port, "POST", "/echo", "Content-Type: x\r\n", "abc\0def", 7, &r), 0);
    T_EQ_INT(r.status, 200);
    T_EQ_MEM(r.body.p, r.body.len, "abc\0def", 7);
    T_EQ_STR(tc_header(&r, "x-on-loop", v, sizeof v), "no");
    T_EQ_STR(tc_header(&r, "access-control-allow-origin", v, sizeof v), "*");
    tc_response_free(&r);
    T_EQ_INT(tc_http(g_port, "GET", "/loop/a?b=c", NULL, NULL, 0, &r), 0);
    T_EQ_INT(r.status, 200);
    T_EQ_STR(cb_buf_cstr(&r.body), "b=c");
    T_EQ_STR(tc_header(&r, "x-on-loop", v, sizeof v), "yes");
    tc_response_free(&r);
    T_EQ_INT(tc_http(g_port, "GET", "/nothing", NULL, NULL, 0, &r), 0);
    T_EQ_INT(r.status, 404);
    // The router's own refusals are plain text, never a JSON gateway error.
    T_EQ_STR(tc_header(&r, "content-type", v, sizeof v), "text/plain; charset=utf-8");
    T_EQ_STR(cb_buf_cstr(&r.body), "not found");
    tc_response_free(&r);
    T_EQ_INT(tc_http(g_port, "DELETE", "/echo", NULL, NULL, 0, &r), 0);
    T_EQ_INT(r.status, 405);
    tc_response_free(&r);
    T_EQ_INT(tc_http(g_port, "OPTIONS", "/echo", "Origin: https://foolish.cards\r\n", NULL, 0, &r), 0);
    T_EQ_INT(r.status, 200);
    T_EQ_STR(cb_buf_cstr(&r.body), "ok");
    T_EQ_STR(tc_header(&r, "access-control-max-age", v, sizeof v), "7200");
    tc_response_free(&r);
    T_EQ_INT(tc_http(g_port, "POST", "/echo", NULL, "x", 1, &r), 0);   // body within the limit
    tc_response_free(&r);
    char big[5000];
    memset(big, 'z', sizeof big);
    T_EQ_INT(tc_http(g_port, "POST", "/echo", NULL, big, sizeof big, &r), 0);
    T_EQ_INT(r.status, 413);
    tc_response_free(&r);
}

static void keep_alive_and_pipelining(void) {
    int fd = tc_connect(g_port);
    T_CHECK(fd >= 0);
    for (int i = 0; i < 3; i++) {
        tc_response r;
        T_EQ_INT(tc_request(fd, "POST", "/echo", NULL, "k", 1, 5000, &r), 0);
        T_EQ_INT(r.status, 200);
        char v[32];
        T_EQ_STR(tc_header(&r, "connection", v, sizeof v), "keep-alive");
        tc_response_free(&r);
    }
    // Two requests in one write: both are answered, in order.
    const char *two = "GET /loop/1 HTTP/1.1\r\nHost: x\r\n\r\nGET /loop/2?n=2 HTTP/1.1\r\nHost: x\r\n\r\n";
    T_CHECK(write(fd, two, strlen(two)) == (ssize_t)strlen(two));
    cb_buf in;
    cb_buf_init(&in);
    for (int tries = 0; tries < 100 && !strstr(cb_buf_cstr(&in), "n=2"); tries++) {
        char b[4096];
        ssize_t n = read(fd, b, sizeof b);
        if (n <= 0) break;
        cb_buf_append(&in, b, (size_t)n);
    }
    T_CHECK(strstr(cb_buf_cstr(&in), "n=2") != NULL);
    cb_buf_free(&in);
    close(fd);
}

static void slow_handler_does_not_block_the_loop(void) {
    // A pool handler sleeping 300 ms must not delay a loop request meanwhile.
    int fd = tc_connect(g_port);
    const char *req = "GET /slow HTTP/1.1\r\nHost: x\r\n\r\n";
    T_CHECK(write(fd, req, strlen(req)) == (ssize_t)strlen(req));
    tc_response r;
    int64_t t0 = cb_mono_ms();
    T_EQ_INT(tc_http(g_port, "GET", "/loop/fast", NULL, NULL, 0, &r), 0);
    T_CHECK(cb_mono_ms() - t0 < 250);
    tc_response_free(&r);
    cb_buf in;
    cb_buf_init(&in);
    char b[1024];
    ssize_t n;
    while (!strstr(cb_buf_cstr(&in), "slow") && (n = read(fd, b, sizeof b)) > 0) cb_buf_append(&in, b, (size_t)n);
    T_CHECK(strstr(cb_buf_cstr(&in), "200 OK") != NULL);
    cb_buf_free(&in);
    close(fd);
}

static void websocket_echo_and_broadcast(void) {
    int fd = tc_ws_open(g_port, "/ws?vsn=1.0.0", NULL);
    T_CHECK(fd >= 0);
    cb_buf m;
    cb_buf_init(&m);
    T_EQ_INT(tc_ws_recv(fd, 2000, &m), CB_WS_OP_TEXT);   // on_open's greeting
    T_EQ_STR(cb_buf_cstr(&m), "hello");
    T_EQ_INT(tc_ws_send(fd, CB_WS_OP_TEXT, "ping me", 7), 0);
    T_EQ_INT(tc_ws_recv(fd, 2000, &m), CB_WS_OP_TEXT);
    T_EQ_STR(cb_buf_cstr(&m), "ping me");
    T_EQ_INT(tc_ws_send(fd, CB_WS_OP_PING, "p", 1), 0);
    T_EQ_INT(tc_ws_recv(fd, 2000, &m), CB_WS_OP_PONG);
    // A large message in the 2-byte length form.
    char big[3000];
    memset(big, 'q', sizeof big);
    T_EQ_INT(tc_ws_send(fd, CB_WS_OP_BIN, big, sizeof big), 0);
    T_EQ_INT(tc_ws_recv(fd, 2000, &m), CB_WS_OP_BIN);
    T_EQ_INT(m.len, sizeof big);
    // From another thread, through the loop.
    T_EQ_INT(cb_loop_post(g_ctx.loop, post_broadcast, "to everyone"), CB_OK);
    T_EQ_INT(tc_ws_recv(fd, 2000, &m), CB_WS_OP_TEXT);
    T_EQ_STR(cb_buf_cstr(&m), "to everyone");
    int closed_before = atomic_load(&g_closed);
    T_EQ_INT(tc_ws_send(fd, CB_WS_OP_TEXT, "close", 5), 0);
    T_EQ_INT(tc_ws_recv(fd, 2000, &m), CB_WS_OP_CLOSE);
    for (int i = 0; i < 100 && atomic_load(&g_closed) == closed_before; i++) usleep(10000);
    T_EQ_INT(atomic_load(&g_closed), closed_before + 1);
    close(fd);
    cb_buf_free(&m);
    // An unmasked client frame is a protocol error (RFC 6455 5.1).
    fd = tc_ws_open(g_port, "/ws", NULL);
    cb_buf_init(&m);
    T_EQ_INT(tc_ws_recv(fd, 2000, &m), CB_WS_OP_TEXT);
    uint8_t unmasked[] = { 0x81, 1, 'x' };
    T_CHECK(write(fd, unmasked, 3) == 3);
    T_EQ_INT(tc_ws_recv(fd, 2000, &m), CB_WS_OP_CLOSE);
    cb_buf_free(&m);
    close(fd);
    // A plain GET to the WebSocket route is told to upgrade.
    tc_response r;
    T_EQ_INT(tc_http(g_port, "GET", "/ws", NULL, NULL, 0, &r), 0);
    T_EQ_INT(r.status, 426);
    tc_response_free(&r);
}

static void drain_post(cb_loop *l, void *ud) { cb_server_drain(g_ctx.server, 2000); }

static void shutdown_cleanly(void) {
    int fd = tc_ws_open(g_port, "/ws", NULL);
    cb_buf m;
    cb_buf_init(&m);
    T_EQ_INT(tc_ws_recv(fd, 2000, &m), CB_WS_OP_TEXT);
    T_EQ_INT(cb_loop_post(g_ctx.loop, drain_post, NULL), CB_OK);
    T_EQ_INT(tc_ws_recv(fd, 2000, &m), CB_WS_OP_CLOSE);   // 1001 going away
    T_EQ_INT(m.len, 2);
    T_EQ_INT((m.p[0] << 8) | m.p[1], 1001);
    pthread_join(g_thread, NULL);   // the drain stops the loop
    T_CHECK(tc_connect(g_port) < 0);   // the listener is gone
    cb_pool_free(g_ctx.req_pool);
    cb_server_free(g_ctx.server);
    cb_router_free(g_ctx.router);
    cb_loop_free(g_ctx.loop);
    cb_buf_free(&m);
    close(fd);
}

int main(void) {
    start();
    T_RUN(http_basics);
    T_RUN(keep_alive_and_pipelining);
    T_RUN(slow_handler_does_not_block_the_loop);
    T_RUN(websocket_echo_and_broadcast);
    T_RUN(shutdown_cleanly);
    return T_DONE();
}
