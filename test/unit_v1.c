// unit_v1.c - the binary surface's HTTP routes (app/v1_http.h), driven the
// way every client drives them: through the kernel's own client library
// (netclient.h) building each request and reading each answer, against a real
// store (SQLite, or DynamoDB Local under CBASE_STORE=dynamo) and real auth,
// every request through cb_handle.
#include "t.h"

#include "app/fanout.h"
#include "app/functions_http.h"
#include "app/table_ops.h"
#include "app/v1_http.h"
#include "auth/auth.h"
#include "handler.h"
#include "lib_store_backend.h"
#include "net/router.h"
#include "realtime/realtime.h"
#include "util/clock.h"
#include "util/log.h"
#include "util/uuid.h"

#include "ask.h"
#include "netclient.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <unistd.h>

#define JWT_SECRET "unit-v1-secret-unit-v1-secret-unit-v1-secret"

static cb_ctx g_ctx;
static cb_config g_cfg;
static char g_db[512];
static int64_t g_now;
// The server's clock is the client's: a test that moves time moves both.
static int64_t test_now(void) { return g_now; }

// What the realtime transport was handed: the last binary-stream frame per publish.
static struct { int n; char topic[128]; uint8_t nw[4096]; size_t nw_len; } g_pub;
static int cap_deliver(cb_rt *rt, const char *topic, const uint8_t *frame, size_t len) {
    g_pub.n++;
    cb_strlcpy(g_pub.topic, topic, sizeof g_pub.topic);
    g_pub.nw_len = len < sizeof g_pub.nw ? len : 0;
    memcpy(g_pub.nw, frame, g_pub.nw_len);
    return 0;
}
static const cb_rt_transport CAP_T = { "cap", cap_deliver, NULL };
static cb_rt g_rt = { &CAP_T, &g_ctx, NULL };

static void setup(void) {
    cb_log_init(CB_LOG_ERROR, 0);
    memset(&g_ctx, 0, sizeof g_ctx);
    memset(&g_cfg, 0, sizeof g_cfg);
    cb_strlcpy(g_cfg.jwt_secret, JWT_SECRET, sizeof g_cfg.jwt_secret);
    cb_strlcpy(g_cfg.allowed_origins, "*", sizeof g_cfg.allowed_origins);
    cb_strlcpy(g_cfg.public_url, "http://127.0.0.1:8080", sizeof g_cfg.public_url);
    g_cfg.jwt_ttl_s = 3600;
    g_ctx.cfg = &g_cfg;
    g_ctx.mode = CB_MODE_SERVE;
    g_ctx.version = "test";
    const char *tmp = getenv("TMPDIR");
    snprintf(g_db, sizeof g_db, "%s/cbase-v1-%d.sqlite", tmp && *tmp ? tmp : "/tmp", (int)getpid());
    unlink(g_db);
    g_ctx.store = tsb_open_fresh(g_db);
    T_CHECK(g_ctx.store != NULL);
    T_EQ_INT(cb_auth_new(&g_ctx, &g_ctx.auth), CB_OK);
    g_ctx.rt = &g_rt;
    g_ctx.router = cb_router_new();
    T_EQ_INT(cb_functions_routes(&g_ctx), CB_OK);
    T_EQ_INT(cb_v1_routes(&g_ctx), CB_OK);
    nc_reset();
    g_now = cb_now_ms();
    cb_auth_now = test_now;
}

static void teardown(void) {
    cb_router_free(g_ctx.router);
    cb_auth_free(g_ctx.auth);
    cb_store_close(g_ctx.store);
    unlink(g_db);
    memset(&g_ctx, 0, sizeof g_ctx);
}

// ---- the host: one request through cb_handle ------------------------------------------

static int g_status;
static const char *g_origin;   // X-Cbase-Origin to send, NULL for none
static uint8_t g_body[1 << 20];
static int g_len;
static char g_hdrs[4096];   // the last answer's header lines

static int send_req(const NcRequest *q) {
    T_CHECK(q != NULL);
    if (!q) return 0;
    char path[NC_PATH_MAX + 1], authz[NC_AUTH_MAX + 1];
    memcpy(path, q->path, q->path_len);
    path[q->path_len] = 0;
    char *qs = strchr(path, '?');
    if (qs) *qs++ = 0;
    cb_http_req r;
    memset(&r, 0, sizeof r);
    cb_strlcpy(r.method, q->method == NC_GET ? "GET" : "POST", sizeof r.method);
    r.path = path;
    r.query = qs ? qs : "";
    r.minor = 1;
    if (q->auth_len) {
        memcpy(authz, q->auth, q->auth_len);
        authz[q->auth_len] = 0;
        r.headers[r.n_headers++] = (cb_http_header){ "Authorization", authz };
    }
    if (q->method == NC_POST) r.headers[r.n_headers++] = (cb_http_header){ "Content-Type", "application/octet-stream" };
    if (g_origin) r.headers[r.n_headers++] = (cb_http_header){ "x-cbase-origin", g_origin };
    r.body = q->body;
    r.body_len = q->body_len;
    cb_http_res res;
    cb_res_init(&res);
    cb_handle(&g_ctx, &r, &res);
    g_status = res.status;
    cb_strlcpy(g_hdrs, cb_buf_cstr(&res.headers), sizeof g_hdrs);
    g_len = (int)(res.body.len < sizeof g_body ? res.body.len : 0);
    if (g_len) memcpy(g_body, res.body.p, (size_t)g_len);   // an empty body may have no buffer
    cb_res_free(&res);
    return g_status;
}

// What a host's one transport function does: a refresh the client asks for
// first is sent and read, then the request is built again.
typedef const NcRequest *(*build_fn)(void *ud);
static int call(build_fn build, void *ud) {
    for (int i = 0; i < 3; i++) {
        const NcRequest *q = build(ud);
        if (!q) return -1;
        if (!q->is_refresh) return send_req(q);
        send_req(q);
        if (nc_session_read(g_status, g_body, g_len) != 0) return g_status;
    }
    return -1;
}

static const NcRequest *b_games(void *ud) { return nc_req_games(g_now); }
static const NcRequest *b_create(void *ud) { return nc_req_create(g_now); }
static const NcRequest *b_bots(void *ud) { return nc_req_bots(g_now); }
static const NcRequest *b_history(void *ud) { return nc_req_history(g_now); }
static const NcRequest *b_game(void *ud) { return nc_req_game(ud, (int)strlen(ud), g_now); }
static const NcRequest *b_chats(void *ud) { return nc_req_chats(ud, (int)strlen(ud), g_now); }
static const NcRequest *b_replay(void *ud) { return nc_req_replay(ud, (int)strlen(ud), g_now); }

static int sign(int up, const char *name, const char *pw) {
    send_req(up ? nc_req_signup(name, (int)strlen(name), pw, (int)strlen(pw))
                : nc_req_signin(name, (int)strlen(name), pw, (int)strlen(pw)));
    return nc_session_read(g_status, g_body, g_len);
}

static void as(const uint8_t *blob, int n) { T_EQ_INT(nc_session_load(blob, n), NW_OK); }

// ---- tests ----------------------------------------------------------------------------

static void the_email_is_the_clients_derivation(void) {
    char e[CB_V1_EMAIL_BYTES];
    cb_v1_email_of("ALICE", 5, e);
    T_EQ_STR(e, "e7dcee3cc63d170b@foolish.cards");   // hashlib.sha256(b"ALICE").hexdigest()[:16]
    cb_v1_email_of("\xc3\x84LICE", 6, e);              // UTF-8 bytes as given, never re-cased here
    T_EQ_STR(e, "9077030be756b4fe@foolish.cards");
}

static void accounts(void) {
    setup();
    T_EQ_INT(sign(1, "ALICE", "secret1"), 0);
    const NcSessionInfo *i = nc_session_info();
    T_EQ_INT(i->signed_in, 1);
    T_EQ_MEM(i->username, i->username_len, "ALICE", 5);
    T_EQ_INT(i->user_id_len, 36);
    // The account is the one the clients' email always named.
    cb_user_row row;
    T_EQ_INT(CB_STORE_CALL(g_ctx.store, user_by_email, "e7dcee3cc63d170b@foolish.cards", &row), CB_OK);
    T_EQ_MEM(row.id, strlen(row.id), i->user_id, i->user_id_len);

    nc_reset();
    T_EQ_INT(sign(1, "ALICE", "another1"), ASK_WHY_ACCOUNT);
    T_EQ_INT(nc_error()->code, NW_ACCOUNT_E_EXISTS);
    T_EQ_INT(nc_error()->status, 409);
    T_EQ_INT(sign(0, "ALICE", "wrong-pw"), ASK_WHY_ACCOUNT);
    T_EQ_INT(nc_error()->code, NW_ACCOUNT_E_BAD_LOGIN);
    T_EQ_INT(nc_session_info()->signed_in, 0);
    T_EQ_INT(sign(1, "B%T", "secret1"), ASK_WHY_ACCOUNT);
    T_EQ_INT(nc_error()->code, NW_ACCOUNT_E_BAD_NAME);
    T_EQ_INT(sign(1, "BOB", "abc"), ASK_WHY_ACCOUNT);
    T_EQ_INT(nc_error()->code, NW_ACCOUNT_E_WEAK_PASSWORD);
    T_EQ_INT(sign(0, "ALICE", "secret1"), 0);
    T_EQ_INT(nc_session_info()->signed_in, 1);

    // A body that is not a request of the route's kind.
    NcRequest bad = *nc_req_signin("A", 1, "pw", 2);
    bad.body[1] = NW_SIGNUP;
    T_EQ_INT(send_req(&bad), 400);
    T_EQ_INT(nc_status_read(g_status, g_body, g_len), ASK_WHY_MALFORMED);
    // No bearer.
    nc_reset();
    T_EQ_INT(send_req(nc_req_board()), 200);   // the leaderboard needs nobody
    NcRequest anon = *nc_req_board();
    memcpy(anon.path, "/v1/games", 9);
    anon.path_len = 9;
    T_EQ_INT(send_req(&anon), 401);
    T_EQ_INT(nc_status_read(g_status, g_body, g_len), ASK_WHY_AUTH);
    teardown();
}

// A backend not up yet (no auth module, no store) answers 503 INTERNAL, never crashes.
static void without_a_backend(void) {
    setup();
    T_EQ_INT(sign(1, "ALICE", "secret1"), 0);
    cb_auth *auth = g_ctx.auth;
    g_ctx.auth = NULL;
    nc_reset();
    T_EQ_INT(sign(1, "BOB", "secret1"), ASK_WHY_INTERNAL);
    T_EQ_INT(nc_error()->status, 503);
    g_ctx.auth = auth;
    T_EQ_INT(sign(0, "ALICE", "secret1"), 0);
    cb_store *store = g_ctx.store;
    g_ctx.store = NULL;
    T_EQ_INT(call(b_games, NULL), 503);
    T_EQ_INT(nc_status_read(g_status, g_body, g_len), ASK_WHY_INTERNAL);
    T_EQ_INT(send_req(nc_req_board()), 503);
    g_ctx.store = store;
    teardown();
}

static void refresh_password_signout_delete(void) {
    setup();
    T_EQ_INT(sign(1, "CARL", "secret1"), 0);
    uint8_t blob[NC_SESSION_BLOB_MAX];
    int n = nc_session_save(blob, sizeof blob);
    T_CHECK(n > 0);
    // Due: the next authenticated request is the refresh first.
    g_now += 3600 * 1000;
    const NcRequest *q = nc_req_games(g_now);
    T_CHECK(q && q->is_refresh);
    T_EQ_INT(call(b_games, NULL), 200);
    T_EQ_INT(nc_list_read(g_status, g_body, g_len, NW_GAMES), 0);
    uint8_t fresh[NC_SESSION_BLOB_MAX];
    int fn = nc_session_save(fresh, sizeof fresh);
    T_CHECK(fn > 0 && (fn != n || memcmp(fresh, blob, (size_t)n) != 0));

    // Change the password: the same one is refused, a new one signs in.
    T_EQ_INT(send_req(nc_req_password("secret1", 7, g_now)), 400);
    T_EQ_INT(nc_status_read(g_status, g_body, g_len), ASK_WHY_ACCOUNT);
    T_EQ_INT(nc_error()->code, NW_ACCOUNT_E_SAME_PASSWORD);
    T_EQ_INT(send_req(nc_req_password("secret2", 7, g_now)), 204);
    T_EQ_INT(nc_status_read(g_status, g_body, g_len), 0);
    nc_reset();
    T_EQ_INT(sign(0, "CARL", "secret1"), ASK_WHY_ACCOUNT);
    T_EQ_INT(sign(0, "CARL", "secret2"), 0);
    n = nc_session_save(blob, sizeof blob);

    // Sign out everywhere: the refresh token no longer refreshes, and the
    // client that tries it is signed out.
    T_EQ_INT(send_req(nc_req_signout(1, g_now)), 204);
    as(blob, n);
    T_EQ_INT(send_req(nc_req_refresh()), 400);
    T_EQ_INT(nc_session_read(g_status, g_body, g_len), ASK_WHY_ACCOUNT);
    T_CHECK(nc_error()->code == NW_ACCOUNT_E_REFRESH_USED || nc_error()->code == NW_ACCOUNT_E_BAD_REFRESH);
    T_EQ_INT(nc_session_info()->signed_in, 0);

    // Delete the account: it no longer signs in.
    T_EQ_INT(sign(0, "CARL", "secret2"), 0);
    T_EQ_INT(send_req(nc_req_delete_account(g_now)), 204);
    nc_reset();
    T_EQ_INT(sign(0, "CARL", "secret2"), ASK_WHY_ACCOUNT);
    T_EQ_INT(nc_error()->code, NW_ACCOUNT_E_BAD_LOGIN);
    teardown();
}

static bool first_game(const cb_member_row *m, void *ud) { cb_strlcpy(ud, m->game_id, CB_GAME_ID_MAX + 1); return false; }
static int g_seen;
static bool second_game(const cb_member_row *m, void *ud) {
    if (g_seen++ == 0) return true;
    cb_strlcpy(ud, m->game_id, CB_GAME_ID_MAX + 1);
    return false;
}
static int hr_skip;

static void games_views_and_chat(void) {
    setup();
    T_EQ_INT(sign(1, "DORA", "secret1"), 0);
    uint8_t dora[NC_SESSION_BLOB_MAX];
    int dn = nc_session_save(dora, sizeof dora);
    char dora_id[40];
    memcpy(dora_id, nc_session_info()->user_id, 36);
    dora_id[36] = 0;
    T_EQ_INT(call(b_create, NULL), 200);
    T_EQ_INT(nc_status_read(g_status, g_body, g_len), 0);
    uint8_t created[8192];
    int created_len = g_len;
    memcpy(created, g_body, (size_t)g_len);
    char gid[CB_GAME_ID_MAX + 1] = "";
    CB_STORE_CALL(g_ctx.store, games_of_user, dora_id, 1, first_game, gid);
    T_CHECK(gid[0] != 0);

    // The dashboard: one game, the envelope create answered with.
    T_EQ_INT(call(b_games, NULL), 200);
    T_EQ_INT(nc_list_read(g_status, g_body, g_len, NW_GAMES), 1);
    const NcGameRow *row = nc_game_row(g_body, g_len, 0);
    T_CHECK(row != NULL);
    if (row) T_EQ_MEM(g_body + row->env_off, row->env_len, created, created_len);
    T_EQ_INT(nc_list_read(g_status, g_body, g_len, NW_BOTS), -ASK_WHY_MALFORMED);   // not that list

    // One game: the player's own view.
    T_EQ_INT(call(b_game, gid), 200);
    row = nc_view_read(g_status, g_body, g_len);
    T_CHECK(row != NULL);
    uint8_t mine[8192];
    int mine_len = row ? (int)row->env_len : 0;
    if (row) memcpy(mine, g_body + row->env_off, row->env_len);
    T_EQ_MEM(mine, mine_len, created, created_len);
    T_EQ_INT(call(b_game, "zzzzzz"), 404);
    T_CHECK(nc_view_read(g_status, g_body, g_len) == NULL);
    T_EQ_INT(nc_error()->why, ASK_WHY_NOT_FOUND);

    // A bot through meta, the ask the kernel writes.
    TableAsk ask;
    memset(&ask, 0, sizeof ask);
    ask.kind = ASK_ADD_BOT;
    ask.gid_len = (uint8_t)strlen(gid);
    memcpy(ask.gid, gid, ask.gid_len);
    uint8_t ab[ASK_BYTES_MAX];
    int an = ask_encode(&ask, ab, sizeof ab);
    T_EQ_INT(send_req(nc_req_meta(ab, an, g_now)), 200);

    // Someone else sees the spectators' view, which is not the player's.
    nc_reset();
    T_EQ_INT(sign(1, "EVE", "secret1"), 0);
    T_EQ_INT(call(b_game, gid), 200);
    row = nc_view_read(g_status, g_body, g_len);
    T_CHECK(row != NULL);
    if (row) T_CHECK(row->env_len != (uint32_t)mine_len || memcmp(g_body + row->env_off, mine, (size_t)mine_len) != 0);

    // Chat: members only. A non-member reads an empty chat and may not post.
    T_EQ_INT(call(b_chats, gid), 200);
    T_EQ_INT(nc_list_read(g_status, g_body, g_len, NW_CHATS), 0);
    T_EQ_INT(send_req(nc_req_chat_send(gid, (int)strlen(gid), "hi", 2, g_now)), 403);
    uint8_t eve[NC_SESSION_BLOB_MAX];
    int en = nc_session_save(eve, sizeof eve);
    as(dora, dn);
    g_pub.n = 0;
    T_EQ_INT(send_req(nc_req_chat_send(gid, (int)strlen(gid), "hello there", 11, g_now)), 204);
    T_EQ_INT(g_pub.n, 1);
    char want_topic[64];
    snprintf(want_topic, sizeof want_topic, "chat:%s", gid);
    T_EQ_STR(g_pub.topic, want_topic);
    NwFrame f;
    T_EQ_INT(nw_frame_decode(g_pub.nw, (int)g_pub.nw_len, &f), NW_OK);
    T_EQ_INT(f.kind, NW_WS_CHAT);
    T_EQ_MEM(f.chat.message, f.chat.message_len, "hello there", 11);
    T_EQ_MEM(f.chat.user_id, f.chat.user_id_len, dora_id, 36);
    T_EQ_INT(call(b_chats, gid), 200);
    T_EQ_INT(nc_list_read(g_status, g_body, g_len, NW_CHATS), 1);
    const NcChatRow *c = nc_chat_row(g_body, g_len, 0);
    T_CHECK(c && c->message_len == 11 && memcmp(c->message, "hello there", 11) == 0 && !c->is_system);
    T_CHECK(c && c->created_ms > 0);
    // Eve still reads none of it.
    as(eve, en);
    T_EQ_INT(call(b_chats, gid), 200);
    T_EQ_INT(nc_list_read(g_status, g_body, g_len, NW_CHATS), 0);
    teardown();
}

static void bots_board_history_replay(void) {
    setup();
    T_EQ_INT(sign(1, "FAY", "secret1"), 0);
    char fay[40];
    memcpy(fay, nc_session_info()->user_id, 36);
    fay[36] = 0;
    T_EQ_INT(call(b_bots, NULL), 200);
    int nb = nc_list_read(g_status, g_body, g_len, NW_BOTS);
    T_EQ_INT(nb, 49);
    const NcBotRow *b = nc_bot_row(g_body, g_len, 0);
    T_CHECK(b && b->nickname_len > 1 && b->nickname[0] == '%' && b->key_len > 0);

    // The leaderboard: whoever has played, best first, each population on its own.
    cb_rating_row r;
    memset(&r, 0, sizeof r);
    cb_strlcpy(r.user_id, fay, sizeof r.user_id);
    cb_strlcpy(r.username, "FAY", sizeof r.username);
    r.elo_rating = 1600;
    r.previous_elo = -1;
    r.games_played = 3;
    T_EQ_INT(CB_STORE_CALL(g_ctx.store, rating_import, &r), CB_OK);
    // Someone who has not played is not on the board (sign-up made their row).
    uint8_t fay_blob[NC_SESSION_BLOB_MAX];
    int fbn = nc_session_save(fay_blob, sizeof fay_blob);
    T_EQ_INT(sign(1, "HAL", "secret1"), 0);
    as(fay_blob, fbn);
    T_EQ_INT(send_req(nc_req_board()), 200);
    int nboard = nc_list_read(g_status, g_body, g_len, NW_BOARD);
    T_CHECK(nboard >= 1);
    int humans = 0;
    for (int i = 0; i < nboard; i++) {
        const NcBoardRow *br = nc_board_row(g_body, g_len, i);
        T_CHECK(br && br->games > 0);
        if (br && !br->is_bot) {
            humans++;
            T_EQ_MEM(br->name, br->name_len, "FAY", 3);
            T_EQ_INT(br->rating, 1600);
        }
    }
    T_EQ_INT(humans, 1);

    // History: the rating, and the replays Fay played in, newest first.
    T_EQ_INT(call(b_history, NULL), 200);
    T_EQ_INT(nc_list_read(g_status, g_body, g_len, NW_HISTORY), 0);
    const NcHistoryHead *h = nc_history_head(g_body, g_len);
    T_CHECK(h && h->has_rating && h->rating == 1600 && h->n == 0);
    T_EQ_INT(call(b_create, NULL), 200);
    char gid[CB_GAME_ID_MAX + 1] = "";
    CB_STORE_CALL(g_ctx.store, games_of_user, fay, 1, first_game, gid);
    cb_snapshot_row s;
    memset(&s, 0, sizeof s);
    cb_uuid_v4(s.id);
    cb_strlcpy(s.game_id, gid, sizeof s.game_id);
    s.n_players = 1;
    cb_strlcpy(s.player_ids[0], fay, sizeof s.player_ids[0]);
    uint8_t moves[] = { 1, 2, 3 }, extras[] = { 9 };
    s.moves = moves;
    s.moves_len = 3;
    s.extras = extras;
    s.extras_len = 1;
    s.created_at = 1700000000000;
    T_EQ_INT(CB_STORE_CALL(g_ctx.store, snapshot_put, &s), CB_OK);
    // A newer replay of another of Fay's games: the replay route still answers the one asked for.
    T_EQ_INT(call(b_create, NULL), 200);
    char gid2[CB_GAME_ID_MAX + 1] = "";
    CB_STORE_CALL(g_ctx.store, games_of_user, fay, 1, first_game, gid2);
    if (strcmp(gid2, gid) == 0) CB_STORE_CALL(g_ctx.store, games_of_user, fay, 2, second_game, gid2);
    cb_snapshot_row s2 = s;
    cb_uuid_v4(s2.id);
    cb_strlcpy(s2.game_id, gid2, sizeof s2.game_id);
    uint8_t moves2[] = { 7 };
    s2.moves = moves2;
    s2.moves_len = 1;
    s2.created_at = s.created_at + 1000;
    T_EQ_INT(CB_STORE_CALL(g_ctx.store, snapshot_put, &s2), CB_OK);
    T_EQ_INT(call(b_history, NULL), 200);
    T_EQ_INT(nc_list_read(g_status, g_body, g_len, NW_HISTORY), 2);
    T_CHECK(nc_history_row(g_body, g_len, 0) && nc_history_row(g_body, g_len, 0)->moves_len == 1);   // newest first
    hr_skip = 1;
    const NcHistoryRow *hr = nc_history_row(g_body, g_len, hr_skip);
    T_CHECK(hr != NULL);
    if (hr) {
        T_EQ_MEM(g_body + hr->moves_off, hr->moves_len, moves, 3);
        T_EQ_MEM(g_body + hr->extras_off, hr->extras_len, extras, 1);
        T_EQ_INT(hr->n_players, 1);
        T_EQ_MEM(hr->players[0].id, hr->players[0].len, fay, 36);
        T_EQ_INT(hr->created_ms, 1700000000000);
    }
    T_EQ_INT(call(b_replay, gid), 200);
    const NcReplay *rp = nc_replay_read(g_status, g_body, g_len);
    T_CHECK(rp != NULL);
    if (rp) T_EQ_MEM(g_body + rp->moves_off, rp->moves_len, moves, 3);
    // Someone who did not play it is not shown it.
    nc_reset();
    T_EQ_INT(sign(1, "GUS", "secret1"), 0);
    T_EQ_INT(call(b_replay, gid), 404);
    T_CHECK(nc_replay_read(g_status, g_body, g_len) == NULL);
    T_EQ_INT(call(b_history, NULL), 200);
    h = nc_history_head(g_body, g_len);
    T_CHECK(h && h->has_rating && h->n == 0);   // sign-up made the rating row (store.h user_create)
    teardown();
}

// The value of header `name` in the last answer ("" when absent).
static const char *res_header(const char *name) {
    static char v[256];
    v[0] = 0;
    char pat[64];
    snprintf(pat, sizeof pat, "%s: ", name);
    const char *p = strstr(g_hdrs, pat);
    if (!p) return v;
    p += strlen(pat);
    const char *e = strstr(p, "\r\n");
    cb_strncpy0(v, sizeof v, p, e ? (size_t)(e - p) : strlen(p));
    return v;
}

// Each route's Cache-Control: only the two public reads may be kept by the
// edge, they never need a caller, and they never vary by Origin.
static void cache_control_per_route(void) {
    setup();
    cb_strlcpy(g_cfg.allowed_origins, "https://foolish.cards", sizeof g_cfg.allowed_origins);
    // Nobody signed in: the public reads carry no token and are answered.
    const NcRequest *q = nc_req_bots(g_now);
    T_CHECK(q && q->auth_len == 0);
    T_EQ_INT(send_req(q), 200);
    T_EQ_INT(nc_list_read(g_status, g_body, g_len, NW_BOTS), 49);
    T_EQ_STR(res_header("Cache-Control"), "public, max-age=0, s-maxage=86400");
    T_EQ_STR(res_header("Access-Control-Allow-Origin"), "*");
    T_EQ_STR(res_header("Vary"), "");
    q = nc_req_board();
    T_CHECK(q && q->auth_len == 0);
    T_EQ_INT(send_req(q), 200);
    T_EQ_STR(res_header("Cache-Control"), "public, max-age=0, s-maxage=3600");
    T_EQ_STR(res_header("Access-Control-Allow-Origin"), "*");
    // Signed in, every other read is the caller's own and is never stored.
    T_EQ_INT(sign(1, "IDA", "secret1"), 0);
    T_EQ_INT(call(b_games, NULL), 200);
    T_EQ_STR(res_header("Cache-Control"), "no-store");
    T_EQ_INT(call(b_history, NULL), 200);
    T_EQ_STR(res_header("Cache-Control"), "no-store");
    // A token sent anyway changes nothing about the public answer.
    NcRequest with = *nc_req_board();
    const NcRequest *g = nc_req_games(g_now);
    memcpy(with.auth, g->auth, g->auth_len);
    with.auth_len = g->auth_len;
    T_EQ_INT(send_req(&with), 200);
    T_EQ_STR(res_header("Cache-Control"), "public, max-age=0, s-maxage=3600");
    // A refusal is never stored either.
    NcRequest anon = *nc_req_board();
    memcpy(anon.path, "/v1/games", 9);
    anon.path_len = 9;
    nc_reset();
    T_EQ_INT(send_req(&anon), 401);
    T_EQ_STR(res_header("Cache-Control"), "no-store");
    teardown();
}

// A store that gives up on a throttle: the leaderboard's read and the
// history's rating read fail as DynamoDB's does (store.h CB_STORE_CALL), and
// the full ratings read is not to be called by any route.
static cb_store_ops g_busy_ops;
static const cb_store_ops *g_real_ops;
static bool g_busy;
static int busy_top(cb_store *s, int limit, cb_rating_fn fn, void *ud) {
    if (g_busy) { cb_store_mark_busy(); return CB_E_DB; }
    return g_real_ops->ratings_top(s, limit, fn, ud);
}
static int busy_for(cb_store *s, const char *const *ids, int n, int32_t *out) {
    if (g_busy) { cb_store_mark_busy(); return CB_E_DB; }
    return g_real_ops->ratings_for(s, ids, n, out);
}
static int no_list(cb_store *s, cb_rating_fn fn, void *ud) { return CB_E_DB; }
static int plain_fail(cb_store *s, int limit, cb_rating_fn fn, void *ud) { return CB_E_DB; }

static void a_throttled_read_is_busy_and_retried(void) {
    setup();
    g_real_ops = g_ctx.store->ops;
    g_busy_ops = *g_real_ops;
    g_busy_ops.ratings_top = busy_top;
    g_busy_ops.ratings_for = busy_for;
    g_busy_ops.ratings_list = no_list;
    g_ctx.store->ops = &g_busy_ops;
    T_EQ_INT(sign(1, "JO", "secret1"), 0);
    // The history reads only the caller's rating, never every rating row.
    g_busy = false;
    T_EQ_INT(call(b_history, NULL), 200);
    T_EQ_INT(send_req(nc_req_board()), 200);
    g_busy = true;
    T_EQ_INT(send_req(nc_req_board()), 503);
    T_EQ_STR(res_header("Retry-After"), "1");
    T_EQ_STR(res_header("Cache-Control"), "no-store");
    T_EQ_INT(nc_list_read(g_status, g_body, g_len, NW_BOARD), -ASK_WHY_BUSY);
    // The client sends a read again, after 250 ms doubling, three times; never a write.
    T_EQ_INT(nc_retry_ms(NC_GET, 0, g_status, g_body, g_len), 250);
    T_EQ_INT(nc_retry_ms(NC_GET, 1, g_status, g_body, g_len), 500);
    T_EQ_INT(nc_retry_ms(NC_GET, 2, g_status, g_body, g_len), 1000);
    T_EQ_INT(nc_retry_ms(NC_GET, 3, g_status, g_body, g_len), 0);
    T_EQ_INT(nc_retry_ms(NC_POST, 0, g_status, g_body, g_len), 0);
    T_EQ_INT(call(b_history, NULL), 503);
    T_EQ_INT(nc_retry_ms(NC_GET, 0, g_status, g_body, g_len), 250);
    // A plain server failure is not busy, and is not sent again.
    g_busy_ops.ratings_top = plain_fail;
    T_EQ_INT(send_req(nc_req_board()), 500);
    T_EQ_INT(nc_retry_ms(NC_GET, 0, g_status, g_body, g_len), 0);
    T_EQ_STR(res_header("Retry-After"), "");
    // Nor is any other 503 (a backend not up yet): only BUSY is.
    cb_store *store = g_ctx.store;
    g_ctx.store = NULL;
    T_EQ_INT(send_req(nc_req_board()), 503);
    T_EQ_INT(nc_retry_ms(NC_GET, 0, g_status, g_body, g_len), 0);
    g_ctx.store = store;
    g_ctx.store->ops = g_real_ops;
    teardown();
}

// The leaderboard's two reads and the history's two reads are independent,
// so each pair is in flight at once (cb_store_side), the side thread under
// the request's store deadline. Each wrapped read waits (up to 2 s) for its
// partner to start: one after the other, neither ever sees the other.
static cb_store_ops g_pair_ops;
static atomic_int g_in_flight;
static atomic_int g_overlapped;
static int64_t g_side_deadline[2];
// g_overlapped: the most reads ever in flight together.
static void pair_enter(int k) {
    int now = atomic_fetch_add(&g_in_flight, 1) + 1;
    for (int peak = atomic_load(&g_overlapped); now > peak && !atomic_compare_exchange_weak(&g_overlapped, &peak, now);) {}
    g_side_deadline[k] = cb_store_deadline();
    for (int i = 0; i < 200 && atomic_load(&g_overlapped) < 2; i++) usleep(10000);
}
static void pair_leave(void) { atomic_fetch_sub(&g_in_flight, 1); }
static int pair_top(cb_store *s, int limit, cb_rating_fn fn, void *ud) {
    pair_enter(0);
    int rc = g_real_ops->ratings_top(s, limit, fn, ud);
    pair_leave();
    return rc;
}
static int pair_bots(cb_store *s, cb_bot_fn fn, void *ud) {
    pair_enter(1);
    int rc = g_real_ops->bots_list(s, fn, ud);
    pair_leave();
    return rc;
}
static int pair_for(cb_store *s, const char *const *ids, int n, int32_t *out) {
    pair_enter(1);
    int rc = g_real_ops->ratings_for(s, ids, n, out);
    pair_leave();
    return rc;
}
static int pair_snaps(cb_store *s, const cb_snapshot_query *q, cb_snapshot_fn fn, void *ud) {
    pair_enter(0);
    int rc = g_real_ops->snapshots_query(s, q, fn, ud);
    pair_leave();
    return rc;
}

static void independent_reads_run_at_once(void) {
    setup();
    T_EQ_INT(sign(1, "JO", "secret1"), 0);
    g_real_ops = g_ctx.store->ops;
    g_pair_ops = *g_real_ops;
    g_pair_ops.ratings_top = pair_top;
    g_pair_ops.bots_list = pair_bots;
    g_pair_ops.ratings_for = pair_for;
    g_pair_ops.snapshots_query = pair_snaps;
    g_ctx.store->ops = &g_pair_ops;
    const int64_t deadline = cb_now_ms() + 60000;
    cb_store_set_deadline(deadline);
    atomic_store(&g_overlapped, 0);
    T_EQ_INT(send_req(nc_req_board()), 200);
    T_EQ_INT(atomic_load(&g_overlapped), 2);
    T_EQ_INT(g_side_deadline[1], deadline);   // the bots' read, on the side thread
    atomic_store(&g_overlapped, 0);
    g_side_deadline[1] = 0;
    T_EQ_INT(call(b_history, NULL), 200);
    T_EQ_INT(atomic_load(&g_overlapped), 2);
    T_EQ_INT(g_side_deadline[1], deadline);   // the rating's read, on the side thread
    cb_store_set_deadline(0);
    g_ctx.store->ops = g_real_ops;
    teardown();
}

// With CBASE_ORIGIN_SECRET set, a request without the edge's X-Cbase-Origin
// (the Function URL hit directly) is refused before any route runs: no store
// read, no bcrypt. Unset, nothing is checked (serve, tests).
static void only_through_the_edge(void) {
    setup();
    g_origin = NULL;
    T_EQ_INT(call(b_bots, NULL), 200);
    cb_strlcpy(g_cfg.origin_secret, "0123456789abcdef0123456789abcdef", sizeof g_cfg.origin_secret);
    T_EQ_INT(call(b_bots, NULL), 403);
    T_EQ_INT(send_req(nc_req_signin("ANA", 3, "password1", 9)), 403);
    g_origin = "0123456789abcdef0123456789abcdeF";
    T_EQ_INT(call(b_bots, NULL), 403);
    g_origin = "0123456789abcdef0123456789abcdef ";
    T_EQ_INT(call(b_bots, NULL), 403);
    g_origin = "0123456789abcdef0123456789abcdef";
    T_EQ_INT(call(b_bots, NULL), 200);
    g_origin = NULL;
    teardown();
}

int main(void) {
    T_RUN(only_through_the_edge);
    T_RUN(the_email_is_the_clients_derivation);
    T_RUN(accounts);
    T_RUN(without_a_backend);
    T_RUN(refresh_password_signout_delete);
    T_RUN(games_views_and_chat);
    T_RUN(bots_board_history_replay);
    T_RUN(independent_reads_run_at_once);
    T_RUN(cache_control_per_route);
    T_RUN(a_throttled_read_is_busy_and_retried);
    return T_DONE();
}
