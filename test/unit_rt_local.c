// unit_rt_local.c - realtime under `cbase serve` (realtime/rt_local.h), over
// a real loopback server: the upgrade route, joins handled on the request
// pool, publishes from another thread, both serializers on one server, and a
// chat insert over HTTP reaching a WebSocket subscriber.
#include "t.h"

#include "handler.h"
#include "lib_client.h"
#include "lib_rt_fakestore.h"
#include "net/loop.h"
#include "net/router.h"
#include "net/server.h"
#include "realtime/postgrest.h"
#include "realtime/realtime.h"
#include "realtime/rt_local.h"
#include "util/log.h"
#include "util/pool.h"

#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
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
    strcpy(g_cfg.anon_key, "anon-key");
    strcpy(g_cfg.allowed_origins, "*");
    g_ctx.cfg = &g_cfg;
    g_ctx.mode = CB_MODE_SERVE;
    g_ctx.store = &rtf.store;
    g_ctx.loop = cb_loop_new();
    g_ctx.req_pool = cb_pool_new(3, "trt");
    g_ctx.router = cb_router_new();
    T_EQ_INT(cb_rt_local_new(&g_ctx, &g_ctx.rt), CB_OK);
    T_EQ_INT(cb_rt_local_routes(&g_ctx), CB_OK);
    T_EQ_INT(cb_postgrest_routes(&g_ctx), CB_OK);
    cb_server_opts o;
    cb_server_opts_default(&o);
    g_ctx.server = cb_server_new(&g_ctx, &o);
    T_EQ_INT(cb_server_listen(g_ctx.server, "127.0.0.1", 0, &g_port), CB_OK);
    pthread_create(&g_thread, NULL, run, NULL);
}

static int open_ws(const char *vsn) {
    char path[128];
    snprintf(path, sizeof path, "/realtime/v1/websocket?apikey=anon-key&vsn=%s", vsn);
    return tc_ws_open(g_port, path, NULL);
}

static void send_text(int fd, const char *s) { T_EQ_INT(tc_ws_send(fd, CB_WS_OP_TEXT, s, strlen(s)), 0); }

// The next text frame, or "" on a timeout.
static const char *recv_text(int fd, int timeout_ms, cb_buf *m) {
    cb_buf_reset(m);
    int op = tc_ws_recv(fd, timeout_ms, m);
    if (op != CB_WS_OP_TEXT) { cb_buf_reset(m); return ""; }
    return cb_buf_cstr(m);
}

static void the_upgrade_route(void) {
    tc_response r;
    T_EQ_INT(tc_http(g_port, "GET", "/realtime/v1/websocket?vsn=2.0.0", NULL, NULL, 0, &r), 0);
    T_EQ_INT(r.status, 401);   // no apikey
    tc_response_free(&r);
    T_EQ_INT(tc_http(g_port, "GET", "/realtime/v1/websocket?apikey=anon-key&vsn=3.0.0", NULL, NULL, 0, &r), 0);
    T_EQ_INT(r.status, 400);
    tc_response_free(&r);
    // A client that offers subprotocols is given "phoenix" back, or a browser fails the handshake.
    int fd = tc_connect(g_port);
    const char *req = "GET /realtime/v1/websocket?apikey=anon-key&vsn=2.0.0 HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\n"
                      "Connection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                      "Sec-WebSocket-Protocol: phoenix, base64url.bearer.phx.abc\r\n\r\n";
    T_CHECK(write(fd, req, strlen(req)) == (ssize_t)strlen(req));
    char head[1024] = { 0 };
    size_t n = 0;
    while (n < sizeof head - 1 && !strstr(head, "\r\n\r\n")) {
        struct pollfd p = { fd, POLLIN, 0 };
        if (poll(&p, 1, 2000) <= 0 || recv(fd, head + n, 1, 0) != 1) break;
        n++;
    }
    T_CHECK(strstr(head, "HTTP/1.1 101") != NULL);
    T_CHECK(strstr(head, "Sec-WebSocket-Protocol: phoenix\r\n") != NULL);
    close(fd);
    fd = open_ws("2.0.0");
    T_CHECK(fd >= 0);
    close(fd);
}

static void join_publish_and_isolation(void) {
    cb_buf m;
    cb_buf_init(&m);
    int a = open_ws("2.0.0"), b = open_ws("1.0.0"), c = open_ws("2.0.0");
    T_CHECK(a >= 0 && b >= 0 && c >= 0);
    // a: a seated player's stream; b (vsn 1): the spectator stream; c: another game.
    send_text(a, "[\"1\",\"1\",\"realtime:gu-" G "-" U1 "\",\"phx_join\",{\"config\":{\"private\":true},\"access_token\":\"tok-" U1 "\"}]");
    T_EQ_STR(recv_text(a, 2000, &m), "[\"1\",\"1\",\"realtime:gu-" G "-" U1 "\",\"phx_reply\",{\"response\":{},\"status\":\"ok\"}]");
    send_text(b, "{\"join_ref\":\"1\",\"ref\":\"1\",\"topic\":\"realtime:game-" G "\",\"event\":\"phx_join\",\"payload\":{\"access_token\":\"tok-" U2 "\"}}");
    T_EQ_STR(recv_text(b, 2000, &m), "{\"join_ref\":\"1\",\"ref\":\"1\",\"topic\":\"realtime:game-" G "\",\"event\":\"phx_reply\","
                                     "\"payload\":{\"response\":{},\"status\":\"ok\"}}");
    send_text(c, "[\"1\",\"1\",\"realtime:game-zzzzzz\",\"phx_join\",{\"access_token\":\"tok-" U2 "\"}]");
    T_CHECK(strstr(recv_text(c, 2000, &m), "\"status\":\"ok\"") != NULL);
    // U2 may not hear U1's stream.
    send_text(c, "[\"2\",\"2\",\"realtime:gu-" G "-" U1 "\",\"phx_join\",{\"access_token\":\"tok-" U2 "\"}]");
    T_CHECK(strstr(recv_text(c, 2000, &m), "\"status\":\"error\"") != NULL);

    // Publish from this thread (as a bot drive would): each hears its own topic, in its own serializer.
    const uint8_t cast[] = { 1, 2 };
    rt_publish_cast(g_ctx.rt, "gu-" G "-" U1, cast, 2);
    rt_publish_cast(g_ctx.rt, "game-" G, cast, 1);
    T_EQ_STR(recv_text(a, 2000, &m), "[null,null,\"realtime:gu-" G "-" U1 "\",\"broadcast\",{\"event\":\"animation_events\",\"payload\":{\"b\":\"AQI=\"},\"type\":\"broadcast\"}]");
    T_EQ_STR(recv_text(b, 2000, &m), "{\"join_ref\":null,\"ref\":null,\"topic\":\"realtime:game-" G "\",\"event\":\"broadcast\","
                                     "\"payload\":{\"event\":\"animation_events\",\"payload\":{\"b\":\"AQ==\"},\"type\":\"broadcast\"}}");
    T_EQ_STR(recv_text(c, 300, &m), "");   // nothing for another game
    T_EQ_STR(recv_text(a, 300, &m), "");   // and a's stream got only its own

    // Frames sent back to back are answered in order (one job per connection at a time).
    send_text(a, "[null,\"10\",\"phoenix\",\"heartbeat\",{}]");
    send_text(a, "[null,\"11\",\"phoenix\",\"heartbeat\",{}]");
    send_text(a, "[\"1\",\"12\",\"realtime:gu-" G "-" U1 "\",\"phx_leave\",{}]");
    send_text(a, "[null,\"13\",\"phoenix\",\"heartbeat\",{}]");
    T_CHECK(strstr(recv_text(a, 2000, &m), "\"10\"") != NULL);
    T_CHECK(strstr(recv_text(a, 2000, &m), "\"11\"") != NULL);
    T_CHECK(strstr(recv_text(a, 2000, &m), "\"12\",\"realtime:gu-") != NULL);
    T_CHECK(strstr(recv_text(a, 2000, &m), "\"phx_close\"") != NULL);
    T_CHECK(strstr(recv_text(a, 2000, &m), "\"13\"") != NULL);
    // After the leave, a's stream is quiet.
    rt_publish_cast(g_ctx.rt, "gu-" G "-" U1, cast, 2);
    T_EQ_STR(recv_text(a, 300, &m), "");
    close(a);
    close(b);
    close(c);
    cb_buf_free(&m);
}

static void chat_over_http_reaches_the_socket(void) {
    cb_buf m;
    cb_buf_init(&m);
    int a = open_ws("2.0.0");
    send_text(a, "[\"1\",\"1\",\"realtime:chat:" G "\",\"phx_join\",{\"config\":{\"private\":true},\"access_token\":\"tok-" U1 "\"}]");
    T_CHECK(strstr(recv_text(a, 2000, &m), "\"status\":\"ok\"") != NULL);
    const char *body = "{\"game_id\":\"" G "\",\"user_id\":\"" U1 "\",\"message\":\"hello\",\"is_system\":false}";
    tc_response r;
    T_EQ_INT(tc_http(g_port, "POST", "/rest/v1/chat_messages", "apikey: anon-key\r\nAuthorization: Bearer tok-" U1 "\r\nContent-Type: application/json\r\n",
                     body, strlen(body), &r), 0);
    T_EQ_INT(r.status, 201);
    tc_response_free(&r);
    const char *f = recv_text(a, 2000, &m);
    T_CHECK(cb_starts_with(f, "[null,null,\"realtime:chat:" G "\",\"broadcast\",{\"event\":\"INSERT\",\"payload\":{\"old_record\":null,\"operation\":\"INSERT\""));
    T_CHECK(strstr(f, "\"message\":\"hello\"") != NULL);
    close(a);
    cb_buf_free(&m);
}

static void garbage_closes_the_socket(void) {
    cb_buf m;
    cb_buf_init(&m);
    int a = open_ws("2.0.0");
    send_text(a, "this is not phoenix");
    T_EQ_INT(tc_ws_recv(a, 2000, &m), CB_WS_OP_CLOSE);
    T_EQ_INT(m.len, 2);
    T_EQ_INT((m.p[0] << 8) | m.p[1], 1003);
    close(a);
    // A socket that closes with a join still out on the pool does not take the server down.
    for (int i = 0; i < 20; i++) {
        int b = open_ws("2.0.0");
        send_text(b, "[\"1\",\"1\",\"realtime:chat:" G "\",\"phx_join\",{\"access_token\":\"tok-" U1 "\"}]");
        close(b);
    }
    int c = open_ws("2.0.0");
    send_text(c, "[null,\"1\",\"phoenix\",\"heartbeat\",{}]");
    T_CHECK(strstr(recv_text(c, 2000, &m), "phx_reply") != NULL);
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
    T_RUN(join_publish_and_isolation);
    T_RUN(chat_over_http_reaches_the_socket);
    T_RUN(garbage_closes_the_socket);
    stop();
    return T_DONE();
}
