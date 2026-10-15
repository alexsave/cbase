// unit_stream.c - the binary push stream (realtime/stream.h): the protocol
// against the fake world, the frames a publish builds, both transports (API
// Gateway's base64 posts, and a real loopback `cbase serve` socket carrying
// binary frames), and the kernel's client library (netclient.h) driving the
// loopback socket end to end.
#include "t.h"

#include "lib_client.h"
#include "lib_rt_fakestore.h"
#include "net/loop.h"
#include "net/router.h"
#include "net/server.h"
#include "net/ws.h"
#include "realtime/realtime.h"
#include "realtime/rt_apigw.h"
#include "realtime/rt_local.h"
#include "realtime/stream.h"
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

// ---- the protocol ------------------------------------------------------------------------

static struct { int n; uint8_t f[8][256]; size_t len[8]; } g_out;
static void collect(void *ud, const char *frame, size_t len) {
    if (g_out.n < 8 && len <= sizeof g_out.f[0]) {
        memcpy(g_out.f[g_out.n], frame, len);
        g_out.len[g_out.n] = len;
        g_out.n++;
    }
}

static int run(cb_conn_row *c, const NwFrame *f) {
    uint8_t b[NW_ACCESS_MAX + 64];
    int n = nw_frame_encode(f, b, sizeof b);
    T_CHECK(n > 0);
    memset(&g_out, 0, sizeof g_out);
    return cb_nw_on_frame(&g_ctx, c, b, (size_t)n, collect, NULL);
}

static void auth(cb_conn_row *c, const char *tok) {
    NwFrame f;
    memset(&f, 0, sizeof f);
    f.kind = NW_WS_AUTH;
    f.token_len = (uint16_t)strlen(tok);
    memcpy(f.token, tok, f.token_len);
    T_EQ_INT(run(c, &f), CB_OK);
}

static void sub(cb_conn_row *c, uint16_t ref, int what, const char *gid, uint32_t since) {
    NwFrame f;
    memset(&f, 0, sizeof f);
    f.kind = NW_WS_SUB;
    f.ref = ref;
    f.what = (uint8_t)what;
    f.gid_len = (uint8_t)strlen(gid);
    memcpy(f.gid, gid, f.gid_len);
    f.version = since;
    T_EQ_INT(run(c, &f), CB_OK);
}

// Output frame i as a decoded frame.
static NwFrame out(int i) {
    NwFrame f;
    memset(&f, 0, sizeof f);
    T_CHECK(i < g_out.n && nw_frame_decode(g_out.f[i], (int)g_out.len[i], &f) == NW_OK);
    return f;
}

static bool has_topic(const cb_conn_row *c, const char *t) {
    for (int i = 0; i < c->n_topics; i++)
        if (strcmp(c->topics[i], t) == 0) return true;
    return false;
}

static void setup_world(void) {
    cb_log_init(CB_LOG_ERROR, 0);
    rtf_install();
    rtf_reset();
    memset(&g_cfg, 0, sizeof g_cfg);
    strcpy(g_cfg.anon_key, "anon-key");
    strcpy(g_cfg.allowed_origins, "*");
    g_ctx.cfg = &g_cfg;
    g_ctx.store = &rtf.store;
}

static void protocol(void) {
    setup_world();
    rtf_member(G, U1);
    const uint8_t mine[] = { 0xe1, 0xe2 }, theirs[] = { 0x5e };
    rtf_add_view(G, U1, "playing", 5, 1, mine, sizeof mine);
    rtf_add_view(G2, "", "playing", 3, 1, theirs, sizeof theirs);
    cb_conn_row c;
    memset(&c, 0, sizeof c);
    c.vsn = CB_VSN_NW;

    // Nothing before a token, and a token that does not verify is nobody.
    sub(&c, 1, NW_SUB_GAMES, "", 0);
    NwFrame a = out(0);
    T_CHECK(a.kind == NW_WS_ACK && a.ref == 1 && a.why == ASK_WHY_AUTH);
    auth(&c, "not-a-token");
    T_EQ_STR(c.user_id, "");
    auth(&c, "tok-" U1);
    T_EQ_STR(c.user_id, U1);

    sub(&c, 2, NW_SUB_GAMES, "", 0);
    a = out(0);
    T_CHECK(a.kind == NW_WS_ACK && a.ref == 2 && a.why == 0);
    T_CHECK(has_topic(&c, "pv-" U1));

    // A member's game: their own seat's stream, then the catch-up view.
    sub(&c, 3, NW_SUB_GAME, G, 0);
    T_EQ_INT(g_out.n, 2);
    a = out(0);
    T_CHECK(a.kind == NW_WS_ACK && a.ref == 3 && a.why == 0 && a.seated == 1);
    NwFrame v = out(1);
    T_CHECK(v.kind == NW_WS_VIEW && v.version == 5 && v.gid_len == 6);
    T_EQ_MEM(v.blob.p, v.blob.len, mine, sizeof mine);
    T_CHECK(has_topic(&c, "gu-" G "-" U1));
    // Asked again by a client that holds version 5: no view to catch up.
    sub(&c, 4, NW_SUB_GAME, G, 5);
    T_EQ_INT(g_out.n, 1);

    // Someone else's game: the spectators' stream and view.
    sub(&c, 5, NW_SUB_GAME, G2, 0);
    T_EQ_INT(g_out.n, 2);
    a = out(0);
    T_CHECK(a.seated == 0 && a.why == 0);
    v = out(1);
    T_EQ_MEM(v.blob.p, v.blob.len, theirs, sizeof theirs);
    T_CHECK(has_topic(&c, "game-" G2));

    // Chat: members only.
    sub(&c, 6, NW_SUB_CHAT, G2, 0);
    T_CHECK(out(0).why == ASK_WHY_FORBIDDEN && !has_topic(&c, "chat:" G2));
    sub(&c, 7, NW_SUB_CHAT, G, 0);
    T_CHECK(out(0).why == 0 && has_topic(&c, "chat:" G));
    sub(&c, 8, NW_SUB_GAME, "bad id!", 0);
    T_EQ_INT(out(0).why, ASK_WHY_MALFORMED);

    // Unsubscribe, ping.
    NwFrame u;
    memset(&u, 0, sizeof u);
    u.kind = NW_WS_UNSUB;
    u.what = NW_SUB_GAME;
    u.gid_len = 6;
    memcpy(u.gid, G, 6);
    T_EQ_INT(run(&c, &u), CB_OK);
    T_CHECK(!has_topic(&c, "gu-" G "-" U1) && has_topic(&c, "chat:" G));
    NwFrame p;
    memset(&p, 0, sizeof p);
    p.kind = NW_WS_PING;
    T_EQ_INT(run(&c, &p), CB_OK);
    const uint8_t pong[] = { NW_V1, NW_WS_PONG };
    T_EQ_MEM(g_out.f[0], g_out.len[0], pong, 2);
    // A frame a server sends is not a client's.
    p.kind = NW_WS_PONG;
    T_EQ_INT(run(&c, &p), CB_E_INVALID);
    T_EQ_INT(cb_nw_on_frame(&g_ctx, &c, (const uint8_t *)"\x01", 1, collect, NULL), CB_E_INVALID);
    // A token that does not verify ends the user and everything subscribed as them.
    T_CHECK(c.n_topics > 0);
    auth(&c, "expired-token");
    T_EQ_STR(c.user_id, "");
    T_EQ_INT(c.n_topics, 0);
    // Another user on the same socket: the old user's subscriptions end.
    auth(&c, "tok-" U1);
    sub(&c, 9, NW_SUB_GAMES, "", 0);
    T_EQ_INT(c.n_topics, 1);
    auth(&c, "tok-" U2);
    T_EQ_STR(c.user_id, U2);
    T_EQ_INT(c.n_topics, 0);

    T_EQ_INT(cb_nw_proto_of_query("proto=nw1"), CB_VSN_NW);
    T_EQ_INT(cb_nw_proto_of_query("vsn=2.0.0"), 0);
    T_EQ_INT(cb_nw_proto_of_query("proto=nw2"), CB_E_INVALID);
}

// ---- what a publish builds ---------------------------------------------------------------

static struct { int n; char topic[128]; uint8_t nw[512]; size_t nw_len; } g_cap;
static void cap_deliver(cb_rt *rt, const char *topic, const char *const frames[CB_RT_PROTOS], const size_t lens[CB_RT_PROTOS]) {
    g_cap.n++;
    cb_strlcpy(g_cap.topic, topic, sizeof g_cap.topic);
    g_cap.nw_len = lens[2] <= sizeof g_cap.nw ? lens[2] : 0;
    memcpy(g_cap.nw, frames[2], g_cap.nw_len);
}
static const cb_rt_transport CAP_T = { "cap", cap_deliver, NULL };

static void publishes(void) {
    cb_rt rt = { &CAP_T, &g_ctx, NULL };
    memset(&g_cap, 0, sizeof g_cap);
    const uint8_t cast[] = { 1, 2 };
    rt_publish_cast(&rt, "gu-" G "-" U1, G, 7, cast, 2);
    const uint8_t want_cast[] = { 1, NW_WS_CAST, 6, 'a', 'b', '1', '2', 'c', 'd', 7, 0, 0, 0, 2, 0, 0, 0, 1, 2 };
    T_EQ_MEM(g_cap.nw, g_cap.nw_len, want_cast, sizeof want_cast);

    cb_view_row r;
    memset(&r, 0, sizeof r);
    strcpy(r.game_id, G);
    strcpy(r.player_id, U1);
    uint8_t env[] = { 0xee };
    r.view = env;
    r.view_len = 1;
    r.version = 9;
    strcpy(r.status, "playing");
    rt_publish_pv_update(&rt, U1, CB_RT_UPDATE, &r);
    T_EQ_STR(g_cap.topic, "pv-" U1);
    const uint8_t want_view[] = { 1, NW_WS_VIEW, 6, 'a', 'b', '1', '2', 'c', 'd', 9, 0, 0, 0, 1, 0, 0, 0, 0xee };
    T_EQ_MEM(g_cap.nw, g_cap.nw_len, want_view, sizeof want_view);
    // A seat given up: the view with no envelope.
    rt_publish_pv_update(&rt, U1, CB_RT_DELETE, &r);
    const uint8_t want_gone[] = { 1, NW_WS_VIEW, 6, 'a', 'b', '1', '2', 'c', 'd', 0, 0, 0, 0, 0, 0, 0, 0 };
    T_EQ_MEM(g_cap.nw, g_cap.nw_len, want_gone, sizeof want_gone);

    cb_chat_row c;
    memset(&c, 0, sizeof c);
    strcpy(c.id, "m");
    strcpy(c.game_id, G);
    strcpy(c.user_id, "u");
    c.message = "hi";
    c.created_at = 6;
    rt_publish_chat(&rt, G, &c);
    const uint8_t want_chat[] = { 1, NW_WS_CHAT, 6, 'a', 'b', '1', '2', 'c', 'd', 1, 'm', 1, 'u', 6, 0, 0, 0, 0, 0, 0, 2, 0, 'h', 'i' };
    T_EQ_MEM(g_cap.nw, g_cap.nw_len, want_chat, sizeof want_chat);
    T_EQ_INT(g_cap.n, 4);
}

// ---- API Gateway: base64 text both ways ----------------------------------------------------

static int apigw_ev(const char *route, const char *conn, const char *query, const char *body) {
    cb_ws_event e;
    memset(&e, 0, sizeof e);
    cb_strlcpy(e.route_key, route, sizeof e.route_key);
    cb_strlcpy(e.connection_id, conn, sizeof e.connection_id);
    strcpy(e.domain, "abc.execute-api.us-east-1.amazonaws.com");
    strcpy(e.stage, "prod");
    e.query = query ? query : "";
    e.body = (const uint8_t *)body;
    e.body_len = body ? strlen(body) : 0;
    int status = 0;
    cb_rt_apigw_event(&g_ctx, &e, &status);
    return status;
}

// A frame as the client sends it to API Gateway: base64 text.
static const char *as_text(const NwFrame *f) {
    static char text[NC_TEXT_MAX + 1];
    uint8_t raw[NW_ACCESS_MAX + 64];
    int n = nw_frame_encode(f, raw, sizeof raw);
    int t = nw_text_encode(raw, n, text, NC_TEXT_MAX);
    text[t > 0 ? t : 0] = 0;
    return text;
}

// The post at i, as the frame its base64 says.
static NwFrame posted(int i) {
    static uint8_t raw[2048];
    NwFrame f;
    memset(&f, 0, sizeof f);
    int n = nw_text_decode(rtf.posts[i].frame, (int)strlen(rtf.posts[i].frame), raw, sizeof raw);
    T_CHECK(n > 0 && nw_frame_decode(raw, n, &f) == NW_OK);
    return f;
}

static void apigw(void) {
    setup_world();
    rtf_member(G, U1);
    strcpy(g_cfg.ws_endpoint, "https://abc.execute-api.us-east-1.amazonaws.com/prod");
    cb_rt_free(g_ctx.rt);
    T_EQ_INT(cb_rt_apigw_new(&g_ctx, &g_ctx.rt), CB_OK);
    // The stream needs no API key; a protocol that is not one is refused.
    T_EQ_INT(apigw_ev("$connect", "s1", "proto=nw1", NULL), 200);
    T_EQ_INT(rtf.conns[rtf_conn_find("s1")].vsn, CB_VSN_NW);
    T_EQ_INT(apigw_ev("$connect", "s2", "proto=nw9", NULL), 400);
    T_EQ_INT(apigw_ev("$connect", "s2", "vsn=2.0.0", NULL), 401);   // Phoenix still needs the key

    NwFrame f;
    memset(&f, 0, sizeof f);
    f.kind = NW_WS_AUTH;
    f.token_len = (uint16_t)strlen("tok-" U1);
    memcpy(f.token, "tok-" U1, f.token_len);
    T_EQ_INT(apigw_ev("$default", "s1", NULL, as_text(&f)), 200);
    T_EQ_STR(rtf.conns[rtf_conn_find("s1")].user_id, U1);
    memset(&f, 0, sizeof f);
    f.kind = NW_WS_SUB;
    f.ref = 4;
    f.what = NW_SUB_GAME;
    f.gid_len = 6;
    memcpy(f.gid, G, 6);
    f.version = 0;
    int posts0 = rtf.n_posts;
    T_EQ_INT(apigw_ev("$default", "s1", NULL, as_text(&f)), 200);
    T_EQ_INT(rtf.n_posts, posts0 + 1);   // the ACK (the fake world has no view of G to catch up)
    NwFrame a = posted(posts0);
    T_CHECK(a.kind == NW_WS_ACK && a.ref == 4 && a.seated == 1 && a.why == 0);
    T_CHECK(has_topic(&rtf.conns[rtf_conn_find("s1")], "gu-" G "-" U1));

    // A publish reaches it as base64 text.
    const uint8_t cast[] = { 7 };
    rt_publish_cast(g_ctx.rt, "gu-" G "-" U1, G, 3, cast, 1);
    NwFrame c = posted(rtf.n_posts - 1);
    T_CHECK(c.kind == NW_WS_CAST && c.version == 3 && c.blob.len == 1 && c.blob.p[0] == 7);
    T_EQ_STR(rtf.posts[rtf.n_posts - 1].conn_id, "s1");

    // Not base64, or not a frame: the socket is closed and the row dropped.
    int del0 = rtf.n_deletes;
    T_EQ_INT(apigw_ev("$default", "s1", NULL, "!!!!"), 200);
    T_EQ_INT(rtf.n_deletes, del0 + 1);
    T_EQ_INT(rtf_conn_find("s1"), -1);
    // A lost row is rebuilt as the protocol the frame's shape says.
    memset(&f, 0, sizeof f);
    f.kind = NW_WS_PING;
    T_EQ_INT(apigw_ev("$default", "s3", NULL, as_text(&f)), 200);
    T_EQ_INT(rtf.conns[rtf_conn_find("s3")].vsn, CB_VSN_NW);
    T_EQ_INT(posted(rtf.n_posts - 1).kind, NW_WS_PONG);
    cb_rt_free(g_ctx.rt);
    g_ctx.rt = NULL;
}

// ---- cbase serve: a real socket, driven by the client library -------------------------------

static int g_port;
static pthread_t g_thread;
static void *loop_run(void *arg) { cb_loop_run(g_ctx.loop); return NULL; }
static void drain_post(cb_loop *l, void *ud) { cb_server_drain(g_ctx.server, 2000); }

// Sends every frame the client has due, as text.
static void flush(int fd, int64_t now) {
    for (const NcOut *o; (o = nc_stream_next(now));) T_EQ_INT(tc_ws_send(fd, CB_WS_OP_TEXT, o->text, o->len), 0);
}

// The next event the server's frames make, skipping NC_EV_NONE; kind -1 on a timeout.
static const NcEvent *next_event(int fd, int timeout_ms, cb_buf *m) {
    static NcEvent none = { .kind = 255 };
    for (;;) {
        cb_buf_reset(m);
        int op = tc_ws_recv(fd, timeout_ms, m);
        if (op != CB_WS_OP_BIN && op != CB_WS_OP_TEXT) return &none;
        const NcEvent *e = nc_stream_in(m->p, (int)m->len, op == CB_WS_OP_TEXT, cb_now_ms());
        if (e->kind != NC_EV_NONE) return e;
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
    rtf_member(G, U1);
    g_ctx.mode = CB_MODE_SERVE;
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

    tc_response r;
    T_EQ_INT(tc_http(g_port, "GET", "/v1/stream?proto=nw9", NULL, NULL, 0, &r), 0);
    T_EQ_INT(r.status, 400);
    tc_response_free(&r);

    nc_reset();
    session_for(U1);
    T_EQ_INT(nc_stream_sub(NW_SUB_GAMES, NULL, 0), NW_OK);
    T_EQ_INT(nc_stream_sub(NW_SUB_GAME, G, 6), NW_OK);
    T_EQ_INT(nc_stream_sub(NW_SUB_CHAT, G2, 6), NW_OK);   // not a member: refused, retried
    const NcRequest *q = nc_req_stream();
    char path[NC_PATH_MAX + 1];
    memcpy(path, q->path, q->path_len);
    path[q->path_len] = 0;
    int fd = tc_ws_open(g_port, path, NULL);
    T_CHECK(fd >= 0);
    int64_t t0 = cb_now_ms();
    nc_stream_opened(t0);
    flush(fd, t0);
    cb_buf m;
    cb_buf_init(&m);
    const NcEvent *e = next_event(fd, 2000, &m);
    T_CHECK(e->kind == NC_EV_SEATED && e->seated == 1 && e->gid_len == 6);
    e = next_event(fd, 2000, &m);
    T_CHECK(e->kind == NC_EV_REFUSED && e->why == ASK_WHY_FORBIDDEN && e->what == NW_SUB_CHAT);
    // The refusal is retried after NC_RETRY_FIRST_MS, not before.
    T_CHECK(nc_stream_next(t0 + 100) == NULL);
    int due = nc_stream_due_ms(t0 + 100);
    T_CHECK(due > 0 && due <= NC_RETRY_FIRST_MS);
    rtf_member(G2, U1);
    flush(fd, cb_now_ms() + NC_RETRY_FIRST_MS);
    // Its ACK makes no event; next_event below reads past it.

    // Pushes: a cast and a view, each once; the same version again is stale.
    const uint8_t cast[] = { 0xca, 0x57 };
    rt_publish_cast(g_ctx.rt, "gu-" G "-" U1, G, 11, cast, 2);
    e = next_event(fd, 2000, &m);
    T_CHECK(e->kind == NC_EV_CAST && e->version == 11 && e->payload_len == 2);
    T_EQ_MEM(nc_event_payload(), e->payload_len, cast, 2);
    rt_publish_cast(g_ctx.rt, "gu-" G "-" U1, G, 11, cast, 2);
    cb_view_row v;
    memset(&v, 0, sizeof v);
    strcpy(v.game_id, G);
    strcpy(v.player_id, U1);
    uint8_t env[] = { 0xe0, 0x01 };
    v.view = env;
    v.view_len = 2;
    v.version = 11;
    strcpy(v.status, "playing");
    rt_publish_pv_update(g_ctx.rt, U1, CB_RT_UPDATE, &v);
    e = next_event(fd, 2000, &m);   // the stale cast made no event: this is the view
    T_CHECK(e->kind == NC_EV_VIEW && e->version == 11 && e->what == NW_SUB_GAMES);
    T_EQ_MEM(nc_event_payload(), e->payload_len, env, 2);
    // A client frame sent as binary is taken too (rt_local is not API Gateway).
    uint8_t ping[] = { NW_V1, NW_WS_PING };
    T_EQ_INT(tc_ws_send(fd, CB_WS_OP_BIN, ping, 2), 0);
    // The retried chat subscription's ACK runs on the pool while the pushes
    // above went straight to the loop, so it may arrive only now: skip it.
    int op;
    do {
        cb_buf_reset(&m);
        op = tc_ws_recv(fd, 2000, &m);
    } while (op == CB_WS_OP_BIN && m.len >= 2 && m.p[1] == NW_WS_ACK);
    T_EQ_INT(op, CB_WS_OP_BIN);
    T_EQ_MEM(m.p, m.len, ((const uint8_t[]){ NW_V1, NW_WS_PONG }), 2);
    // Garbage closes the socket.
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
    T_RUN(protocol);
    T_RUN(publishes);
    T_RUN(apigw);
    T_RUN(serve_end_to_end);
    return T_DONE();
}
