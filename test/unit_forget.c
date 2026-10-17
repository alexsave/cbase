// unit_forget.c - account deletion ends the user's streams (realtime.h
// cb_rt_forget_user) on API Gateway: every CONN row of the user, found through
// the topics they could hold, is closed at API Gateway and deleted, and
// nobody else's is touched. (`cbase serve`'s sockets: smoke_matrix.c.)
#include "t.h"

#include "lib_rt_fakestore.h"
#include "realtime/realtime.h"
#include "realtime/rt_apigw.h"
#include "util/clock.h"
#include "util/log.h"

#define U1 "11111111-1111-4111-8111-111111111111"
#define U2 "22222222-2222-4222-8222-222222222222"

static cb_ctx g_ctx;
static cb_config g_cfg;

static void conn(const char *id, const char *user, const char *t1, const char *t2) {
    cb_conn_row c;
    memset(&c, 0, sizeof c);
    snprintf(c.conn_id, sizeof c.conn_id, "%s", id);
    snprintf(c.user_id, sizeof c.user_id, "%s", user);
    c.expires_at = cb_now_ms() + 600000;
    if (t1) snprintf(c.topics[c.n_topics++], sizeof c.topics[0], "%s", t1);
    if (t2) snprintf(c.topics[c.n_topics++], sizeof c.topics[0], "%s", t2);
    cb_conn_row none;
    memset(&none, 0, sizeof none);   // no row before it
    T_EQ_INT(CB_STORE_CALL(&rtf.store, conn_put, &none, &c), CB_OK);
}

static void apigw_forgets_only_the_user(void) {
    cb_log_init(CB_LOG_ERROR + 1, 0);
    rtf_install();
    rtf_reset();
    memset(&g_cfg, 0, sizeof g_cfg);
    snprintf(g_cfg.ws_endpoint, sizeof g_cfg.ws_endpoint, "https://abc.execute-api.us-east-1.amazonaws.com/prod");
    g_ctx.cfg = &g_cfg;
    g_ctx.store = &rtf.store;
    T_EQ_INT(cb_rt_apigw_new(&g_ctx, &g_ctx.rt), CB_OK);
    conn("A1", U1, "pv-" U1, "gu-ab12cd-" U1);   // two of U1's topics: one connection
    conn("A2", U1, "chat:ab12cd", NULL);
    conn("A3", U1, "game-zz99zz", NULL);          // a spectator stream: not found, nothing of U1's in it
    conn("B1", U2, "pv-" U2, "chat:ab12cd");      // another user, on a topic U1 held too
    char games[1][CB_GAME_ID_MAX + 1] = { "ab12cd" };
    cb_rt_forget_user(g_ctx.rt, U1, (const char (*)[CB_GAME_ID_MAX + 1])games, 1);
    T_EQ_INT(rtf.n_deletes, 2);                   // A1 once, A2
    T_EQ_INT(rtf_conn_find("A1"), -1);
    T_EQ_INT(rtf_conn_find("A2"), -1);
    T_CHECK(rtf_conn_find("A3") >= 0);
    T_CHECK(rtf_conn_find("B1") >= 0);
    // No user, or no transport that keeps connections: nothing.
    cb_rt_forget_user(g_ctx.rt, "", NULL, 0);
    cb_rt_forget_user(NULL, U2, NULL, 0);
    T_EQ_INT(rtf.n_deletes, 2);
    // No endpoint: the rows still go.
    g_cfg.ws_endpoint[0] = 0;
    cb_rt_forget_user(g_ctx.rt, U2, NULL, 0);
    T_EQ_INT(rtf.n_deletes, 2);
    T_EQ_INT(rtf_conn_find("B1"), -1);
    cb_rt_free(g_ctx.rt);
}

int main(void) {
    T_RUN(apigw_forgets_only_the_user);
    return T_DONE();
}
