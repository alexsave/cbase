// unit_stream_mqtt.c - the push stream over MQTT (realtime/stream_mqtt.h): the
// policy (who may connect, what they may subscribe to) as an authorization
// matrix, and the hub's side of a connection fed bytes the way a WebSocket
// delivers them: whole packets, packets split anywhere, several in one
// message, and every packet a client must not send.
#include "t.h"

#include "realtime/rt_deps.h"
#include "realtime/stream_mqtt.h"
#include "util/err.h"
#include "util/log.h"

#include <stdlib.h>

#define UA "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"
#define UB "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb"
#define NOW_MS 1700000000000LL
#define NOW_S  (NOW_MS / 1000)

static cb_ctx g_ctx;
static int64_t g_exp = NOW_S + 3600;   // the exp the fake token check reports
static int g_verifies;

// "tok-<uid>" is that user's token; "tok-gone" an account deleted since;
// "tok-db" a check that could not run. Anything else does not verify.
static int fake_verify(cb_ctx *ctx, const char *jwt, cb_user *out) {
    g_verifies++;
    if (strcmp(jwt, "tok-db") == 0) return CB_E_DB;
    if (strncmp(jwt, "tok-", 4) != 0 || strcmp(jwt, "tok-gone") == 0) return CB_E_AUTH;
    memset(out, 0, sizeof *out);
    cb_strlcpy(out->id, jwt + 4, sizeof out->id);
    out->expires_at = g_exp;
    return CB_OK;
}

static void setup(void) {
    cb_log_init(CB_LOG_ERROR, 0);
    memset(&g_ctx, 0, sizeof g_ctx);
    cb_rt_deps_g.verify_token = fake_verify;
    g_exp = NOW_S + 3600;
    g_verifies = 0;
}

static void user_ids(void) {
    T_CHECK(cb_mqtt_user_ok(UA));
    T_CHECK(cb_mqtt_user_ok("00000000-0000-4000-8000-000000000000"));
    const char *bad[] = { "", "aaaa", UA "a", "AAAAAAAA-aaaa-4aaa-8aaa-aaaaaaaaaaaa", "aaaaaaaa+aaaa-4aaa-8aaa-aaaaaaaaaaaa",
                          "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaa/", "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaa*",
                          "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaa#", "gaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa",
                          "aaaaaaaaaaaaa-4aaa-8aaa-aaaaaaaaaaaa-" };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) T_CHECK(!cb_mqtt_user_ok(bad[i]));
    T_CHECK(!cb_mqtt_user_ok(NULL));
}

// THE AUTHZ MATRIX, subscribe side: one user, every filter it might ask for.
static void subscribe_policy(void) {
    struct { const char *f; int want; } m[] = {
        { "u/" UA "/v", 1 },        { "u/" UA "/c/g1", 1 },     { "u/" UA "/m/g1", 1 },     { "u/" UA "/#", 1 },
        { "u/" UA "/+/g1", 1 },     { "s/g1", 1 },              { "s/#", 1 },               { "s/+", 1 },
        { "u/" UB "/v", 0 },        { "u/" UB "/#", 0 },        { "u/+/v", 0 },             { "u/#", 0 },
        { "#", 0 },                 { "+/v", 0 },               { "u/" UA, 0 },             { "u/" UA "v", 0 },
        { "u/" UA "/", 0 },         { "s/", 0 },                { "s", 0 },                 { "$SYS/#", 0 },
        { "u//v", 0 },              { "U/" UA "/v", 0 },        { "x/" UA "/v", 0 },        { "u/aaaaaaaa/v", 0 },
        { "u/" UA "0/v", 0 },       { "u/" UA "-/v", 0 },
    };
    for (size_t i = 0; i < sizeof m / sizeof m[0]; i++) {
        int got = cb_mqtt_may_subscribe(UA, m[i].f, (int)strlen(m[i].f));
        if (got != m[i].want) fprintf(stderr, "     filter %s\n", m[i].f);
        T_EQ_INT(got, m[i].want);
    }
    // A user id the policy cannot be written for gets only the public topics.
    T_EQ_INT(cb_mqtt_may_subscribe("u+", "u/u+/v", 6), 0);
    T_EQ_INT(cb_mqtt_may_subscribe("u+", "s/g1", 4), 1);
    T_EQ_INT(cb_mqtt_may_subscribe(UA, NULL, 3), 0);
}

// THE AUTHZ MATRIX, connect side.
static void connect_policy(void) {
    setup();
    cb_mqtt_grant g;
    const char *id = UA "-tab1";
    T_EQ_INT(cb_mqtt_authorize(&g_ctx, "tok-" UA, 40, id, strlen(id), NOW_MS, &g), CB_OK);
    T_EQ_STR(g.user_id, UA);
    T_EQ_INT(g.expires_s, NOW_S + 3600);
    // No client id to check: the broker holds it to the user (IoT's policy).
    T_EQ_INT(cb_mqtt_authorize(&g_ctx, "tok-" UA, 40, NULL, 0, NOW_MS, &g), CB_OK);
    // Refused: a forged or expired token (the check says no), a deleted account,
    // no token, another user's client id, an id that is only the user, or the
    // user with nothing after the dash, or no dash.
    T_EQ_INT(cb_mqtt_authorize(&g_ctx, "forged", 6, id, strlen(id), NOW_MS, &g), CB_E_AUTH);
    T_EQ_INT(cb_mqtt_authorize(&g_ctx, "tok-gone", 8, id, strlen(id), NOW_MS, &g), CB_E_AUTH);
    T_EQ_INT(cb_mqtt_authorize(&g_ctx, "", 0, id, strlen(id), NOW_MS, &g), CB_E_AUTH);
    T_EQ_INT(cb_mqtt_authorize(&g_ctx, NULL, 4, id, strlen(id), NOW_MS, &g), CB_E_AUTH);
    const char *bad_ids[] = { UB "-tab1", UA, UA "-", UA "x", "x" UA "-1", "" };
    for (size_t i = 0; i < sizeof bad_ids / sizeof bad_ids[0]; i++)
        T_EQ_INT(cb_mqtt_authorize(&g_ctx, "tok-" UA, 40, bad_ids[i], strlen(bad_ids[i]), NOW_MS, &g), CB_E_AUTH);
    T_EQ_STR(g.user_id, "");   // a refusal grants nothing
    // A token whose user is not a UUID cannot be written into a policy.
    T_EQ_INT(cb_mqtt_authorize(&g_ctx, "tok-u+", 6, NULL, 0, NOW_MS, &g), CB_E_AUTH);
    // A NUL inside the token, a token longer than the wire carries.
    T_EQ_INT(cb_mqtt_authorize(&g_ctx, "tok-" UA "\0junk", 45, NULL, 0, NOW_MS, &g), CB_E_AUTH);
    static char huge[CB_RT_TOKEN_MAX + 2];
    memset(huge, 'a', sizeof huge - 1);
    int before = g_verifies;
    T_EQ_INT(cb_mqtt_authorize(&g_ctx, huge, CB_RT_TOKEN_MAX + 1, NULL, 0, NOW_MS, &g), CB_E_AUTH);
    T_EQ_INT(g_verifies, before);   // refused before the token is checked
    // The check could not run: not a refusal, the server's failure.
    T_EQ_INT(cb_mqtt_authorize(&g_ctx, "tok-db", 6, NULL, 0, NOW_MS, &g), CB_E_DB);
    // The connection lives until the token's exp, within IoT's floor and cap.
    g_exp = NOW_S + 10;
    T_EQ_INT(cb_mqtt_authorize(&g_ctx, "tok-" UA, 40, NULL, 0, NOW_MS, &g), CB_OK);
    T_EQ_INT(g.expires_s, NOW_S + CB_MQTT_MIN_LIFE_S);
    g_exp = NOW_S + 10 * 86400;
    T_EQ_INT(cb_mqtt_authorize(&g_ctx, "tok-" UA, 40, NULL, 0, NOW_MS, &g), CB_OK);
    T_EQ_INT(g.expires_s, NOW_S + CB_MQTT_MAX_LIFE_S);
}

// ---- the hub's side ---------------------------------------------------------------------

static struct { int n; uint8_t b[16][512]; int len[16]; } g_out;
static void out_fn(void *ud, const char *frame, size_t len) {
    if (g_out.n < 16 && len <= sizeof g_out.b[0]) {
        memcpy(g_out.b[g_out.n], frame, len);
        g_out.len[g_out.n] = (int)len;
    }
    g_out.n++;
}

static int feed(cb_mqtt_conn *c, const uint8_t *b, int n, int64_t now) {
    uint8_t *copy = malloc(n ? (size_t)n : 1);   // exact size: an over-read is an ASan report
    memcpy(copy, b, (size_t)n);
    int rc = cb_mqtt_on_bytes(&g_ctx, c, copy, (size_t)n, now, out_fn, NULL);
    free(copy);
    return rc;
}

static int connect_pkt(uint8_t *out, const char *id, const char *tok, uint16_t ka) {
    return mqtt_write_connect(out, 2048, id, (int)strlen(id), ka, tok ? "jwt" : NULL, 3, (const uint8_t *)tok,
                              tok ? (int)strlen(tok) : 0);
}

static int sub_pkt(uint8_t *out, uint16_t id, const char *f) {
    MqttTopic t = { f, (int)strlen(f), 0 };
    return mqtt_write_subscribe(out, 2048, id, &t, 1);
}

static int out_type(int i) { return i < g_out.n ? g_out.b[i][0] >> 4 : -1; }

static void open_conn(cb_mqtt_conn *c) {
    cb_mqtt_conn_init(c);
    uint8_t b[2048];
    memset(&g_out, 0, sizeof g_out);
    T_EQ_INT(feed(c, b, connect_pkt(b, UA "-t", "tok-" UA, 60), NOW_MS), CB_OK);
    T_EQ_INT(c->state, CB_MQTT_OPEN);
    T_EQ_INT(g_out.n, 1);
    static const uint8_t connack[] = { 0x20, 2, 0, 0 };
    T_EQ_MEM(g_out.b[0], g_out.len[0], connack, 4);
    memset(&g_out, 0, sizeof g_out);
}

static void a_connection_lives(void) {
    setup();
    cb_mqtt_conn *c = malloc(sizeof *c);
    open_conn(c);
    T_EQ_STR(c->user_id, UA);
    T_EQ_INT(c->keepalive_s, 60);
    uint8_t b[2048];
    // SUBSCRIBE: granted at QoS 0, and heard from then on.
    T_EQ_INT(feed(c, b, sub_pkt(b, 7, "u/" UA "/v"), NOW_MS), CB_OK);
    static const uint8_t suback[] = { 0x90, 3, 0, 7, 0 };
    T_EQ_MEM(g_out.b[0], g_out.len[0], suback, 5);
    T_CHECK(cb_mqtt_hears(c, "u/" UA "/v", 2 + 36 + 2));
    T_CHECK(!cb_mqtt_hears(c, "u/" UA "/c/g1", 2 + 36 + 5));
    T_CHECK(!cb_mqtt_hears(c, "u/" UB "/v", 2 + 36 + 2));
    // Two filters in one SUBSCRIBE, one of them a wildcard, and a filter asked twice.
    MqttTopic two[2] = { { "s/+", 3, 0 }, { "u/" UA "/v", 2 + 36 + 2, 1 } };
    memset(&g_out, 0, sizeof g_out);
    T_EQ_INT(feed(c, b, mqtt_write_subscribe(b, sizeof b, 8, two, 2), NOW_MS), CB_OK);
    static const uint8_t suback2[] = { 0x90, 4, 0, 8, 0, 0 };
    T_EQ_MEM(g_out.b[0], g_out.len[0], suback2, 6);
    T_EQ_INT(c->n_subs, 2);
    T_CHECK(cb_mqtt_hears(c, "s/g1", 4));
    T_CHECK(!cb_mqtt_hears(c, "s/g1/x", 6));
    // PINGREQ.
    memset(&g_out, 0, sizeof g_out);
    T_EQ_INT(feed(c, b, mqtt_write_bare(b, sizeof b, MQTT_PINGREQ), NOW_MS), CB_OK);
    T_EQ_INT(out_type(0), MQTT_PINGRESP);
    // UNSUBSCRIBE (one held, one never held): UNSUBACK, and silence on it.
    MqttTopic un[2] = { { "s/+", 3, 0 }, { "s/zz", 4, 0 } };
    memset(&g_out, 0, sizeof g_out);
    T_EQ_INT(feed(c, b, mqtt_write_unsubscribe(b, sizeof b, 9, un, 2), NOW_MS), CB_OK);
    static const uint8_t unsuback[] = { 0xB0, 2, 0, 9 };
    T_EQ_MEM(g_out.b[0], g_out.len[0], unsuback, 4);
    T_CHECK(!cb_mqtt_hears(c, "s/g1", 4));
    T_EQ_INT(c->n_subs, 1);
    // A PUBACK is ignored (the hub sends no QoS 1).
    T_EQ_INT(feed(c, b, mqtt_write_puback(b, sizeof b, 3), NOW_MS), CB_OK);
    // DISCONNECT ends it, quietly.
    memset(&g_out, 0, sizeof g_out);
    T_EQ_INT(feed(c, b, mqtt_write_bare(b, sizeof b, MQTT_DISCONNECT), NOW_MS), CB_E_STATE);
    T_EQ_INT(g_out.n, 0);
    T_EQ_INT(c->state, CB_MQTT_CLOSED);
    T_CHECK(!cb_mqtt_hears(c, "u/" UA "/v", 2 + 36 + 2));
    T_EQ_INT(feed(c, b, mqtt_write_bare(b, sizeof b, MQTT_PINGREQ), NOW_MS), CB_E_STATE);
    free(c);
}

// The ways a connection ends, each with no reply (as IoT ends it).
static void a_connection_ends(void) {
    setup();
    cb_mqtt_conn *c = malloc(sizeof *c);
    uint8_t b[2048];
    struct { const char *why; const char *id, *tok; uint16_t ka; int want; } refused[] = {
        { "forged token", UA "-t", "forged", 60, CB_E_AUTH },
        { "deleted account", UA "-t", "tok-gone", 60, CB_E_AUTH },
        { "no token", UA "-t", NULL, 60, CB_E_AUTH },
        { "another user's id", UB "-t", "tok-" UA, 60, CB_E_AUTH },
        { "keep-alive past IoT's", UA "-t", "tok-" UA, CB_MQTT_KEEPALIVE_MAX + 1, CB_E_INVALID },
        { "check could not run", UA "-t", "tok-db", 60, CB_E_DB },
    };
    for (size_t i = 0; i < sizeof refused / sizeof refused[0]; i++) {
        cb_mqtt_conn_init(c);
        memset(&g_out, 0, sizeof g_out);
        int rc = feed(c, b, connect_pkt(b, refused[i].id, refused[i].tok, refused[i].ka), NOW_MS);
        if (rc != refused[i].want) fprintf(stderr, "     %s: %d\n", refused[i].why, rc);
        T_EQ_INT(rc, refused[i].want);
        T_EQ_INT(g_out.n, 0);   // no CONNACK at all
        T_EQ_INT(c->state, CB_MQTT_CLOSED);
    }
    // The keep-alive at IoT's cap is taken.
    cb_mqtt_conn_init(c);
    T_EQ_INT(feed(c, b, connect_pkt(b, UA "-t", "tok-" UA, CB_MQTT_KEEPALIVE_MAX), NOW_MS), CB_OK);

    // Anything before CONNECT.
    cb_mqtt_conn_init(c);
    T_EQ_INT(feed(c, b, sub_pkt(b, 1, "s/g1"), NOW_MS), CB_E_INVALID);
    cb_mqtt_conn_init(c);
    T_EQ_INT(feed(c, b, mqtt_write_bare(b, sizeof b, MQTT_PINGREQ), NOW_MS), CB_E_INVALID);
    // A second CONNECT.
    open_conn(c);
    T_EQ_INT(feed(c, b, connect_pkt(b, UA "-t", "tok-" UA, 60), NOW_MS), CB_E_INVALID);
    // A subscription outside the policy ends the connection, and nothing in
    // its SUBSCRIBE is kept, not even the filters that were allowed.
    open_conn(c);
    MqttTopic mixed[2] = { { "s/g1", 4, 0 }, { "u/" UB "/v", 2 + 36 + 2, 0 } };
    T_EQ_INT(feed(c, b, mqtt_write_subscribe(b, sizeof b, 2, mixed, 2), NOW_MS), CB_E_FORBIDDEN);
    T_EQ_INT(g_out.n, 0);
    T_EQ_INT(c->n_subs, 0);
    const char *forbidden[] = { "u/" UB "/#", "u/+/v", "#", "u/#" };
    for (size_t i = 0; i < 4; i++) {
        open_conn(c);
        T_EQ_INT(feed(c, b, sub_pkt(b, 2, forbidden[i]), NOW_MS), CB_E_FORBIDDEN);
        T_EQ_INT(g_out.n, 0);
    }
    // A PUBLISH: no client publishes.
    open_conn(c);
    T_EQ_INT(feed(c, b, mqtt_write_publish(b, sizeof b, "u/" UA "/v", 2 + 36 + 2, (const uint8_t *)"x", 1), NOW_MS),
             CB_E_FORBIDDEN);
    // Bytes that are not MQTT (a nw1 frame), a packet a client never sends.
    open_conn(c);
    static const uint8_t nw1[] = { 1, 5, 0, 0 };
    T_EQ_INT(feed(c, nw1, 4, NOW_MS), CB_E_INVALID);
    open_conn(c);
    T_EQ_INT(feed(c, b, mqtt_write_connack(b, sizeof b, 0, 0), NOW_MS), CB_E_INVALID);
    // A packet announcing more than CB_MQTT_PACKET_MAX: refused on its length.
    open_conn(c);
    static const uint8_t big[] = { 0x82, 0xFF, 0x7F };   // SUBSCRIBE of 16383 bytes
    T_EQ_INT(feed(c, big, 3, NOW_MS), CB_E_FULL);
    // The token's expiry: the first packet after it ends the connection.
    open_conn(c);
    int64_t later = (c->expires_s) * 1000;
    T_EQ_INT(feed(c, b, mqtt_write_bare(b, sizeof b, MQTT_PINGREQ), later - 1), CB_OK);
    T_EQ_INT(feed(c, b, mqtt_write_bare(b, sizeof b, MQTT_PINGREQ), later), CB_E_AUTH);
    // CB_MQTT_SUBS filters, then one more.
    open_conn(c);
    char f[64];
    for (int i = 0; i < CB_MQTT_SUBS; i++) {
        snprintf(f, sizeof f, "s/g%d", i);
        T_EQ_INT(feed(c, b, sub_pkt(b, (uint16_t)(i + 1), f), NOW_MS), CB_OK);
    }
    T_EQ_INT(feed(c, b, sub_pkt(b, 99, "s/g0"), NOW_MS), CB_OK);   // held already: no new slot
    T_EQ_INT(feed(c, b, sub_pkt(b, 100, "s/one-more"), NOW_MS), CB_E_FULL);
    free(c);
}

// MQTT over WebSocket: a message may hold part of a packet, or several.
static void packets_across_messages(void) {
    setup();
    cb_mqtt_conn *c = malloc(sizeof *c);
    uint8_t b[4096];
    int n = connect_pkt(b, UA "-t", "tok-" UA, 60);
    int s = sub_pkt(b + n, 5, "u/" UA "/c/g1");
    int p = mqtt_write_bare(b + n + s, 16, MQTT_PINGREQ);
    int total = n + s + p;
    // Byte by byte.
    cb_mqtt_conn_init(c);
    memset(&g_out, 0, sizeof g_out);
    for (int i = 0; i < total; i++) T_EQ_INT(feed(c, b + i, 1, NOW_MS), CB_OK);
    T_EQ_INT(g_out.n, 3);
    T_EQ_INT(out_type(0), MQTT_CONNACK);
    T_EQ_INT(out_type(1), MQTT_SUBACK);
    T_EQ_INT(out_type(2), MQTT_PINGRESP);
    // All three in one message.
    cb_mqtt_conn_init(c);
    memset(&g_out, 0, sizeof g_out);
    T_EQ_INT(feed(c, b, total, NOW_MS), CB_OK);
    T_EQ_INT(g_out.n, 3);
    T_EQ_INT(c->in_len, 0);
    // Split at every point between two messages.
    for (int cut = 1; cut < total; cut++) {
        cb_mqtt_conn_init(c);
        memset(&g_out, 0, sizeof g_out);
        T_EQ_INT(feed(c, b, cut, NOW_MS), CB_OK);
        T_EQ_INT(feed(c, b + cut, total - cut, NOW_MS), CB_OK);
        T_EQ_INT(g_out.n, 3);
        T_CHECK(cb_mqtt_hears(c, "u/" UA "/c/g1", 2 + 36 + 5));
    }
    // A packet that ends the connection stops the message there: what follows is not run.
    cb_mqtt_conn_init(c);
    memset(&g_out, 0, sizeof g_out);
    int k = connect_pkt(b, UA "-t", "tok-" UA, 60);
    k += sub_pkt(b + k, 1, "u/" UB "/v");
    k += sub_pkt(b + k, 2, "s/g1");
    T_EQ_INT(feed(c, b, k, NOW_MS), CB_E_FORBIDDEN);
    T_EQ_INT(g_out.n, 1);   // the CONNACK only
    T_EQ_INT(c->n_subs, 0);
    free(c);
}

// The token may ride in the upgrade URL instead of CONNECT's password.
static void the_query_token(void) {
    setup();
    char t[CB_RT_TOKEN_MAX + 1];
    T_CHECK(cb_mqtt_query_token("token=tok-" UA, t, sizeof t));
    T_EQ_STR(t, "tok-" UA);
    T_CHECK(cb_mqtt_query_token("x-amz-customauthorizer-name=cbase-stream&token=abc%2Edef", t, sizeof t));
    T_EQ_STR(t, "abc.def");
    T_CHECK(cb_mqtt_query_token("", t, sizeof t));
    T_EQ_STR(t, "");
    T_CHECK(cb_mqtt_query_token(NULL, t, sizeof t));
    T_CHECK(!cb_mqtt_query_token("token=0123456789", t, 5));   // does not fit: none at all
    T_EQ_STR(t, "");

    cb_mqtt_conn *c = malloc(sizeof *c);
    cb_mqtt_conn_init(c);
    cb_strlcpy(c->query_token, "tok-" UA, sizeof c->query_token);
    uint8_t b[2048];
    memset(&g_out, 0, sizeof g_out);
    T_EQ_INT(feed(c, b, connect_pkt(b, UA "-q", NULL, 60), NOW_MS), CB_OK);
    T_EQ_INT(out_type(0), MQTT_CONNACK);
    // The password wins over the query: a bad password is refused whatever the URL held.
    cb_mqtt_conn_init(c);
    cb_strlcpy(c->query_token, "tok-" UA, sizeof c->query_token);
    T_EQ_INT(feed(c, b, connect_pkt(b, UA "-q", "forged", 60), NOW_MS), CB_E_AUTH);
    free(c);
}

int main(void) {
    T_RUN(user_ids);
    T_RUN(subscribe_policy);
    T_RUN(connect_policy);
    T_RUN(a_connection_lives);
    T_RUN(a_connection_ends);
    T_RUN(packets_across_messages);
    T_RUN(the_query_token);
    return T_DONE();
}
