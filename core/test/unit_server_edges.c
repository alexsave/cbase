// unit_server_edges.c - the server's defences against a client, over real
// sockets: a request that never finishes, a connection that says nothing,
// one connection too many, a body past the cap, and the WebSocket frames
// RFC 6455 refuses (a continuation with nothing to continue, a new message
// inside an unfinished one, an assembled message past the cap), with the
// ones it allows (fragments, PING with a payload, PONG, CLOSE) and a reader
// too slow to keep up. Timeouts are made short so the sweep runs here.
#include "t.h"

#include "lib_client.h"
#include "net/loop.h"
#include "net/router.h"
#include "net/server.h"
#include "net/ws.h"
#include "util/clock.h"
#include "util/err.h"
#include "util/log.h"
#include "util/pool.h"
#include "util/uuid.h"

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
static atomic_int g_flood_rc;

static void h_echo(cb_ctx *c, const cb_http_req *q, cb_http_res *s) { cb_res_send(s, 200, "text/plain", q->body, q->body_len); }

static void ws_msg(cb_ws *ws, int op, const uint8_t *d, size_t n, void *ud) {
    if (n == 5 && memcmp(d, "flood", 5) == 0) {
        // Past what a reader may owe: the server drops it rather than buffer forever.
        static uint8_t chunk[256 * 1024];
        int rc = CB_OK;
        for (int i = 0; i < 64 && rc == CB_OK; i++) rc = cb_ws_send(ws, CB_WS_OP_BIN, chunk, sizeof chunk);
        atomic_store(&g_flood_rc, rc);
        return;
    }
    cb_ws_send(ws, op, d, n);
}
static const cb_ws_handlers WS = { NULL, ws_msg, NULL };
static void h_ws(cb_ctx *c, const cb_http_req *q, cb_http_res *s) { cb_ws_upgrade(q, s, &WS, NULL); }

static void *run(void *arg) {
    cb_loop_run(g_ctx.loop);
    return NULL;
}

static void start(void) {
    cb_log_init(CB_LOG_ERROR + 1, 0);
    memset(&g_cfg, 0, sizeof g_cfg);
    strcpy(g_cfg.allowed_origins, "*");
    g_ctx.cfg = &g_cfg;
    g_ctx.loop = cb_loop_new();
    g_ctx.req_pool = cb_pool_new(2, "tedge");
    g_ctx.router = cb_router_new();
    cb_router_add(g_ctx.router, "POST", "/echo", CB_ROUTE_POOL, h_echo);
    cb_router_add(g_ctx.router, "GET", "/ws", CB_ROUTE_LOOP, h_ws);
    cb_server_opts o;
    cb_server_opts_default(&o);
    o.max_body = 1024;
    o.idle_ms = 300;
    o.head_ms = 300;
    o.ws_idle_ms = 2500;
    o.max_conns = 6;
    g_ctx.server = cb_server_new(&g_ctx, &o);
    T_EQ_INT(cb_server_listen(g_ctx.server, "127.0.0.1", 0, &g_port), CB_OK);
    pthread_create(&g_thread, NULL, run, NULL);
}

// Everything the server sends on fd until it closes (or timeout_ms passes).
static void read_until_closed(int fd, cb_buf *out, int timeout_ms, bool *closed) {
    *closed = false;
    int64_t t0 = cb_mono_ms();
    while (cb_mono_ms() - t0 < timeout_ms) {
        struct pollfd p = { fd, POLLIN, 0 };
        if (poll(&p, 1, 100) <= 0) continue;
        char b[4096];
        ssize_t n = recv(fd, b, sizeof b, 0);
        if (n <= 0) { *closed = true; return; }
        cb_buf_append(out, b, (size_t)n);
    }
}

static void http_defences(void) {
    cb_buf in;
    bool closed;
    // A request line that is not one: 400, then closed.
    int fd = tc_connect(g_port);
    T_CHECK(write(fd, "BLAH\r\n\r\n", 8) == 8);
    cb_buf_init(&in);
    read_until_closed(fd, &in, 3000, &closed);
    T_CHECK(cb_starts_with(cb_buf_cstr(&in), "HTTP/1.1 400 "));
    T_CHECK(closed);
    close(fd);
    cb_buf_free(&in);
    // A body past the cap: 413.
    tc_response r;
    char big[2048];
    memset(big, 'b', sizeof big);
    T_EQ_INT(tc_http(g_port, "POST", "/echo", NULL, big, sizeof big, &r), 0);
    T_EQ_INT(r.status, 413);
    tc_response_free(&r);
    // A request that started and never finished: 408 once head_ms has gone.
    fd = tc_connect(g_port);
    T_CHECK(write(fd, "GET /echo HTTP/1.1\r\nHost:", 25) == 25);
    cb_buf_init(&in);
    read_until_closed(fd, &in, 4000, &closed);
    T_CHECK(cb_starts_with(cb_buf_cstr(&in), "HTTP/1.1 408 "));
    T_CHECK(closed);
    close(fd);
    cb_buf_free(&in);
    // A connection that never says anything: closed once idle_ms has gone, with nothing sent.
    fd = tc_connect(g_port);
    cb_buf_init(&in);
    read_until_closed(fd, &in, 4000, &closed);
    T_CHECK(closed);
    T_EQ_INT(in.len, 0);
    close(fd);
    cb_buf_free(&in);
}

static void one_connection_too_many(void) {
    int fds[6];
    for (int i = 0; i < 6; i++) fds[i] = tc_connect(g_port);
    usleep(100 * 1000);
    // The seventh is closed at once, its request unanswered.
    int extra = tc_connect(g_port);
    static const char REQ[] = "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\n\r\nx";
    if (write(extra, REQ, sizeof REQ - 1) < 0) {}
    cb_buf in;
    cb_buf_init(&in);
    bool closed;
    read_until_closed(extra, &in, 1000, &closed);
    T_CHECK(closed);
    T_EQ_INT(in.len, 0);
    close(extra);
    cb_buf_free(&in);
    for (int i = 0; i < 6; i++) close(fds[i]);
    usleep(1500 * 1000);   // the sweep lets the six go
    tc_response r;
    T_EQ_INT(tc_http(g_port, "POST", "/echo", NULL, "x", 1, &r), 0);
    T_EQ_INT(r.status, 200);
    tc_response_free(&r);
}

// One frame with the fin bit and opcode given, masked as a client's must be.
static void frame(int fd, bool fin, int op, const void *data, size_t len) {
    uint8_t mask[4] = { 1, 2, 3, 4 };
    uint8_t hdr[14];
    size_t hn = cb_ws_frame_header(hdr, fin, op, len, mask);
    uint8_t *f = malloc(hn + len + 1);
    memcpy(f, hdr, hn);
    memcpy(f + hn, data, len);
    cb_ws_unmask(f + hn, len, mask);
    T_CHECK(write(fd, f, hn + len) == (ssize_t)(hn + len));
    free(f);
}

// The close code of the next frame, which must be a CLOSE (-1 otherwise).
static int close_code(int fd) {
    cb_buf m;
    cb_buf_init(&m);
    int op = tc_ws_recv(fd, 3000, &m);
    int code = op == CB_WS_OP_CLOSE && m.len >= 2 ? (m.p[0] << 8 | m.p[1]) : -1;
    cb_buf_free(&m);
    return code;
}

static void websocket_rules(void) {
    cb_buf m;
    cb_buf_init(&m);
    // Fragments: TEXT without fin, then CONT with it: one message, echoed whole.
    int fd = tc_ws_open(g_port, "/ws", NULL);
    T_CHECK(fd >= 0);
    frame(fd, false, CB_WS_OP_TEXT, "hel", 3);
    frame(fd, false, CB_WS_OP_CONT, "lo ", 3);
    frame(fd, true, CB_WS_OP_CONT, "you", 3);
    T_EQ_INT(tc_ws_recv(fd, 3000, &m), CB_WS_OP_TEXT);
    T_EQ_STR(cb_buf_cstr(&m), "hello you");
    // A PING's payload comes back in its PONG; a PONG is answered with nothing.
    frame(fd, true, CB_WS_OP_PONG, "x", 1);
    frame(fd, true, CB_WS_OP_PING, "abc", 3);
    T_EQ_INT(tc_ws_recv(fd, 3000, &m), CB_WS_OP_PONG);
    T_EQ_STR(cb_buf_cstr(&m), "abc");
    // A CLOSE is answered with 1000 and the connection ends.
    frame(fd, true, CB_WS_OP_CLOSE, "\x03\xe8", 2);
    T_EQ_INT(close_code(fd), 1000);
    close(fd);
    // A continuation with nothing to continue: 1002.
    fd = tc_ws_open(g_port, "/ws", NULL);
    frame(fd, true, CB_WS_OP_CONT, "x", 1);
    T_EQ_INT(close_code(fd), 1002);
    close(fd);
    // A new message inside an unfinished one: 1002.
    fd = tc_ws_open(g_port, "/ws", NULL);
    frame(fd, false, CB_WS_OP_TEXT, "a", 1);
    frame(fd, true, CB_WS_OP_BIN, "b", 1);
    T_EQ_INT(close_code(fd), 1002);
    close(fd);
    // Fragments adding up past the message cap: 1009.
    fd = tc_ws_open(g_port, "/ws", NULL);
    size_t part = CB_WS_MAX_MESSAGE / 2 + 16;
    char *big = calloc(1, part);
    frame(fd, false, CB_WS_OP_BIN, big, part);
    frame(fd, true, CB_WS_OP_CONT, big, part);
    T_EQ_INT(close_code(fd), 1009);
    free(big);
    close(fd);
    // A reader that owes more than CB_WS_MAX_QUEUED is dropped with 1008.
    fd = tc_ws_open(g_port, "/ws", NULL);
    int small = 4096;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &small, sizeof small);
    atomic_store(&g_flood_rc, 1);
    frame(fd, true, CB_WS_OP_TEXT, "flood", 5);
    for (int i = 0; i < 300 && atomic_load(&g_flood_rc) == 1; i++) usleep(10000);
    T_EQ_INT(atomic_load(&g_flood_rc), CB_E_FULL);
    close(fd);
    // A socket that sends nothing for ws_idle_ms: closed with 1001.
    fd = tc_ws_open(g_port, "/ws", NULL);
    int64_t t0 = cb_mono_ms();
    int code = close_code(fd);
    if (code == -1) code = close_code(fd);   // the sweep runs once a second
    T_EQ_INT(code, 1001);
    T_CHECK(cb_mono_ms() - t0 >= 2000);
    close(fd);
    cb_buf_free(&m);
}

static void drain_post(cb_loop *l, void *ud) { cb_server_drain(g_ctx.server, 2000); }

// A drain answers what is open, closes every WebSocket with 1001 and stops.
static void drain_closes_sockets(void) {
    int fd = tc_ws_open(g_port, "/ws", NULL);
    T_CHECK(fd >= 0);
    T_EQ_INT(cb_loop_post(g_ctx.loop, drain_post, NULL), CB_OK);
    T_EQ_INT(close_code(fd), 1001);
    close(fd);
    pthread_join(g_thread, NULL);
    cb_pool_free(g_ctx.req_pool);
    cb_server_free(g_ctx.server);
    cb_router_free(g_ctx.router);
    cb_loop_free(g_ctx.loop);
}

int main(void) {
    start();
    T_RUN(http_defences);
    T_RUN(one_connection_too_many);
    T_RUN(websocket_rules);
    T_RUN(drain_closes_sockets);
    return T_DONE();
}
