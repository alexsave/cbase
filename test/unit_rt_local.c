// unit_rt_local.c - realtime under `cbase serve` (realtime/rt_local.h), over
// a real loopback server: the upgrade route, subscriptions handled on the
// request pool, publishes from another thread reaching only their topic's
// sockets, a connection's frames answered in order, and garbage closing.
// unit_stream.c drives the same server with the kernel's client library.
#include "t.h"

#include "handler.h"
#include "lib_client.h"
#include "lib_rt_fakestore.h"
#include "net/loop.h"
#include "net/router.h"
#include "net/server.h"
#include "realtime/stream.h"
#include "realtime/realtime.h"
#include "realtime/rt_local.h"
#include "util/log.h"
#include "util/pool.h"

#include "netwire.h"

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

static void start(void) {
    cb_log_init(CB_LOG_ERROR, 0);
    rtf_install();
    rtf_member(G, U1);
    memset(&g_cfg, 0, sizeof g_cfg);
    strcpy(g_cfg.allowed_origins, "*");
    g_ctx.cfg = &g_cfg;
    g_ctx.mode = CB_MODE_SERVE;
    g_ctx.store = &rtf.store;
    g_ctx.loop = cb_loop_new();
    g_ctx.req_pool = cb_pool_new(3, "trt");
    g_ctx.router = cb_router_new();
    T_EQ_INT(cb_rt_local_new(&g_ctx, &g_ctx.rt), CB_OK);
    T_EQ_INT(cb_rt_local_routes(&g_ctx), CB_OK);
    cb_server_opts o;
    cb_server_opts_default(&o);
    g_ctx.server = cb_server_new(&g_ctx, &o);
    T_EQ_INT(cb_server_listen(g_ctx.server, "127.0.0.1", 0, &g_port), CB_OK);
    pthread_create(&g_thread, NULL, run, NULL);
}

static int open_ws(void) { return tc_ws_open(g_port, "/v1/stream?proto=nw1", NULL); }

static void send_frame(int fd, const NwFrame *f) {
    uint8_t raw[NW_ACCESS_MAX + 64];
    int n = nw_frame_encode(f, raw, sizeof raw);
    T_CHECK(n > 0);
    T_EQ_INT(tc_ws_send(fd, CB_WS_OP_BIN, raw, (size_t)n), 0);
}

static void send_auth(int fd, const char *uid) {
    NwFrame f;
    memset(&f, 0, sizeof f);
    f.kind = NW_WS_AUTH;
    f.token_len = (uint16_t)snprintf(f.token, sizeof f.token, "tok-%s", uid);
    send_frame(fd, &f);
}

static void send_sub(int fd, uint8_t kind, uint16_t ref, uint8_t what, const char *gid) {
    NwFrame f;
    memset(&f, 0, sizeof f);
    f.kind = kind;
    f.ref = ref;
    f.what = what;
    f.gid_len = (uint8_t)strlen(gid);
    memcpy(f.gid, gid, f.gid_len);
    send_frame(fd, &f);
}

static void send_ping(int fd) {
    NwFrame f;
    memset(&f, 0, sizeof f);
    f.kind = NW_WS_PING;
    send_frame(fd, &f);
}

// The next binary frame, decoded; kind 0 on a timeout.
static NwFrame recv_frame(int fd, int timeout_ms, cb_buf *m) {
    NwFrame f;
    memset(&f, 0, sizeof f);
    cb_buf_reset(m);
    if (tc_ws_recv(fd, timeout_ms, m) != CB_WS_OP_BIN) return f;
    T_EQ_INT(nw_frame_decode(m->p, (int)m->len, &f), NW_OK);
    return f;
}

static void the_upgrade_route(void) {
    tc_response r;
    T_EQ_INT(tc_http(g_port, "GET", "/v1/stream", NULL, NULL, 0, &r), 0);
    T_EQ_INT(r.status, 400);   // no proto
    tc_response_free(&r);
    T_EQ_INT(tc_http(g_port, "GET", "/v1/stream?proto=nw2", NULL, NULL, 0, &r), 0);
    T_EQ_INT(r.status, 400);
    tc_response_free(&r);
    // The facade's old URL is not routed.
    T_EQ_INT(tc_http(g_port, "GET", "/realtime/v1/websocket?apikey=k&vsn=2.0.0", NULL, NULL, 0, &r), 0);
    T_EQ_INT(r.status, 404);
    tc_response_free(&r);
    int fd = open_ws();
    T_CHECK(fd >= 0);
    close(fd);
}

static void subscribe_publish_and_isolation(void) {
    cb_buf m;
    cb_buf_init(&m);
    int a = open_ws(), b = open_ws(), c = open_ws();
    T_CHECK(a >= 0 && b >= 0 && c >= 0);
    // a: a seated player's game; b: a spectator of it; c: another game.
    send_auth(a, U1);
    send_sub(a, NW_WS_SUB, 1, NW_SUB_GAME, G);
    NwFrame f = recv_frame(a, 2000, &m);
    T_CHECK(f.kind == NW_WS_ACK && f.ref == 1 && f.why == 0 && f.seated == 1);
    send_auth(b, U2);
    send_sub(b, NW_WS_SUB, 1, NW_SUB_GAME, G);
    f = recv_frame(b, 2000, &m);
    T_CHECK(f.kind == NW_WS_ACK && f.why == 0 && f.seated == 0);
    send_auth(c, U2);
    send_sub(c, NW_WS_SUB, 1, NW_SUB_GAME, "zzzzzz");
    T_EQ_INT(recv_frame(c, 2000, &m).kind, NW_WS_ACK);
    // U2 may not hear U1's chat.
    send_sub(c, NW_WS_SUB, 2, NW_SUB_CHAT, G);
    f = recv_frame(c, 2000, &m);
    T_CHECK(f.kind == NW_WS_ACK && f.ref == 2 && f.why != 0);

    // Publish from this thread (as a bot drive would): each hears its own topic.
    const uint8_t cast[] = { 1, 2 };
    rt_publish_cast(g_ctx.rt, "gu-" G "-" U1, G, 7, cast, 2);
    rt_publish_cast(g_ctx.rt, "game-" G, G, 7, cast, 1);
    f = recv_frame(a, 2000, &m);
    T_CHECK(f.kind == NW_WS_CAST && f.version == 7 && f.blob.len == 2);
    f = recv_frame(b, 2000, &m);
    T_CHECK(f.kind == NW_WS_CAST && f.version == 7 && f.blob.len == 1);
    T_EQ_INT(recv_frame(c, 300, &m).kind, 0);   // nothing for another game
    T_EQ_INT(recv_frame(a, 300, &m).kind, 0);   // and a's stream got only its own

    // Frames sent back to back are answered in order (one job per connection at a time).
    send_sub(a, NW_WS_SUB, 10, NW_SUB_CHAT, G);
    send_ping(a);
    send_sub(a, NW_WS_SUB, 11, NW_SUB_GAMES, "");
    send_sub(a, NW_WS_UNSUB, 0, NW_SUB_GAME, G);
    send_ping(a);
    f = recv_frame(a, 2000, &m);
    T_CHECK(f.kind == NW_WS_ACK && f.ref == 10);
    T_EQ_INT(recv_frame(a, 2000, &m).kind, NW_WS_PONG);
    f = recv_frame(a, 2000, &m);
    T_CHECK(f.kind == NW_WS_ACK && f.ref == 11);
    T_EQ_INT(recv_frame(a, 2000, &m).kind, NW_WS_PONG);   // UNSUB has no reply
    // After the UNSUB, a hears no casts, but still its chat.
    rt_publish_cast(g_ctx.rt, "gu-" G "-" U1, G, 8, cast, 2);
    cb_chat_row ch;
    memset(&ch, 0, sizeof ch);
    strcpy(ch.id, "m1");
    strcpy(ch.game_id, G);
    strcpy(ch.user_id, U2);
    ch.message = "hello";
    ch.created_at = 5;
    rt_publish_chat(g_ctx.rt, G, &ch);
    f = recv_frame(a, 2000, &m);
    T_CHECK(f.kind == NW_WS_CHAT && f.chat.message_len == 5 && memcmp(f.chat.message, "hello", 5) == 0);
    T_EQ_INT(recv_frame(b, 300, &m).kind, 0);   // a spectator did not subscribe to the chat
    // A spectator's UNSUB quiets the spectators' stream too.
    send_sub(b, NW_WS_UNSUB, 0, NW_SUB_GAME, G);
    send_ping(b);
    T_EQ_INT(recv_frame(b, 2000, &m).kind, NW_WS_PONG);
    rt_publish_cast(g_ctx.rt, "game-" G, G, 9, cast, 1);
    T_EQ_INT(recv_frame(b, 300, &m).kind, 0);
    close(a);
    close(b);
    close(c);
    cb_buf_free(&m);
}

static void garbage_closes_the_socket(void) {
    cb_buf m;
    cb_buf_init(&m);
    int a = open_ws();
    T_EQ_INT(tc_ws_send(a, CB_WS_OP_BIN, "this is not a frame", 19), 0);
    cb_buf_reset(&m);
    T_EQ_INT(tc_ws_recv(a, 2000, &m), CB_WS_OP_CLOSE);
    T_EQ_INT(m.len, 2);
    T_EQ_INT((m.p[0] << 8) | m.p[1], 1003);
    close(a);
    // A socket that closes with a subscription still out on the pool does not take the server down.
    for (int i = 0; i < 20; i++) {
        int b = open_ws();
        send_auth(b, U1);
        send_sub(b, NW_WS_SUB, 1, NW_SUB_CHAT, G);
        close(b);
    }
    int c = open_ws();
    send_ping(c);
    T_EQ_INT(recv_frame(c, 2000, &m).kind, NW_WS_PONG);
    close(c);
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
    rtf_reset();
}

int main(void) {
    start();
    T_RUN(the_upgrade_route);
    T_RUN(subscribe_publish_and_isolation);
    T_RUN(garbage_closes_the_socket);
    stop();
    return T_DONE();
}
