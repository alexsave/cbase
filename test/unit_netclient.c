// unit_netclient.c - the client library every host runs (foolish/c/src/
// netclient.h), with no server: the session as a blob, what each request
// says, when the token is refreshed, how an answer is read, and the stream's
// own decisions (what to send in what order, retries, reconnects, pings, and
// the version gates).
#include "t.h"

#include "ask.h"
#include "netclient.h"
#include "netwire.h"
#include "roster.h"
#include "table.h"
#include "view.h"

#define T0 1700000000000LL

static void signed_in(uint64_t expires_at_ms) {
    NwSession s;
    memset(&s, 0, sizeof s);
    s.access_len = 3; memcpy(s.access, "jwt", 3);
    s.refresh_len = 2; memcpy(s.refresh, "rt", 2);
    s.expires_at_ms = expires_at_ms;
    s.user_id_len = 2; memcpy(s.user_id, "u1", 2);
    s.username_len = 3; memcpy(s.username, "ANA", 3);
    uint8_t b[64];
    int n = nw_session_encode(&s, b, sizeof b);
    nc_reset();
    T_EQ_INT(nc_session_load(b, n), NW_OK);
}

static void session_blob(void) {
    nc_reset();
    uint8_t b[NC_SESSION_BLOB_MAX];
    T_EQ_INT(nc_session_save(b, sizeof b), 0);   // signed out: nothing to keep
    T_EQ_INT(nc_session_info()->signed_in, 0);
    signed_in(T0 + 3600000);
    int n = nc_session_save(b, sizeof b);
    T_CHECK(n > 0);
    nc_reset();
    T_EQ_INT(nc_session_load(b, n), NW_OK);
    const NcSessionInfo *i = nc_session_info();
    T_CHECK(i->signed_in && i->username_len == 3 && memcmp(i->username, "ANA", 3) == 0);
    // A blob cut short signs the client out.
    T_EQ_INT(nc_session_load(b, n - 1), NW_E_WIRE);
    T_EQ_INT(nc_session_info()->signed_in, 0);
    T_EQ_INT(nc_session_save(b, sizeof b), 0);
}

static void requests(void) {
    nc_reset();
    // Nobody signed in: nothing that needs a user is built.
    T_CHECK(nc_req_games(T0) == NULL && nc_req_create(T0) == NULL && nc_req_refresh() == NULL);
    const NcRequest *q = nc_req_signin("ANA", 3, "pw", 2);
    T_CHECK(q && q->method == NC_POST && q->auth_len == 0 && !q->is_refresh);
    T_EQ_MEM(q->path, q->path_len, "/v1/auth/signin", 15);
    const uint8_t signin[] = { 1, NW_SIGNIN, 3, 'A', 'N', 'A', 2, 'p', 'w' };
    T_EQ_MEM(q->body, q->body_len, signin, sizeof signin);
    T_CHECK(nc_req_signup("ANA", 3, "x", NW_PASSWORD_MAX + 1) == NULL);
    q = nc_req_board();
    T_CHECK(q && q->method == NC_GET && q->auth_len == 0 && q->body_len == 0);

    signed_in(T0 + 3600000);
    q = nc_req_game("ab12cd", 6, T0);
    T_CHECK(q && q->method == NC_GET && !q->is_refresh);
    T_EQ_MEM(q->path, q->path_len, "/v1/games/ab12cd", 16);
    T_EQ_MEM(q->auth, q->auth_len, "Bearer jwt", 10);
    T_CHECK(nc_req_game("ab/../x", 7, T0) == NULL);   // never a path the id did not say
    q = nc_req_action("ab12cd", 6, (const uint8_t *)"\x03\x00", 2, 9, T0);
    const uint8_t action[] = { 2, 6, 'a', 'b', '1', '2', 'c', 'd', 9, 0, 0, 0, 3, 0 };   // table.h v2
    T_CHECK(q != NULL);
    if (q) T_EQ_MEM(q->body, q->body_len, action, sizeof action);
    q = nc_req_action("ab12cd", 6, (const uint8_t *)"\x03\x00", 2, -1, T0);
    const uint8_t action1[] = { 1, 6, 'a', 'b', '1', '2', 'c', 'd', 3, 0 };   // no intent: v1
    if (q) T_EQ_MEM(q->body, q->body_len, action1, sizeof action1);
    q = nc_req_chat_send("ab12cd", 6, "yo", 2, T0);
    T_EQ_MEM(q->path, q->path_len, "/v1/chat/ab12cd", 15);
    const uint8_t chat[] = { 1, NW_CHAT_SEND, 2, 0, 'y', 'o' };
    T_EQ_MEM(q->body, q->body_len, chat, sizeof chat);
    q = nc_req_signout(1, T0);
    const uint8_t out_g[] = { 1, NW_SIGNOUT, NW_SCOPE_GLOBAL };
    T_EQ_MEM(q->body, q->body_len, out_g, sizeof out_g);
    q = nc_req_stream();
    T_EQ_MEM(q->path, q->path_len, "/v1/stream?proto=nw1", 20);

    // Due within NC_REFRESH_EARLY_MS of the expiry: the refresh comes first.
    q = nc_req_games(T0 + 3600000 - NC_REFRESH_EARLY_MS - 1);
    T_CHECK(q && !q->is_refresh);
    q = nc_req_games(T0 + 3600000 - NC_REFRESH_EARLY_MS);
    T_CHECK(q && q->is_refresh && q->auth_len == 0);
    T_EQ_MEM(q->path, q->path_len, "/v1/auth/refresh", 16);
    const uint8_t refresh[] = { 1, NW_REFRESH, 2, 'r', 't' };
    T_EQ_MEM(q->body, q->body_len, refresh, sizeof refresh);
}

static void answers(void) {
    signed_in(T0 + 3600000);
    // A refused refresh for the token's own sake signs out; any other refusal does not.
    uint8_t r[ASK_REFUSAL_BYTES];
    AskRefusal ar = { ASK_WHY_ACCOUNT, NW_ACCOUNT_E_BAD_LOGIN, 0 };
    ask_refusal_encode(&ar, r, sizeof r);
    T_EQ_INT(nc_session_read(400, r, sizeof r), ASK_WHY_ACCOUNT);
    T_EQ_INT(nc_session_info()->signed_in, 1);
    ar.code = NW_ACCOUNT_E_REFRESH_USED;
    ask_refusal_encode(&ar, r, sizeof r);
    T_EQ_INT(nc_session_read(400, r, sizeof r), ASK_WHY_ACCOUNT);
    T_EQ_INT(nc_session_info()->signed_in, 0);
    T_EQ_INT(nc_error()->code, NW_ACCOUNT_E_REFRESH_USED);
    // No refusal body (a proxy's error): the status speaks.
    T_EQ_INT(nc_status_read(502, (const uint8_t *)"<html>", 6), ASK_WHY_INTERNAL);
    T_EQ_INT(nc_status_read(401, NULL, 0), ASK_WHY_AUTH);
    T_EQ_INT(nc_status_read(204, NULL, 0), 0);
    T_EQ_INT(nc_error()->why, 0);
    // A 200 that is not a session is not taken for one.
    T_EQ_INT(nc_session_read(200, (const uint8_t *)"\x01\x14", 2), ASK_WHY_MALFORMED);
    T_EQ_INT(nc_session_info()->signed_in, 0);

    const uint8_t act[] = { 1, 1, 7, 9, 0, 0, 0 };   // rejected, code 7, version 9
    const NcAction *a = nc_action_read(200, act, sizeof act);
    T_CHECK(a && a->status == 1 && a->reject_code == 7 && a->version == 9);
    T_CHECK(nc_action_read(200, act, 3) == NULL);

    const uint8_t view[] = { 1, NW_VIEW, 4, 0, 0, 0, 2, 0, 0, 0, 0xaa, 0xbb };
    const NcGameRow *g = nc_view_read(200, view, sizeof view);
    T_CHECK(g && g->version == 4 && g->env_off == 10 && g->env_len == 2);
    const uint8_t games[] = { 1, NW_GAMES, 1, 0, 3, 0, 0, 0, 1, 0, 0, 0, 0xcc };
    T_EQ_INT(nc_list_read(200, games, sizeof games, NW_GAMES), 1);
    g = nc_game_row(games, sizeof games, 0);
    T_CHECK(g && g->version == 3 && g->env_off == 12 && g->env_len == 1);
    T_CHECK(nc_game_row(games, sizeof games, 1) == NULL);
    T_EQ_INT(nc_list_read(200, games, sizeof games - 1, NW_GAMES), -ASK_WHY_MALFORMED);
}

// ---- the stream ---------------------------------------------------------------------------

static NwFrame next_frame(int64_t now) {
    NwFrame f;
    memset(&f, 0, sizeof f);
    const NcOut *o = nc_stream_next(now);
    if (!o) { f.kind = 0; return f; }
    static uint8_t raw[NW_ACCESS_MAX + 64];
    int n = nw_text_decode(o->text, o->len, raw, sizeof raw);
    T_CHECK(n > 0 && nw_frame_decode(raw, n, &f) == NW_OK);
    return f;
}

// The first event a feed made that is not the session's (a session loaded by
// the test is told with the next feed), or a NONE one; g_at is its index.
static int g_at;
static const NcEvent *first(int n) {
    static NcEvent none;
    for (g_at = 0; g_at < n; g_at++)
        if (nc_event(g_at)->kind != NC_EV_SESSION) return nc_event(g_at);
    memset(&none, 0, sizeof none);
    return &none;
}

static const NcEvent *feed(const NwFrame *f, int as_text, int64_t now) {
    static uint8_t raw[1024];
    static char text[1500];
    int n = nw_frame_encode(f, raw, sizeof raw);
    if (!as_text) return first(nc_stream_in(raw, n, 0, now));
    int t = nw_text_encode(raw, n, text, sizeof text);
    return first(nc_stream_in((const uint8_t *)text, t, 1, now));
}

static NwFrame ack(uint16_t ref, int why, int seated) {
    NwFrame f;
    memset(&f, 0, sizeof f);
    f.kind = NW_WS_ACK;
    f.ref = ref;
    f.why = (uint8_t)why;
    f.seated = (uint8_t)seated;
    return f;
}

static NwFrame push(int kind, const char *gid, uint32_t v, const uint8_t *p, uint32_t n) {
    NwFrame f;
    memset(&f, 0, sizeof f);
    f.kind = (uint8_t)kind;
    f.gid_len = (uint8_t)strlen(gid);
    memcpy(f.gid, gid, f.gid_len);
    f.version = v;
    f.blob.p = p;
    f.blob.len = n;
    return f;
}

static void stream_order_and_retries(void) {
    signed_in(T0 + 3600000);
    T_EQ_INT(nc_stream_sub(NW_SUB_GAMES, NULL, 0), NW_OK);
    T_EQ_INT(nc_stream_sub(NW_SUB_GAME, "g1", 2), NW_OK);
    T_EQ_INT(nc_stream_sub(NW_SUB_GAME, NULL, 0), NW_E_WIRE);
    T_EQ_INT(nc_stream_sub(9, "g1", 2), NW_E_WIRE);
    // Shut: nothing to send, nothing due.
    T_CHECK(nc_stream_next(T0) == NULL);
    T_EQ_INT(nc_stream_due_ms(T0), -1);
    nc_stream_opened(T0);
    T_EQ_INT(nc_stream_due_ms(T0), 0);
    // Each subscription says who asks (no AUTH frame it could overtake).
    NwFrame s1 = next_frame(T0), s2 = next_frame(T0);
    T_CHECK(s1.kind == NW_WS_SUB_AS && s1.what == NW_SUB_GAMES && s1.gid_len == 0);
    T_CHECK(s1.token_len == 3 && memcmp(s1.token, "jwt", 3) == 0);
    T_CHECK(s2.kind == NW_WS_SUB_AS && s2.what == NW_SUB_GAME && s2.gid_len == 2 && s2.ref != s1.ref && s2.version == 0);
    T_EQ_INT(next_frame(T0).kind, 0);
    T_EQ_INT(nc_stream_due_ms(T0), NC_PING_MS);

    // Accepted, and the game's says which stream it hears.
    NwFrame a = ack(s1.ref, 0, 0);
    T_EQ_INT(feed(&a, 1, T0)->kind, NC_EV_NONE);
    a = ack(s2.ref, 0, 1);
    const NcEvent *e = feed(&a, 0, T0);
    T_CHECK(e->kind == NC_EV_SEATED && e->seated == 1 && e->gid_len == 2);
    // An ACK for a ref nobody waits on is nothing.
    a = ack(999, 0, 0);
    T_EQ_INT(feed(&a, 0, T0)->kind, NC_EV_NONE);

    // A refused chat is retried after 0.5 s, then 1 s, then 2 s.
    T_EQ_INT(nc_stream_sub(NW_SUB_CHAT, "g1", 2), NW_OK);
    NwFrame c = next_frame(T0);
    T_CHECK(c.kind == NW_WS_SUB_AS && c.what == NW_SUB_CHAT);
    a = ack(c.ref, ASK_WHY_FORBIDDEN, 0);
    e = feed(&a, 0, T0);
    T_CHECK(e->kind == NC_EV_REFUSED && e->why == ASK_WHY_FORBIDDEN && e->what == NW_SUB_CHAT);
    T_EQ_INT(next_frame(T0 + NC_RETRY_FIRST_MS - 1).kind, 0);
    T_EQ_INT(nc_stream_due_ms(T0), NC_RETRY_FIRST_MS);
    c = next_frame(T0 + NC_RETRY_FIRST_MS);
    T_CHECK(c.kind == NW_WS_SUB_AS && c.what == NW_SUB_CHAT);
    a = ack(c.ref, ASK_WHY_FORBIDDEN, 0);
    feed(&a, 0, T0 + 1000);
    T_EQ_INT(next_frame(T0 + 1000 + 2 * NC_RETRY_FIRST_MS - 1).kind, 0);
    T_EQ_INT(next_frame(T0 + 1000 + 2 * NC_RETRY_FIRST_MS).kind, NW_WS_SUB_AS);
    // Held twice, let go once: still held, and nothing is sent.
    T_EQ_INT(nc_stream_sub(NW_SUB_GAME, "g1", 2), NW_OK);
    T_EQ_INT(nc_stream_unsub(NW_SUB_GAME, "g1", 2), NW_OK);
    T_EQ_INT(next_frame(T0 + 2000).kind, 0);
    // Let go and held again: asked again (the pending unsubscribe is moot).
    T_EQ_INT(nc_stream_unsub(NW_SUB_GAME, "g1", 2), NW_OK);
    T_EQ_INT(nc_stream_sub(NW_SUB_GAME, "g1", 2), NW_OK);
    // Refused for the token (an old server's race, or a token gone stale): asked
    // again later, with whatever token the session holds then.
    NwFrame s3 = next_frame(T0 + 2000);
    T_CHECK(s3.kind == NW_WS_SUB_AS && s3.what == NW_SUB_GAME);
    a = ack(s3.ref, ASK_WHY_AUTH, 0);
    feed(&a, 0, T0 + 2000);
    T_EQ_INT(next_frame(T0 + 2000).kind, 0);
    uint8_t blob[256];
    NwSession ses;
    T_EQ_INT(nw_session_decode(blob, nc_session_save(blob, sizeof blob), &ses), NW_OK);
    ses.access_len = 4; memcpy(ses.access, "jwt2", 4);
    T_EQ_INT(nc_session_load(blob, nw_session_encode(&ses, blob, sizeof blob)), NW_OK);
    s3 = next_frame(T0 + 2000 + NC_RETRY_FIRST_MS);
    T_CHECK(s3.kind == NW_WS_SUB_AS && s3.token_len == 4 && memcmp(s3.token, "jwt2", 4) == 0);

    // Unsubscribing a live subscription tells the server; one never sent does not.
    T_EQ_INT(nc_stream_unsub(NW_SUB_GAMES, NULL, 0), NW_OK);
    NwFrame f = next_frame(T0 + 2000);
    T_CHECK(f.kind == NW_WS_UNSUB && f.what == NW_SUB_GAMES);

    // The ping, every NC_PING_MS.
    while (next_frame(T0 + 10000).kind) {}
    T_EQ_INT(next_frame(T0 + NC_PING_MS - 1).kind, 0);
    T_EQ_INT(next_frame(T0 + NC_PING_MS).kind, NW_WS_PING);
    T_EQ_INT(next_frame(T0 + NC_PING_MS).kind, 0);

    // Closed: the reopen waits 0.5 s, doubling to 10 s, and starts over once a frame arrives.
    T_EQ_INT(nc_stream_closed(T0), NC_RETRY_FIRST_MS);
    T_EQ_INT(nc_stream_closed(T0), 2 * NC_RETRY_FIRST_MS);
    for (int i = 0; i < 10; i++) nc_stream_closed(T0);
    T_EQ_INT(nc_stream_closed(T0), NC_RETRY_MAX_MS);
    T_CHECK(nc_stream_next(T0) == NULL);
    nc_stream_opened(T0);
    NwFrame pong;
    memset(&pong, 0, sizeof pong);
    pong.kind = NW_WS_PONG;
    feed(&pong, 0, T0);
    T_EQ_INT(nc_stream_closed(T0), NC_RETRY_FIRST_MS);
    // Reopened: every subscription again, the game's with what the client holds.
    nc_stream_opened(T0);
    int subs = 0;
    for (NwFrame x; (x = next_frame(T0)).kind == NW_WS_SUB_AS;) subs++;
    T_EQ_INT(subs, 2);   // the game and the chat; the dashboard was dropped
    // Signed out: nothing at all.
    nc_reset();
    nc_stream_opened(T0);
    T_CHECK(nc_stream_next(T0) == NULL);
}

static void gates(void) {
    signed_in(T0 + 3600000);
    nc_stream_opened(T0);
    const uint8_t env[] = { 1, 2, 3 };
    NwFrame v = push(NW_WS_VIEW, "g1", 5, env, 3);
    const NcEvent *e = feed(&v, 0, T0);
    T_CHECK(e->kind == NC_EV_VIEW && e->version == 5 && e->payload_len == 3 && e->what == NW_SUB_GAMES);
    T_EQ_MEM(nc_event_payload(g_at), e->payload_len, env, 3);
    // At or below what was applied: stale.
    T_EQ_INT(feed(&v, 0, T0)->kind, NC_EV_NONE);
    v.version = 4;
    T_EQ_INT(feed(&v, 1, T0)->kind, NC_EV_NONE);
    v.version = 6;
    T_EQ_INT(feed(&v, 1, T0)->kind, NC_EV_VIEW);
    // Casts have a gate of their own.
    NwFrame c = push(NW_WS_CAST, "g1", 6, env, 1);
    T_EQ_INT(feed(&c, 0, T0)->kind, NC_EV_CAST);
    T_EQ_INT(feed(&c, 0, T0)->kind, NC_EV_NONE);
    // Another game is its own.
    NwFrame o = push(NW_WS_VIEW, "g2", 1, env, 3);
    T_EQ_INT(feed(&o, 0, T0)->kind, NC_EV_VIEW);
    // Held on its own stream, a game's dashboard views are not handed out (its
    // casts animate it), and do not count as applied; let go, they are again.
    T_EQ_INT(nc_stream_sub(NW_SUB_GAME, "g2", 2), NW_OK);
    o.version = 2;
    T_EQ_INT(feed(&o, 0, T0)->kind, NC_EV_NONE);
    T_EQ_INT(nc_stream_unsub(NW_SUB_GAME, "g2", 2), NW_OK);
    T_EQ_INT(feed(&o, 0, T0)->kind, NC_EV_VIEW);
    // A resubscription carries what is held, so the server catches up only what is newer.
    T_EQ_INT(nc_stream_sub(NW_SUB_GAME, "g1", 2), NW_OK);
    NwFrame s;
    while ((s = next_frame(T0)).kind && s.kind != NW_WS_SUB_AS) {}
    T_CHECK(s.kind == NW_WS_SUB_AS && s.version == 6);
    T_EQ_INT(nc_stream_unsub(NW_SUB_GAME, "g1", 2), NW_OK);
    // The resync: the gate forgets, and an older view applies.
    nc_gate_reset("g1", 2);
    v.version = 2;
    T_EQ_INT(feed(&v, 0, T0)->kind, NC_EV_VIEW);
    // The seat is gone: always applied, and the gate starts over.
    NwFrame gone = push(NW_WS_VIEW, "g1", 0, NULL, 0);
    e = feed(&gone, 0, T0);
    T_CHECK(e->kind == NC_EV_VIEW && e->payload_len == 0);
    v.version = 1;
    T_EQ_INT(feed(&v, 0, T0)->kind, NC_EV_VIEW);
    // Many games: the least recently seen gives way, and nothing breaks.
    char gid[16];
    for (int i = 0; i < NC_GATES_MAX + 5; i++) {
        snprintf(gid, sizeof gid, "x%d", i);
        NwFrame w = push(NW_WS_VIEW, gid, 1, env, 3);
        T_EQ_INT(feed(&w, 0, T0)->kind, NC_EV_VIEW);
    }
    // Not this protocol.
    T_EQ_INT(first(nc_stream_in((const uint8_t *)"!!!!", 4, 1, T0))->kind, NC_EV_BAD);
    T_EQ_INT(first(nc_stream_in((const uint8_t *)"\x01\x29", 2, 0, T0))->kind, NC_EV_BAD);   // a client's frame
    NwFrame chat;
    memset(&chat, 0, sizeof chat);
    chat.kind = NW_WS_CHAT;
    chat.gid_len = 2;
    memcpy(chat.gid, "g1", 2);
    chat.chat.message_len = 2;
    memcpy(chat.chat.message, "hi", 2);
    e = feed(&chat, 1, T0);
    T_CHECK(e->kind == NC_EV_CHAT && e->chat.message_len == 2 && memcmp(e->chat.message, "hi", 2) == 0);
}

// ---- the pump ------------------------------------------------------------------------

// A session answer, as the server writes it.
static int session_body(uint8_t *b, int cap, const char *access, uint64_t expires) {
    NwSession s;
    memset(&s, 0, sizeof s);
    s.access_len = (uint16_t)strlen(access); memcpy(s.access, access, s.access_len);
    s.refresh_len = 3; memcpy(s.refresh, "rt2", 3);
    s.expires_at_ms = expires;
    s.user_id_len = 2; memcpy(s.user_id, "u1", 2);
    s.username_len = 3; memcpy(s.username, "ANA", 3);
    return nw_session_encode(&s, b, cap);
}

static NcIntent intent(int kind, const char *gid) {
    NcIntent in;
    memset(&in, 0, sizeof in);
    in.kind = (uint8_t)kind;
    if (gid) { in.gid_len = (uint8_t)strlen(gid); memcpy(in.gid, gid, in.gid_len); }
    in.intent = -1;
    return in;
}

static int path_is(const NcRequest *q, const char *path) {
    return q && q->path_len == strlen(path) && memcmp(q->path, path, q->path_len) == 0;
}

static int refusal_body(uint8_t *b, int why, int code) {
    AskRefusal r;
    memset(&r, 0, sizeof r);
    r.why = (uint8_t)why;
    r.code = code;
    return ask_refusal_encode(&r, b, 16);
}

static void pump_signs_in_and_reads_answers(void) {
    nc_reset();
    // Nobody signed in: an intent that needs the user is refused at once; the leaderboard is not.
    NcIntent games = intent(NC_DO_GAMES, NULL);
    T_EQ_INT(nc_do(&games, T0), 0);
    T_EQ_INT(nc_error()->why, ASK_WHY_AUTH);
    T_EQ_INT(nc_pump_due_ms(T0), -1);

    NcIntent in = intent(NC_DO_SIGN_IN, NULL);
    in.name_len = 3; memcpy(in.name, "ANA", 3);
    in.password_len = 2; memcpy(in.password, "pw", 2);
    uint32_t t = nc_do(&in, T0);
    T_CHECK(t != 0);
    T_EQ_INT(nc_pump_due_ms(T0), 0);
    const NcRequest *q = nc_next_request(T0);
    T_CHECK(path_is(q, "/v1/auth/signin") && q->method == NC_POST && q->auth_len == 0 && q->id != 0);
    const uint8_t signin[] = { 1, NW_SIGNIN, 3, 'A', 'N', 'A', 2, 'p', 'w' };
    T_EQ_MEM(q->body, q->body_len, signin, sizeof signin);
    uint32_t id = q->id;
    T_CHECK(nc_next_request(T0) == NULL);   // nothing else to send
    uint8_t b[256];
    int n = session_body(b, sizeof b, "jwt", T0 + 3600000);
    const NcDone *d = nc_on_response(id, 200, b, n, T0);
    T_CHECK(d && d->ticket == t && d->kind == NC_DO_SIGN_IN && d->why == 0);
    T_EQ_INT(nc_session_info()->signed_in, 1);
    T_CHECK(nc_on_response(id, 200, b, n, T0) == NULL);   // the same answer twice finishes nothing

    // A move: the request is the kernel's, the answer read as a table response.
    NcIntent play = intent(NC_DO_PLAY, "ab12cd");
    play.bytes_len = 2; play.bytes[0] = 3; play.bytes[1] = 0;
    play.intent = 9;
    t = nc_do(&play, T0);
    q = nc_next_request(T0);
    T_CHECK(path_is(q, "/v1/action"));
    T_EQ_MEM(q->auth, q->auth_len, "Bearer jwt", 10);
    const uint8_t action[] = { 2, 6, 'a', 'b', '1', '2', 'c', 'd', 9, 0, 0, 0, 3, 0 };
    T_EQ_MEM(q->body, q->body_len, action, sizeof action);
    uint8_t r[TABLE_RESP_BYTES];
    T_EQ_INT(table_action_response(TABLE_REJECTED, 4, 11, r, sizeof r), TABLE_RESP_BYTES);
    d = nc_on_response(q->id, 200, r, sizeof r, T0);
    T_CHECK(d && d->ticket == t && d->why == 0 && d->action_status == TABLE_STATUS_REJECTED && d->reject_code == 4 && d->version == 11);

    // One game: the view's version and the envelope's span.
    NcIntent open = intent(NC_DO_OPEN_GAME, "ab12cd");
    t = nc_do(&open, T0);
    q = nc_next_request(T0);
    T_CHECK(path_is(q, "/v1/games/ab12cd") && q->method == NC_GET);
    const uint8_t env[] = { 0xe1, 0xe2, 0xe3 };
    NwBlob eb = { env, 3 };
    n = nw_view_encode(7, eb, b, sizeof b);
    d = nc_on_response(q->id, 200, b, n, T0);
    T_CHECK(d && d->version == 7 && d->a_len == 3);
    if (d) T_EQ_MEM(b + d->a_off, d->a_len, env, 3);
    // A refusal is read as one.
    t = nc_do(&open, T0);
    q = nc_next_request(T0);
    n = refusal_body(b, ASK_WHY_NOT_FOUND, 0);
    d = nc_on_response(q->id, 404, b, n, T0);
    T_CHECK(d && d->ticket == t && d->why == ASK_WHY_NOT_FOUND && d->status == 404);

    // A list: its rows and their kind; the history carries the rating.
    NcIntent hist = intent(NC_DO_HISTORY, NULL);
    t = nc_do(&hist, T0);
    q = nc_next_request(T0);
    T_CHECK(path_is(q, "/v1/history"));
    n = nw_history_head(1, 1234, 0, b, sizeof b);
    d = nc_on_response(q->id, 200, b, n, T0);
    T_CHECK(d && d->list_kind == NW_HISTORY && d->n_rows == 0 && d->has_rating == 1 && d->rating == 1234);
}

static void pump_refreshes_first_and_after_a_401(void) {
    signed_in(T0 + 30000);   // within NC_REFRESH_EARLY_MS of expiring
    NcIntent games = intent(NC_DO_GAMES, NULL), board = intent(NC_DO_LEADERBOARD, NULL);
    uint32_t tg = nc_do(&games, T0), tb = nc_do(&board, T0);
    T_CHECK(tg && tb);
    const NcRequest *q = nc_next_request(T0);
    T_CHECK(path_is(q, "/v1/auth/refresh"));
    uint32_t refresh = q->id;
    // While it is out, only what needs nobody goes.
    q = nc_next_request(T0);
    T_CHECK(path_is(q, "/v1/leaderboard"));
    uint32_t board_id = q->id;
    T_CHECK(nc_next_request(T0) == NULL);
    T_EQ_INT(nc_pump_due_ms(T0), -1);   // the refresh's answer wakes the rest
    uint8_t b[256];
    int n = session_body(b, sizeof b, "jwt2", T0 + 3600000);
    T_CHECK(nc_on_response(refresh, 200, b, n, T0) == NULL);
    q = nc_next_request(T0);
    T_CHECK(path_is(q, "/v1/games"));
    T_EQ_MEM(q->auth, q->auth_len, "Bearer jwt2", 11);
    uint32_t games_id = q->id;
    n = nw_list_head(NW_BOARD, 0, b, sizeof b);
    const NcDone *d = nc_on_response(board_id, 200, b, n, T0);
    T_CHECK(d && d->ticket == tb && d->list_kind == NW_BOARD);

    // A 401: refresh, then the same request once more.
    T_CHECK(nc_on_response(games_id, 401, NULL, 0, T0) == NULL);
    q = nc_next_request(T0);
    T_CHECK(path_is(q, "/v1/auth/refresh"));
    n = session_body(b, sizeof b, "jwt3", T0 + 3600000);
    nc_on_response(q->id, 200, b, n, T0);
    q = nc_next_request(T0);
    T_CHECK(path_is(q, "/v1/games"));
    T_EQ_MEM(q->auth, q->auth_len, "Bearer jwt3", 11);
    // A second 401 is the answer.
    d = nc_on_response(q->id, 401, NULL, 0, T0);
    T_CHECK(d && d->ticket == tg && d->why == ASK_WHY_AUTH);

    // A refresh refused for its token ends the session and what waited on it.
    signed_in(T0 + 30000);
    tg = nc_do(&games, T0);
    q = nc_next_request(T0);
    T_CHECK(path_is(q, "/v1/auth/refresh"));
    n = refusal_body(b, ASK_WHY_ACCOUNT, NW_ACCOUNT_E_BAD_REFRESH);
    T_CHECK(nc_on_response(q->id, 400, b, n, T0) == NULL);
    T_EQ_INT(nc_session_info()->signed_in, 0);
    T_EQ_INT(nc_pump_due_ms(T0), 0);
    d = nc_next_done();
    T_CHECK(d && d->ticket == tg && d->why == ASK_WHY_AUTH);
    T_CHECK(nc_next_done() == NULL);
}

static void pump_retries_reads_not_writes(void) {
    signed_in(T0 + 3600000);
    NcIntent games = intent(NC_DO_GAMES, NULL);
    uint32_t t = nc_do(&games, T0);
    int64_t now = T0;
    for (int k = 1; k < NC_PUMP_TRIES; k++) {
        const NcRequest *q = nc_next_request(now);
        T_CHECK(path_is(q, "/v1/games"));
        T_CHECK(nc_on_response(q->id, 0, NULL, 0, now) == NULL);   // it never arrived
        int due = nc_pump_due_ms(now);
        T_CHECK(due > 0 && due <= NC_RETRY_MAX_MS);
        T_CHECK(nc_next_request(now + due - 1) == NULL);           // not before its backoff
        now += due;
    }
    const NcRequest *q = nc_next_request(now);
    T_CHECK(path_is(q, "/v1/games"));
    const NcDone *d = nc_on_response(q->id, 0, NULL, 0, now);
    T_CHECK(d && d->ticket == t && d->why == ASK_WHY_INTERNAL && d->status == 0);

    // A write that never arrived is not sent again: it may have landed.
    NcIntent create = intent(NC_DO_CREATE, NULL);
    t = nc_do(&create, now);
    q = nc_next_request(now);
    T_CHECK(path_is(q, "/v1/create"));
    d = nc_on_response(q->id, 0, NULL, 0, now);
    T_CHECK(d && d->ticket == t && d->why == ASK_WHY_INTERNAL);
    T_CHECK(nc_next_request(now + NC_RETRY_MAX_MS) == NULL);

    // At most NC_PUMP_MAX wait at once.
    for (int i = 0; i < NC_PUMP_MAX; i++) T_CHECK(nc_do(&games, now) != 0);
    T_EQ_INT(nc_do(&games, now), 0);
    T_EQ_INT(nc_error()->why, ASK_WHY_INTERNAL);
}

static void pump_signs_out(void) {
    signed_in(T0 + 3600000);
    NcIntent games = intent(NC_DO_GAMES, NULL), out = intent(NC_DO_SIGN_OUT, NULL);
    out.flag = 1;
    uint32_t to = nc_do(&out, T0);
    const NcRequest *q = nc_next_request(T0);
    T_CHECK(path_is(q, "/v1/auth/signout"));
    const uint8_t out_g[] = { 1, NW_SIGNOUT, NW_SCOPE_GLOBAL };
    T_EQ_MEM(q->body, q->body_len, out_g, sizeof out_g);
    uint32_t tg = nc_do(&games, T0);   // still signed in when asked
    T_CHECK(tg != 0);
    // Whatever the server answers, the session is over here, and so is what waited for it.
    const NcDone *d = nc_on_response(q->id, 500, NULL, 0, T0);
    T_CHECK(d && d->ticket == to);
    T_EQ_INT(nc_session_info()->signed_in, 0);
    d = nc_next_done();
    T_CHECK(d && d->ticket == tg && d->why == ASK_WHY_AUTH);
}

// ---- one state, two feeds ----------------------------------------------------------------

// What a host does with what changed: the board it shows for one game (the
// last payload it was handed for the table, and its version), its dashboard
// row, and every event kind it was told.
typedef struct {
    uint8_t  table[64];  int table_len;  uint32_t table_v;
    uint8_t  row[64];    int row_len;    uint32_t row_v;
    int      views, casts, refetched, sessions, tickets, stale;
} host_t;

// Every event the last feed made: nc_event(i) until NULL, as a host reads them.
static void host_events(host_t *h) {
    const NcEvent *e;
    for (int i = 0; (e = nc_event(i)); i++) {
        const uint8_t *p = nc_event_payload(i);
        int len = (int)e->payload_len;
        if (e->kind == NC_EV_SESSION) h->sessions++;
        if (e->kind == NC_EV_CAST || (e->kind == NC_EV_VIEW && e->what == NW_SUB_GAME)) {
            if (e->kind == NC_EV_CAST) h->casts++; else h->refetched++;
            memcpy(h->table, p, (size_t)len); h->table_len = len; h->table_v = e->version;
        } else if (e->kind == NC_EV_VIEW) {
            h->views++;
            memcpy(h->row, p, (size_t)len); h->row_len = len; h->row_v = e->version;
        }
    }
}

static void host_done(host_t *h, const NcDone *d, const uint8_t *body) {
    if (d) h->tickets++;
    if (d && d->stale) h->stale++;
    if (!d || d->why || d->stale || !d->a_len) return;
    memcpy(h->table, body + d->a_off, d->a_len); h->table_len = (int)d->a_len; h->table_v = d->version;
}

static void host_push(host_t *h, int kind, const char *gid, uint32_t v, const uint8_t *p, uint32_t n, int64_t now) {
    static uint8_t raw[1024];
    NwFrame f = push(kind, gid, v, p, n);
    int len = nw_frame_encode(&f, raw, sizeof raw);
    nc_stream_in(raw, len, 0, now);
    host_events(h);
}

static void host_answer(host_t *h, uint32_t id, uint32_t v, const uint8_t *env, uint32_t n, int64_t now) {
    static uint8_t b[256];
    NwBlob eb = { env, n };
    int len = nw_view_encode(v, eb, b, sizeof b);
    const NcDone *d = nc_on_response(id, 200, b, len, now);
    host_events(h);
    host_done(h, d, b);
}

// The one the coordinator asked for: a push and a stale answer for the same game
// reach the client in both orders, and the board ends the same.
static void a_cast_and_a_stale_answer_end_alike(void) {
    const uint8_t old_env[] = { 0xe4 }, cast5[] = { 0x55, 0x05 }, view5[] = { 0x5f };
    host_t end[2][2];
    for (int kind = 0; kind < 2; kind++) {           // the push: a cast (the table's), a view (the dashboard's)
        for (int order = 0; order < 2; order++) {    // 0: the push first; 1: the answer first
            signed_in(T0 + 3600000);
            nc_stream_opened(T0);
            host_t *h = &end[kind][order];
            memset(h, 0, sizeof *h);
            // The table already holds version 4 (a cast).
            uint8_t c4[] = { 0x44 };
            host_push(h, NW_WS_CAST, "g1", 4, c4, 1, T0);
            NcIntent open = intent(NC_DO_OPEN_GAME, "g1");
            T_CHECK(nc_do(&open, T0) != 0);
            const NcRequest *q = nc_next_request(T0);
            T_CHECK(path_is(q, "/v1/games/g1"));
            uint32_t id = q->id;
            // The server answered at 4, then committed 5 and pushed it: the
            // answer is stale whichever reaches the client first.
            if (order == 0) {
                host_push(h, kind == 0 ? NW_WS_CAST : NW_WS_VIEW, "g1", 5, kind == 0 ? cast5 : view5, kind == 0 ? 2 : 1, T0);
                host_answer(h, id, 4, old_env, 1, T0);
            } else {
                host_answer(h, id, 4, old_env, 1, T0);
                host_push(h, kind == 0 ? NW_WS_CAST : NW_WS_VIEW, "g1", 5, kind == 0 ? cast5 : view5, kind == 0 ? 2 : 1, T0);
            }
            T_CHECK(nc_next_request(T0 + 60000) == NULL);   // nothing left to fetch
        }
        host_t *a = &end[kind][0], *b = &end[kind][1];
        T_EQ_INT(a->table_v, b->table_v);
        T_EQ_MEM(a->table, a->table_len, b->table, b->table_len);
        T_EQ_INT(a->row_v, b->row_v);
        T_EQ_MEM(a->row, a->row_len, b->row, b->row_len);
    }
    T_EQ_INT(end[0][0].table_v, 5);   // the cast: the table is at 5 whichever came first
    T_EQ_INT(end[1][0].row_v, 5);     // the view: the dashboard is
}

static void a_skipped_version_refetches(void) {
    signed_in(T0 + 3600000);
    nc_stream_opened(T0);
    host_t h;
    memset(&h, 0, sizeof h);
    const uint8_t c[] = { 0xc5 }, c7[] = { 0xc7 }, c8[] = { 0xc8 }, env7[] = { 0xe7 };
    host_push(&h, NW_WS_CAST, "g1", 5, c, 1, T0);
    T_CHECK(nc_next_request(T0) == NULL);
    // 6 never came: 7 is not applied on a board it does not follow; the game is fetched.
    host_push(&h, NW_WS_CAST, "g1", 7, c7, 1, T0);
    T_EQ_INT(h.casts, 1);
    T_EQ_INT(h.table_v, 5);
    T_EQ_INT(nc_pump_due_ms(T0), 0);
    const NcRequest *q = nc_next_request(T0);
    T_CHECK(path_is(q, "/v1/games/g1"));
    T_CHECK(q && nc_next_request(T0) == NULL);
    host_push(&h, NW_WS_CAST, "g1", 7, c7, 1, T0);
    T_EQ_INT(h.casts, 1);
    T_CHECK(nc_next_request(T0) == NULL);   // one fetch, however many gaps
    // Its answer is the host's as an event (no ticket of its own), and the casts go on from it.
    host_answer(&h, q->id, 7, env7, 1, T0);
    T_EQ_INT(h.refetched, 1);
    T_EQ_INT(h.table_v, 7);
    T_EQ_MEM(h.table, h.table_len, env7, 1);
    T_EQ_INT(h.tickets, 0);
    T_CHECK(nc_next_done() == NULL);
    // The host's own read of the game at that very version: stale, nothing to take.
    NcIntent open = intent(NC_DO_OPEN_GAME, "g1");
    nc_do(&open, T0);
    q = nc_next_request(T0);
    const uint8_t again[] = { 0xa7 };
    host_answer(&h, q->id, 7, again, 1, T0);
    T_EQ_INT(h.tickets, 1);
    T_EQ_INT(h.stale, 1);
    T_EQ_MEM(h.table, h.table_len, env7, 1);
    // A dashboard view at 9 the table may not have shown: an answer at 9 is taken.
    const uint8_t v9[] = { 0x99 }, e9[] = { 0xe9 };
    host_push(&h, NW_WS_VIEW, "g1", 9, v9, 1, T0);
    nc_do(&open, T0);
    q = nc_next_request(T0);
    host_answer(&h, q->id, 9, e9, 1, T0);
    T_EQ_INT(h.stale, 1);
    T_EQ_INT(h.table_v, 9);
    T_EQ_MEM(h.table, h.table_len, e9, 1);
    host_push(&h, NW_WS_CAST, "g1", 7, c7, 1, T0);
    T_EQ_INT(h.casts, 1);
    host_push(&h, NW_WS_CAST, "g1", 10, c8, 1, T0);
    T_EQ_INT(h.casts, 2);
    T_EQ_INT(h.table_v, 10);
}

static void a_refused_move_catches_up(void) {
    uint8_t r[TABLE_RESP_BYTES];
    const uint8_t c[] = { 0xc5 };
    for (int pushed = 0; pushed < 2; pushed++) {
        signed_in(T0 + 3600000);
        nc_stream_opened(T0);
        host_t h;
        memset(&h, 0, sizeof h);
        host_push(&h, NW_WS_VIEW, "g1", 5, c, 1, T0);
        NcIntent play = intent(NC_DO_PLAY, "g1");
        play.bytes_len = 2; play.bytes[0] = 3; play.bytes[1] = 0;
        play.intent = 5;
        nc_do(&play, T0);
        const NcRequest *q = nc_next_request(T0);
        T_CHECK(path_is(q, "/v1/action"));
        // Refused: the server is at 9.
        T_EQ_INT(table_action_response(TABLE_REJECTED, 4, 9, r, sizeof r), TABLE_RESP_BYTES);
        const NcDone *d = nc_on_response(q->id, 200, r, sizeof r, T0);
        T_CHECK(d && d->action_status == TABLE_STATUS_REJECTED);
        // The pushes get a moment to arrive.
        T_EQ_INT(nc_pump_due_ms(T0), NC_RECONCILE_MS);
        T_CHECK(nc_next_request(T0 + NC_RECONCILE_MS - 1) == NULL);
        if (pushed) {
            // They did: nothing is fetched.
            uint8_t c9[] = { 0xc9 };
            host_push(&h, NW_WS_CAST, "g1", 9, c9, 1, T0 + 10);
            T_CHECK(nc_next_request(T0 + NC_RECONCILE_MS) == NULL);
            T_EQ_INT(nc_pump_due_ms(T0 + NC_RECONCILE_MS), -1);
        } else {
            q = nc_next_request(T0 + NC_RECONCILE_MS);
            T_CHECK(path_is(q, "/v1/games/g1"));
        }
    }
    // A move that applied fetches nothing.
    signed_in(T0 + 3600000);
    NcIntent play = intent(NC_DO_PLAY, "g1");
    play.bytes_len = 2; play.bytes[0] = 3;
    nc_do(&play, T0);
    const NcRequest *q = nc_next_request(T0);
    T_EQ_INT(table_action_response(TABLE_APPLIED, 0, 9, r, sizeof r), TABLE_RESP_BYTES);
    nc_on_response(q->id, 200, r, sizeof r, T0);
    T_EQ_INT(nc_pump_due_ms(T0), -1);
}

static void accepted_again_refetches(void) {
    signed_in(T0 + 3600000);
    T_EQ_INT(nc_stream_sub(NW_SUB_GAME, "g1", 2), NW_OK);
    T_EQ_INT(nc_stream_sub(NW_SUB_CHAT, "g1", 2), NW_OK);
    nc_stream_opened(T0);
    NwFrame s = next_frame(T0), c = next_frame(T0);
    NwFrame a = ack(s.ref, 0, 1);
    feed(&a, 0, T0);
    a = ack(c.ref, 0, 0);
    feed(&a, 0, T0);
    T_CHECK(nc_next_request(T0) == NULL);   // the first acceptance missed nothing
    // The socket reopened: what was pushed meanwhile is gone, so the game is fetched.
    nc_stream_closed(T0);
    nc_stream_opened(T0);
    while ((s = next_frame(T0)).kind && !(s.kind == NW_WS_SUB_AS && s.what == NW_SUB_GAME)) {}
    a = ack(s.ref, 0, 1);
    feed(&a, 0, T0);
    const NcRequest *q = nc_next_request(T0);
    T_CHECK(path_is(q, "/v1/games/g1"));
    T_CHECK(nc_next_request(T0) == NULL);   // the chat's acceptance fetches no game
    // Refused (a membership the server did not see yet), then accepted: fetched too.
    nc_on_response(q->id, 0, NULL, 0, T0);
    nc_reset();
    signed_in(T0 + 3600000);
    nc_stream_sub(NW_SUB_GAME, "g2", 2);
    nc_stream_opened(T0);
    s = next_frame(T0);
    a = ack(s.ref, ASK_WHY_FORBIDDEN, 0);
    feed(&a, 0, T0);
    T_CHECK(nc_next_request(T0) == NULL);
    s = next_frame(T0 + NC_RETRY_FIRST_MS);
    a = ack(s.ref, 0, 1);
    feed(&a, 0, T0 + NC_RETRY_FIRST_MS);
    T_CHECK(path_is(nc_next_request(T0 + NC_RETRY_FIRST_MS), "/v1/games/g2"));
}

// An envelope as the server writes one: seat 0 of a one-seat lobby of `gid` at `v`.
static int envelope(const char *gid, uint32_t v, uint8_t *out, int cap) {
    int at = env_header_write(out, cap, 0, v, NULL);
    T_CHECK(at > 0);
    env_header_set_view_len(out, at, 2);
    static Roster r;
    memset(&r, 0, sizeof r);
    T_EQ_INT(roster_seat_add(&r, "u1", 2, "ANA", 3, NULL, 0), 0);
    int t = roster_trailer_write(&r, gid, (int)strlen(gid), 0, 0, 1, out + at + 2, cap - at - 2);
    T_CHECK(t > 0);
    return at + 2 + t;
}

static void create_and_join_move_the_subscription(void) {
    signed_in(T0 + 3600000);
    nc_stream_opened(T0);
    uint8_t b[512];
    // A create: the new game is named, and heard from now on.
    NcIntent create = intent(NC_DO_CREATE, NULL);
    nc_do(&create, T0);
    const NcRequest *q = nc_next_request(T0);
    int n = envelope("newg01", 1, b, sizeof b);
    const NcDone *d = nc_on_response(q->id, 200, b, n, T0);
    T_CHECK(d && d->why == 0 && d->gid_len == 6 && memcmp(d->gid, "newg01", 6) == 0);
    T_CHECK(d && d->version == 1 && d->a_off == 0 && (int)d->a_len == n && !d->stale);
    NwFrame s = next_frame(T0);
    T_CHECK(s.kind == NW_WS_SUB_AS && s.what == NW_SUB_GAME && s.gid_len == 6 && memcmp(s.gid, "newg01", 6) == 0);
    // The host holds it and lets it go: it is let go.
    nc_stream_sub(NW_SUB_GAME, "newg01", 6);
    nc_stream_unsub(NW_SUB_GAME, "newg01", 6);
    T_EQ_INT(next_frame(T0).kind, NW_WS_UNSUB);

    // Watching g2, then joining it: asked again, so the seat's stream is heard.
    nc_stream_sub(NW_SUB_GAME, "g2", 2);
    s = next_frame(T0);
    NwFrame a = ack(s.ref, 0, 0);
    T_EQ_INT(feed(&a, 0, T0)->seated, 0);
    TableAsk ask;
    memset(&ask, 0, sizeof ask);
    ask.kind = ASK_JOIN;
    ask.gid_len = 2; memcpy(ask.gid, "g2", 2);
    NcIntent join = intent(NC_DO_ASK, NULL);
    int an = ask_encode(&ask, join.bytes, sizeof join.bytes);
    T_CHECK(an > 0);
    join.bytes_len = (uint16_t)an;
    nc_do(&join, T0);
    q = nc_next_request(T0);
    T_CHECK(path_is(q, "/v1/meta"));
    n = envelope("g2", 8, b, sizeof b);
    d = nc_on_response(q->id, 200, b, n, T0);
    T_CHECK(d && d->gid_len == 2 && d->version == 8 && (int)d->a_len == n);
    s = next_frame(T0);
    T_CHECK(s.kind == NW_WS_SUB_AS && s.what == NW_SUB_GAME && s.version == 8);
    // The join's own cast went out before the seat could hear it: at 8 it is stale.
    const uint8_t c[] = { 1 };
    NwFrame c8 = push(NW_WS_CAST, "g2", 8, c, 1);
    T_EQ_INT(feed(&c8, 0, T0)->kind, NC_EV_NONE);
    // Any other ask's answer is not a board: its cast animates instead.
    ask.kind = ASK_START;
    NcIntent start = intent(NC_DO_ASK, NULL);
    start.bytes_len = (uint16_t)ask_encode(&ask, start.bytes, sizeof start.bytes);
    nc_do(&start, T0);
    q = nc_next_request(T0);
    n = envelope("g2", 9, b, sizeof b);
    d = nc_on_response(q->id, 200, b, n, T0);
    T_CHECK(d && d->why == 0 && d->gid_len == 2 && d->a_len == 0);
    NwFrame c9 = push(NW_WS_CAST, "g2", 9, c, 1);
    T_EQ_INT(feed(&c9, 0, T0)->kind, NC_EV_CAST);
}

static void the_session_is_told_once(void) {
    nc_reset();
    NcIntent in = intent(NC_DO_SIGN_IN, NULL);
    in.name_len = 3; memcpy(in.name, "ANA", 3);
    in.password_len = 2; memcpy(in.password, "pw", 2);
    nc_do(&in, T0);
    const NcRequest *q = nc_next_request(T0);
    uint8_t b[256];
    int n = session_body(b, sizeof b, "jwt", T0 + 30000);
    nc_on_response(q->id, 200, b, n, T0);
    host_t h;
    memset(&h, 0, sizeof h);
    host_events(&h);
    T_EQ_INT(h.sessions, 1);
    T_CHECK(nc_event(1) == NULL);
    // A refresh made by the pump itself is told with the answer that took it.
    NcIntent games = intent(NC_DO_GAMES, NULL);
    nc_do(&games, T0);
    q = nc_next_request(T0);
    T_CHECK(path_is(q, "/v1/auth/refresh"));
    n = session_body(b, sizeof b, "jwt2", T0 + 3600000);
    nc_on_response(q->id, 200, b, n, T0);
    T_CHECK(nc_event(0) && nc_event(0)->kind == NC_EV_SESSION);
    // A feed that changed no session tells none.
    q = nc_next_request(T0);
    n = nw_list_head(NW_GAMES, 0, b, sizeof b);
    nc_on_response(q->id, 200, b, n, T0);
    T_CHECK(nc_event(0) == NULL);
}

static void another_user_starts_the_book_over(void) {
    signed_in(T0 + 3600000);
    nc_stream_sub(NW_SUB_GAMES, NULL, 0);
    nc_stream_opened(T0);
    const uint8_t env[] = { 1 };
    NwFrame v = push(NW_WS_VIEW, "g1", 5, env, 1);
    T_EQ_INT(feed(&v, 0, T0)->kind, NC_EV_VIEW);
    // The same user again (a refreshed tab): the book holds.
    uint8_t b[256];
    int n = session_body(b, sizeof b, "jwt9", T0 + 3600000);
    T_EQ_INT(nc_session_load(b, n), NW_OK);
    v.version = 4;
    T_EQ_INT(feed(&v, 0, T0)->kind, NC_EV_NONE);
    // Another user: their versions and subscriptions start from nothing.
    NwSession s;
    T_EQ_INT(nw_session_decode(b, n, &s), NW_OK);
    memcpy(s.user_id, "u2", 2);
    n = nw_session_encode(&s, b, sizeof b);
    T_EQ_INT(nc_session_load(b, n), NW_OK);
    T_EQ_INT(feed(&v, 0, T0)->kind, NC_EV_VIEW);
    nc_stream_opened(T0);
    T_EQ_INT(next_frame(T0).kind, 0);
}

// The bytes an intent carries wait in one shared arena: each request is built
// from its own, a full arena refuses at once, and room let go is used again.
static void waiting_intents_keep_their_bytes(void) {
    signed_in(T0 + 3600000);
    NcIntent chat[3];
    for (int k = 0; k < 3; k++) {
        chat[k] = intent(NC_DO_SEND_CHAT, "g1");
        chat[k].bytes_len = 1000;
        memset(chat[k].bytes, 'a' + k, 1000);
    }
    NcIntent move = intent(NC_DO_PLAY, "g1");
    move.bytes_len = 2; move.bytes[0] = 3; move.bytes[1] = 7;
    T_CHECK(nc_do(&chat[0], T0) != 0);
    T_CHECK(nc_do(&move, T0) != 0);
    T_CHECK(nc_do(&chat[1], T0) != 0);
    T_EQ_INT(nc_do(&chat[2], T0), 0);   // 2,002 bytes wait; 1,000 more do not fit
    T_EQ_INT(nc_error()->why, ASK_WHY_INTERNAL);
    // Each request carries its own bytes (the chat's: [v][kind][u16 len][message]).
    const NcRequest *q = nc_next_request(T0);
    T_CHECK(path_is(q, "/v1/chat/g1") && q->body_len == 1004 && q->body[4] == 'a' && q->body[1003] == 'a');
    uint32_t first = q->id;
    q = nc_next_request(T0);
    T_CHECK(path_is(q, "/v1/action") && q->body[q->body_len - 1] == 7);
    q = nc_next_request(T0);
    T_CHECK(path_is(q, "/v1/chat/g1") && q->body[4] == 'b' && q->body[1003] == 'b');
    // The first chat answered: its room takes the third.
    T_CHECK(nc_on_response(first, 204, NULL, 0, T0) != NULL);
    T_CHECK(nc_do(&chat[2], T0) != 0);
    q = nc_next_request(T0);
    T_CHECK(path_is(q, "/v1/chat/g1") && q->body[4] == 'c' && q->body[1003] == 'c');
}

// The iOS app shows boards and never decodes a cast: a cast is the client's
// cue to take the game whole, a view of a game held on its own stream is still
// handed out, and a create does not subscribe the game's stream.
static void a_host_of_boards_gets_boards(void) {
    signed_in(T0 + 3600000);
    nc_set_boards(1);
    nc_stream_sub(NW_SUB_GAME, "g1", 2);
    nc_stream_opened(T0);
    while (next_frame(T0).kind) {}
    host_t h;
    memset(&h, 0, sizeof h);
    const uint8_t c[] = { 0xc1 }, v[] = { 0x55 }, env[] = { 0xe6 };
    host_push(&h, NW_WS_VIEW, "g1", 5, v, 1, T0);
    T_EQ_INT(h.views, 1);   // held on its own stream, and handed out all the same
    // A cast at a version already shown: nothing to fetch.
    host_push(&h, NW_WS_CAST, "g1", 5, c, 1, T0);
    T_EQ_INT(h.casts, 0);
    T_CHECK(nc_next_request(T0) == NULL);
    // A newer one: not handed out, and the game is fetched; its board comes as a view.
    host_push(&h, NW_WS_CAST, "g1", 6, c, 1, T0);
    T_EQ_INT(h.casts, 0);
    const NcRequest *q = nc_next_request(T0);
    T_CHECK(path_is(q, "/v1/games/g1"));
    host_answer(&h, q->id, 6, env, 1, T0);
    T_EQ_INT(h.refetched, 1);
    T_EQ_INT(h.table_v, 6);
    // Its view arriving first makes the fetch moot.
    host_push(&h, NW_WS_CAST, "g1", 7, c, 1, T0);
    host_push(&h, NW_WS_VIEW, "g1", 7, v, 1, T0);
    T_CHECK(nc_next_request(T0) == NULL);
    // A create subscribes nothing.
    uint8_t b[512];
    NcIntent create = intent(NC_DO_CREATE, NULL);
    nc_do(&create, T0);
    q = nc_next_request(T0);
    int n = envelope("newg02", 1, b, sizeof b);
    T_CHECK(nc_on_response(q->id, 200, b, n, T0) != NULL);
    T_EQ_INT(next_frame(T0).kind, 0);
    nc_set_boards(0);
}

// 503 ASK_WHY_BUSY (the server's store was throttled): a read is sent again
// after NC_BUSY_FIRST_MS doubling, NC_BUSY_TRIES times; a write's answer stands.
static void a_busy_read_goes_again_a_write_does_not(void) {
    signed_in(T0 + 3600000);
    uint8_t busy[16];
    int n = refusal_body(busy, ASK_WHY_BUSY, 0);
    NcIntent games = intent(NC_DO_GAMES, NULL);
    uint32_t t = nc_do(&games, T0);
    int64_t now = T0;
    for (int k = 0; k < NC_BUSY_TRIES; k++) {
        const NcRequest *q = nc_next_request(now);
        T_CHECK(path_is(q, "/v1/games"));
        T_CHECK(nc_on_response(q->id, 503, busy, n, now) == NULL);
        T_EQ_INT(nc_pump_due_ms(now), NC_BUSY_FIRST_MS << k);
        T_CHECK(nc_next_request(now + (NC_BUSY_FIRST_MS << k) - 1) == NULL);
        now += NC_BUSY_FIRST_MS << k;
    }
    const NcRequest *q = nc_next_request(now);
    const NcDone *d = nc_on_response(q->id, 503, busy, n, now);
    T_CHECK(d && d->ticket == t && d->why == ASK_WHY_BUSY);
    NcIntent create = intent(NC_DO_CREATE, NULL);
    t = nc_do(&create, now);
    q = nc_next_request(now);
    d = nc_on_response(q->id, 503, busy, n, now);
    T_CHECK(d && d->ticket == t && d->why == ASK_WHY_BUSY);
}

int main(void) {
    T_RUN(session_blob);
    T_RUN(requests);
    T_RUN(answers);
    T_RUN(stream_order_and_retries);
    T_RUN(gates);
    T_RUN(pump_signs_in_and_reads_answers);
    T_RUN(pump_refreshes_first_and_after_a_401);
    T_RUN(pump_retries_reads_not_writes);
    T_RUN(pump_signs_out);
    T_RUN(a_cast_and_a_stale_answer_end_alike);
    T_RUN(a_skipped_version_refetches);
    T_RUN(a_refused_move_catches_up);
    T_RUN(accepted_again_refetches);
    T_RUN(create_and_join_move_the_subscription);
    T_RUN(the_session_is_told_once);
    T_RUN(another_user_starts_the_book_over);
    T_RUN(waiting_intents_keep_their_bytes);
    T_RUN(a_host_of_boards_gets_boards);
    T_RUN(a_busy_read_goes_again_a_write_does_not);
    return T_DONE();
}
