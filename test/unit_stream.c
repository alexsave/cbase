// unit_stream.c - the binary push stream (realtime/stream.h): the protocol
// against the fake world, the frames a publish builds, and a real loopback
// `cbase serve` socket carrying binary frames, driven end to end by the
// kernel's client library (netclient.h). API Gateway's transport is
// unit_rt_apigw.c's.
#include "t.h"

#include "lib_client.h"
#include "lib_rt_fakestore.h"
#include "net/loop.h"
#include "net/router.h"
#include "net/server.h"
#include "net/ws.h"
#include "realtime/realtime.h"
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
    T_CHECK(v.kind == NW_WS_VIEW && v.version == 5 && v.gid_len == 6);   // a bare SUB: an older client
    T_EQ_MEM(v.blob.p, v.blob.len, mine, sizeof mine);
    T_CHECK(has_topic(&c, "gu-" G "-" U1));
    // Asked again by a client that holds version 5: no view to catch up.
    sub(&c, 4, NW_SUB_GAME, G, 5);
    T_EQ_INT(g_out.n, 1);
    // A subscription that says who asks gets its catch-up as its own kind, so
    // the client tells it from a dashboard push.
    NwFrame sa;
    memset(&sa, 0, sizeof sa);
    sa.kind = NW_WS_SUB_AS;
    sa.token_len = (uint16_t)strlen("tok-" U1);
    memcpy(sa.token, "tok-" U1, sa.token_len);
    sa.ref = 9;
    sa.what = NW_SUB_GAME;
    sa.gid_len = 6;
    memcpy(sa.gid, G, 6);
    T_EQ_INT(run(&c, &sa), CB_OK);
    T_EQ_INT(g_out.n, 2);
    T_CHECK(out(0).kind == NW_WS_ACK && out(0).ref == 9 && out(0).seated == 1);
    v = out(1);
    T_CHECK(v.kind == NW_WS_CATCHUP && v.version == 5);
    T_EQ_MEM(v.blob.p, v.blob.len, mine, sizeof mine);

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
    T_EQ_INT(cb_nw_proto_of_query("vsn=2.0.0"), CB_E_INVALID);
    T_EQ_INT(cb_nw_proto_of_query("proto=nw2"), CB_E_INVALID);
}

// ---- what a publish builds ---------------------------------------------------------------

// Each target a publish hands the transport: its nw1 topic and its MQTT topic
// ("" where it has none), and the last message.
static struct { int n; char nw_topic[8][128], topic[8][128]; uint8_t nw[512]; size_t nw_len; } g_cap;
static int cap_deliver(cb_rt *rt, const cb_rt_to *to, const uint8_t *frame, size_t len) {
    if (g_cap.n < 8) {
        cb_rt_nw_topic(to, g_cap.nw_topic[g_cap.n], sizeof g_cap.nw_topic[0]);
        cb_rt_topic(to, g_cap.topic[g_cap.n], sizeof g_cap.topic[0]);
    }
    g_cap.n++;
    g_cap.nw_len = len <= sizeof g_cap.nw ? len : 0;
    memcpy(g_cap.nw, frame, g_cap.nw_len);
    return 0;
}
static const cb_rt_transport CAP_T = { .name = "cap", .deliver = cap_deliver };

static void publishes(void) {
    cb_rt rt = { &CAP_T, &g_ctx, NULL, NULL };
    memset(&g_cap, 0, sizeof g_cap);
    const uint8_t cast[] = { 1, 2 };
    rt_publish_cast(&rt, U1, G, 7, cast, 2);
    const uint8_t want_cast[] = { 1, NW_WS_CAST, 6, 'a', 'b', '1', '2', 'c', 'd', 7, 0, 0, 0, 2, 0, 0, 0, 1, 2 };
    T_EQ_MEM(g_cap.nw, g_cap.nw_len, want_cast, sizeof want_cast);
    T_EQ_STR(g_cap.nw_topic[0], "gu-" G "-" U1);
    T_EQ_STR(g_cap.topic[0], "u/" U1 "/c/" G);
    rt_publish_cast(&rt, NULL, G, 7, cast, 1);
    T_EQ_STR(g_cap.nw_topic[1], "game-" G);
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
    T_EQ_STR(g_cap.nw_topic[2], "pv-" U1);
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
    // Two members: the game's nw1 chat once, then each member's own MQTT topic.
    cb_uuid members[2] = { U1, U2 };
    rt_publish_chat(&rt, G, &c, members, 2);
    const uint8_t want_chat[] = { 1, NW_WS_CHAT, 6, 'a', 'b', '1', '2', 'c', 'd', 1, 'm', 1, 'u', 6, 0, 0, 0, 0, 0, 0, 2, 0, 'h', 'i' };
    T_EQ_MEM(g_cap.nw, g_cap.nw_len, want_chat, sizeof want_chat);
    T_EQ_INT(g_cap.n, 7);
    T_EQ_STR(g_cap.nw_topic[4], "chat:" G);
    T_EQ_STR(g_cap.topic[4], "");
    T_EQ_STR(g_cap.nw_topic[5], "");
    T_EQ_STR(g_cap.topic[5], "u/" U1 "/m/" G);
    T_EQ_STR(g_cap.topic[6], "u/" U2 "/m/" G);
    // No members (none read): the nw1 chat alone.
    rt_publish_chat(&rt, G, &c, NULL, 0);
    T_EQ_INT(g_cap.n, 8);
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
    T_EQ_INT(cb_rt_nw_topic(&chat_all, t, sizeof t), 5 + 6);
    cb_rt_to chat_one = { CB_RT_CHAT, U1, G };
    T_EQ_INT(cb_rt_nw_topic(&chat_one, t, sizeof t), -1);
    const char *bad[] = { "", "a/b", "a+", "#", "a b", "a.b", "a\n",
                          "x0123456789012345678901234567890123456789012345678901234567890123" };   // 65 bytes
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        cb_rt_to a = { CB_RT_SEAT, bad[i], G }, b = { CB_RT_SEAT, U1, bad[i] }, w = { CB_RT_WATCH, NULL, bad[i] };
        T_EQ_INT(cb_rt_topic(&a, t, sizeof t), -1);
        T_EQ_INT(cb_rt_topic(&b, t, sizeof t), -1);
        T_EQ_INT(cb_rt_topic(&w, t, sizeof t), -1);
        T_EQ_INT(cb_rt_nw_topic(&a, t, sizeof t), -1);
    }
    cb_rt_to none = { CB_RT_SEAT, NULL, G };
    T_EQ_INT(cb_rt_topic(&none, t, sizeof t), -1);
    cb_rt_to odd = { (cb_rt_family)99, U1, G };
    T_EQ_INT(cb_rt_topic(&odd, t, sizeof t), -1);
    T_EQ_INT(cb_rt_nw_topic(&odd, t, sizeof t), -1);
    // Too small a buffer is a refusal, not a cut topic.
    cb_rt_to seat = { CB_RT_SEAT, U1, G };
    T_EQ_INT(cb_rt_topic(&seat, t, 10), -1);
    T_EQ_INT(cb_rt_topic(&seat, t, 2 + 36 + 3 + 6 + 1), 2 + 36 + 3 + 6);
}

// ---- cbase serve: a real socket, driven by the client library -------------------------------

static int g_port;
static pthread_t g_thread;
// A batch's frames as one message: entries from encoded frames.
static int run_batch(cb_conn_row *c, const NwFrame *frames, int n) {
    static uint8_t entries[4 * (NW_ACCESS_MAX + 64)], b[sizeof entries + 64];
    int at = 0;
    for (int i = 0; i < n; i++) {
        uint8_t one[NW_ACCESS_MAX + 64];
        int k = nw_frame_encode(&frames[i], one, sizeof one);
        T_CHECK(k > 0);
        at += nw_batch_add(one, k, entries + at, (int)sizeof entries - at);
    }
    NwFrame f;
    memset(&f, 0, sizeof f);
    f.kind = NW_WS_BATCH;
    f.blob.p = entries;
    f.blob.len = (uint32_t)at;
    int len = nw_frame_encode(&f, b, sizeof b);
    T_CHECK(len > 0);
    memset(&g_out, 0, sizeof g_out);
    return cb_nw_on_frame(&g_ctx, c, b, (size_t)len, collect, NULL);
}

// The entries of output message i, a batch, decoded into fs; their count.
static int out_batch(int i, NwFrame *fs, int cap) {
    NwFrame f = out(i);
    T_EQ_INT(f.kind, NW_WS_BATCH);
    static uint8_t copy[256];
    memcpy(copy, g_out.f[i], g_out.len[i]);
    T_CHECK(nw_frame_decode(copy, (int)g_out.len[i], &f) == NW_OK);
    NwBlob all = f.blob, one;
    int k = 0;
    for (uint32_t pos = 0; k < cap && nw_batch_next(all, &pos, &one) == 1; k++)
        T_CHECK(nw_frame_decode(one.p, (int)one.len, &fs[k]) == NW_OK);
    return k;
}

static NwFrame sub_as(uint16_t ref, int what, const char *gid, const char *tok) {
    NwFrame f;
    memset(&f, 0, sizeof f);
    f.kind = NW_WS_SUB_AS;
    f.token_len = (uint16_t)strlen(tok);
    memcpy(f.token, tok, f.token_len);
    f.ref = ref;
    f.what = (uint8_t)what;
    f.gid_len = (uint8_t)strlen(gid);
    memcpy(f.gid, gid, f.gid_len);
    return f;
}

// A batch runs its frames in order on the one row and answers them all in one
// message; one entry that is not a client frame refuses the whole of it.
static void batches(void) {
    setup_world();
    rtf_member(G, U1);
    const uint8_t mine[] = { 0xe1 };
    rtf_add_view(G, U1, "playing", 5, 1, mine, sizeof mine);
    cb_conn_row c;
    memset(&c, 0, sizeof c);
    NwFrame in[4] = { sub_as(1, NW_SUB_GAMES, "", "tok-" U1), sub_as(2, NW_SUB_GAME, G, "tok-" U1),
                      sub_as(3, NW_SUB_CHAT, G, "tok-" U1) };
    memset(&in[3], 0, sizeof in[3]);
    in[3].kind = NW_WS_PING;
    T_EQ_INT(run_batch(&c, in, 4), CB_OK);
    T_EQ_INT(g_out.n, 1);   // one message
    NwFrame got[8];
    T_EQ_INT(out_batch(0, got, 8), 5);
    T_CHECK(got[0].kind == NW_WS_ACK && got[0].ref == 1 && got[0].why == 0);
    T_CHECK(got[1].kind == NW_WS_ACK && got[1].ref == 2 && got[1].seated == 1);
    T_CHECK(got[2].kind == NW_WS_CATCHUP && got[2].version == 5);
    T_CHECK(got[3].kind == NW_WS_ACK && got[3].ref == 3 && got[3].why == 0);
    T_EQ_INT(got[4].kind, NW_WS_PONG);
    T_EQ_INT(c.n_topics, 3);
    T_CHECK(has_topic(&c, "pv-" U1) && has_topic(&c, "gu-" G "-" U1) && has_topic(&c, "chat:" G));
    // A lone reply goes as itself, not as a batch of one.
    T_EQ_INT(run_batch(&c, &in[3], 1), CB_OK);
    T_EQ_INT(g_out.n, 1);
    T_EQ_INT(out(0).kind, NW_WS_PONG);
    // A server's frame inside: refused whole, the row untouched.
    cb_conn_row d;
    memset(&d, 0, sizeof d);
    NwFrame bad[2] = { sub_as(1, NW_SUB_GAMES, "", "tok-" U1) };
    memset(&bad[1], 0, sizeof bad[1]);
    bad[1].kind = NW_WS_PONG;
    T_EQ_INT(run_batch(&d, bad, 2), CB_E_INVALID);
    T_EQ_INT(g_out.n, 0);
    T_EQ_INT(d.n_topics, 0);
    T_EQ_STR(d.user_id, "");
}

// Replies past NW_BATCH_MAX are cut into several batches, in order.
static void long_replies_split(void) {
    static uint8_t entries[(NW_BATCH_MAX + 3) * 16];
    int at = 0;
    for (int i = 0; i < NW_BATCH_MAX + 3; i++) {
        NwFrame a;
        memset(&a, 0, sizeof a);
        a.kind = NW_WS_ACK;
        a.ref = (uint16_t)i;
        uint8_t one[16];
        int k = nw_frame_encode(&a, one, sizeof one);
        int w = nw_batch_add(one, k, entries + at, (int)sizeof entries - at);
        T_EQ_INT(w, 10);
        at += w;
    }
    memset(&g_out, 0, sizeof g_out);
    cb_nw_send_batched(entries, (size_t)at, NW_BATCH_MAX + 3, collect, NULL);
    T_EQ_INT(g_out.n, 2);
    NwFrame got[NW_BATCH_MAX];
    T_EQ_INT(out_batch(0, got, NW_BATCH_MAX), NW_BATCH_MAX);
    T_EQ_INT(got[NW_BATCH_MAX - 1].ref, NW_BATCH_MAX - 1);
    T_EQ_INT(out_batch(1, got, NW_BATCH_MAX), 3);
    T_EQ_INT(got[0].ref, NW_BATCH_MAX);
    T_EQ_INT(got[2].ref, NW_BATCH_MAX + 2);
    // One past a whole batch leaves a lone reply, sent as itself.
    memset(&g_out, 0, sizeof g_out);
    int one_more = at / (NW_BATCH_MAX + 3) * (NW_BATCH_MAX + 1);
    cb_nw_send_batched(entries, (size_t)one_more, NW_BATCH_MAX + 1, collect, NULL);
    T_EQ_INT(g_out.n, 2);
    T_EQ_INT(out(1).kind, NW_WS_ACK);
    T_EQ_INT(out(1).ref, NW_BATCH_MAX);
}

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
    T_RUN(protocol);
    T_RUN(publishes);
    T_RUN(targets_name_topics);
    T_RUN(batches);
    T_RUN(long_replies_split);
    T_RUN(serve_end_to_end);
    return T_DONE();
}
