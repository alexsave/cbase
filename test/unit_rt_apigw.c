// unit_rt_apigw.c - realtime under API Gateway (realtime/rt_apigw.h): the
// three events against the store's CONN rows, and publishing through the
// management API with a 410 cleaning up.
#include "t.h"

#include "lib_rt_fakestore.h"
#include "realtime/realtime.h"
#include "realtime/rt_apigw.h"
#include "util/clock.h"
#include "util/log.h"

#define U1 "11111111-1111-4111-8111-111111111111"
#define G  "ab12cd"

static cb_ctx g_ctx;
static cb_config g_cfg;

static int ev(const char *route, const char *conn, const char *query, const char *body) {
    cb_ws_event e;
    memset(&e, 0, sizeof e);
    cb_strlcpy(e.route_key, route, sizeof e.route_key);
    cb_strlcpy(e.connection_id, conn, sizeof e.connection_id);
    strcpy(e.domain, "abc.execute-api.us-west-1.amazonaws.com");
    strcpy(e.stage, "prod");
    e.query = query ? query : "";
    e.body = (const uint8_t *)body;
    e.body_len = body ? strlen(body) : 0;
    int status = 0;
    cb_rt_apigw_event(&g_ctx, &e, &status);
    return status;
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
    strcpy(g_cfg.anon_key, "anon-key");
    g_ctx.cfg = &g_cfg;
    g_ctx.store = &rtf.store;
    cb_rt_free(g_ctx.rt);
    T_EQ_INT(cb_rt_apigw_new(&g_ctx, &g_ctx.rt), CB_OK);
}

static void connect_and_disconnect(void) {
    setup();
    T_EQ_INT(ev("$connect", "c1", "apikey=anon-key&vsn=2.0.0", NULL), 200);
    int i = rtf_conn_find("c1");
    T_CHECK(i >= 0);
    T_EQ_INT(rtf.conns[i].vsn, 2);
    T_EQ_INT(rtf.conns[i].n_topics, 0);
    T_EQ_STR(rtf.conns[i].user_id, "");
    int64_t ttl = rtf.conns[i].expires_at - cb_now_ms();
    T_CHECK(ttl > 7100000 && ttl <= 7200000);
    T_EQ_INT(ev("$connect", "c2", "apikey=anon-key", NULL), 200);   // no vsn: Phoenix's default
    T_EQ_INT(rtf.conns[rtf_conn_find("c2")].vsn, 1);
    T_EQ_INT(ev("$connect", "c3", "apikey=wrong&vsn=2.0.0", NULL), 401);
    T_EQ_INT(ev("$connect", "c3", "vsn=2.0.0", NULL), 401);
    T_EQ_INT(ev("$connect", "c3", "apikey=anon-key&vsn=9", NULL), 400);
    T_EQ_INT(rtf_conn_find("c3"), -1);
    T_EQ_INT(ev("$disconnect", "c1", NULL, NULL), 200);
    T_EQ_INT(rtf_conn_find("c1"), -1);
    T_EQ_INT(ev("$disconnect", "c1", NULL, NULL), 200);   // already gone
    T_EQ_INT(ev("$whatever", "c1", NULL, NULL), 400);
    // Off: no anon key configured.
    g_cfg.anon_key[0] = 0;
    T_EQ_INT(ev("$connect", "c4", "vsn=2.0.0", NULL), 200);
}

static void frames_round_trip_through_the_conn_row(void) {
    setup();
    rtf_member(G, U1);
    T_EQ_INT(ev("$connect", "c1", "apikey=anon-key&vsn=2.0.0", NULL), 200);
    int puts0 = rtf.conn_puts;
    T_EQ_INT(ev("$default", "c1", NULL, "[\"1\",\"1\",\"realtime:gu-" G "-" U1 "\",\"phx_join\",{\"access_token\":\"tok-" U1 "\"}]"), 200);
    T_EQ_INT(rtf.n_posts, 1);
    T_EQ_STR(rtf.posts[0].conn_id, "c1");
    T_EQ_STR(rtf.posts[0].frame, "[\"1\",\"1\",\"realtime:gu-" G "-" U1 "\",\"phx_reply\",{\"response\":{},\"status\":\"ok\"}]");
    int i = rtf_conn_find("c1");
    T_EQ_INT(rtf.conns[i].n_topics, 1);
    T_EQ_STR(rtf.conns[i].topics[0], "gu-" G "-" U1);
    T_EQ_STR(rtf.conns[i].user_id, U1);
    T_EQ_INT(rtf.conn_puts, puts0 + 1);
    // A heartbeat is answered and costs no write (the row did not change).
    T_EQ_INT(ev("$default", "c1", NULL, "[null,\"2\",\"phoenix\",\"heartbeat\",{}]"), 200);
    T_EQ_INT(rtf.n_posts, 2);
    T_EQ_INT(rtf.conn_puts, puts0 + 1);
    // ...until the TTL needs pushing forward.
    rtf.conns[i].expires_at = cb_now_ms() + 60000;
    T_EQ_INT(ev("$default", "c1", NULL, "[null,\"3\",\"phoenix\",\"heartbeat\",{}]"), 200);
    T_EQ_INT(rtf.conn_puts, puts0 + 2);
    T_CHECK(rtf.conns[rtf_conn_find("c1")].expires_at > cb_now_ms() + 7000000);
    // A refused join changes nothing.
    T_EQ_INT(ev("$default", "c1", NULL, "[\"4\",\"4\",\"realtime:chat:zzzzzz\",\"phx_join\",{\"access_token\":\"tok-" U1 "\"}]"), 200);
    T_CHECK(strstr(rtf.posts[rtf.n_posts - 1].frame, "\"status\":\"error\"") != NULL);
    T_EQ_INT(rtf.conns[rtf_conn_find("c1")].n_topics, 1);
    // phx_leave removes the topic from the row.
    T_EQ_INT(ev("$default", "c1", NULL, "[\"1\",\"5\",\"realtime:gu-" G "-" U1 "\",\"phx_leave\",{}]"), 200);
    T_EQ_INT(rtf.conns[rtf_conn_find("c1")].n_topics, 0);
}

static void a_lost_row_is_rebuilt_and_garbage_closes(void) {
    setup();
    // No $connect seen (or its TTL ran out): the frame's own shape says the serializer.
    T_EQ_INT(ev("$default", "c9", NULL, "{\"join_ref\":null,\"ref\":\"1\",\"topic\":\"phoenix\",\"event\":\"heartbeat\",\"payload\":{}}"), 200);
    T_EQ_INT(rtf.n_posts, 1);
    T_CHECK(cb_starts_with(rtf.posts[0].frame, "{\"join_ref\":null,\"ref\":\"1\""));
    T_EQ_INT(rtf.conns[rtf_conn_find("c9")].vsn, 1);
    // Not Phoenix: the socket is closed and the row dropped.
    T_EQ_INT(ev("$default", "c9", NULL, "hello"), 200);
    T_EQ_INT(rtf.n_deletes, 1);
    T_EQ_INT(rtf_conn_find("c9"), -1);
    // A reply to a connection that has gone (410) drops the row.
    T_EQ_INT(ev("$connect", "c8", "apikey=anon-key&vsn=2.0.0", NULL), 200);
    strcpy(rtf.gone[0], "c8");
    T_EQ_INT(ev("$default", "c8", NULL, "[null,\"1\",\"phoenix\",\"heartbeat\",{}]"), 200);
    T_EQ_INT(rtf_conn_find("c8"), -1);
    // No store: 503.
    g_ctx.store = NULL;
    T_EQ_INT(ev("$connect", "c7", "apikey=anon-key", NULL), 503);
}

// API Gateway runs a connection's frames as concurrent invocations: a page
// joins pv-, chat: and gu- within a few milliseconds. Recorded against the
// live backend 2026-10-14: three joins each read the empty row and each wrote
// back its own topic, so only the last stuck and the page never heard the
// gu- pushes it had been answered ok for. Here the chat: join runs whole
// between the gu- join's read and its write.
static void g_join_chat(void *ud) {
    (void)ud;
    T_EQ_INT(ev("$default", "c1", NULL, "[\"9\",\"9\",\"realtime:chat:" G "\",\"phx_join\",{\"access_token\":\"tok-" U1 "\"}]"), 200);
}

static void concurrent_frames_of_one_connection_both_stick(void) {
    setup();
    rtf_member(G, U1);
    T_EQ_INT(ev("$connect", "c1", "apikey=anon-key&vsn=2.0.0", NULL), 200);
    rtf.after_conn_get = g_join_chat;
    T_EQ_INT(ev("$default", "c1", NULL, "[\"12\",\"12\",\"realtime:gu-" G "-" U1 "\",\"phx_join\",{\"access_token\":\"tok-" U1 "\"}]"), 200);
    int i = rtf_conn_find("c1");
    T_EQ_INT(rtf.conns[i].n_topics, 2);
    T_EQ_STR(rtf.conns[i].topics[0], "chat:" G);
    T_EQ_STR(rtf.conns[i].topics[1], "gu-" G "-" U1);
    T_EQ_INT(rtf.conn_conflicts, 1);   // the gu- join lost once and ran again
    // Each join answered exactly once, the one that lost only after it stored.
    T_EQ_INT(rtf.n_posts, 2);
    T_CHECK(cb_starts_with(rtf.posts[0].frame, "[\"9\",\"9\",\"realtime:chat:" G "\",\"phx_reply\""));
    T_CHECK(cb_starts_with(rtf.posts[1].frame, "[\"12\",\"12\",\"realtime:gu-" G "-" U1 "\",\"phx_reply\""));
    // Both topics are pushed to.
    T_EQ_INT(CB_STORE_CALL(&rtf.store, conns_by_topic, "gu-" G "-" U1, count_conn, &(int){ 0 }), 1);
    T_EQ_INT(CB_STORE_CALL(&rtf.store, conns_by_topic, "chat:" G, count_conn, &(int){ 0 }), 1);
}

static void publish_posts_each_subscriber_in_its_serializer(void) {
    setup();
    rtf_member(G, U1);
    ev("$connect", "v2", "apikey=anon-key&vsn=2.0.0", NULL);
    ev("$connect", "v1", "apikey=anon-key&vsn=1.0.0", NULL);
    ev("$connect", "other", "apikey=anon-key&vsn=2.0.0", NULL);
    ev("$connect", "gone", "apikey=anon-key&vsn=2.0.0", NULL);
    ev("$connect", "stale", "apikey=anon-key&vsn=2.0.0", NULL);
    ev("$default", "v2", NULL, "[\"1\",\"1\",\"realtime:game-" G "\",\"phx_join\",{\"access_token\":\"tok-" U1 "\"}]");
    ev("$default", "v1", NULL, "{\"join_ref\":\"1\",\"ref\":\"1\",\"topic\":\"realtime:game-" G "\",\"event\":\"phx_join\",\"payload\":{\"access_token\":\"tok-" U1 "\"}}");
    ev("$default", "other", NULL, "[\"1\",\"1\",\"realtime:game-zzzzzz\",\"phx_join\",{\"access_token\":\"tok-" U1 "\"}]");
    ev("$default", "gone", NULL, "[\"1\",\"1\",\"realtime:game-" G "\",\"phx_join\",{\"access_token\":\"tok-" U1 "\"}]");
    ev("$default", "stale", NULL, "[\"1\",\"1\",\"realtime:game-" G "\",\"phx_join\",{\"access_token\":\"tok-" U1 "\"}]");
    rtf.conns[rtf_conn_find("stale")].expires_at = cb_now_ms() - 1;   // the TTL has passed; DynamoDB has not swept it yet
    strcpy(rtf.gone[0], "gone");
    rtf.n_posts = 0;
    // No endpoint configured: the event's own domain is not known here, so pushes need CBASE_WS_ENDPOINT.
    const uint8_t cast[] = { 7 };
    rt_publish_cast(g_ctx.rt, "game-" G, cast, 1);
    T_EQ_INT(rtf.n_posts, 0);
    strcpy(g_cfg.ws_endpoint, "https://abc.execute-api.us-west-1.amazonaws.com/prod");
    rt_publish_cast(g_ctx.rt, "game-" G, cast, 1);
    T_EQ_INT(rtf.n_posts, 2);
    int seen = 0;
    for (int i = 0; i < rtf.n_posts; i++) {
        if (strcmp(rtf.posts[i].conn_id, "v2") == 0) {
            seen |= 1;
            T_EQ_STR(rtf.posts[i].frame, "[null,null,\"realtime:game-" G "\",\"broadcast\",{\"event\":\"animation_events\",\"payload\":{\"b\":\"Bw==\"},\"type\":\"broadcast\"}]");
        }
        if (strcmp(rtf.posts[i].conn_id, "v1") == 0) {
            seen |= 2;
            T_CHECK(cb_starts_with(rtf.posts[i].frame, "{\"join_ref\":null,\"ref\":null,\"topic\":\"realtime:game-" G "\",\"event\":\"broadcast\""));
        }
    }
    T_EQ_INT(seen, 3);
    T_EQ_INT(rtf_conn_find("gone"), -1);     // 410 deleted it
    T_CHECK(rtf_conn_find("other") >= 0);
    cb_rt_free(g_ctx.rt);
    g_ctx.rt = NULL;
}

int main(void) {
    T_RUN(connect_and_disconnect);
    T_RUN(frames_round_trip_through_the_conn_row);
    T_RUN(a_lost_row_is_rebuilt_and_garbage_closes);
    T_RUN(concurrent_frames_of_one_connection_both_stick);
    T_RUN(publish_posts_each_subscriber_in_its_serializer);
    rtf_reset();
    return T_DONE();
}
