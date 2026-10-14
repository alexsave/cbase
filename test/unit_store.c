// unit_store.c - the SQLite store (store/store.h) against a real file: every
// table round-trips, the CAS, the bot lease, the heartbeat scan's windows, the
// ratings a commit applies, the admin console's reads, the import writes.
#include "t.h"

#include "store/store.h"
#include "table.h"
#include "util/clock.h"
#include "util/err.h"
#include "util/uuid.h"

#include <pthread.h>
#include <sqlite3.h>
#include <stdlib.h>
#include <unistd.h>

static char g_dir[256];
static int g_seq;

static cb_store *open_fresh(void) {
    char path[320];
    snprintf(path, sizeof path, "%s/s%d.sqlite", g_dir, ++g_seq);
    cb_store *s = NULL;
    T_EQ_INT(cb_store_open_sqlite(path, &s), CB_OK);
    return s;
}

static cb_user_row mkuser(const char *email, const char *name, int64_t at) {
    cb_user_row u = { 0 };
    cb_uuid_v4(u.id);
    snprintf(u.email, sizeof u.email, "%s", email);
    snprintf(u.password_hash, sizeof u.password_hash, "$2a$10$abcdefghijklmnopqrstuuABCDEFGHIJKLMNOPQRSTUVWXYZ01234");
    snprintf(u.username, sizeof u.username, "%s", name);
    u.created_at = at;
    return u;
}

static uint8_t g_state[3] = { 1, 2, 3 }, g_roster[5] = { 9, 8, 7, 6, 5 }, g_logs[2] = { 0xaa, 0xbb };

static cb_game_row mkgame(const char *id, int64_t at) {
    cb_game_row g = { 0 };
    snprintf(g.id, sizeof g.id, "%s", id);
    g.state = g_state;
    g.state_len = sizeof g_state;
    g.roster = g_roster;
    g.roster_len = sizeof g_roster;
    g.created_at = g.updated_at = g.last_commit_at = at;
    return g;
}

// ---- collectors ------------------------------------------------------------------------

typedef struct { int n; char ids[16][CB_EMAIL_MAX + 1]; int64_t at[16]; } names_t;
static bool collect_name(const char *name, void *ud) {
    names_t *n = ud;
    if (n->n < 16) snprintf(n->ids[n->n++], sizeof n->ids[0], "%s", name);
    return true;
}
static bool collect_member(const cb_member_row *r, void *ud) {
    names_t *n = ud;
    if (n->n < 16) { n->at[n->n] = r->updated_at; snprintf(n->ids[n->n++], sizeof n->ids[0], "%s", r->game_id); }
    return true;
}
static bool collect_user(const cb_user_row *r, void *ud) { return collect_name(r->email, ud); }
static bool collect_snap(const cb_snapshot_row *r, void *ud) { return collect_name(r->id, ud); }
static bool collect_chat(const cb_chat_row *r, void *ud) { return collect_name(r->message, ud); }
static bool collect_conn(const cb_conn_row *r, void *ud) { return collect_name(r->conn_id, ud); }

typedef struct { int n; cb_bot_row rows[64]; } bots_t;
static bool collect_bot(const cb_bot_row *r, void *ud) {
    bots_t *b = ud;
    if (b->n < 64) b->rows[b->n++] = *r;
    return true;
}
typedef struct { int n; cb_rating_row rows[16]; } ratings_t;
static bool collect_rating(const cb_rating_row *r, void *ud) {
    ratings_t *b = ud;
    if (b->n < 16) b->rows[b->n++] = *r;
    return true;
}

static int32_t rating_of(cb_store *s, const char *id) {
    int32_t out = -99;
    const char *ids[1] = { id };
    T_EQ_INT(CB_STORE_CALL(s, ratings_for, ids, 1, &out), CB_OK);
    return out;
}

// ---- open ----------------------------------------------------------------------------

static void open_applies_schema_and_seeds_bots(void) {
    cb_store *s = open_fresh();
    T_EQ_STR(s->ops->name, "sqlite");
    bots_t b = { 0 };
    T_EQ_INT(CB_STORE_CALL(s, bots_list, collect_bot, &b), 49);
    T_EQ_STR(b.rows[0].nickname, "%Miami 1");
    T_EQ_STR(b.rows[0].strategy_key, "random");
    T_EQ_STR(b.rows[6].nickname, "%Miami 7");
    T_EQ_STR(b.rows[48].nickname, "%Moscow 7");
    T_EQ_STR(b.rows[48].strategy_key, "octogen");
    T_EQ_STR(b.rows[0].id, "2c601cde-63d2-5291-8fcb-735f5860b1ec");
    T_EQ_INT(b.rows[0].elo_rating, 1000);
    T_EQ_INT(b.rows[0].previous_elo, -1);
    cb_store_close(s);

    // Reopening applies the (idempotent) schema again and keeps the rows.
    char path[320];
    snprintf(path, sizeof path, "%s/s%d.sqlite", g_dir, g_seq);
    T_EQ_INT(cb_store_open_sqlite(path, &s), CB_OK);
    b.n = 0;
    T_EQ_INT(CB_STORE_CALL(s, bots_list, collect_bot, &b), 49);
    cb_store_close(s);

    // A database from a newer binary is refused.
    sqlite3 *db;
    T_EQ_INT(sqlite3_open(path, &db), SQLITE_OK);
    T_EQ_INT(sqlite3_exec(db, "PRAGMA user_version = 99", NULL, NULL, NULL), SQLITE_OK);
    sqlite3_close(db);
    s = NULL;
    T_EQ_INT(cb_store_open_sqlite(path, &s), CB_E_STATE);
    T_CHECK(s == NULL);
}

// ---- users and refresh tokens ------------------------------------------------------------

static void users_round_trip(void) {
    cb_store *s = open_fresh();
    cb_user_row a = mkuser("ann@x.io", "Ann", 1000), b = mkuser("bob@x.io", "", 2000), got;
    T_EQ_INT(CB_STORE_CALL(s, user_create, &a), CB_OK);
    T_EQ_INT(CB_STORE_CALL(s, user_create, &b), CB_OK);
    T_EQ_INT(CB_STORE_CALL(s, user_get, a.id, &got), CB_OK);
    T_EQ_STR(got.email, "ann@x.io");
    T_EQ_STR(got.username, "Ann");
    T_EQ_STR(got.password_hash, a.password_hash);
    T_EQ_INT(got.created_at, 1000);
    T_EQ_INT(got.last_sign_in_at, 0);
    T_EQ_INT(CB_STORE_CALL(s, user_by_email, "bob@x.io", &got), CB_OK);
    T_EQ_STR(got.id, b.id);
    T_EQ_STR(got.username, "");
    T_EQ_INT(CB_STORE_CALL(s, user_by_email, "nobody@x.io", &got), CB_E_NOT_FOUND);

    // The email is unique.
    cb_user_row dup = mkuser("ann@x.io", "Other", 3000);
    T_EQ_INT(CB_STORE_CALL(s, user_create, &dup), CB_E_CONFLICT);
    T_EQ_INT(CB_STORE_CALL(s, user_get, dup.id, &got), CB_E_NOT_FOUND);

    // The trigger's rating row came with the account.
    T_EQ_INT(rating_of(s, a.id), TABLE_RATING_START);
    ratings_t r = { 0 };
    T_EQ_INT(CB_STORE_CALL(s, ratings_list, collect_rating, &r), 2);
    T_EQ_STR(r.rows[0].user_id, strcmp(a.id, b.id) < 0 ? a.id : b.id);

    T_EQ_INT(CB_STORE_CALL(s, user_set_password, a.id, "$2b$10$new"), CB_OK);
    T_EQ_INT(CB_STORE_CALL(s, user_touch_sign_in, a.id, 5555), CB_OK);
    T_EQ_INT(CB_STORE_CALL(s, user_get, a.id, &got), CB_OK);
    T_EQ_STR(got.password_hash, "$2b$10$new");
    T_EQ_INT(got.last_sign_in_at, 5555);
    T_EQ_INT(CB_STORE_CALL(s, user_set_password, "00000000-0000-4000-8000-000000000000", "x"), CB_E_NOT_FOUND);

    // users_list: newest first, search on email or username, offset and limit.
    names_t n = { 0 };
    T_EQ_INT(CB_STORE_CALL(s, users_list, "", 0, 10, collect_user, &n), 2);
    T_EQ_STR(n.ids[0], "bob@x.io");
    T_EQ_STR(n.ids[1], "ann@x.io");
    n.n = 0;
    T_EQ_INT(CB_STORE_CALL(s, users_list, "ANN", 0, 10, collect_user, &n), 1);
    T_EQ_STR(n.ids[0], "ann@x.io");
    n.n = 0;
    T_EQ_INT(CB_STORE_CALL(s, users_list, "", 1, 10, collect_user, &n), 1);
    T_EQ_STR(n.ids[0], "ann@x.io");

    T_EQ_INT(CB_STORE_CALL(s, rating_set_username, a.id, NULL), CB_OK);
    r.n = 0;
    CB_STORE_CALL(s, ratings_list, collect_rating, &r);
    for (int i = 0; i < r.n; i++)
        if (strcmp(r.rows[i].user_id, a.id) == 0) T_EQ_STR(r.rows[i].username, "");
    cb_store_close(s);
}

static void refresh_tokens_rotate(void) {
    cb_store *s = open_fresh();
    cb_user_row a = mkuser("a@x.io", "A", 1);
    T_EQ_INT(CB_STORE_CALL(s, user_create, &a), CB_OK);
    cb_refresh_row r = { .token_hash = "aa", .session_id = "sess-1", .created_at = 7 }, t1 = r, got;
    memcpy(r.user_id, a.id, sizeof r.user_id);
    t1 = r;
    snprintf(t1.token_hash, sizeof t1.token_hash, "bb");
    T_EQ_INT(CB_STORE_CALL(s, refresh_put, &r), CB_OK);
    T_EQ_INT(CB_STORE_CALL(s, refresh_put, &t1), CB_OK);
    T_EQ_INT(CB_STORE_CALL(s, refresh_put, &r), CB_E_CONFLICT);
    T_EQ_INT(CB_STORE_CALL(s, refresh_take, "aa", 100, &got), CB_OK);
    T_CHECK(!got.revoked);
    T_EQ_INT(got.revoked_at, 0);
    T_EQ_STR(got.user_id, a.id);
    T_EQ_STR(got.session_id, "sess-1");
    T_EQ_INT(got.created_at, 7);
    T_EQ_INT(CB_STORE_CALL(s, refresh_take, "aa", 200, &got), CB_OK);
    T_CHECK(got.revoked);   // a second use is a reuse
    T_EQ_INT(got.revoked_at, 100);   // stamped by the first take
    T_EQ_INT(CB_STORE_CALL(s, refresh_take, "aa", 300, &got), CB_OK);
    T_EQ_INT(got.revoked_at, 100);   // and only the first
    T_EQ_INT(CB_STORE_CALL(s, refresh_take, "zz", 300, &got), CB_E_NOT_FOUND);
    T_EQ_INT(CB_STORE_CALL(s, refresh_revoke_session, "sess-1"), CB_OK);
    T_EQ_INT(CB_STORE_CALL(s, refresh_take, "bb", 400, &got), CB_OK);
    T_CHECK(got.revoked);
    T_EQ_INT(got.revoked_at, 0);     // a logout's revoke never stamps
    T_EQ_INT(CB_STORE_CALL(s, refresh_take, "aa", 500, &got), CB_OK);
    T_EQ_INT(got.revoked_at, 0);     // and clears a rotation's stamp
    // Per user: every session of the user, nobody else's.
    cb_user_row b = mkuser("b@x.io", "B", 2);
    T_EQ_INT(CB_STORE_CALL(s, user_create, &b), CB_OK);
    cb_refresh_row c1 = r, c2 = r, c3 = r;
    snprintf(c1.token_hash, sizeof c1.token_hash, "c1");
    snprintf(c1.session_id, sizeof c1.session_id, "sess-2");
    snprintf(c2.token_hash, sizeof c2.token_hash, "c2");
    snprintf(c2.session_id, sizeof c2.session_id, "sess-3");
    snprintf(c3.token_hash, sizeof c3.token_hash, "c3");
    memcpy(c3.user_id, b.id, sizeof c3.user_id);
    T_EQ_INT(CB_STORE_CALL(s, refresh_put, &c1), CB_OK);
    T_EQ_INT(CB_STORE_CALL(s, refresh_put, &c2), CB_OK);
    T_EQ_INT(CB_STORE_CALL(s, refresh_put, &c3), CB_OK);
    T_EQ_INT(CB_STORE_CALL(s, refresh_revoke_user, a.id), CB_OK);
    T_EQ_INT(CB_STORE_CALL(s, refresh_take, "c1", 600, &got), CB_OK);
    T_CHECK(got.revoked);
    T_EQ_INT(CB_STORE_CALL(s, refresh_take, "c2", 600, &got), CB_OK);
    T_CHECK(got.revoked);
    T_EQ_INT(CB_STORE_CALL(s, refresh_take, "c3", 600, &got), CB_OK);
    T_CHECK(!got.revoked);
    cb_store_close(s);
}

// ---- games ---------------------------------------------------------------------------

static void game_round_trip(void) {
    cb_store *s = open_fresh();
    cb_user_row a = mkuser("a@x.io", "A", 1);
    T_EQ_INT(CB_STORE_CALL(s, user_create, &a), CB_OK);
    cb_game_row g = mkgame("ab12cd", 100), got;
    T_EQ_INT(CB_STORE_CALL(s, game_create, &g, a.id), CB_OK);
    T_EQ_INT(CB_STORE_CALL(s, game_create, &g, a.id), CB_E_CONFLICT);
    T_EQ_INT(CB_STORE_CALL(s, game_get, "ab12cd", &got), CB_OK);
    T_EQ_MEM(got.state, got.state_len, g_state, sizeof g_state);
    T_EQ_MEM(got.roster, got.roster_len, g_roster, sizeof g_roster);
    T_CHECK(got.logs == NULL && got.logs_len == 0);
    T_CHECK(!got.has_seed && !got.needs_bots);
    T_EQ_INT(got.version, 0);
    T_EQ_INT(got.created_at, 100);
    T_EQ_INT(got.last_commit_at, 100);
    cb_game_row_free(&got);
    T_EQ_INT(CB_STORE_CALL(s, is_member, "ab12cd", a.id), 1);
    T_EQ_INT(CB_STORE_CALL(s, game_get, "ffffff", &got), CB_E_NOT_FOUND);

    // A commit writes every column.
    cb_game_row n = mkgame("ab12cd", 100);
    n.status = 1;
    n.logs = g_logs;
    n.logs_len = sizeof g_logs;
    n.has_seed = true;
    for (int i = 0; i < CB_SEED_LEN; i++) n.seed[i] = (uint8_t)(i * 7);
    n.needs_bots = true;
    n.version = 1;
    n.round_epoch = 1;
    n.updated_at = 200;
    n.last_commit_at = 0;   // the store stamps the commit's time itself
    cb_game_commit c = { .row = &n, .expected_version = 0, .n_humans = 1 };
    memcpy(c.humans[0], a.id, sizeof c.humans[0]);
    T_EQ_INT(CB_STORE_CALL(s, game_commit, &c), CB_OK);
    T_EQ_INT(CB_STORE_CALL(s, game_get, "ab12cd", &got), CB_OK);
    T_EQ_INT(got.status, 1);
    T_EQ_MEM(got.logs, got.logs_len, g_logs, sizeof g_logs);
    T_CHECK(got.has_seed);
    T_EQ_MEM(got.seed, CB_SEED_LEN, n.seed, CB_SEED_LEN);
    T_CHECK(got.needs_bots);
    T_EQ_INT(got.version, 1);
    T_EQ_INT(got.round_epoch, 1);
    T_EQ_INT(got.created_at, 100);
    T_EQ_INT(got.updated_at, 200);
    T_EQ_INT(got.last_commit_at, 200);
    cb_game_row_free(&got);
    cb_store_close(s);
}

// Two writers read version 0; the first commit wins, the second loses and
// writes nothing, its membership and ratings included.
static void cas_second_writer_loses(void) {
    cb_store *s = open_fresh();
    cb_user_row a = mkuser("a@x.io", "A", 1), b = mkuser("b@x.io", "B", 1);
    CB_STORE_CALL(s, user_create, &a);
    CB_STORE_CALL(s, user_create, &b);
    cb_game_row g = mkgame("cas001", 10);
    T_EQ_INT(CB_STORE_CALL(s, game_create, &g, a.id), CB_OK);

    cb_game_row w1 = mkgame("cas001", 10), w2 = mkgame("cas001", 10);
    uint8_t s1[1] = { 0x11 }, s2[1] = { 0x22 };
    w1.state = s1, w1.state_len = 1, w1.version = 1, w1.updated_at = 20;
    w2.state = s2, w2.state_len = 1, w2.version = 1, w2.updated_at = 21;
    cb_game_commit c1 = { .row = &w1, .expected_version = 0, .n_humans = 1 };
    cb_game_commit c2 = { .row = &w2, .expected_version = 0, .n_humans = 2, .n_rated = 1 };
    memcpy(c1.humans[0], a.id, sizeof(cb_uuid));
    memcpy(c2.humans[0], a.id, sizeof(cb_uuid));
    memcpy(c2.humans[1], b.id, sizeof(cb_uuid));
    memcpy(c2.rated_ids[0], b.id, sizeof(cb_uuid));
    c2.rating_delta[0] = 50;
    T_EQ_INT(CB_STORE_CALL(s, game_commit, &c1), CB_OK);
    T_EQ_INT(CB_STORE_CALL(s, game_commit, &c2), CB_E_CONFLICT);
    cb_game_row got;
    T_EQ_INT(CB_STORE_CALL(s, game_get, "cas001", &got), CB_OK);
    T_EQ_INT(got.version, 1);
    T_EQ_MEM(got.state, got.state_len, s1, 1);
    T_EQ_INT(got.updated_at, 20);
    cb_game_row_free(&got);
    T_EQ_INT(CB_STORE_CALL(s, is_member, "cas001", b.id), 0);
    T_EQ_INT(rating_of(s, b.id), 1000);

    // Reloaded at version 1, the second writer wins.
    c2.expected_version = 1;
    w2.version = 2;
    T_EQ_INT(CB_STORE_CALL(s, game_commit, &c2), CB_OK);
    T_EQ_INT(CB_STORE_CALL(s, is_member, "cas001", b.id), 1);
    T_EQ_INT(rating_of(s, b.id), 1050);

    // One transaction: when the last write of a commit (a rating) fails,
    // nothing of it stays, the row included. A trigger makes the rating fail.
    T_EQ_INT(CB_STORE_CALL(s, admin_query,
                           "CREATE TRIGGER no_ratings BEFORE UPDATE ON ratings BEGIN SELECT RAISE(ABORT, 'no'); END",
                           true, NULL, NULL), 0);
    cb_game_row w3 = mkgame("cas001", 10);
    w3.version = 3;
    cb_game_commit c3 = { .row = &w3, .expected_version = 2, .n_humans = 1, .n_rated = 1 };
    memcpy(c3.humans[0], a.id, sizeof(cb_uuid));
    memcpy(c3.rated_ids[0], a.id, sizeof(cb_uuid));
    T_EQ_INT(CB_STORE_CALL(s, game_commit, &c3), CB_E_DB);
    T_EQ_INT(CB_STORE_CALL(s, game_get, "cas001", &got), CB_OK);
    T_EQ_INT(got.version, 2);
    cb_game_row_free(&got);
    T_EQ_INT(CB_STORE_CALL(s, is_member, "cas001", b.id), 1);   // the seats were not rewritten either

    // A seat with no account (deleted, its id still in the roster) is skipped,
    // not a reason to refuse the commit.
    CB_STORE_CALL(s, admin_query, "DROP TRIGGER no_ratings", true, NULL, NULL);
    c3.n_rated = 0;
    snprintf(c3.humans[0], sizeof c3.humans[0], "00000000-0000-4000-8000-0000000000ff");
    T_EQ_INT(CB_STORE_CALL(s, game_commit, &c3), CB_OK);
    T_EQ_INT(CB_STORE_CALL(s, is_member, "cas001", c3.humans[0]), 0);

    // A commit to a game that is gone is NOT_FOUND, not a conflict to retry.
    cb_game_row x = mkgame("nogame", 1);
    x.version = 1;
    cb_game_commit cx = { .row = &x, .expected_version = 0 };
    T_EQ_INT(CB_STORE_CALL(s, game_commit, &cx), CB_E_NOT_FOUND);
    cb_store_close(s);
}

static bool first_value(int n, const char *const *names, const char *const *values, void *ud) {
    char *out = ud;
    snprintf(out, 64, "%s", n > 0 && values[0] ? values[0] : "(null)");
    return true;
}

// Membership is rewritten from every commit: listed seats added, the rest removed.
static void membership_follows_commits(void) {
    cb_store *s = open_fresh();
    cb_user_row a = mkuser("a@x.io", "A", 1), b = mkuser("b@x.io", "B", 1);
    CB_STORE_CALL(s, user_create, &a);
    CB_STORE_CALL(s, user_create, &b);
    cb_game_row g1 = mkgame("game01", 10), g2 = mkgame("game02", 11);
    CB_STORE_CALL(s, game_create, &g1, a.id);
    CB_STORE_CALL(s, game_create, &g2, a.id);

    cb_game_row n = mkgame("game01", 10);
    n.version = 1;
    n.updated_at = 50;
    cb_game_commit c = { .row = &n, .expected_version = 0, .n_humans = 2, .n_bots = 1 };
    memcpy(c.humans[0], a.id, sizeof(cb_uuid));
    memcpy(c.humans[1], b.id, sizeof(cb_uuid));
    snprintf(c.bots[0], sizeof c.bots[0], "2c601cde-63d2-5291-8fcb-735f5860b1ec");
    T_EQ_INT(CB_STORE_CALL(s, game_commit, &c), CB_OK);
    T_EQ_INT(CB_STORE_CALL(s, is_member, "game01", b.id), 1);

    // a's dashboard: most recently updated first.
    names_t m = { 0 };
    T_EQ_INT(CB_STORE_CALL(s, games_of_user, a.id, 10, collect_member, &m), 2);
    T_EQ_STR(m.ids[0], "game01");
    T_EQ_INT(m.at[0], 50);
    T_EQ_STR(m.ids[1], "game02");
    m.n = 0;
    T_EQ_INT(CB_STORE_CALL(s, games_of_user, a.id, 1, collect_member, &m), 1);

    // a leaves game01: the next commit seats only b, and no bot.
    n.version = 2;
    n.updated_at = 60;
    c.expected_version = 1;
    memcpy(c.humans[0], b.id, sizeof(cb_uuid));
    c.n_humans = 1;
    c.n_bots = 0;
    T_EQ_INT(CB_STORE_CALL(s, game_commit, &c), CB_OK);
    T_EQ_INT(CB_STORE_CALL(s, is_member, "game01", a.id), 0);
    T_EQ_INT(CB_STORE_CALL(s, is_member, "game01", b.id), 1);
    char v[64];
    T_EQ_INT(CB_STORE_CALL(s, admin_query, "SELECT count(*) FROM game_bots", false, first_value, v), 1);
    T_EQ_STR(v, "0");
    cb_store_close(s);
}

static void game_delete_cascades(void) {
    cb_store *s = open_fresh();
    cb_user_row a = mkuser("a@x.io", "A", 1);
    CB_STORE_CALL(s, user_create, &a);
    cb_game_row g = mkgame("del001", 10);
    CB_STORE_CALL(s, game_create, &g, a.id);
    cb_chat_row ch = { .message = "hi", .created_at = 5 };
    cb_uuid_v4(ch.id);
    snprintf(ch.game_id, sizeof ch.game_id, "del001");
    memcpy(ch.user_id, a.id, sizeof(cb_uuid));
    T_EQ_INT(CB_STORE_CALL(s, chat_put, &ch), CB_OK);
    uint8_t mv[2] = { 1, 2 };
    cb_snapshot_row sn = { .n_players = 1, .moves = mv, .moves_len = 2, .created_at = 9 };
    cb_uuid_v4(sn.id);
    snprintf(sn.game_id, sizeof sn.game_id, "del001");
    memcpy(sn.player_ids[0], a.id, sizeof(cb_uuid));
    T_EQ_INT(CB_STORE_CALL(s, snapshot_put, &sn), CB_OK);

    T_EQ_INT(CB_STORE_CALL(s, game_delete, "del001", 3), CB_E_CONFLICT);
    T_EQ_INT(CB_STORE_CALL(s, game_delete, "del001", 0), CB_OK);
    T_EQ_INT(CB_STORE_CALL(s, game_delete, "del001", 0), CB_E_NOT_FOUND);
    T_EQ_INT(CB_STORE_CALL(s, is_member, "del001", a.id), 0);
    names_t n = { 0 };
    T_EQ_INT(CB_STORE_CALL(s, chat_list, "del001", 0, collect_chat, &n), 0);
    // The replay outlives the game, its game_id cleared (ON DELETE SET NULL).
    char v[64];
    T_EQ_INT(CB_STORE_CALL(s, admin_query, "SELECT game_id FROM game_snapshots", false, first_value, v), 1);
    T_EQ_STR(v, "(null)");
    cb_store_close(s);
}

// user_delete takes the account's tokens, seats, rating and chat with it;
// snapshots keep its id in player_ids (DESIGN.md "Open defects carried over").
static void user_delete_cascades(void) {
    cb_store *s = open_fresh();
    cb_user_row a = mkuser("a@x.io", "A", 1), b = mkuser("b@x.io", "B", 1);
    CB_STORE_CALL(s, user_create, &a);
    CB_STORE_CALL(s, user_create, &b);
    cb_game_row g = mkgame("ud0001", 10);
    CB_STORE_CALL(s, game_create, &g, a.id);
    cb_refresh_row r = { .token_hash = "t1", .session_id = "s" };
    memcpy(r.user_id, a.id, sizeof(cb_uuid));
    CB_STORE_CALL(s, refresh_put, &r);
    cb_chat_row ch = { .message = "bye", .created_at = 5 };
    cb_uuid_v4(ch.id);
    snprintf(ch.game_id, sizeof ch.game_id, "ud0001");
    memcpy(ch.user_id, a.id, sizeof(cb_uuid));
    CB_STORE_CALL(s, chat_put, &ch);
    uint8_t mv[1] = { 1 };
    cb_snapshot_row sn = { .n_players = 2, .moves = mv, .moves_len = 1, .created_at = 9 };
    cb_uuid_v4(sn.id);
    memcpy(sn.player_ids[0], a.id, sizeof(cb_uuid));
    memcpy(sn.player_ids[1], b.id, sizeof(cb_uuid));
    CB_STORE_CALL(s, snapshot_put, &sn);

    T_EQ_INT(CB_STORE_CALL(s, user_delete, a.id), CB_OK);
    T_EQ_INT(CB_STORE_CALL(s, user_delete, a.id), CB_E_NOT_FOUND);
    cb_refresh_row got;
    T_EQ_INT(CB_STORE_CALL(s, refresh_take, "t1", 1, &got), CB_E_NOT_FOUND);
    T_EQ_INT(CB_STORE_CALL(s, is_member, "ud0001", a.id), 0);
    T_EQ_INT(rating_of(s, a.id), TABLE_RATING_NONE);
    names_t n = { 0 };
    T_EQ_INT(CB_STORE_CALL(s, chat_list, "ud0001", 0, collect_chat, &n), 0);
    cb_snapshot_query q = { .viewer_id = b.id };
    T_EQ_INT(CB_STORE_CALL(s, snapshots_query, &q, collect_snap, &n), 1);
    cb_store_close(s);
}

// ---- the lease -----------------------------------------------------------------------

static void lease_semantics(void) {
    cb_store *s = open_fresh();
    cb_user_row a = mkuser("a@x.io", "A", 1);
    CB_STORE_CALL(s, user_create, &a);
    cb_game_row g = mkgame("lease1", 10);
    CB_STORE_CALL(s, game_create, &g, a.id);
    const int64_t T0 = 1000000, TTL = 25000;
    cb_uuid t1, t2;
    T_EQ_INT(CB_STORE_CALL(s, lease_acquire, "lease1", T0, TTL, t1), CB_OK);
    T_CHECK(cb_uuid_valid(t1, strlen(t1)));
    // A second acquire while the lease is live fails, up to and including its last ms.
    T_EQ_INT(CB_STORE_CALL(s, lease_acquire, "lease1", T0 + 1, TTL, t2), CB_E_CONFLICT);
    T_EQ_INT(CB_STORE_CALL(s, lease_acquire, "lease1", T0 + TTL, TTL, t2), CB_E_CONFLICT);
    // Renew needs the holder's token.
    T_EQ_INT(CB_STORE_CALL(s, lease_renew, "lease1", "00000000-0000-4000-8000-000000000000", T0 + 10, TTL), CB_E_CONFLICT);
    T_EQ_INT(CB_STORE_CALL(s, lease_renew, "lease1", t1, T0 + 10000, TTL), CB_OK);
    // The renewal moved the expiry: T0 + TTL + 1 is inside it now.
    T_EQ_INT(CB_STORE_CALL(s, lease_acquire, "lease1", T0 + TTL + 1, TTL, t2), CB_E_CONFLICT);
    // A release by someone else's token does nothing.
    T_EQ_INT(CB_STORE_CALL(s, lease_release, "lease1", "00000000-0000-4000-8000-000000000000"), CB_OK);
    T_EQ_INT(CB_STORE_CALL(s, lease_acquire, "lease1", T0 + 10001, TTL, t2), CB_E_CONFLICT);
    // Released by its holder, it is free at once.
    T_EQ_INT(CB_STORE_CALL(s, lease_release, "lease1", t1), CB_OK);
    T_EQ_INT(CB_STORE_CALL(s, lease_acquire, "lease1", T0 + 10002, TTL, t2), CB_OK);
    T_CHECK(strcmp(t1, t2) != 0);
    T_EQ_INT(CB_STORE_CALL(s, lease_renew, "lease1", t1, T0 + 10003, TTL), CB_E_CONFLICT);
    // TTL expiry: a holder that died is replaced once the lease has run out.
    cb_uuid t3;
    T_EQ_INT(CB_STORE_CALL(s, lease_acquire, "lease1", T0 + 10002 + TTL + 1, TTL, t3), CB_OK);
    T_EQ_INT(CB_STORE_CALL(s, lease_renew, "lease1", t2, T0 + 10002 + TTL + 2, TTL), CB_E_CONFLICT);
    T_EQ_INT(CB_STORE_CALL(s, lease_renew, "lease1", t3, T0 + 10002 + TTL + 2, TTL), CB_OK);
    T_EQ_INT(CB_STORE_CALL(s, lease_acquire, "nogame", T0, TTL, t2), CB_E_NOT_FOUND);

    // The lease moves updated_at (the scan sees a drive in flight), never last_commit_at.
    cb_game_row got;
    T_EQ_INT(CB_STORE_CALL(s, game_get, "lease1", &got), CB_OK);
    T_EQ_INT(got.updated_at, T0 + 10002 + TTL + 2);
    T_EQ_INT(got.last_commit_at, 10);
    T_EQ_INT(got.version, 0);
    cb_game_row_free(&got);
    cb_store_close(s);
}

// ---- the heartbeat scan --------------------------------------------------------------

static void games_needing_bots_windows(void) {
    cb_store *s = open_fresh();
    cb_user_row a = mkuser("a@x.io", "A", 1);
    CB_STORE_CALL(s, user_create, &a);
    // now = 100000: stale_before = now - 10 s, active_after = now - 1 h.
    const int64_t NOW = 10000000, STALE = NOW - 10000, ACTIVE = NOW - 3600000;
    struct { const char *id; bool needs; int64_t updated, last_commit; } rows[] = {
        { "due001", true, NOW - 20000, NOW - 20000 },     // stalled: due
        { "due002", true, NOW - 3000000, NOW - 3000000 }, // stalled 50 min: due
        { "fresh1", true, NOW - 5000, NOW - 5000 },       // touched 5 s ago: a drive is on it
        { "edge01", true, STALE, STALE },                 // exactly 10 s: not yet (strict)
        { "aband1", true, NOW - 4000000, NOW - 4000000 }, // no move in over an hour: abandoned
        { "edge02", true, ACTIVE, ACTIVE },               // exactly an hour: abandoned (strict)
        { "nobot1", false, NOW - 20000, NOW - 20000 },    // nothing for bots to do
        // The lease kept updated_at fresh although the last move is old: not stale.
        { "lease1", true, NOW - 1000, NOW - 30000 },
    };
    for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        cb_game_row g = mkgame(rows[i].id, rows[i].last_commit);
        g.needs_bots = rows[i].needs;
        cb_uuid none[1];
        T_EQ_INT(CB_STORE_CALL(s, game_import, &g, none, 0, none, 0), CB_OK);
    }
    // lease1: its last commit is old, but a drive renewed the lease a second ago.
    cb_uuid tok;
    T_EQ_INT(CB_STORE_CALL(s, lease_acquire, "lease1", NOW - 1000, 25000, tok), CB_OK);
    names_t n = { 0 };
    T_EQ_INT(CB_STORE_CALL(s, games_needing_bots, STALE, ACTIVE, 100, collect_name, &n), 2);
    T_EQ_STR(n.ids[0], "due002");   // oldest first
    T_EQ_STR(n.ids[1], "due001");
    n.n = 0;
    T_EQ_INT(CB_STORE_CALL(s, games_needing_bots, STALE, ACTIVE, 1, collect_name, &n), 1);
    cb_store_close(s);
}

// ---- ratings ---------------------------------------------------------------------------

static void ratings_applied_by_commit(void) {
    cb_store *s = open_fresh();
    cb_user_row a = mkuser("a@x.io", "A", 1), b = mkuser("b@x.io", "B", 1);
    CB_STORE_CALL(s, user_create, &a);
    CB_STORE_CALL(s, user_import, &b);   // an account with no rating row yet
    T_EQ_INT(rating_of(s, b.id), TABLE_RATING_NONE);
    cb_game_row g = mkgame("rate01", 10);
    CB_STORE_CALL(s, game_create, &g, a.id);
    const char *bot = "2c601cde-63d2-5291-8fcb-735f5860b1ec", *gone = "00000000-0000-4000-8000-0000000000aa";
    cb_game_row n = mkgame("rate01", 10);
    n.version = 1;
    n.updated_at = 77;
    cb_game_commit c = { .row = &n, .expected_version = 0, .n_humans = 2, .n_rated = 4 };
    memcpy(c.humans[0], a.id, sizeof(cb_uuid));
    memcpy(c.humans[1], b.id, sizeof(cb_uuid));
    memcpy(c.rated_ids[0], a.id, sizeof(cb_uuid));
    c.rating_delta[0] = -5000;   // clamped at 0
    memcpy(c.rated_ids[1], b.id, sizeof(cb_uuid));
    c.rating_delta[1] = 12;      // a new row from the start rating
    snprintf(c.rated_names[1], sizeof c.rated_names[1], "Bee");
    snprintf(c.rated_ids[2], sizeof(cb_uuid), "%s", bot);
    c.rated_is_bot[2] = true;
    c.rating_delta[2] = -8;
    snprintf(c.rated_ids[3], sizeof(cb_uuid), "%s", gone);   // a deleted account: skipped
    c.rating_delta[3] = 3;
    T_EQ_INT(CB_STORE_CALL(s, game_commit, &c), CB_OK);

    T_EQ_INT(rating_of(s, a.id), 0);
    T_EQ_INT(rating_of(s, b.id), 1012);
    T_EQ_INT(rating_of(s, bot), 992);
    T_EQ_INT(rating_of(s, gone), TABLE_RATING_NONE);
    ratings_t r = { 0 };
    T_EQ_INT(CB_STORE_CALL(s, ratings_list, collect_rating, &r), 2);
    T_EQ_STR(r.rows[0].user_id, b.id);   // highest first
    T_EQ_STR(r.rows[0].username, "Bee");
    T_EQ_INT(r.rows[0].previous_elo, 1000);
    T_EQ_INT(r.rows[0].games_played, 1);
    T_EQ_INT(r.rows[0].updated_at, 77);
    T_EQ_INT(r.rows[1].elo_rating, 0);
    T_EQ_INT(r.rows[1].previous_elo, 1000);
    bots_t bl = { 0 };
    CB_STORE_CALL(s, bots_list, collect_bot, &bl);
    T_EQ_STR(bl.rows[0].id, bot);
    T_EQ_INT(bl.rows[0].previous_elo, 1000);
    T_EQ_INT(bl.rows[0].games_played, 1);

    // A second rated commit adds to the row as it stands now.
    n.version = 2;
    c.expected_version = 1;
    c.n_rated = 1;
    memcpy(c.rated_ids[0], b.id, sizeof(cb_uuid));
    c.rating_delta[0] = -2;
    T_EQ_INT(CB_STORE_CALL(s, game_commit, &c), CB_OK);
    T_EQ_INT(rating_of(s, b.id), 1010);
    r.n = 0;
    CB_STORE_CALL(s, ratings_list, collect_rating, &r);
    T_EQ_INT(r.rows[0].previous_elo, 1012);
    T_EQ_INT(r.rows[0].games_played, 2);
    cb_store_close(s);
}

// ---- snapshots, chat, connections --------------------------------------------------------

static void snapshots_query_shapes(void) {
    cb_store *s = open_fresh();
    cb_user_row a = mkuser("a@x.io", "A", 1), b = mkuser("b@x.io", "B", 1);
    CB_STORE_CALL(s, user_create, &a);
    CB_STORE_CALL(s, user_create, &b);
    cb_game_row g = mkgame("snap01", 10);
    CB_STORE_CALL(s, game_create, &g, a.id);
    uint8_t mv[3] = { 7, 8, 9 }, ex[2] = { 1, 1 };
    const char *ids[3] = { "00000000-0000-4000-8000-000000000001", "00000000-0000-4000-8000-000000000002",
                           "00000000-0000-4000-8000-000000000003" };
    for (int i = 0; i < 3; i++) {
        cb_snapshot_row sn = { .n_players = i == 2 ? 1 : 2, .moves = mv, .moves_len = 3, .created_at = 100 + i };
        snprintf(sn.id, sizeof sn.id, "%s", ids[i]);
        if (i == 0) {
            snprintf(sn.game_id, sizeof sn.game_id, "snap01");
            sn.extras = ex;
            sn.extras_len = 2;
        }
        memcpy(sn.player_ids[0], i == 2 ? b.id : a.id, sizeof(cb_uuid));
        memcpy(sn.player_ids[1], b.id, sizeof(cb_uuid));
        T_EQ_INT(CB_STORE_CALL(s, snapshot_put, &sn), CB_OK);
    }
    names_t n = { 0 };
    cb_snapshot_query q = { .viewer_id = a.id, .newest_first = true };
    T_EQ_INT(CB_STORE_CALL(s, snapshots_query, &q, collect_snap, &n), 2);
    T_EQ_STR(n.ids[0], ids[1]);
    T_EQ_STR(n.ids[1], ids[0]);
    n.n = 0;
    q.viewer_id = b.id;
    q.newest_first = false;
    q.limit = 2;
    T_EQ_INT(CB_STORE_CALL(s, snapshots_query, &q, collect_snap, &n), 2);
    T_EQ_STR(n.ids[0], ids[0]);
    n.n = 0;
    q.limit = 0;
    q.game_id = "snap01";
    T_EQ_INT(CB_STORE_CALL(s, snapshots_query, &q, collect_snap, &n), 1);
    n.n = 0;
    q.game_id = NULL;
    q.id = ids[2];
    T_EQ_INT(CB_STORE_CALL(s, snapshots_query, &q, collect_snap, &n), 1);
    // Not a player of it: not visible.
    n.n = 0;
    q.viewer_id = a.id;
    T_EQ_INT(CB_STORE_CALL(s, snapshots_query, &q, collect_snap, &n), 0);
    q.viewer_id = NULL;
    T_EQ_INT(CB_STORE_CALL(s, snapshots_query, &q, collect_snap, &n), CB_E_INVALID);
    T_EQ_INT(CB_STORE_CALL(s, snapshot_put, &(cb_snapshot_row){ .id = "00000000-0000-4000-8000-000000000001",
                                                                .moves = mv, .moves_len = 3 }),
             CB_E_CONFLICT);
    cb_store_close(s);
}

typedef struct { cb_snapshot_row row; } snapcopy;
static bool copy_snap(const cb_snapshot_row *r, void *ud) {
    snapcopy *c = ud;
    c->row = *r;
    c->row.moves = malloc(r->moves_len);
    memcpy(c->row.moves, r->moves, r->moves_len);
    c->row.extras = r->extras_len ? malloc(r->extras_len) : NULL;
    if (r->extras_len) memcpy(c->row.extras, r->extras, r->extras_len);
    return true;
}

static void snapshot_round_trip(void) {
    cb_store *s = open_fresh();
    uint8_t mv[4] = { 1, 2, 3, 4 }, ex[3] = { 5, 6, 7 };
    cb_snapshot_row sn = { .n_players = 3, .moves = mv, .moves_len = 4, .extras = ex, .extras_len = 3, .created_at = 42 };
    cb_uuid_v4(sn.id);
    for (int i = 0; i < 3; i++) cb_uuid_v4(sn.player_ids[i]);
    T_EQ_INT(CB_STORE_CALL(s, snapshot_put, &sn), CB_OK);
    snapcopy c = { 0 };
    cb_snapshot_query q = { .viewer_id = sn.player_ids[2] };
    T_EQ_INT(CB_STORE_CALL(s, snapshots_query, &q, copy_snap, &c), 1);
    T_EQ_STR(c.row.id, sn.id);
    T_EQ_STR(c.row.game_id, "");
    T_EQ_INT(c.row.n_players, 3);
    for (int i = 0; i < 3; i++) T_EQ_STR(c.row.player_ids[i], sn.player_ids[i]);
    T_EQ_MEM(c.row.moves, c.row.moves_len, mv, 4);
    T_EQ_MEM(c.row.extras, c.row.extras_len, ex, 3);
    T_EQ_INT(c.row.created_at, 42);
    cb_snapshot_row_free(&c.row);
    cb_store_close(s);
}

// Account deletion's rewrite: the extras change and nothing else does; an
// unknown replay is NOT_FOUND; zero bytes clear them.
static void snapshot_set_extras_rewrites_only_extras(void) {
    cb_store *s = open_fresh();
    uint8_t mv[4] = { 1, 2, 3, 4 }, ex[3] = { 5, 6, 7 }, ex2[5] = { 9, 8, 7, 6, 5 };
    cb_snapshot_row sn = { .n_players = 2, .moves = mv, .moves_len = 4, .extras = ex, .extras_len = 3, .created_at = 42 };
    cb_snapshot_row other = sn;
    cb_uuid_v4(sn.id);
    cb_uuid_v4(other.id);
    for (int i = 0; i < 2; i++) cb_uuid_v4(sn.player_ids[i]);
    memcpy(other.player_ids, sn.player_ids, sizeof sn.player_ids);
    T_EQ_INT(CB_STORE_CALL(s, snapshot_put, &sn), CB_OK);
    T_EQ_INT(CB_STORE_CALL(s, snapshot_put, &other), CB_OK);
    T_EQ_INT(CB_STORE_CALL(s, snapshot_set_extras, sn.id, ex2, sizeof ex2), CB_OK);
    snapcopy c = { 0 };
    cb_snapshot_query q = { .viewer_id = sn.player_ids[1], .id = sn.id };
    T_EQ_INT(CB_STORE_CALL(s, snapshots_query, &q, copy_snap, &c), 1);
    T_EQ_MEM(c.row.extras, c.row.extras_len, ex2, 5);
    T_EQ_MEM(c.row.moves, c.row.moves_len, mv, 4);
    T_EQ_INT(c.row.created_at, 42);
    T_EQ_STR(c.row.player_ids[0], sn.player_ids[0]);
    cb_snapshot_row_free(&c.row);
    snapcopy o = { 0 };
    cb_snapshot_query qo = { .viewer_id = other.player_ids[0], .id = other.id };
    T_EQ_INT(CB_STORE_CALL(s, snapshots_query, &qo, copy_snap, &o), 1);
    T_EQ_MEM(o.row.extras, o.row.extras_len, ex, 3);   // another replay is untouched
    cb_snapshot_row_free(&o.row);
    T_EQ_INT(CB_STORE_CALL(s, snapshot_set_extras, "00000000-0000-4000-8000-00000000dead", ex2, 5), CB_E_NOT_FOUND);
    T_EQ_INT(CB_STORE_CALL(s, snapshot_set_extras, sn.id, NULL, 0), CB_OK);
    memset(&c, 0, sizeof c);
    T_EQ_INT(CB_STORE_CALL(s, snapshots_query, &q, copy_snap, &c), 1);
    T_EQ_INT(c.row.extras_len, 0);
    cb_snapshot_row_free(&c.row);
    cb_store_close(s);
}

static void chat_newest_oldest_first(void) {
    cb_store *s = open_fresh();
    cb_user_row a = mkuser("a@x.io", "A", 1);
    CB_STORE_CALL(s, user_create, &a);
    cb_game_row g = mkgame("chat01", 10);
    CB_STORE_CALL(s, game_create, &g, a.id);
    const char *msgs[4] = { "one", "two", "three", "four" };
    for (int i = 0; i < 4; i++) {
        cb_chat_row c = { .message = (char *)msgs[i], .created_at = 100 + i, .is_system = i == 0 };
        cb_uuid_v4(c.id);
        snprintf(c.game_id, sizeof c.game_id, "chat01");
        memcpy(c.user_id, a.id, sizeof(cb_uuid));
        T_EQ_INT(CB_STORE_CALL(s, chat_put, &c), CB_OK);
    }
    names_t n = { 0 };
    T_EQ_INT(CB_STORE_CALL(s, chat_list, "chat01", 2, collect_chat, &n), 2);
    T_EQ_STR(n.ids[0], "three");
    T_EQ_STR(n.ids[1], "four");
    n.n = 0;
    T_EQ_INT(CB_STORE_CALL(s, chat_list, "chat01", 0, collect_chat, &n), 4);
    T_EQ_STR(n.ids[0], "one");
    // A message for a game that does not exist is refused.
    cb_chat_row bad = { .message = "x", .created_at = 1 };
    cb_uuid_v4(bad.id);
    snprintf(bad.game_id, sizeof bad.game_id, "nogame");
    memcpy(bad.user_id, a.id, sizeof(cb_uuid));
    T_EQ_INT(CB_STORE_CALL(s, chat_put, &bad), CB_E_NOT_FOUND);
    cb_store_close(s);
}

static void conns_round_trip(void) {
    cb_store *s = open_fresh();
    int64_t later = cb_now_ms() + 3600000;
    cb_conn_row c = { .conn_id = "Abc=", .vsn = 2, .n_topics = 2, .expires_at = later };
    snprintf(c.user_id, sizeof c.user_id, "00000000-0000-4000-8000-000000000001");
    snprintf(c.topics[0], sizeof c.topics[0], "game-ab12cd");
    snprintf(c.join_refs[0], sizeof c.join_refs[0], "1");
    snprintf(c.topics[1], sizeof c.topics[1], "pv-u1");
    snprintf(c.join_refs[1], sizeof c.join_refs[1], "2");
    T_EQ_INT(CB_STORE_CALL(s, conn_put, &c), CB_OK);
    cb_conn_row d = { .conn_id = "Def=", .vsn = 1, .n_topics = 1, .expires_at = later };
    snprintf(d.topics[0], sizeof d.topics[0], "game-ab12cd");
    snprintf(d.join_refs[0], sizeof d.join_refs[0], "9");
    T_EQ_INT(CB_STORE_CALL(s, conn_put, &d), CB_OK);
    cb_conn_row got;
    T_EQ_INT(CB_STORE_CALL(s, conn_get, "Abc=", &got), CB_OK);
    T_EQ_INT(got.vsn, 2);
    T_EQ_STR(got.user_id, c.user_id);
    T_EQ_INT(got.n_topics, 2);
    T_EQ_STR(got.topics[1], "pv-u1");
    T_EQ_STR(got.join_refs[1], "2");
    T_EQ_INT(got.expires_at, later);
    names_t n = { 0 };
    T_EQ_INT(CB_STORE_CALL(s, conns_by_topic, "game-ab12cd", collect_conn, &n), 2);
    T_EQ_STR(n.ids[0], "Abc=");
    // Leaving a topic: the row is put back without it.
    c.n_topics = 1;
    snprintf(c.topics[0], sizeof c.topics[0], "pv-u1");
    T_EQ_INT(CB_STORE_CALL(s, conn_put, &c), CB_OK);
    n.n = 0;
    T_EQ_INT(CB_STORE_CALL(s, conns_by_topic, "game-ab12cd", collect_conn, &n), 1);
    T_EQ_STR(n.ids[0], "Def=");
    // TTL: an expired connection reads as gone.
    d.expires_at = cb_now_ms() - 1;
    T_EQ_INT(CB_STORE_CALL(s, conn_put, &d), CB_OK);
    T_EQ_INT(CB_STORE_CALL(s, conn_get, "Def=", &got), CB_E_NOT_FOUND);
    n.n = 0;
    T_EQ_INT(CB_STORE_CALL(s, conns_by_topic, "game-ab12cd", collect_conn, &n), 0);
    T_EQ_INT(CB_STORE_CALL(s, conn_delete, "Abc="), CB_OK);
    T_EQ_INT(CB_STORE_CALL(s, conn_get, "Abc=", &got), CB_E_NOT_FOUND);
    cb_store_close(s);
}

// ---- admin -----------------------------------------------------------------------------

typedef struct { int rows; char first[8][64]; char col0[32]; int ncols; } rows_t;
static bool collect_row(int n, const char *const *names, const char *const *values, void *ud) {
    rows_t *r = ud;
    if (r->rows < 8) snprintf(r->first[r->rows], 64, "%s", values[0] ? values[0] : "(null)");
    snprintf(r->col0, sizeof r->col0, "%s", names[0]);
    r->ncols = n;
    r->rows++;
    return true;
}

static void admin_reads(void) {
    cb_store *s = open_fresh();
    names_t n = { 0 };
    T_CHECK(CB_STORE_CALL(s, admin_collections, collect_name, &n) >= 10);
    bool has_games = false;
    for (int i = 0; i < n.n; i++) has_games |= strcmp(n.ids[i], "games") == 0;
    T_CHECK(has_games);

    // Pages of 20 over the 49 bots: 20, 20, 9, and the cursor runs out.
    rows_t r = { 0 };
    char cur[64] = "", next[64];
    int pages = 0, total = 0;
    do {
        int k = CB_STORE_CALL(s, admin_browse, "bots", NULL, cur, 20, collect_row, &r, next, sizeof next);
        T_CHECK(k >= 0);
        total += k;
        pages++;
        snprintf(cur, sizeof cur, "%s", next);
    } while (cur[0] && pages < 10);
    T_EQ_INT(total, 49);
    T_EQ_INT(pages, 3);
    T_EQ_STR(r.col0, "id");
    T_EQ_INT(r.ncols, 8);
    // The eq filter, and a key prefix.
    r.rows = 0;
    T_EQ_INT(CB_STORE_CALL(s, admin_browse, "bots", "strategy_key=cordite", "", 100, collect_row, &r, next, sizeof next), 7);
    T_EQ_STR(next, "");
    r.rows = 0;
    T_EQ_INT(CB_STORE_CALL(s, admin_browse, "bots", "2c601cde", "", 100, collect_row, &r, next, sizeof next), 1);
    T_EQ_STR(r.first[0], "2c601cde-63d2-5291-8fcb-735f5860b1ec");
    T_EQ_INT(CB_STORE_CALL(s, admin_browse, "bots; DROP TABLE bots", NULL, "", 10, collect_row, &r, next, sizeof next),
             CB_E_INVALID);
    T_EQ_INT(CB_STORE_CALL(s, admin_browse, "bots", "nosuch=1", "", 10, collect_row, &r, next, sizeof next), CB_E_INVALID);
    T_EQ_INT(CB_STORE_CALL(s, admin_browse, "nosuch", NULL, "", 10, collect_row, &r, next, sizeof next), CB_E_NOT_FOUND);

    // Query: reads always, writes only when allowed, one statement only.
    r.rows = 0;
    T_EQ_INT(CB_STORE_CALL(s, admin_query, "SELECT count(*) AS n FROM bots", false, collect_row, &r), 1);
    T_EQ_STR(r.first[0], "49");
    T_EQ_STR(r.col0, "n");
    T_EQ_INT(CB_STORE_CALL(s, admin_query, "DELETE FROM bots", false, collect_row, &r), CB_E_FORBIDDEN);
    T_EQ_INT(CB_STORE_CALL(s, admin_query, "  -- c\n DELETE FROM bots", false, collect_row, &r), CB_E_FORBIDDEN);
    T_EQ_INT(CB_STORE_CALL(s, admin_query, "ATTACH DATABASE ':memory:' AS x", false, collect_row, &r), CB_E_FORBIDDEN);
    T_EQ_INT(CB_STORE_CALL(s, admin_query, "SELECT 1; DELETE FROM bots", true, collect_row, &r), CB_E_INVALID);
    T_EQ_INT(CB_STORE_CALL(s, admin_query, "SELECT 1;", false, collect_row, &r), 1);
    T_EQ_INT(CB_STORE_CALL(s, admin_query, "SELEKT", false, collect_row, &r), CB_E_INVALID);
    T_EQ_INT(CB_STORE_CALL(s, admin_query, "DELETE FROM bots WHERE nickname = '%Miami 1'", true, collect_row, &r), 0);
    r.rows = 0;
    CB_STORE_CALL(s, admin_query, "SELECT count(*) FROM bots", false, collect_row, &r);
    T_EQ_STR(r.first[0], "48");
    // A blob reads as "\x" hex.
    cb_user_row a = mkuser("a@x.io", "A", 1);
    CB_STORE_CALL(s, user_create, &a);
    cb_game_row g = mkgame("adm001", 1);
    CB_STORE_CALL(s, game_create, &g, a.id);
    r.rows = 0;
    CB_STORE_CALL(s, admin_query, "SELECT state FROM games", false, collect_row, &r);
    T_EQ_STR(r.first[0], "\\x010203");
    // CB_ADMIN_GAMES: rows carry the game's id (the console's Games panel).
    r.rows = 0;
    T_EQ_INT(CB_STORE_CALL(s, admin_browse, CB_ADMIN_GAMES, NULL, "", 10, collect_row, &r, next, sizeof next), 1);
    T_EQ_STR(r.col0, "id");
    T_EQ_STR(r.first[0], "adm001");
    // The console's backup: VACUUM INTO is a write, so only when allowed.
    char vac[512], stmt[600];
    const char *tmp = getenv("TMPDIR");
    snprintf(vac, sizeof vac, "%s/cbase-vacuum-%d.sqlite", tmp && *tmp ? tmp : "/tmp", (int)getpid());
    unlink(vac);
    snprintf(stmt, sizeof stmt, "VACUUM INTO '%s'", vac);
    T_EQ_INT(CB_STORE_CALL(s, admin_query, stmt, false, NULL, NULL), CB_E_FORBIDDEN);
    T_CHECK(access(vac, F_OK) != 0);
    T_EQ_INT(CB_STORE_CALL(s, admin_query, stmt, true, NULL, NULL), 0);
    cb_store *copy = NULL;
    T_EQ_INT(cb_store_open_sqlite(vac, &copy), CB_OK);
    if (copy) {
        cb_game_row back;
        T_EQ_INT(CB_STORE_CALL(copy, game_get, "adm001", &back), CB_OK);
        cb_game_row_free(&back);
        cb_store_close(copy);
    }
    unlink(vac);
    cb_store_close(s);
}

// ---- import writes -------------------------------------------------------------------------

static void import_writes(void) {
    cb_store *s = open_fresh();
    cb_user_row a = mkuser("a@x.io", "A", 1);
    a.last_sign_in_at = 99;
    T_EQ_INT(CB_STORE_CALL(s, user_import, &a), CB_OK);
    T_EQ_INT(rating_of(s, a.id), TABLE_RATING_NONE);   // no side effects
    snprintf(a.username, sizeof a.username, "A2");
    T_EQ_INT(CB_STORE_CALL(s, user_import, &a), CB_OK);   // replace
    cb_user_row got;
    CB_STORE_CALL(s, user_get, a.id, &got);
    T_EQ_STR(got.username, "A2");
    T_EQ_INT(got.last_sign_in_at, 99);
    cb_rating_row r = { .elo_rating = 1234, .previous_elo = 1200, .games_played = 9, .updated_at = 5 };
    memcpy(r.user_id, a.id, sizeof(cb_uuid));
    snprintf(r.username, sizeof r.username, "A2");
    T_EQ_INT(CB_STORE_CALL(s, rating_import, &r), CB_OK);
    T_EQ_INT(rating_of(s, a.id), 1234);

    cb_bot_row bots[2] = { { .id = "11111111-1111-4111-8111-111111111111", .nickname = "%Seoul 1",
                             .strategy_key = "robusta", .elo_rating = 1100, .previous_elo = 1090, .games_played = 3 },
                           { .id = "22222222-2222-4222-8222-222222222222", .nickname = "%Miami 1",
                             .strategy_key = "random", .elo_rating = 900, .previous_elo = -1 } };
    T_EQ_INT(CB_STORE_CALL(s, bots_replace, bots, 2), CB_OK);
    bots_t bl = { 0 };
    T_EQ_INT(CB_STORE_CALL(s, bots_list, collect_bot, &bl), 2);
    T_EQ_STR(bl.rows[0].nickname, "%Miami 1");   // ladder order, not insertion order
    T_EQ_INT(bl.rows[0].previous_elo, -1);
    T_EQ_INT(bl.rows[1].previous_elo, 1090);

    cb_game_row g = mkgame("imp001", 50);
    g.version = 37;
    g.round_epoch = 30;
    g.updated_at = 60;
    g.last_commit_at = 55;
    cb_uuid humans[1], botids[1];
    memcpy(humans[0], a.id, sizeof(cb_uuid));
    snprintf(botids[0], sizeof botids[0], "%s", bots[0].id);
    T_EQ_INT(CB_STORE_CALL(s, game_import, &g, humans, 1, botids, 1), CB_OK);
    cb_game_row gg;
    T_EQ_INT(CB_STORE_CALL(s, game_get, "imp001", &gg), CB_OK);
    T_EQ_INT(gg.version, 37);
    T_EQ_INT(gg.round_epoch, 30);
    T_EQ_INT(gg.last_commit_at, 55);
    T_EQ_INT(gg.updated_at, 60);
    cb_game_row_free(&gg);
    T_EQ_INT(CB_STORE_CALL(s, is_member, "imp001", a.id), 1);
    // Imported again with no members: the old seats go.
    T_EQ_INT(CB_STORE_CALL(s, game_import, &g, humans, 0, botids, 0), CB_OK);
    T_EQ_INT(CB_STORE_CALL(s, is_member, "imp001", a.id), 0);
    cb_store_close(s);
}

// ---- concurrency: the CAS under threads ----------------------------------------------------

typedef struct { cb_store *s; int wins, losses; } racer;

static void *race(void *ud) {
    racer *r = ud;
    for (int k = 0; k < 50; k++) {
        cb_game_row g;
        if (CB_STORE_CALL(r->s, game_get, "race01", &g) != CB_OK) continue;
        int64_t v = g.version;
        g.version = v + 1;
        g.updated_at = v + 1;
        cb_game_commit c = { .row = &g, .expected_version = v };
        int rc = CB_STORE_CALL(r->s, game_commit, &c);
        if (rc == CB_OK) r->wins++;
        else if (rc == CB_E_CONFLICT) r->losses++;
        cb_game_row_free(&g);
    }
    return NULL;
}

// Four threads race read-modify-commit on one game: every win is one version,
// so the final version is exactly the number of wins (no lost update), and no
// commit fails for any reason but the fence.
static void cas_under_threads(void) {
    cb_store *s = open_fresh();
    cb_user_row a = mkuser("a@x.io", "A", 1);
    CB_STORE_CALL(s, user_create, &a);
    cb_game_row g = mkgame("race01", 1);
    CB_STORE_CALL(s, game_create, &g, a.id);
    racer r[4];
    pthread_t th[4];
    for (int i = 0; i < 4; i++) {
        r[i] = (racer){ .s = s };
        pthread_create(&th[i], NULL, race, &r[i]);
    }
    int wins = 0, total = 0;
    for (int i = 0; i < 4; i++) {
        pthread_join(th[i], NULL);
        wins += r[i].wins;
        total += r[i].wins + r[i].losses;
    }
    cb_game_row got;
    T_EQ_INT(CB_STORE_CALL(s, game_get, "race01", &got), CB_OK);
    T_EQ_INT(got.version, wins);
    T_EQ_INT(total, 200);
    T_CHECK(wins >= 50);
    cb_game_row_free(&got);
    cb_store_close(s);
}

int main(void) {
    snprintf(g_dir, sizeof g_dir, "/tmp/cbase-unit-store-XXXXXX");
    if (!mkdtemp(g_dir)) { perror("mkdtemp"); return 1; }
    T_RUN(open_applies_schema_and_seeds_bots);
    T_RUN(users_round_trip);
    T_RUN(refresh_tokens_rotate);
    T_RUN(game_round_trip);
    T_RUN(cas_second_writer_loses);
    T_RUN(membership_follows_commits);
    T_RUN(game_delete_cascades);
    T_RUN(user_delete_cascades);
    T_RUN(lease_semantics);
    T_RUN(games_needing_bots_windows);
    T_RUN(ratings_applied_by_commit);
    T_RUN(snapshots_query_shapes);
    T_RUN(snapshot_round_trip);
    T_RUN(snapshot_set_extras_rewrites_only_extras);
    T_RUN(chat_newest_oldest_first);
    T_RUN(conns_round_trip);
    T_RUN(admin_reads);
    T_RUN(import_writes);
    T_RUN(cas_under_threads);
    char cmd[300];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", g_dir);
    if (system(cmd) != 0) fprintf(stderr, "could not remove %s\n", g_dir);
    return T_DONE();
}
