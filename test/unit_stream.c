// unit_stream.c - the push stream: the messages and topics a publish builds
// (realtime.h), and a real loopback `cbase serve` socket speaking MQTT, driven
// end to end by the kernel's client library (netclient.h). IoT Core's
// transport is unit_rt_iot.c's, the hub's packets unit_stream_mqtt.c's.
#include "t.h"
#include "cbase/provider.h"

#include "lib_client.h"
#include "lib_rt_token.h"
#include "net/loop.h"
#include "net/router.h"
#include "net/server.h"
#include "net/ws.h"
#include "realtime/realtime.h"
#include "realtime/rt_local.h"
#include "util/clock.h"
#include "util/log.h"
#include "util/pool.h"

#include "ask.h"
#include "netclient.h"
#include "netwire.h"

#include <pthread.h>
#include <unistd.h>

#define U1 "11111111-1111-4111-8111-111111111111"
#define U2 "22222222-2222-4222-8222-222222222222"
#define G  "ab12cd"
#define G2 "ef34gh"

static cb_ctx g_ctx;
static cb_config g_cfg;

static void setup_world(void) {
    cb_log_init(CB_LOG_ERROR, 0);
    rtt_install();
    memset(&g_cfg, 0, sizeof g_cfg);
    strcpy(g_cfg.allowed_origins, "*");
    g_ctx.cfg = &g_cfg;
}

// ---- what a publish builds ---------------------------------------------------------------

// Each target a publish hands the transport: its topic, and the last message.
static struct { int n; char topic[8][128]; uint8_t nw[512]; size_t nw_len; } g_cap;
static int cap_deliver(cb_rt *rt, const cb_rt_to *to, const uint8_t *frame, size_t len) {
    if (g_cap.n < 8) cb_rt_topic(to, g_cap.topic[g_cap.n], sizeof g_cap.topic[0]);
    g_cap.n++;
    g_cap.nw_len = len <= sizeof g_cap.nw ? len : 0;
    memcpy(g_cap.nw, frame, g_cap.nw_len);
    return 0;
}
static const cb_rt_transport CAP_T = { .name = "cap", .deliver = cap_deliver };

static void publishes(void) {
    cb_rt rt = { &CAP_T, &g_ctx, NULL };
    memset(&g_cap, 0, sizeof g_cap);
    const uint8_t cast[] = { 1, 2 };
    rt_publish_cast(&rt, U1, G, 7, cast, 2);
    const uint8_t want_cast[] = { 1, NW_WS_CAST, 6, 'a', 'b', '1', '2', 'c', 'd', 7, 0, 0, 0, 2, 0, 0, 0, 1, 2 };
    T_EQ_MEM(g_cap.nw, g_cap.nw_len, want_cast, sizeof want_cast);
    T_EQ_STR(g_cap.topic[0], "u/" U1 "/c/" G);
    rt_publish_cast(&rt, NULL, G, 7, cast, 1);
    T_EQ_STR(g_cap.topic[1], "s/" G);

    cb_view_row r;
    memset(&r, 0, sizeof r);
    strcpy(r.game_id, G);
    strcpy(r.player_id, U1);
    uint8_t env[] = { 0xee };
    r.view = env;
    r.view_len = 1;
    r.version = 9;
    strcpy(r.status, "playing");
    rt_publish_view(&rt, U1, &r, false);
    T_EQ_STR(g_cap.topic[2], "u/" U1 "/v");
    const uint8_t want_view[] = { 1, NW_WS_VIEW, 6, 'a', 'b', '1', '2', 'c', 'd', 9, 0, 0, 0, 1, 0, 0, 0, 0xee };
    T_EQ_MEM(g_cap.nw, g_cap.nw_len, want_view, sizeof want_view);
    // A seat given up: the view with no envelope.
    rt_publish_view(&rt, U1, &r, true);
    const uint8_t want_gone[] = { 1, NW_WS_VIEW, 6, 'a', 'b', '1', '2', 'c', 'd', 0, 0, 0, 0, 0, 0, 0, 0 };
    T_EQ_MEM(g_cap.nw, g_cap.nw_len, want_gone, sizeof want_gone);

    cb_chat_row c;
    memset(&c, 0, sizeof c);
    strcpy(c.id, "m");
    strcpy(c.game_id, G);
    strcpy(c.user_id, "u");
    c.message = "hi";
    c.created_at = 6;
    // Two members: each member's own topic.
    cb_uuid members[2] = { U1, U2 };
    rt_publish_chat(&rt, G, &c, members, 2);
    const uint8_t want_chat[] = { 1, NW_WS_CHAT, 6, 'a', 'b', '1', '2', 'c', 'd', 1, 'm', 1, 'u', 6, 0, 0, 0, 0, 0, 0, 2, 0, 'h', 'i' };
    T_EQ_MEM(g_cap.nw, g_cap.nw_len, want_chat, sizeof want_chat);
    T_EQ_INT(g_cap.n, 6);
    T_EQ_STR(g_cap.topic[4], "u/" U1 "/m/" G);
    T_EQ_STR(g_cap.topic[5], "u/" U2 "/m/" G);
    // No members: nobody hears it.
    rt_publish_chat(&rt, G, &c, NULL, 0);
    T_EQ_INT(g_cap.n, 6);
    // An empty slot in the members (a row that named nobody) is skipped, not sent to "u//m/<game>".
    cb_uuid gap[3] = { U1, "", U2 };
    rt_publish_chat(&rt, G, &c, gap, 3);
    T_EQ_INT(g_cap.n, 8);
    T_EQ_STR(g_cap.topic[7], "u/" U2 "/m/" G);
}

// A target's topics, and the ids a topic level refuses: nothing an id holds
// can add a level or a wildcard to an MQTT topic.
static void targets_name_topics(void) {
    char t[128];
    cb_rt_to views = { CB_RT_VIEWS, U1, NULL };
    T_EQ_INT(cb_rt_topic(&views, t, sizeof t), 2 + 36 + 2);
    T_EQ_STR(t, "u/" U1 "/v");
    cb_rt_to chat_all = { CB_RT_CHAT, NULL, G };
    T_EQ_INT(cb_rt_topic(&chat_all, t, sizeof t), -1);
    const char *bad[] = { "", "a/b", "a+", "#", "a b", "a.b", "a\n",
                          "x0123456789012345678901234567890123456789012345678901234567890123" };   // 65 bytes
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        cb_rt_to a = { CB_RT_SEAT, bad[i], G }, b = { CB_RT_SEAT, U1, bad[i] }, w = { CB_RT_WATCH, NULL, bad[i] };
        T_EQ_INT(cb_rt_topic(&a, t, sizeof t), -1);
        T_EQ_INT(cb_rt_topic(&b, t, sizeof t), -1);
        T_EQ_INT(cb_rt_topic(&w, t, sizeof t), -1);
    }
    cb_rt_to none = { CB_RT_SEAT, NULL, G };
    T_EQ_INT(cb_rt_topic(&none, t, sizeof t), -1);
    cb_rt_to odd = { (cb_rt_family)99, U1, G };
    T_EQ_INT(cb_rt_topic(&odd, t, sizeof t), -1);
    // Too small a buffer is a refusal, not a cut topic.
    cb_rt_to seat = { CB_RT_SEAT, U1, G };
    T_EQ_INT(cb_rt_topic(&seat, t, 10), -1);
    T_EQ_INT(cb_rt_topic(&seat, t, 2 + 36 + 3 + 6 + 1), 2 + 36 + 3 + 6);
}

// ---- cbase serve: a real socket, driven by the client library -------------------------------

static int g_port;
static pthread_t g_thread;

static void *loop_run(void *arg) { cb_loop_run(g_ctx.loop); return NULL; }
static void drain_post(cb_loop *l, void *ud) { cb_server_drain(g_ctx.server, 2000); }

// Sends every message the client has due, as binary (MQTT packets).
static void flush(int fd, int64_t now) {
    for (const NcOut *o; (o = nc_stream_next(now));) T_EQ_INT(tc_ws_send(fd, CB_WS_OP_BIN, o->bytes, o->len), 0);
}

// The next event the server's frames make, skipping NC_EV_NONE and the
// session's; kind 255 on a timeout. g_ev is its index, for nc_event_payload.
static int g_ev;
// The next event that is not NONE or the session's: first from what is left
// of the last message (a batch's replies are one message), then from new ones.
static int g_ev_n;
static const NcEvent *next_event(int fd, int timeout_ms, cb_buf *m) {
    static NcEvent none = { .kind = 255 };
    for (;;) {
        for (g_ev++; g_ev < g_ev_n; g_ev++) {
            const NcEvent *e = nc_event(g_ev);
            if (!e) break;   // the client has fed something else since
            if (e->kind != NC_EV_NONE && e->kind != NC_EV_SESSION) return e;
        }
        cb_buf_reset(m);
        int op = tc_ws_recv(fd, timeout_ms, m);
        if (op != CB_WS_OP_BIN && op != CB_WS_OP_TEXT) { g_ev_n = 0; return &none; }
        g_ev_n = nc_stream_in(m->p, (int)m->len, op == CB_WS_OP_TEXT, cb_now_ms());
        g_ev = -1;
    }
}

static void session_for(const char *uid) {
    // A session the fake token check accepts: "tok-<uid>".
    NwSession s;
    memset(&s, 0, sizeof s);
    s.access_len = (uint16_t)strlen("tok-") + 36;
    snprintf(s.access, sizeof s.access, "tok-%s", uid);
    s.refresh_len = 1;
    s.refresh[0] = 'r';
    s.expires_at_ms = (uint64_t)cb_now_ms() + 3600 * 1000;
    s.user_id_len = 36;
    memcpy(s.user_id, uid, 36);
    uint8_t blob[NC_SESSION_BLOB_MAX];
    int n = nw_session_encode(&s, blob, sizeof blob);
    T_EQ_INT(nc_session_load(blob, n), NW_OK);
}

static void serve_end_to_end(void) {
    setup_world();
    g_ctx.prov = &cb_provider_local;
    g_ctx.loop = cb_loop_new();
    g_ctx.req_pool = cb_pool_new(2, "tst");
    g_ctx.router = cb_router_new();
    T_EQ_INT(cb_rt_local_new(&g_ctx, &g_ctx.rt), CB_OK);
    T_EQ_INT(cb_rt_local_routes(&g_ctx), CB_OK);
    cb_server_opts o;
    cb_server_opts_default(&o);
    g_ctx.server = cb_server_new(&g_ctx, &o);
    T_EQ_INT(cb_server_listen(g_ctx.server, "127.0.0.1", 0, &g_port), CB_OK);
    pthread_create(&g_thread, NULL, loop_run, NULL);

    // The old stream's route is gone.
    tc_response r;
    T_EQ_INT(tc_http(g_port, "GET", "/v1/stream?proto=nw1", NULL, NULL, 0, &r), 0);
    T_EQ_INT(r.status, 404);
    tc_response_free(&r);

    // The kernel's client over MQTT to the hub, as against IoT Core.
    nc_reset();
    session_for(U1);
    T_EQ_INT(nc_stream_sub(NW_SUB_GAMES, NULL, 0), NW_OK);
    T_EQ_INT(nc_stream_sub(NW_SUB_GAME, G, 6), NW_OK);
    T_EQ_INT(nc_stream_sub(NW_SUB_CHAT, G2, 6), NW_OK);
    const NcRequest *q = nc_req_stream();
    char path[NC_PATH_MAX + 1];
    memcpy(path, q->path, q->path_len);
    path[q->path_len] = 0;
    T_EQ_STR(path, "/mqtt");
    int fd = tc_ws_open(g_port, path, "Sec-WebSocket-Protocol: mqtt\r\n");
    T_CHECK(fd >= 0);
    int64_t t0 = cb_now_ms();
    nc_stream_opened(t0);
    flush(fd, t0);   // the CONNECT
    cb_buf m;
    cb_buf_init(&m);
    cb_buf_reset(&m);
    T_EQ_INT(tc_ws_recv(fd, 2000, &m), CB_WS_OP_BIN);   // the CONNACK
    nc_stream_in(m.p, (int)m.len, 0, t0);
    flush(fd, t0);   // the three SUBSCRIBEs, in one message
    const NcEvent *e = next_event(fd, 2000, &m);
    T_CHECK(e->kind == NC_EV_SEATED && e->gid_len == 6);
    T_CHECK(nc_stream_next(t0 + 100) == NULL);   // all answered or waiting: nothing to send

    // Pushes: a cast and a view, each once; the same version again is stale.
    const uint8_t cast[] = { 0xca, 0x57 };
    rt_publish_cast(g_ctx.rt, U1, G, 11, cast, 2);
    e = next_event(fd, 2000, &m);
    T_CHECK(e->kind == NC_EV_CAST && e->version == 11 && e->payload_len == 2);
    T_EQ_MEM(nc_event_payload(g_ev), e->payload_len, cast, 2);
    rt_publish_cast(g_ctx.rt, U1, G, 11, cast, 2);
    cb_view_row v;
    memset(&v, 0, sizeof v);
    strcpy(v.game_id, G);
    strcpy(v.player_id, U1);
    uint8_t env[] = { 0xe0, 0x01 };
    v.view = env;
    v.view_len = 2;
    v.version = 11;
    strcpy(v.status, "playing");
    // A game held on its own stream is given no dashboard views (its casts
    // animate it): let it go, and the view of it is handed out.
    T_EQ_INT(nc_stream_unsub(NW_SUB_GAME, G, 6), NW_OK);
    flush(fd, t0);   // the UNSUBSCRIBE
    rt_publish_view(g_ctx.rt, U1, &v, false);
    e = next_event(fd, 2000, &m);   // the stale cast made no event: this is the view
    T_CHECK(e->kind == NC_EV_VIEW && e->version == 11 && e->what == NW_SUB_GAMES);
    T_EQ_MEM(nc_event_payload(g_ev), e->payload_len, env, 2);
    // The chat of a game, published to its member's own topic.
    cb_chat_row ch;
    memset(&ch, 0, sizeof ch);
    strcpy(ch.id, "m1");
    strcpy(ch.game_id, G2);
    strcpy(ch.user_id, U1);
    ch.message = "hey";
    cb_uuid members[1] = { U1 };
    rt_publish_chat(g_ctx.rt, G2, &ch, members, 1);
    e = next_event(fd, 2000, &m);
    T_CHECK(e->kind == NC_EV_CHAT && e->chat.message_len == 3 && e->gid_len == 6);
    // The keep-alive.
    flush(fd, cb_now_ms() + NC_PING_MS);
    // The UNSUBACK runs on the pool while pushes go straight to the loop, so it
    // may arrive only now: skip it.
    int op;
    do {
        cb_buf_reset(&m);
        op = tc_ws_recv(fd, 2000, &m);
    } while (op == CB_WS_OP_BIN && m.len >= 1 && m.p[0] == 0xB0);
    T_EQ_INT(op, CB_WS_OP_BIN);
    T_EQ_MEM(m.p, m.len, ((const uint8_t[]){ 0xD0, 0 }), 2);   // PINGRESP
    // A text frame closes the socket.
    T_EQ_INT(tc_ws_send(fd, CB_WS_OP_TEXT, "!!!!", 4), 0);
    cb_buf_reset(&m);
    T_EQ_INT(tc_ws_recv(fd, 2000, &m), CB_WS_OP_CLOSE);
    close(fd);
    cb_buf_free(&m);

    T_EQ_INT(cb_loop_post(g_ctx.loop, drain_post, NULL), CB_OK);
    pthread_join(g_thread, NULL);
    cb_pool_free(g_ctx.req_pool);
    cb_server_free(g_ctx.server);
    cb_rt_free(g_ctx.rt);
    cb_router_free(g_ctx.router);
    cb_loop_free(g_ctx.loop);
    memset(&g_ctx, 0, sizeof g_ctx);
}

int main(void) {
    T_RUN(publishes);
    T_RUN(targets_name_topics);
    T_RUN(serve_end_to_end);
    return T_DONE();
}
