// unit_rt_local.c - realtime under `cbase serve` (realtime/rt_local.h), the
// MQTT hub, over a real loopback server: the upgrade route, packets handled on
// the request pool, publishes from another thread reaching only their topic's
// sockets, and every refusal closing.
// unit_stream.c drives the same server with the kernel's client library.
#include "t.h"
#include "cbase/provider.h"

#include "handler.h"
#include "lib_client.h"
#include "lib_rt_token.h"
#include "net/loop.h"
#include "net/router.h"
#include "net/server.h"
#include "realtime/realtime.h"
#include "realtime/rt_local.h"
#include "realtime/stream_mqtt.h"
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
    cb_server_opts o;
    cb_server_opts_default(&o);
    g_ctx.server = cb_server_new(&g_ctx, &o);
    T_EQ_INT(cb_server_listen(g_ctx.server, "127.0.0.1", 0, &g_port), CB_OK);
    pthread_create(&g_thread, NULL, run, NULL);
}

// ---- the hub (stream_mqtt.h) ------------------------------------------------

static int open_mqtt(void) { return tc_ws_open(g_port, "/mqtt", "Sec-WebSocket-Protocol: mqtt\r\n"); }

static void mq_send(int fd, const uint8_t *b, int n) {
    T_CHECK(n > 0);
    T_EQ_INT(tc_ws_send(fd, CB_WS_OP_BIN, b, (size_t)n), 0);
}

// The next packet the hub sent, read into *p (its spans into m); type 0 on a
// timeout, -1 when the socket closed (m then holds the close code).
static int mq_recv(int fd, int timeout_ms, cb_buf *m, MqttPacket *p) {
    memset(p, 0, sizeof *p);
    cb_buf_reset(m);
    int op = tc_ws_recv(fd, timeout_ms, m);
    if (op == CB_WS_OP_CLOSE) return -1;
    if (op != CB_WS_OP_BIN) return 0;
    T_EQ_INT(mqtt_parse(m->p, (int)m->len, 1 << 20, MQTT_FROM_SERVER, p), (int)m->len);
    return p->type;
}

static int close_code(const cb_buf *m) { return m->len == 2 ? (m->p[0] << 8) | m->p[1] : -1; }

// CONNECT as `uid` (token in the password), and its CONNACK.
static int mq_open_as(const char *uid, cb_buf *m) {
    int fd = open_mqtt();
    T_CHECK(fd >= 0);
    uint8_t b[512];
    char id[64], tok[64];
    snprintf(id, sizeof id, "%s-test", uid);
    snprintf(tok, sizeof tok, "tok-%s", uid);
    mq_send(fd, b, mqtt_write_connect(b, sizeof b, id, (int)strlen(id), 60, "jwt", 3, (const uint8_t *)tok, (int)strlen(tok)));
    MqttPacket p;
    T_EQ_INT(mq_recv(fd, 2000, m, &p), MQTT_CONNACK);
    T_EQ_INT(p.return_code, 0);
    return fd;
}

static void mq_subscribe(int fd, uint16_t id, const char *filter, cb_buf *m) {
    uint8_t b[512];
    MqttTopic t = { filter, (int)strlen(filter), 0 };
    mq_send(fd, b, mqtt_write_subscribe(b, sizeof b, id, &t, 1));
    MqttPacket p;
    T_EQ_INT(mq_recv(fd, 2000, m, &p), MQTT_SUBACK);
    T_EQ_INT(p.packet_id, id);
    T_EQ_INT(p.n_items, 1);
    if (p.n_items) T_EQ_INT(p.items[0], 0);
}

static void mqtt_upgrade_route(void) {
    // No subprotocol offered: refused, so a client that does not speak MQTT never gets a socket.
    int fd = tc_ws_open(g_port, "/mqtt", NULL);
    T_EQ_INT(fd, -1);
    fd = tc_ws_open(g_port, "/mqtt", "Sec-WebSocket-Protocol: wamp, mqtt\r\n");
    T_CHECK(fd >= 0);
    close(fd);
    tc_response r;
    T_EQ_INT(tc_http(g_port, "GET", "/mqtt", NULL, NULL, 0, &r), 0);
    T_EQ_INT(r.status, 426);   // not a WebSocket upgrade at all
    tc_response_free(&r);
    // The API Gateway stream's route and the facade's are not routed.
    T_EQ_INT(tc_http(g_port, "GET", "/v1/stream?proto=nw1", NULL, NULL, 0, &r), 0);
    T_EQ_INT(r.status, 404);
    tc_response_free(&r);
    T_EQ_INT(tc_http(g_port, "GET", "/realtime/v1/websocket?apikey=k&vsn=2.0.0", NULL, NULL, 0, &r), 0);
    T_EQ_INT(r.status, 404);
    tc_response_free(&r);
}

static void mqtt_publishes_reach_their_topics(void) {
    cb_buf m;
    cb_buf_init(&m);
    MqttPacket p;
    // a: U1, everything of its own; b: U2, the spectators of G; c: U1 again,
    // its views alone. One publish each.
    int a = mq_open_as(U1, &m);
    mq_subscribe(a, 1, "u/" U1 "/#", &m);
    int b = mq_open_as(U2, &m);
    mq_subscribe(b, 1, "s/" G, &m);
    int c = mq_open_as(U1, &m);
    mq_subscribe(c, 3, "u/" U1 "/v", &m);

    cb_view_row v;
    memset(&v, 0, sizeof v);
    strcpy(v.game_id, G);
    uint8_t env[] = { 0xee, 0xff };
    v.view = env;
    v.view_len = 2;
    v.version = 4;
    rt_publish_view(g_ctx.rt, U1, &v, false);
    T_EQ_INT(mq_recv(a, 2000, &m, &p), MQTT_PUBLISH);
    T_EQ_INT(p.qos, 0);
    T_EQ_MEM(p.topic, p.topic_len, "u/" U1 "/v", 2 + 36 + 2);
    NwFrame pf;
    T_EQ_INT(nw_frame_decode(p.payload, p.payload_len, &pf), NW_OK);   // the payload is the nw1 message
    T_CHECK(pf.kind == NW_WS_VIEW && pf.version == 4 && pf.blob.len == 2);
    T_EQ_INT(mq_recv(c, 2000, &m, &p), MQTT_PUBLISH);
    T_EQ_MEM(p.topic, p.topic_len, "u/" U1 "/v", 2 + 36 + 2);
    T_EQ_INT(mq_recv(b, 300, &m, &p), 0);   // U2 hears nothing of U1's

    const uint8_t cast[] = { 7, 8, 9 };
    rt_publish_cast(g_ctx.rt, U1, G, 5, cast, 3);
    T_EQ_INT(mq_recv(a, 2000, &m, &p), MQTT_PUBLISH);
    T_EQ_MEM(p.topic, p.topic_len, "u/" U1 "/c/" G, 2 + 36 + 3 + 6);
    rt_publish_cast(g_ctx.rt, NULL, G, 5, cast, 1);
    T_EQ_INT(mq_recv(b, 2000, &m, &p), MQTT_PUBLISH);
    T_EQ_MEM(p.topic, p.topic_len, "s/" G, 2 + 6);
    T_EQ_INT(mq_recv(a, 300, &m, &p), 0);   // a holds no s/ subscription
    T_EQ_INT(mq_recv(c, 300, &m, &p), 0);   // nor c any cast

    cb_chat_row ch;
    memset(&ch, 0, sizeof ch);
    strcpy(ch.id, "m1");
    strcpy(ch.game_id, G);
    strcpy(ch.user_id, U1);
    ch.message = "hi";
    cb_uuid members[1] = { U1 };
    rt_publish_chat(g_ctx.rt, G, &ch, members, 1);
    T_EQ_INT(mq_recv(a, 2000, &m, &p), MQTT_PUBLISH);
    T_EQ_MEM(p.topic, p.topic_len, "u/" U1 "/m/" G, 2 + 36 + 3 + 6);
    T_EQ_INT(nw_frame_decode(p.payload, p.payload_len, &pf), NW_OK);
    T_CHECK(pf.kind == NW_WS_CHAT && pf.chat.message_len == 2);
    T_EQ_INT(mq_recv(b, 300, &m, &p), 0);

    // PINGREQ, then the account deleted: both of U1's sockets end; U2's stays.
    uint8_t pk[8];
    mq_send(a, pk, mqtt_write_bare(pk, sizeof pk, MQTT_PINGREQ));
    T_EQ_INT(mq_recv(a, 2000, &m, &p), MQTT_PINGRESP);
    cb_rt_forget_user(g_ctx.rt, U1);
    T_EQ_INT(mq_recv(a, 2000, &m, &p), -1);
    T_EQ_INT(close_code(&m), 1008);
    T_EQ_INT(mq_recv(c, 2000, &m, &p), -1);
    T_EQ_INT(close_code(&m), 1008);
    mq_send(b, pk, mqtt_write_bare(pk, sizeof pk, MQTT_PINGREQ));
    T_EQ_INT(mq_recv(b, 2000, &m, &p), MQTT_PINGRESP);
    close(a);
    close(b);
    close(c);
    cb_buf_free(&m);
}

// THE AUTHZ MATRIX over a real socket: each refusal ends the connection with
// no CONNACK or SUBACK, as IoT Core does.
static void mqtt_refusals_end_the_connection(void) {
    cb_buf m;
    cb_buf_init(&m);
    MqttPacket p;
    uint8_t b[512];
    // A token that does not verify: closed, no CONNACK.
    int a = open_mqtt();
    mq_send(a, b, mqtt_write_connect(b, sizeof b, U1 "-x", 38, 60, "jwt", 3, (const uint8_t *)"forged", 6));
    T_EQ_INT(mq_recv(a, 2000, &m, &p), -1);
    T_EQ_INT(close_code(&m), 1008);
    close(a);
    // Another user's client id.
    a = open_mqtt();
    mq_send(a, b, mqtt_write_connect(b, sizeof b, U2 "-x", 38, 60, "jwt", 3, (const uint8_t *)"tok-" U1, 40));
    T_EQ_INT(mq_recv(a, 2000, &m, &p), -1);
    close(a);
    // The token in the URL instead of the password.
    a = tc_ws_open(g_port, "/mqtt?token=tok-" U1, "Sec-WebSocket-Protocol: mqtt\r\n");
    mq_send(a, b, mqtt_write_connect(b, sizeof b, U1 "-q", 38, 60, NULL, 0, NULL, 0));
    T_EQ_INT(mq_recv(a, 2000, &m, &p), MQTT_CONNACK);
    close(a);
    // Another user's topics, a wildcard over every user, everything.
    const char *forbidden[] = { "u/" U2 "/v", "u/+/v", "#" };
    for (int i = 0; i < 3; i++) {
        a = mq_open_as(U1, &m);
        MqttTopic t = { forbidden[i], (int)strlen(forbidden[i]), 0 };
        mq_send(a, b, mqtt_write_subscribe(b, sizeof b, 1, &t, 1));
        T_EQ_INT(mq_recv(a, 2000, &m, &p), -1);
        T_EQ_INT(close_code(&m), 1008);
        close(a);
    }
    // A client that publishes.
    a = mq_open_as(U1, &m);
    mq_send(a, b, mqtt_write_publish(b, sizeof b, "u/" U1 "/v", 40, (const uint8_t *)"x", 1));
    T_EQ_INT(mq_recv(a, 2000, &m, &p), -1);
    close(a);
    // A text frame (MQTT over WebSocket is binary), and bytes that are not MQTT.
    a = mq_open_as(U1, &m);
    T_EQ_INT(tc_ws_send(a, CB_WS_OP_TEXT, "wAA=", 4), 0);   // a PINGREQ, as the base64 text nw1 takes
    T_EQ_INT(mq_recv(a, 2000, &m, &p), -1);
    T_EQ_INT(close_code(&m), 1003);
    close(a);
    a = open_mqtt();
    T_EQ_INT(tc_ws_send(a, CB_WS_OP_BIN, "\x01\x05xx", 4), 0);
    T_EQ_INT(mq_recv(a, 2000, &m, &p), -1);
    close(a);
    // DISCONNECT closes normally.
    a = mq_open_as(U1, &m);
    mq_send(a, b, mqtt_write_bare(b, sizeof b, MQTT_DISCONNECT));
    T_EQ_INT(mq_recv(a, 2000, &m, &p), -1);
    T_EQ_INT(close_code(&m), 1000);
    close(a);
    // Sockets that close with their CONNECT and SUBSCRIBE still out on the
    // pool do not take the server down.
    for (int i = 0; i < 20; i++) {
        a = open_mqtt();
        int n = mqtt_write_connect(b, sizeof b, U1 "-z", 38, 60, "jwt", 3, (const uint8_t *)"tok-" U1, 40);
        MqttTopic t = { "u/" U1 "/v", 40, 0 };
        n += mqtt_write_subscribe(b + n, (int)sizeof b - n, 1, &t, 1);
        mq_send(a, b, n);
        close(a);
    }
    a = mq_open_as(U1, &m);
    mq_send(a, b, mqtt_write_bare(b, sizeof b, MQTT_PINGREQ));
    T_EQ_INT(mq_recv(a, 2000, &m, &p), MQTT_PINGRESP);
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
    T_RUN(mqtt_upgrade_route);
    T_RUN(mqtt_publishes_reach_their_topics);
    T_RUN(mqtt_refusals_end_the_connection);
    stop();
    return T_DONE();
}
