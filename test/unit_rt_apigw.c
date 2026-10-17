// unit_rt_apigw.c - realtime under API Gateway (realtime/rt_apigw.h): the
// three events against the store's CONN rows, the stream's frames as base64
// text both ways, and publishing through the management API with a 410
// cleaning up.
#include "t.h"

#include "lib_rt_fakestore.h"
#include "realtime/realtime.h"
#include "realtime/rt_apigw.h"
#include "realtime/stream.h"
#include "util/clock.h"
#include "util/log.h"

#include "netclient.h"
#include "netwire.h"

#define U1 "11111111-1111-4111-8111-111111111111"
#define U2 "22222222-2222-4222-8222-222222222222"
#define G  "ab12cd"

static cb_ctx g_ctx;
static cb_config g_cfg;

// The body is copied: a frame's text comes from a static buffer, and an event
// run inside another (the race below) must not rewrite the outer one's body.
static int ev(const char *route, const char *conn, const char *query, const char *text) {
    char body[NC_TEXT_MAX + 1];
    if (text) cb_strlcpy(body, text, sizeof body);
    cb_ws_event e;
    memset(&e, 0, sizeof e);
    cb_strlcpy(e.route_key, route, sizeof e.route_key);
    cb_strlcpy(e.connection_id, conn, sizeof e.connection_id);
    strcpy(e.domain, "abc.execute-api.us-west-1.amazonaws.com");
    strcpy(e.stage, "prod");
    e.query = query ? query : "";
    e.body = text ? (const uint8_t *)body : NULL;
    e.body_len = text ? strlen(body) : 0;
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

static const char *auth_token_text(const char *tok) {
    NwFrame f;
    memset(&f, 0, sizeof f);
    f.kind = NW_WS_AUTH;
    f.token_len = (uint16_t)strlen(tok);
    memcpy(f.token, tok, f.token_len);
    return as_text(&f);
}

static const char *auth_text(const char *uid) {
    NwFrame f;
    memset(&f, 0, sizeof f);
    f.kind = NW_WS_AUTH;
    char tok[64];
    snprintf(tok, sizeof tok, "tok-%s", uid);
    f.token_len = (uint16_t)strlen(tok);
    memcpy(f.token, tok, f.token_len);
    return as_text(&f);
}

static const char *sub_text(uint8_t kind, uint16_t ref, uint8_t what, const char *gid) {
    NwFrame f;
    memset(&f, 0, sizeof f);
    f.kind = kind;
    f.ref = ref;
    f.what = what;
    f.gid_len = (uint8_t)strlen(gid);
    memcpy(f.gid, gid, f.gid_len);
    return as_text(&f);
}

// A subscription that says who asks (NW_WS_SUB_AS), with `token` as its token.
static const char *sub_as_text(const char *token, uint16_t ref, uint8_t what, const char *gid) {
    NwFrame f;
    memset(&f, 0, sizeof f);
    f.kind = NW_WS_SUB_AS;
    f.token_len = (uint16_t)strlen(token);
    memcpy(f.token, token, f.token_len);
    f.ref = ref;
    f.what = what;
    f.gid_len = (uint8_t)strlen(gid);
    memcpy(f.gid, gid, f.gid_len);
    return as_text(&f);
}

static const char *ping_text(void) {
    NwFrame f;
    memset(&f, 0, sizeof f);
    f.kind = NW_WS_PING;
    return as_text(&f);
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

static bool count_conn(const cb_conn_row *r, void *ud) {
    (void)r;
    (*(int *)ud)++;
    return true;
}

static void setup(void) {
    cb_log_init(CB_LOG_ERROR, 0);
    rtf_install();
    memset(&g_cfg, 0, sizeof g_cfg);
    g_ctx.cfg = &g_cfg;
    g_ctx.store = &rtf.store;
    cb_rt_free(g_ctx.rt);
    T_EQ_INT(cb_rt_apigw_new(&g_ctx, &g_ctx.rt), CB_OK);
}

static void connect_and_disconnect(void) {
    setup();
    int puts0 = rtf.conn_puts;
    T_EQ_INT(ev("$connect", "c1", "proto=nw1", NULL), 200);
    // $connect writes nothing: a socket costs the table no write until a frame names a user.
    T_EQ_INT(rtf_conn_find("c1"), -1);
    T_EQ_INT(rtf.conn_puts, puts0);
    // No proto, another proto, or the old Phoenix URL: refused, no row.
    T_EQ_INT(ev("$connect", "c3", "", NULL), 400);
    T_EQ_INT(ev("$connect", "c3", "proto=nw9", NULL), 400);
    T_EQ_INT(ev("$connect", "c3", "apikey=anon-key&vsn=2.0.0", NULL), 400);
    T_EQ_INT(rtf_conn_find("c3"), -1);
    T_EQ_INT(ev("$default", "c1", NULL, auth_text(U1)), 200);
    int i = rtf_conn_find("c1");
    T_CHECK(i >= 0);
    T_EQ_INT(rtf.conns[i].vsn, CB_VSN_NW);
    T_EQ_INT(rtf.conns[i].n_topics, 0);
    T_EQ_STR(rtf.conns[i].user_id, U1);
    int64_t ttl = rtf.conns[i].expires_at - cb_now_ms();
    T_CHECK(ttl > 7100000 && ttl <= 7200000);
    T_EQ_INT(ev("$disconnect", "c1", NULL, NULL), 200);
    T_EQ_INT(rtf_conn_find("c1"), -1);
    T_EQ_INT(ev("$disconnect", "c1", NULL, NULL), 200);   // already gone
    T_EQ_INT(ev("$whatever", "c1", NULL, NULL), 400);
}

// A connection that has not named a user writes nothing, whatever it sends;
// one whose AUTH stops naming anyone loses its row.
static void nobody_writes_nothing(void) {
    setup();
    int puts0 = rtf.conn_puts;
    T_EQ_INT(ev("$connect", "c1", "proto=nw1", NULL), 200);
    for (int k = 0; k < 5; k++) T_EQ_INT(ev("$default", "c1", NULL, ping_text()), 200);
    T_EQ_INT(ev("$default", "c1", NULL, sub_text(NW_WS_SUB, 1, NW_SUB_GAMES, "")), 200);   // refused: AUTH
    T_EQ_INT(ev("$default", "c1", NULL, sub_as_text("forged", 2, NW_SUB_GAMES, "")), 200);
    T_EQ_INT(rtf.conn_puts, puts0);
    T_EQ_INT(rtf_conn_find("c1"), -1);
    T_EQ_INT(rtf.n_posts, 7);   // every frame still answered: 5 pongs, 2 refusals
    T_EQ_INT(ev("$default", "c1", NULL, auth_text(U1)), 200);
    T_CHECK(rtf_conn_find("c1") >= 0);
    T_EQ_INT(ev("$default", "c1", NULL, auth_token_text("forged")), 200);   // names nobody
    T_EQ_INT(rtf_conn_find("c1"), -1);
}

// With CBASE_ORIGIN_SECRET set, only a $connect carrying it (the edge's
// X-Cbase-Origin) is let in: the WebSocket API's own hostname is closed.
static int ev_origin(const char *conn, const char *origin) {
    cb_ws_event e;
    memset(&e, 0, sizeof e);
    strcpy(e.route_key, "$connect");
    cb_strlcpy(e.connection_id, conn, sizeof e.connection_id);
    e.query = "proto=nw1";
    e.body = (const uint8_t *)"";
    if (origin) { e.has_origin = true; cb_strlcpy(e.origin, origin, sizeof e.origin); }
    int status = 0;
    cb_rt_apigw_event(&g_ctx, &e, &status);
    return status;
}

static void connect_only_through_the_edge(void) {
    setup();
    T_EQ_INT(ev_origin("c1", NULL), 200);   // no secret configured: open (serve, tests)
    strcpy(g_cfg.origin_secret, "0123456789abcdef0123456789abcdef");
    T_EQ_INT(ev_origin("c1", NULL), 403);
    T_EQ_INT(ev_origin("c1", ""), 403);
    T_EQ_INT(ev_origin("c1", "0123456789abcdef0123456789abcdeX"), 403);
    T_EQ_INT(ev_origin("c1", "0123456789abcdef0123456789abcdef0"), 403);
    T_EQ_INT(ev_origin("c1", "0123456789abcdef0123456789abcde"), 403);
    T_EQ_INT(ev_origin("c1", "0123456789abcdef0123456789abcdef"), 200);
    g_cfg.origin_secret[0] = 0;
}

static void frames_round_trip_through_the_conn_row(void) {
    setup();
    rtf_member(G, U1);
    T_EQ_INT(ev("$connect", "c1", "proto=nw1", NULL), 200);
    T_EQ_INT(ev("$default", "c1", NULL, auth_text(U1)), 200);
    int puts0 = rtf.conn_puts;
    T_EQ_INT(ev("$default", "c1", NULL, sub_text(NW_WS_SUB, 1, NW_SUB_GAME, G)), 200);
    T_EQ_INT(rtf.n_posts, 1);
    T_EQ_STR(rtf.posts[0].conn_id, "c1");
    NwFrame a = posted(0);
    T_CHECK(a.kind == NW_WS_ACK && a.ref == 1 && a.why == 0 && a.seated == 1);
    int i = rtf_conn_find("c1");
    T_EQ_INT(rtf.conns[i].n_topics, 1);
    T_EQ_STR(rtf.conns[i].topics[0], "gu-" G "-" U1);
    T_EQ_STR(rtf.conns[i].user_id, U1);
    T_EQ_INT(rtf.conn_puts, puts0 + 1);
    // A ping is answered and costs no write (the row did not change).
    T_EQ_INT(ev("$default", "c1", NULL, ping_text()), 200);
    T_EQ_INT(rtf.n_posts, 2);
    T_EQ_INT(posted(1).kind, NW_WS_PONG);
    T_EQ_INT(rtf.conn_puts, puts0 + 1);
    // ...until the TTL needs pushing forward.
    rtf.conns[i].expires_at = cb_now_ms() + 60000;
    T_EQ_INT(ev("$default", "c1", NULL, ping_text()), 200);
    T_EQ_INT(rtf.conn_puts, puts0 + 2);
    T_CHECK(rtf.conns[rtf_conn_find("c1")].expires_at > cb_now_ms() + 7000000);
    // A refused subscription changes nothing.
    T_EQ_INT(ev("$default", "c1", NULL, sub_text(NW_WS_SUB, 4, NW_SUB_CHAT, "zzzzzz")), 200);
    a = posted(rtf.n_posts - 1);
    T_CHECK(a.kind == NW_WS_ACK && a.ref == 4 && a.why != 0);
    T_EQ_INT(rtf.conns[rtf_conn_find("c1")].n_topics, 1);
    // UNSUB removes the topic from the row.
    T_EQ_INT(ev("$default", "c1", NULL, sub_text(NW_WS_UNSUB, 0, NW_SUB_GAME, G)), 200);
    T_EQ_INT(rtf.conns[rtf_conn_find("c1")].n_topics, 0);
}

// API Gateway runs a connection's frames in no order: a SUB that reaches the
// function before the AUTH sent ahead of it was stored finds nobody. The
// subscription that carries its own token does not depend on that write.
static void a_subscription_says_who_asks(void) {
    setup();
    rtf_member(G, U1);
    T_EQ_INT(ev("$connect", "c1", "proto=nw1", NULL), 200);
    // The race, as a bare SUB loses it: AUTH not stored yet, refused.
    T_EQ_INT(ev("$default", "c1", NULL, sub_text(NW_WS_SUB, 1, NW_SUB_GAME, G)), 200);
    NwFrame a = posted(rtf.n_posts - 1);
    T_CHECK(a.kind == NW_WS_ACK && a.ref == 1 && a.why == ASK_WHY_AUTH);
    // Self-authenticating, in the reversed order: the SUB first, then the AUTH.
    T_EQ_INT(ev("$default", "c1", NULL, sub_as_text("tok-" U1, 2, NW_SUB_GAME, G)), 200);
    a = posted(rtf.n_posts - 1);
    T_CHECK(a.kind == NW_WS_ACK && a.ref == 2 && a.why == 0 && a.seated == 1);
    int i = rtf_conn_find("c1");
    T_EQ_STR(rtf.conns[i].user_id, U1);
    T_EQ_INT(rtf.conns[i].n_topics, 1);
    T_EQ_STR(rtf.conns[i].topics[0], "gu-" G "-" U1);
    T_EQ_INT(ev("$default", "c1", NULL, auth_text(U1)), 200);
    i = rtf_conn_find("c1");
    T_EQ_STR(rtf.conns[i].user_id, U1);
    T_EQ_INT(rtf.conns[i].n_topics, 1);
    // A token that names nobody: refused, and the row is untouched.
    T_EQ_INT(ev("$default", "c1", NULL, sub_as_text("forged", 3, NW_SUB_GAMES, "")), 200);
    a = posted(rtf.n_posts - 1);
    T_CHECK(a.kind == NW_WS_ACK && a.ref == 3 && a.why == ASK_WHY_AUTH);
    i = rtf_conn_find("c1");
    T_EQ_STR(rtf.conns[i].user_id, U1);
    T_EQ_INT(rtf.conns[i].n_topics, 1);
    // Another user's: what was subscribed as the first one ends.
    T_EQ_INT(ev("$default", "c1", NULL, sub_as_text("tok-" U2, 4, NW_SUB_GAMES, "")), 200);
    a = posted(rtf.n_posts - 1);
    T_CHECK(a.kind == NW_WS_ACK && a.ref == 4 && a.why == 0);
    i = rtf_conn_find("c1");
    T_EQ_STR(rtf.conns[i].user_id, U2);
    T_EQ_INT(rtf.conns[i].n_topics, 1);
    T_EQ_STR(rtf.conns[i].topics[0], "pv-" U2);
}

static void a_lost_row_is_rebuilt_and_garbage_closes(void) {
    setup();
    // No row (or its TTL ran out): the row starts afresh with the first user.
    T_EQ_INT(ev("$default", "c9", NULL, auth_text(U1)), 200);
    T_EQ_INT(ev("$default", "c9", NULL, ping_text()), 200);
    T_EQ_INT(rtf.n_posts, 1);
    T_EQ_INT(posted(0).kind, NW_WS_PONG);
    T_EQ_INT(rtf.conns[rtf_conn_find("c9")].vsn, CB_VSN_NW);
    // Not base64, or base64 of no frame: the socket is closed and the row dropped.
    T_EQ_INT(ev("$default", "c9", NULL, "!!!!"), 200);
    T_EQ_INT(rtf.n_deletes, 1);
    T_EQ_INT(rtf_conn_find("c9"), -1);
    T_EQ_INT(ev("$connect", "c9", "proto=nw1", NULL), 200);
    T_EQ_INT(ev("$default", "c9", NULL, auth_text(U1)), 200);
    T_EQ_INT(ev("$default", "c9", NULL, "aGVsbG8="), 200);   // "hello"
    T_EQ_INT(rtf.n_deletes, 2);
    T_EQ_INT(rtf_conn_find("c9"), -1);
    // A reply to a connection that has gone (410) drops the row.
    T_EQ_INT(ev("$connect", "c8", "proto=nw1", NULL), 200);
    T_EQ_INT(ev("$default", "c8", NULL, auth_text(U1)), 200);
    strcpy(rtf.gone[0], "c8");
    T_EQ_INT(ev("$default", "c8", NULL, ping_text()), 200);
    T_EQ_INT(rtf_conn_find("c8"), -1);
    // No store: 503.
    g_ctx.store = NULL;
    T_EQ_INT(ev("$default", "c7", NULL, ping_text()), 503);
}

// API Gateway runs a connection's frames as concurrent invocations: a page
// subscribes to its games, a game and its chat within a few milliseconds.
// Recorded against the live backend 2026-10-14: three subscriptions each read
// the empty row and each wrote back its own topic, so only the last stuck and
// the page never heard the pushes it had been acknowledged for. Here the chat
// subscription runs whole between the game subscription's read and its write.
static void g_sub_chat(void *ud) {
    (void)ud;
    T_EQ_INT(ev("$default", "c1", NULL, sub_text(NW_WS_SUB, 9, NW_SUB_CHAT, G)), 200);
}

static void concurrent_frames_of_one_connection_both_stick(void) {
    setup();
    rtf_member(G, U1);
    T_EQ_INT(ev("$connect", "c1", "proto=nw1", NULL), 200);
    T_EQ_INT(ev("$default", "c1", NULL, auth_text(U1)), 200);
    rtf.n_posts = 0;
    rtf.conn_conflicts = 0;
    rtf.after_conn_get = g_sub_chat;
    T_EQ_INT(ev("$default", "c1", NULL, sub_text(NW_WS_SUB, 12, NW_SUB_GAME, G)), 200);
    int i = rtf_conn_find("c1");
    T_EQ_INT(rtf.conns[i].n_topics, 2);
    T_EQ_STR(rtf.conns[i].topics[0], "chat:" G);
    T_EQ_STR(rtf.conns[i].topics[1], "gu-" G "-" U1);
    T_EQ_INT(rtf.conn_conflicts, 1);   // the game subscription lost once and ran again
    // Each acknowledged exactly once, the one that lost only after it stored.
    T_EQ_INT(rtf.n_posts, 2);
    T_EQ_INT(posted(0).ref, 9);
    T_EQ_INT(posted(1).ref, 12);
    // Both topics are pushed to.
    T_EQ_INT(CB_STORE_CALL(&rtf.store, conns_by_topic, "gu-" G "-" U1, count_conn, &(int){ 0 }), 1);
    T_EQ_INT(CB_STORE_CALL(&rtf.store, conns_by_topic, "chat:" G, count_conn, &(int){ 0 }), 1);
}

static void publish_posts_each_subscriber(void) {
    setup();
    static const char *const ids[] = { "a", "b", "other", "gone", "stale" };
    for (int k = 0; k < 5; k++) {
        ev("$connect", ids[k], "proto=nw1", NULL);
        ev("$default", ids[k], NULL, auth_text(U1));   // a spectator: U1 sits at no game here
        ev("$default", ids[k], NULL, sub_text(NW_WS_SUB, 1, NW_SUB_GAME, k == 2 ? "zzzzzz" : G));
    }
    T_EQ_INT(rtf.conns[rtf_conn_find("a")].n_topics, 1);
    T_EQ_STR(rtf.conns[rtf_conn_find("a")].topics[0], "game-" G);
    rtf.conns[rtf_conn_find("stale")].expires_at = cb_now_ms() - 1;   // the TTL has passed; DynamoDB has not swept it yet
    strcpy(rtf.gone[0], "gone");
    rtf.n_posts = 0;
    // No endpoint configured: the event's own domain is not known here, so pushes need CBASE_WS_ENDPOINT.
    const uint8_t cast[] = { 7 };
    rt_publish_cast(g_ctx.rt, "game-" G, G, 7, cast, 1);
    T_EQ_INT(rtf.n_posts, 0);
    strcpy(g_cfg.ws_endpoint, "https://abc.execute-api.us-west-1.amazonaws.com/prod");
    rt_publish_cast(g_ctx.rt, "game-" G, G, 7, cast, 1);
    T_EQ_INT(rtf.n_posts, 2);   // a and b (gone answered 410, which the fake does not record)
    int seen = 0;
    for (int i = 0; i < rtf.n_posts; i++) {
        NwFrame c = posted(i);
        T_CHECK(c.kind == NW_WS_CAST && c.version == 7 && c.blob.len == 1 && c.blob.p[0] == 7);
        T_CHECK(c.gid_len == 6 && memcmp(c.gid, G, 6) == 0);
        if (strcmp(rtf.posts[i].conn_id, "a") == 0) seen |= 1;
        if (strcmp(rtf.posts[i].conn_id, "b") == 0) seen |= 2;
        T_CHECK(strcmp(rtf.posts[i].conn_id, "stale") != 0 && strcmp(rtf.posts[i].conn_id, "other") != 0);
    }
    T_EQ_INT(seen, 3);
    T_EQ_INT(rtf_conn_find("gone"), -1);     // 410 deleted it
    T_CHECK(rtf_conn_find("other") >= 0);
    cb_rt_free(g_ctx.rt);
    g_ctx.rt = NULL;
}

int main(void) {
    T_RUN(connect_and_disconnect);
    T_RUN(nobody_writes_nothing);
    T_RUN(connect_only_through_the_edge);
    T_RUN(frames_round_trip_through_the_conn_row);
    T_RUN(a_subscription_says_who_asks);
    T_RUN(a_lost_row_is_rebuilt_and_garbage_closes);
    T_RUN(concurrent_frames_of_one_connection_both_stick);
    T_RUN(publish_posts_each_subscriber);
    rtf_reset();
    return T_DONE();
}
