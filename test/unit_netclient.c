// unit_netclient.c - the client library every host runs (foolish/c/src/
// netclient.h), with no server: the session as a blob, what each request
// says, when the token is refreshed, how an answer is read, and the stream's
// own decisions (what to send in what order, retries, reconnects, pings, and
// the version gates).
#include "t.h"

#include "ask.h"
#include "netclient.h"
#include "netwire.h"

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

static const NcEvent *feed(const NwFrame *f, int as_text, int64_t now) {
    static uint8_t raw[1024];
    static char text[1500];
    int n = nw_frame_encode(f, raw, sizeof raw);
    if (!as_text) return nc_stream_in(raw, n, 0, now);
    int t = nw_text_encode(raw, n, text, sizeof text);
    return nc_stream_in((const uint8_t *)text, t, 1, now);
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
    NwFrame f = next_frame(T0);
    T_CHECK(f.kind == NW_WS_AUTH && f.token_len == 3 && memcmp(f.token, "jwt", 3) == 0);
    NwFrame s1 = next_frame(T0), s2 = next_frame(T0);
    T_CHECK(s1.kind == NW_WS_SUB && s1.what == NW_SUB_GAMES && s1.gid_len == 0);
    T_CHECK(s2.kind == NW_WS_SUB && s2.what == NW_SUB_GAME && s2.gid_len == 2 && s2.ref != s1.ref && s2.version == 0);
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
    T_CHECK(c.kind == NW_WS_SUB && c.what == NW_SUB_CHAT);
    a = ack(c.ref, ASK_WHY_FORBIDDEN, 0);
    e = feed(&a, 0, T0);
    T_CHECK(e->kind == NC_EV_REFUSED && e->why == ASK_WHY_FORBIDDEN && e->what == NW_SUB_CHAT);
    T_EQ_INT(next_frame(T0 + NC_RETRY_FIRST_MS - 1).kind, 0);
    T_EQ_INT(nc_stream_due_ms(T0), NC_RETRY_FIRST_MS);
    c = next_frame(T0 + NC_RETRY_FIRST_MS);
    T_CHECK(c.kind == NW_WS_SUB && c.what == NW_SUB_CHAT);
    a = ack(c.ref, ASK_WHY_FORBIDDEN, 0);
    feed(&a, 0, T0 + 1000);
    T_EQ_INT(next_frame(T0 + 1000 + 2 * NC_RETRY_FIRST_MS - 1).kind, 0);
    T_EQ_INT(next_frame(T0 + 1000 + 2 * NC_RETRY_FIRST_MS).kind, NW_WS_SUB);
    // A refused token is told again with the session's token.
    NwFrame s3 = s2;
    T_EQ_INT(nc_stream_sub(NW_SUB_GAME, "g1", 2), NW_OK);   // asked again: sent again
    s3 = next_frame(T0 + 2000);
    T_CHECK(s3.kind == NW_WS_SUB && s3.what == NW_SUB_GAME);
    a = ack(s3.ref, ASK_WHY_AUTH, 0);
    feed(&a, 0, T0 + 2000);
    T_EQ_INT(next_frame(T0 + 2000).kind, NW_WS_AUTH);

    // Unsubscribing a live subscription tells the server; one never sent does not.
    T_EQ_INT(nc_stream_unsub(NW_SUB_GAMES, NULL, 0), NW_OK);
    f = next_frame(T0 + 2000);
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
    // Reopened: the token and every subscription again, the game's with what the client holds.
    nc_stream_opened(T0);
    T_EQ_INT(next_frame(T0).kind, NW_WS_AUTH);
    int subs = 0;
    for (NwFrame x; (x = next_frame(T0)).kind == NW_WS_SUB;) subs++;
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
    T_EQ_MEM(nc_event_payload(), e->payload_len, env, 3);
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
    // A resubscription carries what is held, so the server catches up only what is newer.
    T_EQ_INT(nc_stream_sub(NW_SUB_GAME, "g1", 2), NW_OK);
    NwFrame s;
    while ((s = next_frame(T0)).kind && s.kind != NW_WS_SUB) {}
    T_CHECK(s.kind == NW_WS_SUB && s.version == 6);
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
    T_EQ_INT(nc_stream_in((const uint8_t *)"!!!!", 4, 1, T0)->kind, NC_EV_BAD);
    T_EQ_INT(nc_stream_in((const uint8_t *)"\x01\x29", 2, 0, T0)->kind, NC_EV_BAD);   // a client's frame
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

int main(void) {
    T_RUN(session_blob);
    T_RUN(requests);
    T_RUN(answers);
    T_RUN(stream_order_and_retries);
    T_RUN(gates);
    return T_DONE();
}
