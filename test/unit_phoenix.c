// unit_phoenix.c - the Phoenix protocol (realtime/phoenix.h) and the push
// payloads (realtime/realtime.h): frames in both serializers, the join
// authorization matrix, and the exact bytes of every push.
#include "t.h"

#include "lib_rt_fakestore.h"
#include "realtime/phoenix.h"
#include "realtime/realtime.h"
#include "realtime/rt_rows.h"
#include "util/clock.h"
#include "util/json.h"

#include <stdlib.h>

#define U1 "11111111-1111-4111-8111-111111111111"
#define U2 "22222222-2222-4222-8222-222222222222"
#define G  "ab12cd"

static cb_ctx g_ctx;
static cb_config g_cfg;

// ---- a reply recorder ---------------------------------------------------------------

typedef struct { int n; char f[24][2048]; } sent;

static void rec(void *ud, const char *frame, size_t len) {
    sent *s = ud;
    if (s->n < 24) cb_strncpy0(s->f[s->n++], sizeof s->f[0], frame, len < sizeof s->f[0] ? len : 0);
}

static int on(cb_phx_conn *c, sent *s, const char *frame) {
    s->n = 0;
    return cb_phx_on_frame(&g_ctx, c, frame, strlen(frame), rec, s);
}

static void setup(void) {
    rtf_install();
    memset(&g_cfg, 0, sizeof g_cfg);
    strcpy(g_cfg.anon_key, "anon-key");
    memset(&g_ctx, 0, sizeof g_ctx);
    g_ctx.cfg = &g_cfg;
    g_ctx.store = &rtf.store;
}

// ---- frames -----------------------------------------------------------------------------

static void decode_both_serializers(void) {
    cb_phx_msg m;
    const char *v2 = "[\"3\",\"4\",\"realtime:gu-ab12cd-x\",\"phx_join\",{\"access_token\":\"t\"}]";
    T_EQ_INT(cb_phx_decode(v2, strlen(v2), 2, &m), CB_OK);
    T_CHECK(cb_span_eq(m.join_ref, "3"));
    T_CHECK(cb_span_eq(m.ref, "4"));
    T_EQ_STR(m.topic, "realtime:gu-ab12cd-x");
    T_EQ_STR(m.event, "phx_join");
    T_CHECK(cb_span_eq(m.payload, "{\"access_token\":\"t\"}"));
    // The heartbeat: null join_ref.
    const char *hb = "[null,\"9\",\"phoenix\",\"heartbeat\",{}]";
    T_EQ_INT(cb_phx_decode(hb, strlen(hb), 2, &m), CB_OK);
    T_EQ_INT(m.join_ref.n, 0);
    T_CHECK(cb_span_eq(m.ref, "9"));
    // vsn 1: the same message as an object, keys in any order.
    const char *v1 = "{\"topic\":\"realtime:pv-u\",\"event\":\"phx_leave\",\"payload\":{},\"ref\":\"7\",\"join_ref\":\"6\"}";
    T_EQ_INT(cb_phx_decode(v1, strlen(v1), 1, &m), CB_OK);
    T_EQ_STR(m.topic, "realtime:pv-u");
    T_EQ_STR(m.event, "phx_leave");
    T_CHECK(cb_span_eq(m.join_ref, "6"));
    T_CHECK(cb_span_eq(m.ref, "7"));
    // An escaped topic is unescaped.
    const char *esc = "[null,null,\"realtime:a\\u0062c\",\"x\",{}]";
    T_EQ_INT(cb_phx_decode(esc, strlen(esc), 2, &m), CB_OK);
    T_EQ_STR(m.topic, "realtime:abc");
}

static void decode_refusals(void) {
    cb_phx_msg m;
    static const char *const bad2[] = {
        "",                                           // nothing
        "[\"1\",\"1\",\"t\",\"e\"]",                  // four elements
        "{\"topic\":\"t\",\"event\":\"e\",\"payload\":{}}",   // an object under vsn 2
        "[\"1\",\"1\",7,\"e\",{}]",                   // a number for the topic
        "[\"1\",\"1\",\"t\",null,{}]",                // no event
        "[\"1\\n\",\"1\",\"t\",\"e\",{}]",            // an escape in a ref
        "[1,\"1\",\"t\",\"e\",{}]",                   // a number for a ref
        "[\"1\",\"1\",\"t\",\"e\",{}",                // cut short
        "[\"1\",\"1\",\"t\",\"e\",{}] x",             // trailing garbage
    };
    for (size_t i = 0; i < sizeof bad2 / sizeof bad2[0]; i++) T_EQ_INT(cb_phx_decode(bad2[i], strlen(bad2[i]), 2, &m), CB_E_INVALID);
    const char *arr = "[null,null,\"t\",\"e\",{}]";
    T_EQ_INT(cb_phx_decode(arr, strlen(arr), 1, &m), CB_E_INVALID);   // an array under vsn 1
    const char *nopayload = "{\"topic\":\"t\",\"event\":\"e\"}";
    T_EQ_INT(cb_phx_decode(nopayload, strlen(nopayload), 1, &m), CB_E_INVALID);
    T_EQ_INT(cb_phx_decode(arr, strlen(arr), 3, &m), CB_E_INVALID);
    char longtopic[400] = "[null,null,\"";
    memset(longtopic + strlen(longtopic), 'a', 200);
    strcat(longtopic, "\",\"e\",{}]");
    T_EQ_INT(cb_phx_decode(longtopic, strlen(longtopic), 2, &m), CB_E_INVALID);
}

static void writers_byte_exact(void) {
    cb_phx_msg m;
    const char *hb = "[null,\"5\",\"phoenix\",\"heartbeat\",{}]";
    T_EQ_INT(cb_phx_decode(hb, strlen(hb), 2, &m), CB_OK);
    cb_buf b;
    cb_buf_init(&b);
    cb_phx_reply(&b, 2, &m, true, "{}");
    T_EQ_STR(cb_buf_cstr(&b), "[null,\"5\",\"phoenix\",\"phx_reply\",{\"response\":{},\"status\":\"ok\"}]");
    cb_phx_reply(&b, 1, &m, false, "{\"reason\":\"no\"}");
    T_EQ_STR(cb_buf_cstr(&b), "{\"join_ref\":null,\"ref\":\"5\",\"topic\":\"phoenix\",\"event\":\"phx_reply\","
                              "\"payload\":{\"response\":{\"reason\":\"no\"},\"status\":\"error\"}}");
    cb_phx_push(&b, 2, "realtime:game-ab12cd", "broadcast", "{\"x\":1}");
    T_EQ_STR(cb_buf_cstr(&b), "[null,null,\"realtime:game-ab12cd\",\"broadcast\",{\"x\":1}]");
    cb_phx_push(&b, 1, "realtime:game-ab12cd", "broadcast", "{\"x\":1}");
    T_EQ_STR(cb_buf_cstr(&b), "{\"join_ref\":null,\"ref\":null,\"topic\":\"realtime:game-ab12cd\",\"event\":\"broadcast\",\"payload\":{\"x\":1}}");
    cb_buf_free(&b);
}

static void vsn_and_binding_ids(void) {
    T_EQ_INT(cb_phx_vsn_of_query("apikey=k&vsn=2.0.0"), 2);
    T_EQ_INT(cb_phx_vsn_of_query("vsn=1.0.0&apikey=k"), 1);
    T_EQ_INT(cb_phx_vsn_of_query("apikey=k"), 1);
    T_EQ_INT(cb_phx_vsn_of_query(""), 1);
    T_EQ_INT(cb_phx_vsn_of_query("vsn=3.0.0"), CB_E_INVALID);
    int32_t a = cb_phx_binding_id("UPDATE", "public", "player_views", "player_id=eq." U1);
    T_EQ_INT(a, cb_phx_binding_id("UPDATE", "public", "player_views", "player_id=eq." U1));
    T_CHECK(a > 0);
    T_CHECK(a != cb_phx_binding_id("INSERT", "public", "player_views", "player_id=eq." U1));
    T_CHECK(a != cb_phx_binding_id("UPDATE", "public", "player_views", "player_id=eq." U2));
    // The separator: moving a character between fields changes the id.
    T_CHECK(cb_phx_binding_id("ab", "", "", "") != cb_phx_binding_id("a", "b", "", ""));
}

// ---- the topic policy -----------------------------------------------------------------

static void may_join_matrix(void) {
    setup();
    rtf_member(G, U1);
    T_EQ_INT(cb_phx_may_join(&g_ctx, U1, "pv-" U1), 1);
    T_EQ_INT(cb_phx_may_join(&g_ctx, U2, "pv-" U1), 0);
    T_EQ_INT(cb_phx_may_join(&g_ctx, NULL, "pv-" U1), 0);
    T_EQ_INT(cb_phx_may_join(&g_ctx, "", "pv-"), 0);
    T_EQ_INT(cb_phx_may_join(&g_ctx, U1, "gu-" G "-" U1), 1);
    T_EQ_INT(cb_phx_may_join(&g_ctx, U2, "gu-" G "-" U2), 0);   // not a member
    T_EQ_INT(cb_phx_may_join(&g_ctx, U2, "gu-" G "-" U1), 0);   // someone else's stream
    T_EQ_INT(cb_phx_may_join(&g_ctx, U1, "gu--" U1), 0);        // no game
    T_EQ_INT(cb_phx_may_join(&g_ctx, U1, "gu-" G U1), 0);       // no separator
    T_EQ_INT(cb_phx_may_join(&g_ctx, U1, "gu-zzzzzz-" U1), 0);  // another game
    T_EQ_INT(cb_phx_may_join(&g_ctx, U1, "chat:" G), 1);
    T_EQ_INT(cb_phx_may_join(&g_ctx, U2, "chat:" G), 0);
    T_EQ_INT(cb_phx_may_join(&g_ctx, U1, "chat:"), 0);
    T_EQ_INT(cb_phx_may_join(&g_ctx, U2, "game-" G), 1);        // any signed-in user
    T_EQ_INT(cb_phx_may_join(&g_ctx, NULL, "game-" G), 0);
    T_EQ_INT(cb_phx_may_join(&g_ctx, U1, "game-"), 0);
    T_EQ_INT(cb_phx_may_join(&g_ctx, U1, "user-" U1), 0);       // no such family
    T_EQ_INT(cb_phx_may_join(&g_ctx, U1, "room:lobby"), 0);
    rtf.is_member_rc = CB_E_DB;
    T_EQ_INT(cb_phx_may_join(&g_ctx, U1, "chat:" G), CB_E_DB);
    g_ctx.store = NULL;
    T_EQ_INT(cb_phx_may_join(&g_ctx, U1, "chat:" G), CB_E_STATE);
}

// ---- the server side of a connection -------------------------------------------------------

static cb_phx_conn conn2(void) {
    cb_phx_conn c;
    memset(&c, 0, sizeof c);
    c.vsn = 2;
    strcpy(c.conn_id, "c1");
    return c;
}

static void heartbeat_and_garbage(void) {
    setup();
    cb_phx_conn c = conn2();
    sent s;
    T_EQ_INT(on(&c, &s, "[null,\"1\",\"phoenix\",\"heartbeat\",{}]"), CB_OK);
    T_EQ_INT(s.n, 1);
    T_EQ_STR(s.f[0], "[null,\"1\",\"phoenix\",\"phx_reply\",{\"response\":{},\"status\":\"ok\"}]");
    T_EQ_INT(on(&c, &s, "not json"), CB_E_INVALID);
    T_EQ_INT(s.n, 0);
    // An unknown event on an unjoined topic is ignored.
    T_EQ_INT(on(&c, &s, "[\"1\",\"2\",\"realtime:x\",\"broadcast\",{}]"), CB_OK);
    T_EQ_INT(s.n, 0);
}

static void join_echoes_postgres_bindings(void) {
    setup();
    cb_phx_conn c = conn2();
    sent s;
    // realtime-js's own join payload for the pv- channel (RealtimeChannel.subscribe).
    const char *join =
        "[\"1\",\"1\",\"realtime:pv-" U1 "\",\"phx_join\",{\"config\":{\"broadcast\":{\"ack\":false,\"self\":false},"
        "\"presence\":{\"key\":\"\",\"enabled\":false},\"postgres_changes\":["
        "{\"event\":\"INSERT\",\"schema\":\"public\",\"table\":\"player_views\",\"filter\":\"player_id=eq." U1 "\"},"
        "{\"event\":\"UPDATE\",\"schema\":\"public\",\"table\":\"player_views\",\"filter\":\"player_id=eq." U1 "\"},"
        "{\"event\":\"DELETE\",\"schema\":\"public\",\"table\":\"player_views\",\"filter\":\"player_id=eq." U1 "\"}],"
        "\"private\":false},\"access_token\":\"tok-" U1 "\"}]";
    T_EQ_INT(on(&c, &s, join), CB_OK);
    T_EQ_INT(s.n, 1);
    char want[2048];
    snprintf(want, sizeof want,
             "[\"1\",\"1\",\"realtime:pv-" U1 "\",\"phx_reply\",{\"response\":{\"postgres_changes\":["
             "{\"event\":\"INSERT\",\"schema\":\"public\",\"table\":\"player_views\",\"filter\":\"player_id=eq." U1 "\",\"id\":%d},"
             "{\"event\":\"UPDATE\",\"schema\":\"public\",\"table\":\"player_views\",\"filter\":\"player_id=eq." U1 "\",\"id\":%d},"
             "{\"event\":\"DELETE\",\"schema\":\"public\",\"table\":\"player_views\",\"filter\":\"player_id=eq." U1 "\",\"id\":%d}]},"
             "\"status\":\"ok\"}]",
             (int)cb_phx_binding_id("INSERT", "public", "player_views", "player_id=eq." U1),
             (int)cb_phx_binding_id("UPDATE", "public", "player_views", "player_id=eq." U1),
             (int)cb_phx_binding_id("DELETE", "public", "player_views", "player_id=eq." U1));
    T_EQ_STR(s.f[0], want);
    T_EQ_INT(c.n_topics, 1);
    T_EQ_STR(c.topics[0], "pv-" U1);
    T_EQ_STR(c.join_refs[0], "1");
    T_EQ_STR(c.user_id, U1);
    // A binding with no fields at all still gets an id.
    T_EQ_INT(on(&c, &s, "[\"2\",\"2\",\"realtime:game-" G "\",\"phx_join\",{\"config\":{\"postgres_changes\":[{}]},\"access_token\":\"tok-" U1 "\"}]"), CB_OK);
    snprintf(want, sizeof want, "{\"response\":{\"postgres_changes\":[{\"id\":%d}]},\"status\":\"ok\"}", (int)cb_phx_binding_id("", "", "", ""));
    T_CHECK(strstr(s.f[0], want) != NULL);
    // A broadcast channel answers {}.
    T_EQ_INT(on(&c, &s, "[\"3\",\"3\",\"realtime:chat:" G "\",\"phx_join\",{\"config\":{\"private\":true},\"access_token\":\"tok-" U1 "\"}]"), CB_OK);
    T_CHECK(strstr(s.f[0], "\"status\":\"error\"") != NULL);   // U1 is not a member yet
    rtf_member(G, U1);
    T_EQ_INT(on(&c, &s, "[\"3\",\"3\",\"realtime:chat:" G "\",\"phx_join\",{\"config\":{\"private\":true},\"access_token\":\"tok-" U1 "\"}]"), CB_OK);
    T_EQ_STR(s.f[0], "[\"3\",\"3\",\"realtime:chat:" G "\",\"phx_reply\",{\"response\":{},\"status\":\"ok\"}]");
    T_EQ_INT(c.n_topics, 3);
}

static void join_refusals(void) {
    setup();
    rtf_member(G, U1);
    cb_phx_conn c = conn2();
    sent s;
    struct { const char *frame, *reason; } cases[] = {
        { "[\"1\",\"1\",\"realtime:pv-" U2 "\",\"phx_join\",{\"access_token\":\"tok-" U1 "\"}]",
          "Unauthorized: You do not have permissions to read from this Channel topic: pv-" U2 },
        { "[\"1\",\"1\",\"realtime:gu-" G "-" U1 "\",\"phx_join\",{\"access_token\":\"tok-" U2 "\"}]",
          "Unauthorized: You do not have permissions to read from this Channel topic: gu-" G "-" U1 },
        { "[\"1\",\"1\",\"realtime:game-" G "\",\"phx_join\",{}]",
          "Unauthorized: You do not have permissions to read from this Channel topic: game-" G },
        { "[\"1\",\"1\",\"realtime:game-" G "\",\"phx_join\",{\"access_token\":\"anon-key\"}]",
          "Unauthorized: You do not have permissions to read from this Channel topic: game-" G },
        { "[\"1\",\"1\",\"realtime:game-" G "\",\"phx_join\",{\"access_token\":\"expired\"}]", "Invalid token" },
        { "[\"1\",\"1\",\"game-" G "\",\"phx_join\",{\"access_token\":\"tok-" U1 "\"}]", "topic must be realtime:<topic>" },
        { "[\"1\",\"1\",\"realtime:pv-" U1 "\",\"phx_join\",{\"access_token\":\"tok-" U1 "\",\"config\":{\"postgres_changes\":[7]}}]",
          "malformed postgres_changes binding" },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        T_EQ_INT(on(&c, &s, cases[i].frame), CB_OK);
        T_EQ_INT(s.n, 1);
        char want[512];
        snprintf(want, sizeof want, "{\"response\":{\"reason\":\"%s\"},\"status\":\"error\"}]", cases[i].reason);
        if (!t_report(strstr(s.f[0], want) != NULL, __FILE__, __LINE__, "refusal reason")) fprintf(stderr, "     case %zu got %s\n", i, s.f[0]);
    }
    T_EQ_INT(c.n_topics, 0);
    T_EQ_STR(c.user_id, "");
    // A store failure is an error reply, not a crash or an admission.
    rtf.is_member_rc = CB_E_DB;
    T_EQ_INT(on(&c, &s, "[\"1\",\"1\",\"realtime:chat:" G "\",\"phx_join\",{\"access_token\":\"tok-" U1 "\"}]"), CB_OK);
    T_CHECK(strstr(s.f[0], "Internal error") != NULL);
    T_EQ_INT(c.n_topics, 0);
}

static void too_many_channels(void) {
    setup();
    cb_phx_conn c = conn2();
    sent s;
    char f[256];
    for (int i = 0; i < CB_CONN_TOPICS + 1; i++) {
        snprintf(f, sizeof f, "[\"%d\",\"%d\",\"realtime:game-g%05d\",\"phx_join\",{\"access_token\":\"tok-" U1 "\"}]", i, i, i);
        T_EQ_INT(on(&c, &s, f), CB_OK);
    }
    T_EQ_INT(c.n_topics, CB_CONN_TOPICS);
    T_CHECK(strstr(s.f[0], "too many channels") != NULL);
    // Rejoining a topic already held updates its join_ref, it does not add one.
    T_EQ_INT(on(&c, &s, "[\"99\",\"99\",\"realtime:game-g00003\",\"phx_join\",{\"access_token\":\"tok-" U1 "\"}]"), CB_OK);
    T_CHECK(strstr(s.f[0], "\"status\":\"ok\"") != NULL);
    T_EQ_INT(c.n_topics, CB_CONN_TOPICS);
    T_EQ_STR(c.join_refs[3], "99");
}

static void leave_and_access_token(void) {
    setup();
    rtf_member(G, U1);
    cb_phx_conn c = conn2();
    sent s;
    on(&c, &s, "[\"4\",\"4\",\"realtime:gu-" G "-" U1 "\",\"phx_join\",{\"access_token\":\"tok-" U1 "\"}]");
    on(&c, &s, "[\"5\",\"5\",\"realtime:game-" G "\",\"phx_join\",{\"access_token\":\"tok-" U1 "\"}]");
    T_EQ_INT(c.n_topics, 2);
    // A fresh token for the same user changes nothing and is not answered.
    T_EQ_INT(on(&c, &s, "[\"4\",\"6\",\"realtime:gu-" G "-" U1 "\",\"access_token\",{\"access_token\":\"tok-" U1 "\"}]"), CB_OK);
    T_EQ_INT(s.n, 0);
    T_EQ_INT(c.n_topics, 2);
    // A token that does not verify ends that channel only.
    T_EQ_INT(on(&c, &s, "[\"5\",\"7\",\"realtime:game-" G "\",\"access_token\",{\"access_token\":\"expired\"}]"), CB_OK);
    T_EQ_INT(s.n, 1);
    T_EQ_STR(s.f[0], "[\"5\",\"5\",\"realtime:game-" G "\",\"phx_close\",{}]");
    T_EQ_INT(c.n_topics, 1);
    T_EQ_STR(c.topics[0], "gu-" G "-" U1);
    // phx_leave: ok on the leave's ref, then phx_close on the join_ref.
    T_EQ_INT(on(&c, &s, "[\"4\",\"8\",\"realtime:gu-" G "-" U1 "\",\"phx_leave\",{}]"), CB_OK);
    T_EQ_INT(s.n, 2);
    T_EQ_STR(s.f[0], "[\"4\",\"8\",\"realtime:gu-" G "-" U1 "\",\"phx_reply\",{\"response\":{},\"status\":\"ok\"}]");
    T_EQ_STR(s.f[1], "[\"4\",\"4\",\"realtime:gu-" G "-" U1 "\",\"phx_close\",{}]");
    T_EQ_INT(c.n_topics, 0);
    // Leaving a topic never joined is still answered ok.
    T_EQ_INT(on(&c, &s, "[\"9\",\"9\",\"realtime:game-zz\",\"phx_leave\",{}]"), CB_OK);
    T_EQ_INT(s.n, 1);
}

static void join_as_another_user_drops_the_old_channels(void) {
    setup();
    cb_phx_conn c = conn2();
    sent s;
    on(&c, &s, "[\"1\",\"1\",\"realtime:pv-" U1 "\",\"phx_join\",{\"access_token\":\"tok-" U1 "\"}]");
    T_EQ_INT(c.n_topics, 1);
    T_EQ_INT(on(&c, &s, "[\"2\",\"2\",\"realtime:pv-" U2 "\",\"phx_join\",{\"access_token\":\"tok-" U2 "\"}]"), CB_OK);
    T_EQ_INT(s.n, 2);
    T_EQ_STR(s.f[0], "[\"1\",\"1\",\"realtime:pv-" U1 "\",\"phx_close\",{}]");
    T_EQ_INT(c.n_topics, 1);
    T_EQ_STR(c.topics[0], "pv-" U2);
    T_EQ_STR(c.user_id, U2);
}

static void vsn1_connection_speaks_objects(void) {
    setup();
    cb_phx_conn c = conn2();
    c.vsn = 1;
    sent s;
    T_EQ_INT(on(&c, &s, "{\"join_ref\":\"1\",\"ref\":\"1\",\"topic\":\"realtime:game-" G "\",\"event\":\"phx_join\","
                        "\"payload\":{\"config\":{\"postgres_changes\":[{\"event\":\"*\",\"schema\":\"public\",\"table\":\"spectator_views\",\"filter\":\"game_id=eq." G "\"}]},"
                        "\"access_token\":\"tok-" U1 "\"}}"), CB_OK);
    char want[1024];
    snprintf(want, sizeof want,
             "{\"join_ref\":\"1\",\"ref\":\"1\",\"topic\":\"realtime:game-" G "\",\"event\":\"phx_reply\",\"payload\":{\"response\":"
             "{\"postgres_changes\":[{\"event\":\"*\",\"schema\":\"public\",\"table\":\"spectator_views\",\"filter\":\"game_id=eq." G "\",\"id\":%d}]},"
             "\"status\":\"ok\"}}",
             (int)cb_phx_binding_id("*", "public", "spectator_views", "game_id=eq." G));
    T_EQ_STR(s.f[0], want);
    // An array frame on a vsn 1 connection does not decode.
    T_EQ_INT(on(&c, &s, "[null,\"1\",\"phoenix\",\"heartbeat\",{}]"), CB_E_INVALID);
}

// ---- the push payloads -------------------------------------------------------------------

static int64_t ms_of(const char *iso) {
    int64_t ms = 0;
    cb_iso8601_parse(iso, strlen(iso), &ms);
    return ms;
}

static cb_view_row pv_row(void) {
    static uint8_t view[] = { 0x01, 0x02, 0xff };
    cb_view_row r;
    memset(&r, 0, sizeof r);
    strcpy(r.game_id, G);
    strcpy(r.player_id, U1);
    r.view = view;
    r.view_len = 3;
    r.version = 7;
    strcpy(r.status, "playing");
    r.updated_at = ms_of("2026-10-13T12:34:56.789Z");
    return r;
}

#define PV_COLS_JSON \
    "[{\"name\":\"game_id\",\"type\":\"text\"},{\"name\":\"player_id\",\"type\":\"uuid\"},{\"name\":\"view\",\"type\":\"bytea\"}," \
    "{\"name\":\"version\",\"type\":\"int8\"},{\"name\":\"status\",\"type\":\"text\"},{\"name\":\"updated_at\",\"type\":\"timestamptz\"}]"

static void pv_payloads(void) {
    cb_view_row r = pv_row();
    const char *f = "player_id=eq." U1;
    cb_buf b;
    cb_buf_init(&b);
    char want[4096];
    cb_rt_pv_payload(&b, CB_RT_UPDATE, &r);
    snprintf(want, sizeof want,
             "{\"data\":{\"columns\":" PV_COLS_JSON ",\"commit_timestamp\":\"2026-10-13T12:34:56.789Z\",\"errors\":null,"
             "\"old_record\":{\"game_id\":\"" G "\",\"player_id\":\"" U1 "\"},"
             "\"record\":{\"game_id\":\"" G "\",\"player_id\":\"" U1 "\",\"status\":\"playing\",\"updated_at\":\"2026-10-13T12:34:56.789+00:00\","
             "\"version\":7,\"view\":\"\\\\x0102ff\"},\"schema\":\"public\",\"table\":\"player_views\",\"type\":\"UPDATE\"},"
             "\"ids\":[%d,%d,%d,%d]}",
             (int)cb_phx_binding_id("UPDATE", "public", "player_views", f), (int)cb_phx_binding_id("UPDATE", "public", "player_views", ""),
             (int)cb_phx_binding_id("*", "public", "player_views", f), (int)cb_phx_binding_id("*", "public", "player_views", ""));
    T_EQ_STR(cb_buf_cstr(&b), want);
    T_CHECK(cb_json_valid((const char *)b.p, b.len));
    // INSERT carries no old_record; DELETE no record, only the key.
    cb_buf_reset(&b);
    cb_rt_pv_payload(&b, CB_RT_INSERT, &r);
    T_CHECK(cb_json_find((const char *)b.p, b.len, "data.old_record", NULL) == CB_JSON_MISSING);
    T_CHECK(cb_json_find((const char *)b.p, b.len, "data.record.view", NULL) == CB_JSON_STR);
    char type[16];
    T_CHECK(cb_json_get_str((const char *)b.p, b.len, "data.type", type, sizeof type));
    T_EQ_STR(type, "INSERT");
    int64_t id0 = 0;
    T_CHECK(cb_json_get_i64((const char *)b.p, b.len, "ids.0", &id0));
    T_EQ_INT(id0, cb_phx_binding_id("INSERT", "public", "player_views", f));
    cb_buf_reset(&b);
    cb_rt_pv_payload(&b, CB_RT_DELETE, &r);
    T_CHECK(cb_json_find((const char *)b.p, b.len, "data.record", NULL) == CB_JSON_MISSING);
    char gid[16];
    T_CHECK(cb_json_get_str((const char *)b.p, b.len, "data.old_record.game_id", gid, sizeof gid));
    T_EQ_STR(gid, G);
    T_CHECK(cb_json_find((const char *)b.p, b.len, "data.old_record.view", NULL) == CB_JSON_MISSING);
    cb_buf_free(&b);
}

static void cast_and_chat_payloads(void) {
    cb_buf b;
    cb_buf_init(&b);
    const uint8_t cast[] = { 1, 2, 3, 0xfe };
    cb_rt_cast_payload(&b, cast, sizeof cast);
    T_EQ_STR(cb_buf_cstr(&b), "{\"event\":\"animation_events\",\"payload\":{\"b\":\"AQID/g==\"},\"type\":\"broadcast\"}");
    // The fake backend's recorded frame for the same push (ws_phoenix.ts
    // PhxHub.broadcast) carries the same values at the same paths.
    const char *fake = "{\"type\":\"broadcast\",\"event\":\"animation_events\",\"payload\":{\"b\":\"AQID/g==\"}}";
    static const char *const paths[] = { "type", "event", "payload.b" };
    for (int i = 0; i < 3; i++) {
        char x[64], y[64];
        T_CHECK(cb_json_get_str((const char *)b.p, b.len, paths[i], x, sizeof x));
        T_CHECK(cb_json_get_str(fake, strlen(fake), paths[i], y, sizeof y));
        T_EQ_STR(x, y);
    }
    cb_buf_reset(&b);
    cb_chat_row c;
    memset(&c, 0, sizeof c);
    strcpy(c.id, "33333333-3333-4333-8333-333333333333");
    strcpy(c.game_id, G);
    strcpy(c.user_id, U1);
    c.message = "hi \"there\"";
    c.created_at = ms_of("2026-10-13T12:34:56Z");
    cb_rt_chat_payload(&b, &c);
    T_EQ_STR(cb_buf_cstr(&b),
             "{\"event\":\"INSERT\",\"payload\":{\"old_record\":null,\"operation\":\"INSERT\",\"record\":{"
             "\"created_at\":\"2026-10-13T12:34:56\",\"game_id\":\"" G "\",\"id\":\"33333333-3333-4333-8333-333333333333\","
             "\"is_system\":false,\"message\":\"hi \\\"there\\\"\",\"user_id\":\"" U1 "\"},"
             "\"schema\":\"public\",\"table\":\"chat_messages\"},\"type\":\"broadcast\"}");
    cb_buf_free(&b);
}

static void timestamps_as_postgres_prints_them(void) {
    char t[48];
    T_EQ_INT(cb_rt_ts(ms_of("2026-10-13T12:34:56.789Z"), false, t, sizeof t), 23);
    T_EQ_STR(t, "2026-10-13T12:34:56.789");
    cb_rt_ts(ms_of("2026-10-13T12:34:56.780Z"), true, t, sizeof t);
    T_EQ_STR(t, "2026-10-13T12:34:56.78+00:00");
    cb_rt_ts(ms_of("2026-10-13T12:34:56Z"), true, t, sizeof t);
    T_EQ_STR(t, "2026-10-13T12:34:56+00:00");
    cb_rt_ts(ms_of("2026-01-02T03:04:05.006Z"), false, t, sizeof t);
    T_EQ_STR(t, "2026-01-02T03:04:05.006");
    T_EQ_INT(cb_rt_ts(0, false, t, 39), -1);
    cb_rt_commit_ts(ms_of("2026-10-13T12:34:56Z"), t, sizeof t);
    T_EQ_STR(t, "2026-10-13T12:34:56.000Z");
}

// ---- publish through a transport ------------------------------------------------------------

static struct { int n; char topic[128]; char f[2][4096]; } g_cap;

static void cap_deliver(cb_rt *rt, const char *topic, const char *const frames[2], const size_t lens[2]) {
    g_cap.n++;
    cb_strlcpy(g_cap.topic, topic, sizeof g_cap.topic);
    for (int i = 0; i < 2; i++) cb_strncpy0(g_cap.f[i], sizeof g_cap.f[i], frames[i], lens[i]);
}

static const cb_rt_transport CAP_T = { "cap", cap_deliver, NULL };

static void publish_builds_both_frames(void) {
    cb_rt rt = { &CAP_T, &g_ctx, NULL };
    memset(&g_cap, 0, sizeof g_cap);
    const uint8_t cast[] = { 9 };
    rt_publish_cast(&rt, "gu-" G "-" U1, cast, 1);
    T_EQ_INT(g_cap.n, 1);
    T_EQ_STR(g_cap.topic, "gu-" G "-" U1);
    T_EQ_STR(g_cap.f[1], "[null,null,\"realtime:gu-" G "-" U1 "\",\"broadcast\",{\"event\":\"animation_events\",\"payload\":{\"b\":\"CQ==\"},\"type\":\"broadcast\"}]");
    T_EQ_STR(g_cap.f[0], "{\"join_ref\":null,\"ref\":null,\"topic\":\"realtime:gu-" G "-" U1 "\",\"event\":\"broadcast\","
                         "\"payload\":{\"event\":\"animation_events\",\"payload\":{\"b\":\"CQ==\"},\"type\":\"broadcast\"}}");
    cb_view_row r = pv_row();
    rt_publish_pv_update(&rt, U1, CB_RT_UPDATE, &r);
    T_EQ_INT(g_cap.n, 2);
    T_EQ_STR(g_cap.topic, "pv-" U1);
    T_CHECK(cb_starts_with(g_cap.f[1], "[null,null,\"realtime:pv-" U1 "\",\"postgres_changes\",{\"data\":"));
    rt_publish_pv_update(&rt, U1, 9, &r);   // no such event: nothing
    rt_publish_pv_update(NULL, U1, CB_RT_UPDATE, &r);
    T_EQ_INT(g_cap.n, 2);
    cb_chat_row c;
    memset(&c, 0, sizeof c);
    c.message = "m";
    rt_publish_chat(&rt, G, &c);
    T_EQ_INT(g_cap.n, 3);
    T_EQ_STR(g_cap.topic, "chat:" G);
}

// The ids a join answers and the ids a push carries must meet, or
// realtime-js drops every change (RealtimeChannel._updateFilterMessage).
static void joined_binding_receives_the_push(void) {
    setup();
    cb_phx_conn c = conn2();
    sent s;
    on(&c, &s, "[\"1\",\"1\",\"realtime:pv-" U1 "\",\"phx_join\",{\"config\":{\"postgres_changes\":["
               "{\"event\":\"UPDATE\",\"schema\":\"public\",\"table\":\"player_views\",\"filter\":\"player_id=eq." U1 "\"}]},"
               "\"access_token\":\"tok-" U1 "\"}]");
    int64_t joined_id = 0;
    T_CHECK(cb_json_get_i64(s.f[0], strlen(s.f[0]), "4.response.postgres_changes.0.id", &joined_id));
    cb_view_row r = pv_row();
    cb_buf b;
    cb_buf_init(&b);
    cb_rt_pv_payload(&b, CB_RT_UPDATE, &r);
    bool found = false;
    for (int i = 0; i < 4; i++) {
        char p[8];
        int64_t id;
        snprintf(p, sizeof p, "ids.%d", i);
        if (cb_json_get_i64((const char *)b.p, b.len, p, &id) && id == joined_id) found = true;
    }
    T_CHECK(found);
    cb_buf_free(&b);
}

int main(void) {
    T_RUN(decode_both_serializers);
    T_RUN(decode_refusals);
    T_RUN(writers_byte_exact);
    T_RUN(vsn_and_binding_ids);
    T_RUN(may_join_matrix);
    T_RUN(heartbeat_and_garbage);
    T_RUN(join_echoes_postgres_bindings);
    T_RUN(join_refusals);
    T_RUN(too_many_channels);
    T_RUN(leave_and_access_token);
    T_RUN(join_as_another_user_drops_the_old_channels);
    T_RUN(vsn1_connection_speaks_objects);
    T_RUN(pv_payloads);
    T_RUN(cast_and_chat_payloads);
    T_RUN(timestamps_as_postgres_prints_them);
    T_RUN(publish_builds_both_frames);
    T_RUN(joined_binding_receives_the_push);
    rtf_reset();
    return T_DONE();
}
