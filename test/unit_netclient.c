// unit_netclient.c - the client library every host runs (foolish/c/src/
// netclient.h), with no server: the session as a blob, what each request
// says, when the token is refreshed, how an answer is read, and the stream's
// own decisions over MQTT (what to send in what order, retries, reconnects,
// pings, the token on CONNECT, and the version gates), against a fake broker
// built from the same codec (shared/c/mqtt).
#include "t.h"

#include "ask.h"
#include "netclient.h"
#include "netwire.h"
#include "roster.h"
#include "table.h"
#include "view.h"

#include "../../shared/c/mqtt/mqtt.h"

#include <stdlib.h>

#define T0 1700000000000LL

static void pending_drop(void);

// A request's id, 0 for none: a request a mutant never made fails an
// assertion instead of crashing the suite.
static uint32_t id_of(const NcRequest *q) { return q ? q->id : 0; }

static void signed_in(uint64_t expires_at_ms) {
    pending_drop();
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
    T_EQ_MEM(q->path, q->path_len, "/mqtt", 5);

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

// ---- the stream: a fake broker ------------------------------------------------------------

// The packets the client's last messages held, read one at a time. A packet
// is described by its type, packet id, and for (UN)SUBSCRIBE the
// subscription its topics name: what (NW_SUB_*, from the first topic) and gid.
typedef struct {
    int      type;            // MQTT_*, 0 for none
    uint16_t pid;
    int      what;            // (UN)SUBSCRIBE
    char     gid[16];
    int      n_topics;
    char     topics[2][96];
    char     password[NW_ACCESS_MAX + 1];   // CONNECT
    char     client_id[160];
    uint16_t keepalive;
} cpkt;

static struct { uint8_t b[NC_OUT_MAX]; int len, at, msgs; } g_cout;
static void pending_drop(void) { g_cout.len = g_cout.at = 0; }

// The subscription a topic names: "u/u1/v", "u/u1/c/<g>", "u/u1/m/<g>", "s/<g>".
static void topic_names(const char *t, int n, cpkt *k) {
    if (n == 6 && memcmp(t, "u/u1/v", 6) == 0) { k->what = NW_SUB_GAMES; return; }
    if (n > 7 && memcmp(t, "u/u1/c/", 7) == 0) { k->what = NW_SUB_GAME; snprintf(k->gid, sizeof k->gid, "%.*s", n - 7, t + 7); return; }
    if (n > 7 && memcmp(t, "u/u1/m/", 7) == 0) { k->what = NW_SUB_CHAT; snprintf(k->gid, sizeof k->gid, "%.*s", n - 7, t + 7); return; }
    if (n > 2 && memcmp(t, "s/", 2) == 0 && !k->what) { k->what = -1; snprintf(k->gid, sizeof k->gid, "%.*s", n - 2, t + 2); }
}

static cpkt next_pkt(int64_t now) {
    cpkt k;
    memset(&k, 0, sizeof k);
    if (g_cout.at >= g_cout.len) {
        const NcOut *o = nc_stream_next(now);
        if (!o) return k;
        T_CHECK(o->len > 0 && o->len <= NC_OUT_MAX);
        memcpy(g_cout.b, o->bytes, o->len);
        g_cout.len = o->len;
        g_cout.at = 0;
        g_cout.msgs++;
    }
    MqttPacket p;
    int n = mqtt_parse(g_cout.b + g_cout.at, g_cout.len - g_cout.at, NC_OUT_MAX, MQTT_FROM_CLIENT, &p);
    T_CHECK(n > 0);
    if (n <= 0) { g_cout.len = 0; return k; }
    g_cout.at += n;
    k.type = p.type;
    k.pid = p.packet_id;
    if (p.type == MQTT_CONNECT) {
        snprintf(k.password, sizeof k.password, "%.*s", p.password_len, p.password ? (const char *)p.password : "");
        snprintf(k.client_id, sizeof k.client_id, "%.*s", p.client_id_len, (const char *)p.client_id);
        k.keepalive = p.keepalive_s;
    }
    if (p.type == MQTT_SUBSCRIBE || p.type == MQTT_UNSUBSCRIBE) {
        int pos = 0;
        const uint8_t *f;
        uint16_t fl;
        uint8_t q;
        while (k.n_topics < 2 && mqtt_next_filter(&p, &pos, &f, &fl, &q)) {
            T_EQ_INT(q, 0);
            snprintf(k.topics[k.n_topics], sizeof k.topics[0], "%.*s", fl, (const char *)f);
            topic_names((const char *)f, fl, &k);
            k.n_topics++;
        }
    }
    return k;
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

static const NcEvent *feed_bytes(const uint8_t *b, int n, int64_t now) { return first(nc_stream_in(b, n, 0, now)); }

static const NcEvent *connack(int rc, int64_t now) {
    uint8_t b[4];
    return feed_bytes(b, mqtt_write_connack(b, sizeof b, 0, rc), now);
}

static const NcEvent *suback(uint16_t pid, int code, int n, int64_t now) {
    uint8_t b[16], codes[2] = { (uint8_t)code, (uint8_t)code };
    return feed_bytes(b, mqtt_write_suback(b, sizeof b, pid, codes, n), now);
}

// The socket opened, the client's CONNECT read (with its token) and accepted.
static cpkt live(int64_t now) {
    pending_drop();
    nc_stream_opened(now);
    cpkt c = next_pkt(now);
    T_EQ_INT(c.type, MQTT_CONNECT);
    connack(0, now);
    return c;
}

// The topic a push of `kind` travels on: a cast on the seat's topic, or the
// spectators' when `watch`.
static int topic_for(const NwFrame *f, int watch, char *t, int cap) {
    const NcSessionInfo *me = nc_session_info();
    int ul = me->user_id_len;
    if (f->kind == NW_WS_VIEW) return snprintf(t, (size_t)cap, "u/%.*s/v", ul, me->user_id);
    if (f->kind == NW_WS_CHAT) return snprintf(t, (size_t)cap, "u/%.*s/m/%.*s", ul, me->user_id, f->gid_len, f->gid);
    if (watch) return snprintf(t, (size_t)cap, "s/%.*s", f->gid_len, f->gid);
    return snprintf(t, (size_t)cap, "u/%.*s/c/%.*s", ul, me->user_id, f->gid_len, f->gid);
}

// A push the server published: the nw1 message as a PUBLISH's payload.
static const NcEvent *feed(const NwFrame *f, int watch, int64_t now) {
    static uint8_t raw[1024], pkt[1200];
    int n = nw_frame_encode(f, raw, sizeof raw);
    char t[96];
    int tl = topic_for(f, watch, t, sizeof t);
    int k = mqtt_write_publish(pkt, sizeof pkt, t, tl, raw, n);
    T_CHECK(k > 0);
    return feed_bytes(pkt, k, now);
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

// The CONNECT: the token as its password, a client id of the user's, a keep-alive.
static void connect_carries_the_token(void) {
    signed_in(T0 + 3600000);
    // Shut: nothing to send, nothing due.
    T_CHECK(nc_stream_next(T0) == NULL);
    T_EQ_INT(nc_stream_due_ms(T0), -1);
    nc_stream_opened(T0);
    T_EQ_INT(nc_stream_due_ms(T0), 0);
    cpkt c = next_pkt(T0);
    T_EQ_INT(c.type, MQTT_CONNECT);
    T_EQ_STR(c.password, "jwt");
    T_CHECK(strncmp(c.client_id, "u1-", 3) == 0 && strlen(c.client_id) == 3 + 8);
    T_EQ_INT(c.keepalive, NC_KEEPALIVE_S);
    // Nothing else until the CONNACK: no SUBSCRIBE a refused CONNECT would drop.
    T_EQ_INT(nc_stream_sub(NW_SUB_GAMES, NULL, 0), NW_OK);
    T_EQ_INT(next_pkt(T0).type, 0);
    T_EQ_INT(nc_stream_due_ms(T0), -1);
    T_EQ_INT(connack(0, T0)->kind, NC_EV_NONE);
    cpkt s = next_pkt(T0);
    T_CHECK(s.type == MQTT_SUBSCRIBE && s.what == NW_SUB_GAMES && s.n_topics == 1);
    T_EQ_STR(s.topics[0], "u/u1/v");
    // Another connection, in the same millisecond: another client id (two
    // tabs never share one, or the broker would drop the older).
    nc_stream_closed(T0);
    nc_stream_opened(T0);
    cpkt c2 = next_pkt(T0);
    T_CHECK(c2.type == MQTT_CONNECT && strcmp(c2.client_id, c.client_id) != 0);
    // A CONNACK refusing it is no subscription either; the broker closes the
    // socket, and the next CONNECT waits for a refreshed token.
    connack(MQTT_RC_NOT_AUTHORIZED, T0 + 1);
    T_EQ_INT(next_pkt(T0 + 1).type, 0);
    nc_stream_closed(T0 + 1);
    nc_stream_opened(T0 + 2);
    T_EQ_INT(next_pkt(T0 + 2).type, 0);
    const NcRequest *q = nc_next_request(T0 + 2);
    T_CHECK(q && q->is_refresh);
    uint8_t b[256];
    NwSession ses;
    memset(&ses, 0, sizeof ses);
    ses.access_len = 4; memcpy(ses.access, "jwt3", 4);
    ses.refresh_len = 3; memcpy(ses.refresh, "rt3", 3);
    ses.expires_at_ms = T0 + 3600000;
    ses.user_id_len = 2; memcpy(ses.user_id, "u1", 2);
    ses.username_len = 3; memcpy(ses.username, "ANA", 3);
    if (q) nc_on_response(id_of(q), 200, b, nw_session_encode(&ses, b, sizeof b), T0 + 2);
    cpkt c3 = next_pkt(T0 + 2);
    T_CHECK(c3.type == MQTT_CONNECT && strcmp(c3.password, "jwt3") == 0);
    // A second CONNACK is not the protocol.
    connack(0, T0 + 2);
    T_EQ_INT(connack(0, T0 + 2)->kind, NC_EV_BAD);
    // Signed out: nothing at all.
    nc_reset();
    nc_stream_opened(T0);
    T_CHECK(nc_stream_next(T0) == NULL);
}

// A token about to expire is refreshed before the CONNECT carries it: the pump
// sends the refresh itself, and the CONNECT goes with the new token.
static void connect_waits_for_a_fresh_token(void) {
    signed_in(T0 + NC_REFRESH_EARLY_MS / 2);
    nc_stream_opened(T0);
    T_EQ_INT(next_pkt(T0).type, 0);
    T_EQ_INT(nc_stream_due_ms(T0), -1);
    T_EQ_INT(nc_pump_due_ms(T0), 0);
    const NcRequest *q = nc_next_request(T0);
    T_CHECK(q && q->is_refresh);
    T_CHECK(nc_next_request(T0) == NULL);   // one refresh at a time
    T_EQ_INT(nc_pump_due_ms(T0), -1);
    uint8_t b[256];
    NwSession ses;
    memset(&ses, 0, sizeof ses);
    ses.access_len = 4; memcpy(ses.access, "jwt2", 4);
    ses.refresh_len = 3; memcpy(ses.refresh, "rt2", 3);
    ses.expires_at_ms = T0 + 3600000;
    ses.user_id_len = 2; memcpy(ses.user_id, "u1", 2);
    ses.username_len = 3; memcpy(ses.username, "ANA", 3);
    int n = nw_session_encode(&ses, b, sizeof b);
    T_CHECK(nc_on_response(id_of(q), 200, b, n, T0) == NULL);
    T_EQ_INT(nc_stream_due_ms(T0), 0);
    cpkt c = next_pkt(T0);
    T_CHECK(c.type == MQTT_CONNECT && strcmp(c.password, "jwt2") == 0);
    T_CHECK(nc_next_request(T0) == NULL);   // connected: no refresh wanted
    // A refresh that fails is tried again later, as the pump's always is.
    signed_in(T0 + NC_REFRESH_EARLY_MS / 2);
    nc_stream_opened(T0);
    q = nc_next_request(T0);
    T_CHECK(q && q->is_refresh);
    nc_on_response(id_of(q), 0, NULL, 0, T0);
    T_CHECK(nc_next_request(T0) == NULL);
    T_CHECK(nc_pump_due_ms(T0) > 0);
    q = nc_next_request(T0 + NC_RETRY_FIRST_MS);
    T_CHECK(q && q->is_refresh);
}

// A socket closed before its CONNACK is the broker's refusal: the token is
// refreshed before the next CONNECT, but not more than once every
// NC_FORCE_REFRESH_MS.
static void closed_before_connack_refreshes(void) {
    signed_in(T0 + 3600000);
    nc_stream_opened(T0);
    T_EQ_INT(next_pkt(T0).type, MQTT_CONNECT);
    nc_stream_closed(T0);
    nc_stream_opened(T0 + 500);
    T_EQ_INT(next_pkt(T0 + 500).type, 0);   // waits for the refresh
    const NcRequest *q = nc_next_request(T0 + 500);
    T_CHECK(q && q->is_refresh);
    uint8_t b[256];
    NwSession ses;
    memset(&ses, 0, sizeof ses);
    ses.access_len = 3; memcpy(ses.access, "jw2", 3);
    ses.refresh_len = 3; memcpy(ses.refresh, "rt2", 3);
    ses.expires_at_ms = T0 + 3600000;
    ses.user_id_len = 2; memcpy(ses.user_id, "u1", 2);
    ses.username_len = 3; memcpy(ses.username, "ANA", 3);
    nc_on_response(id_of(q), 200, b, nw_session_encode(&ses, b, sizeof b), T0 + 500);
    T_EQ_INT(next_pkt(T0 + 500).type, MQTT_CONNECT);
    // Refused again at once: no second refresh inside NC_FORCE_REFRESH_MS.
    nc_stream_closed(T0 + 600);
    nc_stream_opened(T0 + 1000);
    T_EQ_INT(next_pkt(T0 + 1000).type, MQTT_CONNECT);
    T_CHECK(nc_next_request(T0 + 1000) == NULL);
    nc_stream_closed(T0 + NC_FORCE_REFRESH_MS + 1);   // refused again, past the window: refreshed again
    nc_stream_opened(T0 + NC_FORCE_REFRESH_MS + 1000);
    T_EQ_INT(next_pkt(T0 + NC_FORCE_REFRESH_MS + 1000).type, 0);
    // A socket that closes after its CONNACK refreshes nothing.
    signed_in(T0 + 3600000);
    live(T0);
    nc_stream_closed(T0);
    nc_stream_opened(T0);
    T_EQ_INT(next_pkt(T0).type, MQTT_CONNECT);
}

static void stream_order_and_retries(void) {
    signed_in(T0 + 3600000);
    T_EQ_INT(nc_stream_sub(NW_SUB_GAMES, NULL, 0), NW_OK);
    T_EQ_INT(nc_stream_sub(NW_SUB_GAME, "g1", 2), NW_OK);
    T_EQ_INT(nc_stream_sub(NW_SUB_GAME, NULL, 0), NW_E_WIRE);
    T_EQ_INT(nc_stream_sub(9, "g1", 2), NW_E_WIRE);
    T_EQ_INT(nc_stream_sub(NW_SUB_GAME, "g/1", 3), NW_E_WIRE);   // not a topic level
    T_EQ_INT(nc_stream_sub(NW_SUB_GAME, "g+", 2), NW_E_WIRE);
    live(T0);
    // Each subscription its own SUBSCRIBE, all in one message; a game's names
    // both its streams.
    int msgs0 = g_cout.msgs;
    cpkt s1 = next_pkt(T0), s2 = next_pkt(T0);
    T_EQ_INT(g_cout.msgs, msgs0 + 1);
    T_CHECK(s1.type == MQTT_SUBSCRIBE && s1.what == NW_SUB_GAMES);
    T_CHECK(s2.type == MQTT_SUBSCRIBE && s2.what == NW_SUB_GAME && s2.n_topics == 2 && s2.pid != s1.pid);
    T_EQ_STR(s2.topics[0], "u/u1/c/g1");
    T_EQ_STR(s2.topics[1], "s/g1");
    T_EQ_INT(next_pkt(T0).type, 0);
    T_EQ_INT(nc_stream_due_ms(T0), NC_ACK_WAIT_MS);   // unanswered, they are asked again then

    // Answered: the game's says whether the user sits there, as the book knows.
    T_EQ_INT(suback(s1.pid, 0, 1, T0)->kind, NC_EV_NONE);
    const NcEvent *e = suback(s2.pid, 0, 2, T0);
    T_CHECK(e->kind == NC_EV_SEATED && e->seated == 0 && e->gid_len == 2);
    // A SUBACK nobody waits on is nothing.
    T_EQ_INT(suback(999, 0, 1, T0)->kind, NC_EV_NONE);

    // A refused chat is retried after 0.5 s, then 1 s, then 2 s.
    T_EQ_INT(nc_stream_sub(NW_SUB_CHAT, "g1", 2), NW_OK);
    cpkt c = next_pkt(T0);
    T_CHECK(c.type == MQTT_SUBSCRIBE && c.what == NW_SUB_CHAT);
    T_EQ_STR(c.topics[0], "u/u1/m/g1");
    e = suback(c.pid, MQTT_SUBACK_FAILURE, 1, T0);
    T_CHECK(e->kind == NC_EV_REFUSED && e->what == NW_SUB_CHAT);
    T_EQ_INT(next_pkt(T0 + NC_RETRY_FIRST_MS - 1).type, 0);
    T_EQ_INT(nc_stream_due_ms(T0), NC_RETRY_FIRST_MS);
    c = next_pkt(T0 + NC_RETRY_FIRST_MS);
    T_CHECK(c.type == MQTT_SUBSCRIBE && c.what == NW_SUB_CHAT);
    suback(c.pid, MQTT_SUBACK_FAILURE, 1, T0 + 1000);
    T_EQ_INT(next_pkt(T0 + 1000 + 2 * NC_RETRY_FIRST_MS - 1).type, 0);
    cpkt c3 = next_pkt(T0 + 1000 + 2 * NC_RETRY_FIRST_MS);
    T_EQ_INT(c3.type, MQTT_SUBSCRIBE);
    // Held twice, let go once: still held, and nothing is sent.
    T_EQ_INT(nc_stream_sub(NW_SUB_GAME, "g1", 2), NW_OK);
    T_EQ_INT(nc_stream_unsub(NW_SUB_GAME, "g1", 2), NW_OK);
    T_EQ_INT(next_pkt(T0 + 2000).type, 0);
    // Let go and held again: asked again (the pending unsubscribe is moot).
    T_EQ_INT(nc_stream_unsub(NW_SUB_GAME, "g1", 2), NW_OK);
    T_EQ_INT(nc_stream_sub(NW_SUB_GAME, "g1", 2), NW_OK);
    cpkt s3 = next_pkt(T0 + 2000);
    T_CHECK(s3.type == MQTT_SUBSCRIBE && s3.what == NW_SUB_GAME);
    T_EQ_INT(next_pkt(T0 + 2000).type, 0);

    // Unsubscribing a live subscription tells the broker, every topic it held;
    // one never sent does not.
    T_EQ_INT(nc_stream_unsub(NW_SUB_GAMES, NULL, 0), NW_OK);
    cpkt u = next_pkt(T0 + 2000);
    T_CHECK(u.type == MQTT_UNSUBSCRIBE && u.what == NW_SUB_GAMES && u.n_topics == 1);
    suback(s3.pid, 0, 2, T0 + 2000);   // answered, so neither is asked again below
    suback(c3.pid, 0, 1, T0 + 2000);
    T_EQ_INT(nc_stream_unsub(NW_SUB_GAME, "g1", 2), NW_OK);
    u = next_pkt(T0 + 2000);
    T_CHECK(u.type == MQTT_UNSUBSCRIBE && u.what == NW_SUB_GAME && u.n_topics == 2);
    T_EQ_INT(nc_stream_sub(NW_SUB_GAME, "g9", 2), NW_OK);
    T_EQ_INT(nc_stream_unsub(NW_SUB_GAME, "g9", 2), NW_OK);   // never sent: nothing to take back
    while (next_pkt(T0 + 2000).type) {}

    // The PINGREQ, once nothing went out for NC_PING_MS.
    T_EQ_INT(next_pkt(T0 + 2000 + NC_PING_MS - 1).type, 0);
    T_EQ_INT(nc_stream_due_ms(T0 + 2000), NC_PING_MS);
    T_EQ_INT(next_pkt(T0 + 2000 + NC_PING_MS).type, MQTT_PINGREQ);
    T_EQ_INT(next_pkt(T0 + 2000 + NC_PING_MS).type, 0);
    uint8_t pr[2];
    T_EQ_INT(feed_bytes(pr, mqtt_write_bare(pr, sizeof pr, MQTT_PINGRESP), T0)->kind, NC_EV_NONE);

    // Closed: the reopen waits 0.5 s, doubling to 10 s, and starts over once a CONNACK arrives.
    T_EQ_INT(nc_stream_closed(T0), NC_RETRY_FIRST_MS);
    T_EQ_INT(nc_stream_closed(T0), 2 * NC_RETRY_FIRST_MS);
    for (int i = 0; i < 10; i++) nc_stream_closed(T0);
    T_EQ_INT(nc_stream_closed(T0), NC_RETRY_MAX_MS);
    T_CHECK(nc_stream_next(T0) == NULL);
    live(T0);
    T_EQ_INT(nc_stream_closed(T0), NC_RETRY_FIRST_MS);
    // Reconnected: every subscription again.
    live(T0);
    int subs = 0;
    for (cpkt x; (x = next_pkt(T0)).type == MQTT_SUBSCRIBE;) subs++;
    T_EQ_INT(subs, 1);   // the chat; the dashboard and the games were let go
}

// A page holds its streams before its kept session is restored (the web's
// providers know the user without the kernel): the holds are that user's.
static void holds_made_before_the_session_are_kept(void) {
    nc_reset();   // the book is nobody's again
    T_EQ_INT(nc_stream_sub(NW_SUB_GAMES, NULL, 0), NW_OK);
    NwSession s;
    memset(&s, 0, sizeof s);
    s.access_len = 3; memcpy(s.access, "jwt", 3);
    s.refresh_len = 2; memcpy(s.refresh, "rt", 2);
    s.expires_at_ms = T0 + 3600000;
    s.user_id_len = 2; memcpy(s.user_id, "u1", 2);
    uint8_t b[64];
    T_EQ_INT(nc_session_load(b, nw_session_encode(&s, b, sizeof b)), NW_OK);
    live(T0);
    cpkt k = next_pkt(T0);
    T_CHECK(k.type == MQTT_SUBSCRIBE && k.what == NW_SUB_GAMES);
    // Another user's session afterwards: their book starts from nothing.
    memcpy(s.user_id, "u2", 2);
    T_EQ_INT(nc_session_load(b, nw_session_encode(&s, b, sizeof b)), NW_OK);
    live(T0);
    T_EQ_INT(next_pkt(T0).type, 0);
}

// What the broker sends that is not the protocol, or not this user's.
static void what_is_not_the_brokers(void) {
    signed_in(T0 + 3600000);
    nc_stream_opened(T0);
    next_pkt(T0);
    const uint8_t env[] = { 1, 2, 3 };
    NwFrame v = push(NW_WS_VIEW, "g1", 5, env, 3);
    T_EQ_INT(feed(&v, 0, T0)->kind, NC_EV_BAD);   // a PUBLISH before the CONNACK
    connack(0, T0);
    T_EQ_INT(feed(&v, 0, T0)->kind, NC_EV_VIEW);
    // A text frame, bytes that are not MQTT, a packet a broker never sends.
    T_EQ_INT(first(nc_stream_in((const uint8_t *)"\xD0\x00", 2, 1, T0))->kind, NC_EV_BAD);   // a PINGRESP, as text
    T_EQ_INT(first(nc_stream_in((const uint8_t *)"\x01\x05", 2, 0, T0))->kind, NC_EV_BAD);
    T_EQ_INT(first(nc_stream_in((const uint8_t *)"\xC0\x00", 2, 0, T0))->kind, NC_EV_BAD);   // a PINGREQ
    // Another user's topic, a level nobody subscribes to, a message whose game
    // is not the topic's, a payload that is not a push, a push of the wrong kind.
    static uint8_t raw[256], pkt[400];
    int rn = nw_frame_encode(&v, raw, sizeof raw);
    const char *bad_topics[] = { "u/u2/v", "u/u1/x/g1", "u/u1/vx", "s/", "x/g1", "u/u1/c/g2" };
    NwFrame cast = push(NW_WS_CAST, "g1", 6, env, 1);
    int cn = 0;
    static uint8_t craw[256];
    cn = nw_frame_encode(&cast, craw, sizeof craw);
    for (int i = 0; i < 6; i++) {
        const uint8_t *body = i == 5 ? craw : raw;
        int bn = i == 5 ? cn : rn;
        int k = mqtt_write_publish(pkt, sizeof pkt, bad_topics[i], (int)strlen(bad_topics[i]), body, bn);
        T_EQ_INT(feed_bytes(pkt, k, T0)->kind, NC_EV_BAD);
    }
    int k = mqtt_write_publish(pkt, sizeof pkt, "u/u1/v", 6, (const uint8_t *)"\x01", 1);
    T_EQ_INT(feed_bytes(pkt, k, T0)->kind, NC_EV_BAD);
    k = mqtt_write_publish(pkt, sizeof pkt, "u/u1/v", 6, craw, cn);   // a cast on the views topic
    T_EQ_INT(feed_bytes(pkt, k, T0)->kind, NC_EV_BAD);
    // After junk the stream still reads: the buffer starts over.
    v.version = 7;
    T_EQ_INT(feed(&v, 0, T0)->kind, NC_EV_VIEW);
    // More than the client ever takes in one go: BAD, and the buffer is let go.
    static uint8_t huge[NC_MQ_IN_MAX + 1];
    T_EQ_INT(first(nc_stream_in(huge, (int)sizeof huge, 0, T0))->kind, NC_EV_BAD);
    v.version = 8;
    T_EQ_INT(feed(&v, 0, T0)->kind, NC_EV_VIEW);
    T_EQ_INT(nc_stream_in((const uint8_t *)"", 0, 0, T0), 0);   // an empty message: nothing
    T_EQ_INT(first(nc_stream_in(NULL, 3, 0, T0))->kind, NC_EV_BAD);
}

// MQTT over WebSocket: a message may hold part of a packet, or several; each
// push's payload is handed out where it lies, until the next feed.
static void packets_across_messages(void) {
    signed_in(T0 + 3600000);
    live(T0);
    static uint8_t raw[256], all[1200];
    const uint8_t env[] = { 0xe1, 0xe2 };
    int at = 0;
    for (int i = 0; i < 3; i++) {
        NwFrame v = push(NW_WS_VIEW, "g1", (uint32_t)(5 + i), env, 2);
        int rn = nw_frame_encode(&v, raw, sizeof raw);
        at += mqtt_write_publish(all + at, (int)sizeof all - at, "u/u1/v", 6, raw, rn);
    }
    // All three in one message: three events, each payload in place.
    int n = nc_stream_in(all, at, 0, T0);
    int views = 0;
    for (int i = 0; i < n; i++) {
        const NcEvent *e = nc_event(i);
        if (e->kind != NC_EV_VIEW) continue;
        T_EQ_INT(e->version, (uint32_t)(5 + views));
        T_EQ_MEM(nc_event_payload(i), e->payload_len, env, 2);
        views++;
    }
    T_EQ_INT(views, 3);
    // Cut at every byte: the first part makes nothing, the rest all three.
    for (int cut = 1; cut < at; cut++) {
        signed_in(T0 + 3600000);
        live(T0);
        int got = 0;
        n = nc_stream_in(all, cut, 0, T0);
        for (int i = 0; i < n; i++) got += nc_event(i)->kind == NC_EV_VIEW;
        n = nc_stream_in(all + cut, at - cut, 0, T0);
        for (int i = 0; i < n; i++) {
            if (nc_event(i)->kind != NC_EV_VIEW) continue;
            T_EQ_MEM(nc_event_payload(i), nc_event(i)->payload_len, env, 2);
            got++;
        }
        T_EQ_INT(got, 3);
    }
    // More packets in one message than one feed's events: the rest wait for the next feed.
    signed_in(T0 + 3600000);
    live(T0);
    at = 0;
    for (int i = 0; i < NC_EVENTS_MAX + 4; i++) {
        NwFrame v = push(NW_WS_VIEW, "g1", (uint32_t)(10 + i), env, 2);
        int rn = nw_frame_encode(&v, raw, sizeof raw);
        at += mqtt_write_publish(all + at, (int)sizeof all - at, "u/u1/v", 6, raw, rn);
    }
    n = nc_stream_in(all, at, 0, T0);
    T_CHECK(n <= NC_EVENTS_MAX);
    int seen = 0;
    for (int i = 0; i < n; i++) seen += nc_event(i)->kind == NC_EV_VIEW;
    n = nc_stream_in(NULL, 0, 0, T0);
    for (int i = 0; i < n; i++) seen += nc_event(i)->kind == NC_EV_VIEW;
    T_EQ_INT(seen, NC_EVENTS_MAX + 4);
}

static void gates(void) {
    signed_in(T0 + 3600000);
    live(T0);
    const uint8_t env[] = { 1, 2, 3 };
    NwFrame v = push(NW_WS_VIEW, "g1", 5, env, 3);
    const NcEvent *e = feed(&v, 0, T0);
    T_CHECK(e->kind == NC_EV_VIEW && e->version == 5 && e->payload_len == 3 && e->what == NW_SUB_GAMES);
    T_EQ_MEM(nc_event_payload(g_at), e->payload_len, env, 3);
    // At or below what was applied: stale.
    T_EQ_INT(feed(&v, 0, T0)->kind, NC_EV_NONE);
    v.version = 4;
    T_EQ_INT(feed(&v, 0, T0)->kind, NC_EV_NONE);
    v.version = 6;
    T_EQ_INT(feed(&v, 0, T0)->kind, NC_EV_VIEW);
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
    NwFrame chat;
    memset(&chat, 0, sizeof chat);
    chat.kind = NW_WS_CHAT;
    chat.gid_len = 2;
    memcpy(chat.gid, "g1", 2);
    chat.chat.message_len = 2;
    memcpy(chat.chat.message, "hi", 2);
    e = feed(&chat, 0, T0);
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
    uint32_t id = id_of(q);
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
    d = nc_on_response(id_of(q), 200, r, sizeof r, T0);
    T_CHECK(d && d->ticket == t && d->why == 0 && d->action_status == TABLE_STATUS_REJECTED && d->reject_code == 4 && d->version == 11);

    // One game: the view's version and the envelope's span.
    NcIntent open = intent(NC_DO_OPEN_GAME, "ab12cd");
    t = nc_do(&open, T0);
    q = nc_next_request(T0);
    T_CHECK(path_is(q, "/v1/games/ab12cd") && q->method == NC_GET);
    const uint8_t env[] = { 0xe1, 0xe2, 0xe3 };
    NwBlob eb = { env, 3 };
    n = nw_view_encode(7, eb, b, sizeof b);
    d = nc_on_response(id_of(q), 200, b, n, T0);
    T_CHECK(d && d->version == 7 && d->a_len == 3);
    if (d) T_EQ_MEM(b + d->a_off, d->a_len, env, 3);
    // A refusal is read as one.
    t = nc_do(&open, T0);
    q = nc_next_request(T0);
    n = refusal_body(b, ASK_WHY_NOT_FOUND, 0);
    d = nc_on_response(id_of(q), 404, b, n, T0);
    T_CHECK(d && d->ticket == t && d->why == ASK_WHY_NOT_FOUND && d->status == 404);

    // A list: its rows and their kind; the history carries the rating.
    NcIntent hist = intent(NC_DO_HISTORY, NULL);
    t = nc_do(&hist, T0);
    q = nc_next_request(T0);
    T_CHECK(path_is(q, "/v1/history"));
    n = nw_history_head(1, 1234, 0, b, sizeof b);
    d = nc_on_response(id_of(q), 200, b, n, T0);
    T_CHECK(d && d->list_kind == NW_HISTORY && d->n_rows == 0 && d->has_rating == 1 && d->rating == 1234);
}

static void pump_refreshes_first_and_after_a_401(void) {
    signed_in(T0 + 30000);   // within NC_REFRESH_EARLY_MS of expiring
    NcIntent games = intent(NC_DO_GAMES, NULL), board = intent(NC_DO_LEADERBOARD, NULL);
    uint32_t tg = nc_do(&games, T0), tb = nc_do(&board, T0);
    T_CHECK(tg && tb);
    const NcRequest *q = nc_next_request(T0);
    T_CHECK(path_is(q, "/v1/auth/refresh"));
    uint32_t refresh = id_of(q);
    // While it is out, only what needs nobody goes.
    q = nc_next_request(T0);
    T_CHECK(path_is(q, "/v1/leaderboard"));
    uint32_t board_id = id_of(q);
    T_CHECK(nc_next_request(T0) == NULL);
    T_EQ_INT(nc_pump_due_ms(T0), -1);   // the refresh's answer wakes the rest
    uint8_t b[256];
    int n = session_body(b, sizeof b, "jwt2", T0 + 3600000);
    T_CHECK(nc_on_response(refresh, 200, b, n, T0) == NULL);
    q = nc_next_request(T0);
    T_CHECK(path_is(q, "/v1/games"));
    T_EQ_MEM(q->auth, q->auth_len, "Bearer jwt2", 11);
    uint32_t games_id = id_of(q);
    n = nw_list_head(NW_BOARD, 0, b, sizeof b);
    const NcDone *d = nc_on_response(board_id, 200, b, n, T0);
    T_CHECK(d && d->ticket == tb && d->list_kind == NW_BOARD);

    // A 401: refresh, then the same request once more.
    T_CHECK(nc_on_response(games_id, 401, NULL, 0, T0) == NULL);
    q = nc_next_request(T0);
    T_CHECK(path_is(q, "/v1/auth/refresh"));
    n = session_body(b, sizeof b, "jwt3", T0 + 3600000);
    nc_on_response(id_of(q), 200, b, n, T0);
    q = nc_next_request(T0);
    T_CHECK(path_is(q, "/v1/games"));
    T_EQ_MEM(q->auth, q->auth_len, "Bearer jwt3", 11);
    // A second 401 is the answer.
    d = nc_on_response(id_of(q), 401, NULL, 0, T0);
    T_CHECK(d && d->ticket == tg && d->why == ASK_WHY_AUTH);

    // A refresh refused for its token ends the session and what waited on it.
    signed_in(T0 + 30000);
    tg = nc_do(&games, T0);
    q = nc_next_request(T0);
    T_CHECK(path_is(q, "/v1/auth/refresh"));
    n = refusal_body(b, ASK_WHY_ACCOUNT, NW_ACCOUNT_E_BAD_REFRESH);
    T_CHECK(nc_on_response(id_of(q), 400, b, n, T0) == NULL);
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
        T_CHECK(nc_on_response(id_of(q), 0, NULL, 0, now) == NULL);   // it never arrived
        int due = nc_pump_due_ms(now);
        T_CHECK(due > 0 && due <= NC_RETRY_MAX_MS);
        T_CHECK(nc_next_request(now + due - 1) == NULL);           // not before its backoff
        now += due;
    }
    const NcRequest *q = nc_next_request(now);
    T_CHECK(path_is(q, "/v1/games"));
    const NcDone *d = nc_on_response(id_of(q), 0, NULL, 0, now);
    T_CHECK(d && d->ticket == t && d->why == ASK_WHY_INTERNAL && d->status == 0);

    // A write that never arrived is not sent again: it may have landed.
    NcIntent create = intent(NC_DO_CREATE, NULL);
    t = nc_do(&create, now);
    q = nc_next_request(now);
    T_CHECK(path_is(q, "/v1/create"));
    d = nc_on_response(id_of(q), 0, NULL, 0, now);
    T_CHECK(d && d->ticket == t && d->why == ASK_WHY_INTERNAL);
    T_CHECK(nc_next_request(now + NC_RETRY_MAX_MS) == NULL);

    // At most NC_PUMP_MAX wait at once: one more is refused at once as busy
    // (no status: it never left), not as a server failure.
    for (int i = 0; i < NC_PUMP_MAX; i++) T_CHECK(nc_do(&games, now) != 0);
    T_EQ_INT(nc_do(&games, now), 0);
    T_EQ_INT(nc_error()->why, ASK_WHY_BUSY);
    T_EQ_INT(nc_error()->status, 0);
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
    const NcDone *d = nc_on_response(id_of(q), 500, NULL, 0, T0);
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
            if (len) memcpy(h->table, p, (size_t)len);
            h->table_len = len; h->table_v = e->version;
        } else if (e->kind == NC_EV_VIEW) {
            h->views++;
            if (len) memcpy(h->row, p, (size_t)len);   // a gone seat's view has no payload
            h->row_len = len; h->row_v = e->version;
        }
    }
}

static void host_done(host_t *h, const NcDone *d, const uint8_t *body) {
    if (d) h->tickets++;
    if (d && d->stale) h->stale++;
    if (!d || d->why || d->stale || !d->a_len) return;
    memcpy(h->table, body + d->a_off, d->a_len); h->table_len = (int)d->a_len; h->table_v = d->version;
}

// A push on its topic (a cast on the seat's, or the spectators' when `watch`).
static void host_push_on(host_t *h, int kind, const char *gid, uint32_t v, const uint8_t *p, uint32_t n, int watch,
                         int64_t now) {
    NwFrame f = push(kind, gid, v, p, n);
    feed(&f, watch, now);
    host_events(h);
}

static void host_push(host_t *h, int kind, const char *gid, uint32_t v, const uint8_t *p, uint32_t n, int64_t now) {
    host_push_on(h, kind, gid, v, p, n, 0, now);
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
            live(T0);
            host_t *h = &end[kind][order];
            memset(h, 0, sizeof *h);
            // The table already holds version 4 (a cast).
            uint8_t c4[] = { 0x44 };
            host_push(h, NW_WS_CAST, "g1", 4, c4, 1, T0);
            NcIntent open = intent(NC_DO_OPEN_GAME, "g1");
            T_CHECK(nc_do(&open, T0) != 0);
            const NcRequest *q = nc_next_request(T0);
            T_CHECK(path_is(q, "/v1/games/g1"));
            uint32_t id = id_of(q);
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
    live(T0);
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
    host_answer(&h, id_of(q), 7, env7, 1, T0);
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
    host_answer(&h, id_of(q), 7, again, 1, T0);
    T_EQ_INT(h.tickets, 1);
    T_EQ_INT(h.stale, 1);
    T_EQ_MEM(h.table, h.table_len, env7, 1);
    // A dashboard view at 9 the table may not have shown: an answer at 9 is taken.
    const uint8_t v9[] = { 0x99 }, e9[] = { 0xe9 };
    host_push(&h, NW_WS_VIEW, "g1", 9, v9, 1, T0);
    nc_do(&open, T0);
    q = nc_next_request(T0);
    host_answer(&h, id_of(q), 9, e9, 1, T0);
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
        live(T0);
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
        const NcDone *d = nc_on_response(id_of(q), 200, r, sizeof r, T0);
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
    nc_on_response(id_of(q), 200, r, sizeof r, T0);
    T_EQ_INT(nc_pump_due_ms(T0), -1);
}


// An envelope as the server writes one, for `viewer` (a seat, or -1 for a
// spectator) of a one-seat lobby of `gid` at `v`.
static int envelope_as(const char *gid, uint32_t v, int viewer, uint8_t *out, int cap) {
    int at = env_header_write(out, cap, viewer, v, NULL);
    T_CHECK(at > 0);
    env_header_set_view_len(out, at, 2);
    static Roster r;
    memset(&r, 0, sizeof r);
    T_EQ_INT(roster_seat_add(&r, "u1", 2, "ANA", 3, NULL, 0), 0);
    int t = roster_trailer_write(&r, gid, (int)strlen(gid), 0, 0, 1, out + at + 2, cap - at - 2);
    T_CHECK(t > 0);
    return at + 2 + t;
}

// Seat 0's.
static int envelope(const char *gid, uint32_t v, uint8_t *out, int cap) { return envelope_as(gid, v, 0, out, cap); }

// An ask's request, answered with `env`: the NcDone.
static const NcDone *ask_answered(int kind, const char *gid, const uint8_t *env, int n, int64_t now) {
    TableAsk ask;
    memset(&ask, 0, sizeof ask);
    ask.kind = (uint8_t)kind;
    ask.gid_len = (uint8_t)strlen(gid);
    memcpy(ask.gid, gid, ask.gid_len);
    NcIntent in = intent(NC_DO_ASK, NULL);
    int an = ask_encode(&ask, in.bytes, sizeof in.bytes);
    T_CHECK(an > 0);
    in.bytes_len = (uint16_t)an;
    T_CHECK(nc_do(&in, now) != 0);
    const NcRequest *q = nc_next_request(now);
    T_CHECK(path_is(q, "/v1/meta"));
    return q ? nc_on_response(id_of(q), 200, env, n, now) : NULL;
}

// A game subscription that goes live takes the game whole once (its catch-up),
// unless something newer than what is held is applied first; a chat's does not.
static void suback_takes_the_game_whole(void) {
    signed_in(T0 + 3600000);
    T_EQ_INT(nc_stream_sub(NW_SUB_GAME, "g1", 2), NW_OK);
    T_EQ_INT(nc_stream_sub(NW_SUB_CHAT, "g1", 2), NW_OK);
    live(T0);
    cpkt s = next_pkt(T0), c = next_pkt(T0);
    T_CHECK(s.what == NW_SUB_GAME && c.what == NW_SUB_CHAT);
    T_CHECK(nc_next_request(T0) == NULL);   // nothing before the subscription is live
    suback(c.pid, 0, 1, T0);
    T_CHECK(nc_next_request(T0) == NULL);   // a chat's fetches no game
    suback(s.pid, 0, 2, T0);
    const NcRequest *q = nc_next_request(T0);
    T_CHECK(path_is(q, "/v1/games/g1"));
    host_t h;
    memset(&h, 0, sizeof h);
    const uint8_t e5[] = { 0xe5 }, c6[] = { 0xc6 };
    host_answer(&h, id_of(q), 5, e5, 1, T0);
    T_EQ_INT(h.refetched, 1);
    T_EQ_INT(h.table_v, 5);
    // Reconnected: live again, and the read is queued, but the next version
    // arriving first (nothing was skipped) makes it moot.
    nc_stream_closed(T0);
    live(T0 + 1);
    while ((s = next_pkt(T0 + 1)).type && s.what != NW_SUB_GAME) {}
    suback(s.pid, 0, 2, T0 + 1);
    host_push(&h, NW_WS_CAST, "g1", 6, c6, 1, T0 + 1);
    T_EQ_INT(h.casts, 1);
    T_CHECK(nc_next_request(T0 + 1) == NULL);
    // ... and a version past the next does not: the game is read.
    nc_stream_closed(T0 + 2);
    live(T0 + 2);
    while ((s = next_pkt(T0 + 2)).type && s.what != NW_SUB_GAME) {}
    suback(s.pid, 0, 2, T0 + 2);
    T_CHECK(path_is(nc_next_request(T0 + 2), "/v1/games/g1"));
}

// A subscription the broker never answers is asked again after
// NC_ACK_WAIT_MS, with a new packet id; its SUBACK then takes the game whole.
static void an_unanswered_subscription_is_asked_again(void) {
    signed_in(T0 + 3600000);
    nc_stream_sub(NW_SUB_GAME, "g1", 2);
    live(T0);
    cpkt s1 = next_pkt(T0);
    T_EQ_INT(s1.type, MQTT_SUBSCRIBE);
    T_EQ_INT(nc_stream_due_ms(T0), NC_ACK_WAIT_MS);
    T_EQ_INT(next_pkt(T0 + NC_ACK_WAIT_MS - 1).type, 0);
    cpkt s2 = next_pkt(T0 + NC_ACK_WAIT_MS);
    T_EQ_INT(s2.type, MQTT_SUBSCRIBE);
    T_CHECK(s2.pid != s1.pid);
    T_EQ_INT(suback(s1.pid, 0, 2, T0 + NC_ACK_WAIT_MS)->kind, NC_EV_NONE);   // the first one's, late
    T_CHECK(nc_next_request(T0 + NC_ACK_WAIT_MS) == NULL);
    T_EQ_INT(suback(s2.pid, 0, 2, T0 + NC_ACK_WAIT_MS)->kind, NC_EV_SEATED);
    T_CHECK(path_is(nc_next_request(T0 + NC_ACK_WAIT_MS), "/v1/games/g1"));
    // Answered: never asked again; only the keep-alive goes.
    T_EQ_INT(next_pkt(T0 + 3 * NC_ACK_WAIT_MS + NC_PING_MS).type, MQTT_PINGREQ);
    // Asked again and again, the packet id wraps past 65535 and is never 0.
    signed_in(T0 + 100000000LL);
    nc_stream_sub(NW_SUB_GAMES, NULL, 0);
    live(T0);
    int zero = 0;
    int64_t t = T0;
    for (int i = 0; i < 65540; i++, t += NC_ACK_WAIT_MS) {
        cpkt k = next_pkt(t);
        zero += k.type == MQTT_SUBSCRIBE && k.pid == 0;
    }
    T_EQ_INT(zero, 0);
}

// A game's subscription hears the seat's casts and the spectators': the book
// takes the seat's while the user sits there, the spectators' otherwise, as
// the last envelope or cast says; a create subscribes the new game itself.
static void seat_and_spectator_casts(void) {
    signed_in(T0 + 3600000);
    live(T0);
    uint8_t b[512];
    // A create: the new game is named, and heard from now on, both its streams.
    NcIntent create = intent(NC_DO_CREATE, NULL);
    nc_do(&create, T0);
    const NcRequest *q = nc_next_request(T0);
    int n = envelope("newg01", 1, b, sizeof b);
    const NcDone *d = nc_on_response(id_of(q), 200, b, n, T0);
    T_CHECK(d && d->why == 0 && d->gid_len == 6 && memcmp(d->gid, "newg01", 6) == 0);
    T_CHECK(d && d->version == 1 && d->a_off == 0 && (int)d->a_len == n && !d->stale);
    cpkt s = next_pkt(T0);
    T_CHECK(s.type == MQTT_SUBSCRIBE && s.what == NW_SUB_GAME && strcmp(s.gid, "newg01") == 0 && s.n_topics == 2);
    // The host holds it and lets it go: it is let go.
    nc_stream_sub(NW_SUB_GAME, "newg01", 6);
    nc_stream_unsub(NW_SUB_GAME, "newg01", 6);
    T_EQ_INT(next_pkt(T0).type, MQTT_UNSUBSCRIBE);

    host_t h;
    memset(&h, 0, sizeof h);
    const uint8_t c[] = { 1 };
    // Watching g2: until the book knows the seat, a spectators' cast is a read
    // of the game, never applied (a seated user hears its own cast of it too).
    nc_stream_sub(NW_SUB_GAME, "g2", 2);
    s = next_pkt(T0);
    T_EQ_INT(suback(s.pid, 0, 2, T0)->seated, 0);
    q = nc_next_request(T0);   // the catch-up
    T_CHECK(path_is(q, "/v1/games/g2"));
    uint32_t catch_id = id_of(q);
    host_push_on(&h, NW_WS_CAST, "g2", 7, c, 1, 1, T0);
    T_EQ_INT(h.casts, 0);
    // The read says: a spectator. The spectators' casts apply from then on.
    n = envelope_as("g2", 7, -1, b, sizeof b);
    NwBlob eb = { b, (uint32_t)n };
    static uint8_t vb[600];
    int vn = nw_view_encode(7, eb, vb, sizeof vb);
    nc_on_response(catch_id, 200, vb, vn, T0);
    host_events(&h);
    T_EQ_INT(h.table_v, 7);
    host_push_on(&h, NW_WS_CAST, "g2", 8, c, 1, 1, T0);
    T_EQ_INT(h.casts, 1);
    T_EQ_INT(h.table_v, 8);
    // Joining: the answer is a seat's board, and the seat's casts take over.
    n = envelope("g2", 9, b, sizeof b);
    d = ask_answered(ASK_JOIN, "g2", b, n, T0);
    T_CHECK(d && d->version == 9 && (int)d->a_len == n);
    T_EQ_INT(next_pkt(T0).type, 0);   // nothing to subscribe again: both streams are held
    host_push_on(&h, NW_WS_CAST, "g2", 9, c, 1, 1, T0);   // the join's spectators' cast: not a seat's board
    host_push_on(&h, NW_WS_CAST, "g2", 9, c, 1, 0, T0);   // its seat cast: stale at 9
    T_EQ_INT(h.casts, 1);
    host_push_on(&h, NW_WS_CAST, "g2", 10, c, 1, 1, T0);
    T_EQ_INT(h.casts, 1);
    host_push_on(&h, NW_WS_CAST, "g2", 10, c, 1, 0, T0);
    T_EQ_INT(h.casts, 2);
    // Leaving (an exit of one's own): the answer is the spectators' board, and
    // so are the casts again.
    n = envelope_as("g2", 11, -1, b, sizeof b);
    d = ask_answered(ASK_EXIT, "g2", b, n, T0);
    T_CHECK(d && d->version == 11);
    host_push_on(&h, NW_WS_CAST, "g2", 12, c, 1, 1, T0);
    T_EQ_INT(h.casts, 3);
    // A seat's cast says the user sits there (a join from another device).
    host_push_on(&h, NW_WS_CAST, "g2", 13, c, 1, 0, T0);
    T_EQ_INT(h.casts, 4);
    host_push_on(&h, NW_WS_CAST, "g2", 14, c, 1, 1, T0);
    T_EQ_INT(h.casts, 4);
    // Removed by someone else: the dashboard's empty view; spectators' casts again.
    host_push(&h, NW_WS_VIEW, "g2", 0, NULL, 0, T0);
    host_push_on(&h, NW_WS_CAST, "g2", 15, c, 1, 1, T0);
    T_EQ_INT(h.casts, 5);
    // Any other ask's answer is not a board: its cast animates instead.
    n = envelope("g2", 16, b, sizeof b);
    d = ask_answered(ASK_START, "g2", b, n, T0);
    T_CHECK(d && d->why == 0 && d->gid_len == 2 && d->a_len == 0);
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
    nc_on_response(id_of(q), 200, b, n, T0);
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
    nc_on_response(id_of(q), 200, b, n, T0);
    T_CHECK(nc_event(0) && nc_event(0)->kind == NC_EV_SESSION);
    // A feed that changed no session tells none.
    q = nc_next_request(T0);
    n = nw_list_head(NW_GAMES, 0, b, sizeof b);
    nc_on_response(id_of(q), 200, b, n, T0);
    T_CHECK(nc_event(0) == NULL);
}

static void another_user_starts_the_book_over(void) {
    signed_in(T0 + 3600000);
    nc_stream_sub(NW_SUB_GAMES, NULL, 0);
    live(T0);
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
    live(T0);
    T_EQ_INT(next_pkt(T0).type, 0);
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
    T_EQ_INT(nc_error()->why, ASK_WHY_BUSY);
    // Each request carries its own bytes (the chat's: [v][kind][u16 len][message]).
    const NcRequest *q = nc_next_request(T0);
    T_CHECK(path_is(q, "/v1/chat/g1") && q->body_len == 1004 && q->body[4] == 'a' && q->body[1003] == 'a');
    uint32_t first = id_of(q);
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
    live(T0);
    while (next_pkt(T0).type) {}
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
    host_answer(&h, id_of(q), 6, env, 1, T0);
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
    T_CHECK(nc_on_response(id_of(q), 200, b, n, T0) != NULL);
    T_EQ_INT(next_pkt(T0).type, 0);
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
        T_CHECK(nc_on_response(id_of(q), 503, busy, n, now) == NULL);
        T_EQ_INT(nc_pump_due_ms(now), NC_BUSY_FIRST_MS << k);
        T_CHECK(nc_next_request(now + (NC_BUSY_FIRST_MS << k) - 1) == NULL);
        now += NC_BUSY_FIRST_MS << k;
    }
    const NcRequest *q = nc_next_request(now);
    const NcDone *d = nc_on_response(id_of(q), 503, busy, n, now);
    T_CHECK(d && d->ticket == t && d->why == ASK_WHY_BUSY);
    NcIntent create = intent(NC_DO_CREATE, NULL);
    t = nc_do(&create, now);
    q = nc_next_request(now);
    d = nc_on_response(id_of(q), 503, busy, n, now);
    T_CHECK(d && d->ticket == t && d->why == ASK_WHY_BUSY);
}


// ---- the pump's own work never crowds out the host's ------------------------------------

// A move refused over version 9 of `gid`, its answer read: the reconcile is queued.
static void refused_at_9(const char *gid, int64_t now) {
    uint8_t r[TABLE_RESP_BYTES];
    NcIntent play = intent(NC_DO_PLAY, gid);
    play.bytes_len = 2; play.bytes[0] = 3; play.bytes[1] = 0;
    play.intent = 5;
    uint32_t t = nc_do(&play, now);
    T_CHECK(t != 0);   // a host intent: never refused while only the pump's own work waits
    const NcRequest *q = nc_next_request(now);
    T_CHECK(path_is(q, "/v1/action"));
    T_EQ_INT(table_action_response(TABLE_REJECTED, 4, 9, r, sizeof r), TABLE_RESP_BYTES);
    const NcDone *d = nc_on_response(id_of(q), 200, r, sizeof r, now);
    T_CHECK(d && d->ticket == t && d->action_status == TABLE_STATUS_REJECTED);
}

// Games played back to back faster than NC_RECONCILE_MS, every move refused
// over a newer version (the p3client harness at zero think time): the
// reconciles they queue never refuse the host's next intent, and once they are
// done with, the pump is as it was: NC_PUMP_MAX tickets and the whole arena.
static void fast_games_leave_the_pump_as_it_was(void) {
    signed_in(T0 + 3600000);
    live(T0);
    host_t h;
    memset(&h, 0, sizeof h);
    const uint8_t v5[] = { 0x55 }, c9[] = { 0xc9 };
    char gid[16];   // "g" and any int: gcc checks the bound, not the loop
    int64_t now = T0;
    for (int k = 0; k < 40; k++, now += 20) {
        snprintf(gid, sizeof gid, "g%d", k);
        host_push(&h, NW_WS_VIEW, gid, 5, v5, 1, now);
        refused_at_9(gid, now);
        // Every other game's pushes catch up; the rest never come.
        if (k % 2 == 0) host_push(&h, NW_WS_CAST, gid, 9, c9, 1, now + 5);
    }
    // Only the pump's own reconciles wait: the host still has every ticket.
    NcIntent games = intent(NC_DO_GAMES, NULL);
    for (int i = 0; i < NC_PUMP_MAX; i++) T_CHECK(nc_do(&games, now) != 0);
    T_EQ_INT(nc_do(&games, now), 0);
    T_EQ_INT(nc_error()->why, ASK_WHY_BUSY);
    // Everything waiting is sent and answered (a refusal will do): no
    // reconcile for a game that caught up goes out.
    now += NC_RECONCILE_MS;
    int sent = 0, refetches = 0;
    for (const NcRequest *q; (q = nc_next_request(now)); sent++) {
        if (q->path_len > 11 && memcmp(q->path, "/v1/games/g", 11) == 0) {
            refetches++;
            T_CHECK((atoi(q->path + 11) % 2) == 1);
        }
        nc_on_response(id_of(q), 500, NULL, 0, now);
    }
    T_EQ_INT(sent, NC_PUMP_MAX + refetches);
    T_CHECK(refetches > 0);
    while (nc_next_done()) {}
    T_EQ_INT(nc_pump_due_ms(now), -1);
    // Baseline: the whole arena (exactly NC_ARENA_BYTES, 2,560) and every ticket.
    NcIntent chat = intent(NC_DO_SEND_CHAT, "g1");
    chat.bytes_len = 1000;
    memset(chat.bytes, 'a', 1000);
    T_CHECK(nc_do(&chat, now) != 0);
    T_CHECK(nc_do(&chat, now) != 0);
    chat.bytes_len = 560;
    T_CHECK(nc_do(&chat, now) != 0);
    for (int i = 3; i < NC_PUMP_MAX; i++) T_CHECK(nc_do(&games, now) != 0);
    T_EQ_INT(nc_do(&games, now), 0);
}

// A reconcile is let go the moment the game reaches the version it waits for,
// whatever brings it there (the seat's cast, the dashboard's view, an answered
// read), not when its time comes; a step short of it does not.
static void a_reconcile_lets_go_once_caught_up(void) {
    const uint8_t v5[] = { 0x55 }, c6[] = { 0xc6 }, c9[] = { 0xc9 }, v9[] = { 0x99 }, e9[] = { 0xe9 };
    for (int how = 0; how < 3; how++) {
        signed_in(T0 + 3600000);
        live(T0);
        host_t h;
        memset(&h, 0, sizeof h);
        host_push(&h, NW_WS_VIEW, "g1", 5, v5, 1, T0);
        refused_at_9("g1", T0);
        T_EQ_INT(nc_pump_due_ms(T0), NC_RECONCILE_MS);
        host_push(&h, NW_WS_CAST, "g1", 6, c6, 1, T0 + 10);
        T_EQ_INT(nc_pump_due_ms(T0 + 10), NC_RECONCILE_MS - 10);   // 6 is not 9: still waiting
        if (how == 0) {
            host_push(&h, NW_WS_CAST, "g1", 7, c6, 1, T0 + 20);
            host_push(&h, NW_WS_CAST, "g1", 8, c6, 1, T0 + 20);
            host_push(&h, NW_WS_CAST, "g1", 9, c9, 1, T0 + 20);
        } else if (how == 1) {
            host_push(&h, NW_WS_VIEW, "g1", 9, v9, 1, T0 + 20);
        } else {
            NcIntent open = intent(NC_DO_OPEN_GAME, "g1");
            T_CHECK(nc_do(&open, T0 + 20) != 0);
            const NcRequest *q = nc_next_request(T0 + 20);
            T_CHECK(path_is(q, "/v1/games/g1"));
            host_answer(&h, id_of(q), 9, e9, 1, T0 + 20);
            T_EQ_INT(h.table_v, 9);
        }
        T_EQ_INT(nc_pump_due_ms(T0 + 20), -1);
        T_CHECK(nc_next_request(T0 + NC_RECONCILE_MS) == NULL);
    }
}

// A reconcile is let go when the host lets go of the game's subscription (its
// last hold), and only that game's.
static void a_reconcile_lets_go_with_the_subscription(void) {
    signed_in(T0 + 3600000);
    T_EQ_INT(nc_stream_sub(NW_SUB_GAME, "g1", 2), NW_OK);
    T_EQ_INT(nc_stream_sub(NW_SUB_GAME, "g1", 2), NW_OK);
    live(T0);
    cpkt s = next_pkt(T0);
    T_CHECK(s.what == NW_SUB_GAME);
    suback(s.pid, 0, 2, T0);
    const NcRequest *q = nc_next_request(T0);
    T_CHECK(path_is(q, "/v1/games/g1"));
    host_t h;
    memset(&h, 0, sizeof h);
    const uint8_t e5[] = { 0xe5 }, v5[] = { 0x55 };
    host_answer(&h, id_of(q), 5, e5, 1, T0);
    host_push(&h, NW_WS_VIEW, "g2", 5, v5, 1, T0);
    refused_at_9("g1", T0);
    refused_at_9("g2", T0 + 100);
    T_EQ_INT(nc_pump_due_ms(T0 + 200), NC_RECONCILE_MS - 200);
    T_EQ_INT(nc_stream_unsub(NW_SUB_GAME, "g1", 2), NW_OK);   // one hold left: still wanted
    T_EQ_INT(nc_pump_due_ms(T0 + 200), NC_RECONCILE_MS - 200);
    T_EQ_INT(nc_stream_unsub(NW_SUB_GAME, "g1", 2), NW_OK);   // the last: g1's goes, g2's stays
    T_EQ_INT(nc_pump_due_ms(T0 + 200), NC_RECONCILE_MS - 100);
    q = nc_next_request(T0 + 100 + NC_RECONCILE_MS);
    T_CHECK(path_is(q, "/v1/games/g2"));
    T_CHECK(nc_next_request(T0 + 100 + NC_RECONCILE_MS) == NULL);
}

int main(void) {
    T_RUN(session_blob);
    T_RUN(requests);
    T_RUN(answers);
    T_RUN(connect_carries_the_token);
    T_RUN(connect_waits_for_a_fresh_token);
    T_RUN(closed_before_connack_refreshes);
    T_RUN(stream_order_and_retries);
    T_RUN(holds_made_before_the_session_are_kept);
    T_RUN(what_is_not_the_brokers);
    T_RUN(packets_across_messages);
    T_RUN(an_unanswered_subscription_is_asked_again);
    T_RUN(gates);
    T_RUN(pump_signs_in_and_reads_answers);
    T_RUN(pump_refreshes_first_and_after_a_401);
    T_RUN(pump_retries_reads_not_writes);
    T_RUN(pump_signs_out);
    T_RUN(a_cast_and_a_stale_answer_end_alike);
    T_RUN(a_skipped_version_refetches);
    T_RUN(a_refused_move_catches_up);
    T_RUN(suback_takes_the_game_whole);
    T_RUN(seat_and_spectator_casts);
    T_RUN(the_session_is_told_once);
    T_RUN(another_user_starts_the_book_over);
    T_RUN(waiting_intents_keep_their_bytes);
    T_RUN(a_host_of_boards_gets_boards);
    T_RUN(a_busy_read_goes_again_a_write_does_not);
    T_RUN(fast_games_leave_the_pump_as_it_was);
    T_RUN(a_reconcile_lets_go_once_caught_up);
    T_RUN(a_reconcile_lets_go_with_the_subscription);
    return T_DONE();
}
