// unit_stream_policy.c - who may hear which topic (realtime/stream.h
// cb_rt_may_hear) and what a subscription becomes, against every shape of
// topic a client could name: another user's feed, a seat topic for another
// user (or one whose id is a suffix of the caller's), a game id with a dash,
// an empty game, an unknown prefix; a store that fails; and every UNSUB kind
// leaving exactly its own topic.
#include "t.h"

#include "lib_rt_fakestore.h"
#include "realtime/stream.h"
#include "store/store.h"
#include "util/err.h"
#include "util/log.h"

#include "ask.h"
#include "netwire.h"

#define U1 "11111111-1111-4111-8111-111111111111"
#define U2 "22222222-2222-4222-8222-222222222222"
#define G  "ab12cd"

static cb_ctx g_ctx;
static cb_config g_cfg;

static void world(void) {
    cb_log_init(CB_LOG_ERROR + 1, 0);
    rtf_install();
    rtf_reset();
    memset(&g_cfg, 0, sizeof g_cfg);
    g_ctx.cfg = &g_cfg;
    g_ctx.store = &rtf.store;
    rtf_member(G, U1);
}

static void topic_policy(void) {
    world();
    // The caller's own views only.
    T_EQ_INT(cb_rt_may_hear(&g_ctx, U1, "pv-" U1), 1);
    T_EQ_INT(cb_rt_may_hear(&g_ctx, U1, "pv-" U2), 0);
    T_EQ_INT(cb_rt_may_hear(&g_ctx, U1, "pv-"), 0);
    T_EQ_INT(cb_rt_may_hear(&g_ctx, U1, "pv-" U1 "x"), 0);
    // The spectators' stream: anyone, of any named game.
    T_EQ_INT(cb_rt_may_hear(&g_ctx, U2, "game-" G), 1);
    T_EQ_INT(cb_rt_may_hear(&g_ctx, U2, "game-"), 0);
    // A seat's stream: the member themself, never another user, whatever the spelling.
    T_EQ_INT(cb_rt_may_hear(&g_ctx, U1, "gu-" G "-" U1), 1);
    T_EQ_INT(cb_rt_may_hear(&g_ctx, U2, "gu-" G "-" U1), 0);   // another's seat
    T_EQ_INT(cb_rt_may_hear(&g_ctx, U2, "gu-" G "-" U2), 0);   // not a member
    T_EQ_INT(cb_rt_may_hear(&g_ctx, "1111-4111-8111-111111111111", "gu-" G "-11111111-1111-4111-8111-111111111111"), 0);
    T_EQ_INT(cb_rt_may_hear(&g_ctx, U1, "gu-" G "x" U1), 0);   // no dash before the id
    T_EQ_INT(cb_rt_may_hear(&g_ctx, U1, "gu--" U1), 0);        // no game
    T_EQ_INT(cb_rt_may_hear(&g_ctx, U1, "gu-" U1), 0);
    T_EQ_INT(cb_rt_may_hear(&g_ctx, U1, "gu-0123456789abcdef0-" U1), 0);   // a game id past its cap
    rtf_member("ab-cd", U1);
    T_EQ_INT(cb_rt_may_hear(&g_ctx, U1, "gu-ab-cd-" U1), 1);   // a game id with a dash still reads whole
    // Chat: members.
    T_EQ_INT(cb_rt_may_hear(&g_ctx, U1, "chat:" G), 1);
    T_EQ_INT(cb_rt_may_hear(&g_ctx, U2, "chat:" G), 0);
    T_EQ_INT(cb_rt_may_hear(&g_ctx, U1, "chat:"), 0);
    T_EQ_INT(cb_rt_may_hear(&g_ctx, U1, "chat:0123456789abcdef0123"), 0);
    // Nobody, nothing, and topics that are not ours.
    T_EQ_INT(cb_rt_may_hear(&g_ctx, "", "game-" G), 0);
    T_EQ_INT(cb_rt_may_hear(&g_ctx, NULL, "game-" G), 0);
    T_EQ_INT(cb_rt_may_hear(&g_ctx, U1, NULL), 0);
    T_EQ_INT(cb_rt_may_hear(&g_ctx, U1, "realtime:public"), 0);
    T_EQ_INT(cb_rt_may_hear(&g_ctx, U1, "GU-" G "-" U1), 0);
    // A store that fails: its error, never a yes.
    rtf.is_member_rc = CB_E_DB;
    T_EQ_INT(cb_rt_may_hear(&g_ctx, U1, "chat:" G), CB_E_DB);
    T_EQ_INT(cb_rt_may_hear(&g_ctx, U1, "gu-" G "-" U1), CB_E_DB);
    rtf.is_member_rc = 0;
    // No store at all.
    g_ctx.store = NULL;
    T_EQ_INT(cb_rt_may_hear(&g_ctx, U1, "chat:" G), CB_E_STATE);
    T_EQ_INT(cb_rt_may_hear(&g_ctx, U1, "pv-" U1), 1);   // decided without one
}

// ---- frames ----------------------------------------------------------------------------------

static struct { int n; NwFrame f[8]; } g_out;
static void collect(void *ud, const char *frame, size_t len) {
    if (g_out.n < 8) nw_frame_decode((const uint8_t *)frame, (int)len, &g_out.f[g_out.n++]);
}

static int send_frame(cb_conn_row *c, int kind, int what, const char *gid, const char *token) {
    static NwFrame f;
    memset(&f, 0, sizeof f);
    f.kind = (uint8_t)kind;
    f.ref = 9;
    f.what = (uint8_t)what;
    if (gid) { f.gid_len = (uint8_t)strlen(gid); memcpy(f.gid, gid, f.gid_len); }
    if (token) { f.token_len = (uint16_t)strlen(token); memcpy(f.token, token, f.token_len); }
    uint8_t b[NW_ACCESS_MAX + 64];
    int n = nw_frame_encode(&f, b, sizeof b);
    memset(&g_out, 0, sizeof g_out);
    return cb_nw_on_frame(&g_ctx, c, b, (size_t)n, collect, NULL);
}

static bool has(const cb_conn_row *c, const char *t) {
    for (int i = 0; i < c->n_topics; i++)
        if (strcmp(c->topics[i], t) == 0) return true;
    return false;
}

static void unsub_leaves_only_its_topic(void) {
    world();
    cb_conn_row c;
    memset(&c, 0, sizeof c);
    T_EQ_INT(send_frame(&c, NW_WS_SUB_AS, NW_SUB_GAMES, NULL, "tok-" U1), CB_OK);
    T_EQ_INT(send_frame(&c, NW_WS_SUB, NW_SUB_GAME, G, NULL), CB_OK);
    T_EQ_INT(send_frame(&c, NW_WS_SUB, NW_SUB_CHAT, G, NULL), CB_OK);
    T_EQ_INT(c.n_topics, 3);
    // A malformed game id unsubscribes nothing.
    T_EQ_INT(send_frame(&c, NW_WS_UNSUB, NW_SUB_GAME, "bad id", NULL), CB_OK);
    T_EQ_INT(c.n_topics, 3);
    T_EQ_INT(send_frame(&c, NW_WS_UNSUB, NW_SUB_CHAT, G, NULL), CB_OK);
    T_CHECK(!has(&c, "chat:" G) && has(&c, "gu-" G "-" U1) && has(&c, "pv-" U1));
    T_EQ_INT(send_frame(&c, NW_WS_UNSUB, NW_SUB_GAME, G, NULL), CB_OK);
    T_CHECK(!has(&c, "gu-" G "-" U1) && has(&c, "pv-" U1));
    T_EQ_INT(send_frame(&c, NW_WS_UNSUB, NW_SUB_GAMES, NULL, NULL), CB_OK);
    T_EQ_INT(c.n_topics, 0);
    T_EQ_INT(g_out.n, 0);   // an UNSUB is never answered
    // A spectator's game topic goes with an UNSUB of the game too.
    T_EQ_INT(send_frame(&c, NW_WS_SUB, NW_SUB_GAME, "zz99zz", NULL), CB_OK);
    T_CHECK(has(&c, "game-zz99zz"));
    T_EQ_INT(send_frame(&c, NW_WS_UNSUB, NW_SUB_GAME, "zz99zz", NULL), CB_OK);
    T_EQ_INT(c.n_topics, 0);
}

static void a_full_connection_refuses_more(void) {
    world();
    cb_conn_row c;
    memset(&c, 0, sizeof c);
    T_EQ_INT(send_frame(&c, NW_WS_SUB_AS, NW_SUB_GAMES, NULL, "tok-" U1), CB_OK);
    char gid[16];
    int accepted = 1;
    for (int i = 0; i < CB_CONN_TOPICS + 2; i++) {
        snprintf(gid, sizeof gid, "g%05d", i);
        T_EQ_INT(send_frame(&c, NW_WS_SUB, NW_SUB_GAME, gid, NULL), CB_OK);
        if (g_out.n && g_out.f[0].why == 0) accepted++;
        else T_EQ_INT(g_out.f[0].why, ASK_WHY_INTERNAL);
    }
    T_EQ_INT(accepted, CB_CONN_TOPICS);
    T_EQ_INT(c.n_topics, CB_CONN_TOPICS);
    // A store that fails mid-subscription: INTERNAL, nothing stored.
    memset(&c, 0, sizeof c);
    T_EQ_INT(send_frame(&c, NW_WS_SUB_AS, NW_SUB_GAMES, NULL, "tok-" U1), CB_OK);
    rtf.is_member_rc = CB_E_DB;
    T_EQ_INT(send_frame(&c, NW_WS_SUB, NW_SUB_GAME, G, NULL), CB_OK);
    T_EQ_INT(g_out.f[0].why, ASK_WHY_INTERNAL);
    T_EQ_INT(send_frame(&c, NW_WS_SUB, NW_SUB_CHAT, G, NULL), CB_OK);
    T_EQ_INT(g_out.f[0].why, ASK_WHY_INTERNAL);
    T_EQ_INT(c.n_topics, 1);
    rtf.is_member_rc = 0;
    // A frame that does not decode, and one only a server sends.
    uint8_t junk[3] = { 0xff, 0, 1 };
    T_EQ_INT(cb_nw_on_frame(&g_ctx, &c, junk, sizeof junk, collect, NULL), CB_E_INVALID);
    static NwFrame ack;
    memset(&ack, 0, sizeof ack);
    ack.kind = NW_WS_PONG;
    uint8_t b[16];
    int n = nw_frame_encode(&ack, b, sizeof b);
    T_EQ_INT(cb_nw_on_frame(&g_ctx, &c, b, (size_t)n, collect, NULL), CB_E_INVALID);
}

int main(void) {
    T_RUN(topic_policy);
    T_RUN(unsub_leaves_only_its_topic);
    T_RUN(a_full_connection_refuses_more);
    return T_DONE();
}
