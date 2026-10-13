// store_sqlite.c - the SQLite backend (store.h): `cbase serve`, the tests, the
// import rehearsal.
//
// One connection (SQLITE_OPEN_FULLMUTEX), WAL, synchronous=NORMAL,
// foreign_keys=ON, a busy timeout for a second process (cbase-import beside a
// running serve). sql/schema.sql (cbase_schema_sql) is applied in one
// transaction at every open, then PRAGMA user_version = CB_SCHEMA_VERSION; a
// database whose user_version is newer is refused.
//
// THREADS. serve calls the store from a pool, and one SQLite connection runs
// one transaction at a time, so every operation holds `mu` from its first
// statement to its last: a transaction can never pick up another thread's
// statements. List functions read their rows under the lock and call `fn`
// after releasing it, so a callback may call the store again.
//
// THE CAS. game_commit is one BEGIN IMMEDIATE transaction: UPDATE games ...
// WHERE id = ? AND version = ? (0 rows changed: rollback, CB_E_CONFLICT), then
// game_members / game_bots rewritten from the commit, then the ratings, exactly
// as commit_table did (seed.sql 611-753).
#include "store.h"
#include "store_priv.h"

#include "table.h"
#include "util/clock.h"
#include "util/hex.h"
#include "util/log.h"
#include "util/str.h"
#include "util/uuid.h"

#include <ctype.h>
#include <pthread.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MOD "store"

// ---- the statements ------------------------------------------------------------------

enum {
    ST_USER_GET, ST_USER_BY_EMAIL, ST_USER_INSERT, ST_RATING_FOR_NEW_USER, ST_USER_SET_PASSWORD, ST_USER_TOUCH,
    ST_USER_DELETE, ST_USERS_ALL,
    ST_REFRESH_PUT, ST_REFRESH_GET, ST_REFRESH_REVOKE, ST_REFRESH_REVOKE_SESSION,
    ST_GAME_GET, ST_GAME_INSERT, ST_GAME_UPDATE_CAS, ST_GAME_EXISTS, ST_GAME_DELETE_CAS,
    ST_MEMBER_INSERT, ST_MEMBERS_OF_GAME, ST_MEMBER_DELETE, ST_MEMBERS_CLEAR,
    ST_BOTSEAT_INSERT, ST_BOTSEATS_OF_GAME, ST_BOTSEAT_DELETE, ST_BOTSEATS_CLEAR,
    ST_GAMES_OF_USER, ST_IS_MEMBER, ST_GAMES_NEEDING_BOTS,
    ST_LEASE_ACQUIRE, ST_LEASE_RENEW, ST_LEASE_RELEASE,
    ST_RATING_INSERT_MISSING, ST_RATING_APPLY, ST_BOT_RATING_APPLY,
    ST_BOTS_ALL, ST_RATINGS_ALL, ST_BOT_ELO, ST_USER_ELO, ST_RATING_SET_USERNAME,
    ST_SNAPSHOT_PUT, ST_SNAPSHOTS_ASC, ST_SNAPSHOTS_DESC,
    ST_CHAT_PUT, ST_CHAT_LIST,
    ST_CONN_PRUNE, ST_CONN_UPSERT, ST_CONN_TOPICS_CLEAR, ST_CONN_TOPIC_INSERT, ST_CONN_GET, ST_CONN_TOPICS_OF,
    ST_CONN_DELETE, ST_CONNS_BY_TOPIC,
    ST_COLLECTIONS,
    ST_USER_IMPORT, ST_RATING_IMPORT, ST_BOTS_CLEAR, ST_BOT_INSERT, ST_GAME_IMPORT,
    ST__COUNT
};

#define USER_COLS "id, email, password_hash, username, created_at, last_sign_in_at"
#define GAME_COLS "id, status, state, roster, logs_packed, game_seed, needs_bots, version, round_epoch, " \
                  "created_at, updated_at, last_commit_at"
#define BOT_COLS "id, nickname, strategy_key, elo_rating, previous_elo, games_played, created_at, updated_at"
#define SNAP_COLS "id, game_id, player_ids, moves, extras, created_at"

static const char *const SQL[ST__COUNT] = {
    [ST_USER_GET] = "SELECT " USER_COLS " FROM users WHERE id = ?1",
    [ST_USER_BY_EMAIL] = "SELECT " USER_COLS " FROM users WHERE email = ?1",
    [ST_USER_INSERT] = "INSERT INTO users (" USER_COLS ") VALUES (?1, ?2, ?3, ?4, ?5, ?6)",
    // The auth.users trigger's row (create_default_elo_rating): the start
    // rating, previous_elo the column default (also the start), the username.
    [ST_RATING_FOR_NEW_USER] =
        "INSERT INTO ratings (user_id, username, elo_rating, previous_elo, games_played, updated_at) "
        "VALUES (?1, ?2, ?3, ?3, 0, ?4) ON CONFLICT (user_id) DO UPDATE SET username = excluded.username",
    [ST_USER_SET_PASSWORD] = "UPDATE users SET password_hash = ?2 WHERE id = ?1",
    [ST_USER_TOUCH] = "UPDATE users SET last_sign_in_at = ?2 WHERE id = ?1",
    [ST_USER_DELETE] = "DELETE FROM users WHERE id = ?1",
    [ST_USERS_ALL] = "SELECT " USER_COLS " FROM users ORDER BY created_at DESC, id",

    [ST_REFRESH_PUT] = "INSERT INTO refresh_tokens (token_hash, user_id, session_id, created_at, revoked) "
                       "VALUES (?1, ?2, ?3, ?4, ?5)",
    [ST_REFRESH_GET] = "SELECT token_hash, user_id, session_id, created_at, revoked FROM refresh_tokens "
                       "WHERE token_hash = ?1",
    [ST_REFRESH_REVOKE] = "UPDATE refresh_tokens SET revoked = 1 WHERE token_hash = ?1",
    [ST_REFRESH_REVOKE_SESSION] = "UPDATE refresh_tokens SET revoked = 1 WHERE session_id = ?1",

    [ST_GAME_GET] = "SELECT " GAME_COLS " FROM games WHERE id = ?1",
    [ST_GAME_INSERT] = "INSERT INTO games (" GAME_COLS ") VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12)",
    [ST_GAME_UPDATE_CAS] =
        "UPDATE games SET status = ?2, state = ?3, roster = ?4, logs_packed = ?5, game_seed = ?6, needs_bots = ?7, "
        "version = ?8, round_epoch = ?9, updated_at = ?10, last_commit_at = ?11 WHERE id = ?1 AND version = ?12",
    [ST_GAME_EXISTS] = "SELECT 1 FROM games WHERE id = ?1",
    [ST_GAME_DELETE_CAS] = "DELETE FROM games WHERE id = ?1 AND version = ?2",

    [ST_MEMBER_INSERT] = "INSERT INTO game_members (game_id, user_id, joined_at) VALUES (?1, ?2, ?3) "
                         "ON CONFLICT (game_id, user_id) DO NOTHING",
    [ST_MEMBERS_OF_GAME] = "SELECT user_id FROM game_members WHERE game_id = ?1",
    [ST_MEMBER_DELETE] = "DELETE FROM game_members WHERE game_id = ?1 AND user_id = ?2",
    [ST_MEMBERS_CLEAR] = "DELETE FROM game_members WHERE game_id = ?1",
    [ST_BOTSEAT_INSERT] = "INSERT INTO game_bots (game_id, bot_id) VALUES (?1, ?2) ON CONFLICT (game_id, bot_id) DO NOTHING",
    [ST_BOTSEATS_OF_GAME] = "SELECT bot_id FROM game_bots WHERE game_id = ?1",
    [ST_BOTSEAT_DELETE] = "DELETE FROM game_bots WHERE game_id = ?1 AND bot_id = ?2",
    [ST_BOTSEATS_CLEAR] = "DELETE FROM game_bots WHERE game_id = ?1",

    [ST_GAMES_OF_USER] = "SELECT g.id, g.updated_at FROM game_members m JOIN games g ON g.id = m.game_id "
                         "WHERE m.user_id = ?1 ORDER BY g.updated_at DESC, g.id LIMIT ?2",
    [ST_IS_MEMBER] = "SELECT 1 FROM game_members WHERE game_id = ?1 AND user_id = ?2",
    [ST_GAMES_NEEDING_BOTS] = "SELECT id FROM games WHERE needs_bots = 1 AND updated_at < ?1 AND last_commit_at > ?2 "
                              "ORDER BY updated_at, id LIMIT ?3",

    // try_acquire_bot_lease / renew_bot_lease / release_bot_lease. The lease
    // writes move updated_at (the update_games_updated_at trigger did) so the
    // heartbeat's staleness window sees a drive in flight; they never move
    // last_commit_at (games_stamp_last_commit left it alone).
    [ST_LEASE_ACQUIRE] = "UPDATE games SET bot_lease_token = ?2, bot_lease_until = ?3 + ?4, updated_at = ?3 "
                         "WHERE id = ?1 AND (bot_lease_until IS NULL OR bot_lease_until < ?3)",
    [ST_LEASE_RENEW] = "UPDATE games SET bot_lease_until = ?3 + ?4, updated_at = ?3 "
                       "WHERE id = ?1 AND bot_lease_token = ?2",
    [ST_LEASE_RELEASE] = "UPDATE games SET bot_lease_until = NULL WHERE id = ?1 AND bot_lease_token = ?2",

    // commit_table's rating block (seed.sql 727-751).
    [ST_RATING_INSERT_MISSING] =
        "INSERT INTO ratings (user_id, username, elo_rating, previous_elo, games_played, updated_at) "
        "SELECT ?1, ?2, ?3, ?3, 0, ?4 WHERE EXISTS (SELECT 1 FROM users WHERE id = ?1) "
        "ON CONFLICT (user_id) DO NOTHING",
    [ST_RATING_APPLY] = "UPDATE ratings SET previous_elo = elo_rating, elo_rating = MAX(0, elo_rating + ?2), "
                        "games_played = games_played + 1, updated_at = ?3 WHERE user_id = ?1",
    [ST_BOT_RATING_APPLY] = "UPDATE bots SET previous_elo = elo_rating, elo_rating = MAX(0, elo_rating + ?2), "
                            "games_played = games_played + 1, updated_at = ?3 WHERE id = ?1",

    [ST_BOTS_ALL] = "SELECT " BOT_COLS " FROM bots",
    [ST_RATINGS_ALL] = "SELECT user_id, username, elo_rating, previous_elo, games_played, updated_at FROM ratings",
    [ST_BOT_ELO] = "SELECT elo_rating FROM bots WHERE id = ?1",
    [ST_USER_ELO] = "SELECT elo_rating FROM ratings WHERE user_id = ?1",
    [ST_RATING_SET_USERNAME] = "UPDATE ratings SET username = ?2 WHERE user_id = ?1",

    [ST_SNAPSHOT_PUT] = "INSERT INTO game_snapshots (" SNAP_COLS ") VALUES (?1, ?2, ?3, ?4, ?5, ?6)",
    [ST_SNAPSHOTS_ASC] = "SELECT " SNAP_COLS " FROM game_snapshots "
                         "WHERE instr(',' || player_ids || ',', ',' || ?1 || ',') > 0 "
                         "AND (?2 IS NULL OR game_id = ?2) AND (?3 IS NULL OR id = ?3) "
                         "ORDER BY created_at, id LIMIT ?4",
    [ST_SNAPSHOTS_DESC] = "SELECT " SNAP_COLS " FROM game_snapshots "
                          "WHERE instr(',' || player_ids || ',', ',' || ?1 || ',') > 0 "
                          "AND (?2 IS NULL OR game_id = ?2) AND (?3 IS NULL OR id = ?3) "
                          "ORDER BY created_at DESC, id DESC LIMIT ?4",

    [ST_CHAT_PUT] = "INSERT INTO chat_messages (id, game_id, user_id, message, is_system, created_at) "
                    "VALUES (?1, ?2, ?3, ?4, ?5, ?6)",
    [ST_CHAT_LIST] = "SELECT id, game_id, user_id, message, is_system, created_at FROM ("
                     "SELECT * FROM chat_messages WHERE game_id = ?1 ORDER BY created_at DESC, id DESC LIMIT ?2"
                     ") ORDER BY created_at, id",

    [ST_CONN_PRUNE] = "DELETE FROM rt_conns WHERE expires_at <= ?1",
    [ST_CONN_UPSERT] = "INSERT INTO rt_conns (conn_id, user_id, vsn, expires_at) VALUES (?1, ?2, ?3, ?4) "
                       "ON CONFLICT (conn_id) DO UPDATE SET user_id = excluded.user_id, vsn = excluded.vsn, "
                       "expires_at = excluded.expires_at",
    [ST_CONN_TOPICS_CLEAR] = "DELETE FROM rt_conn_topics WHERE conn_id = ?1",
    [ST_CONN_TOPIC_INSERT] = "INSERT INTO rt_conn_topics (conn_id, ord, topic, join_ref) VALUES (?1, ?2, ?3, ?4)",
    [ST_CONN_GET] = "SELECT conn_id, user_id, vsn, expires_at FROM rt_conns WHERE conn_id = ?1 AND expires_at > ?2",
    [ST_CONN_TOPICS_OF] = "SELECT topic, join_ref FROM rt_conn_topics WHERE conn_id = ?1 ORDER BY ord",
    [ST_CONN_DELETE] = "DELETE FROM rt_conns WHERE conn_id = ?1",
    [ST_CONNS_BY_TOPIC] = "SELECT DISTINCT c.conn_id FROM rt_conn_topics t JOIN rt_conns c ON c.conn_id = t.conn_id "
                          "WHERE t.topic = ?1 AND c.expires_at > ?2 ORDER BY c.conn_id",

    [ST_COLLECTIONS] = "SELECT name FROM sqlite_master WHERE type = 'table' AND name NOT LIKE 'sqlite_%' ORDER BY name",

    [ST_USER_IMPORT] = "INSERT INTO users (" USER_COLS ") VALUES (?1, ?2, ?3, ?4, ?5, ?6) "
                       "ON CONFLICT (id) DO UPDATE SET email = excluded.email, password_hash = excluded.password_hash, "
                       "username = excluded.username, created_at = excluded.created_at, "
                       "last_sign_in_at = excluded.last_sign_in_at",
    [ST_RATING_IMPORT] = "INSERT INTO ratings (user_id, username, elo_rating, previous_elo, games_played, updated_at) "
                         "VALUES (?1, ?2, ?3, ?4, ?5, ?6) ON CONFLICT (user_id) DO UPDATE SET "
                         "username = excluded.username, elo_rating = excluded.elo_rating, "
                         "previous_elo = excluded.previous_elo, games_played = excluded.games_played, "
                         "updated_at = excluded.updated_at",
    [ST_BOTS_CLEAR] = "DELETE FROM bots",
    [ST_BOT_INSERT] = "INSERT INTO bots (" BOT_COLS ") VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)",
    [ST_GAME_IMPORT] = "INSERT INTO games (" GAME_COLS ") VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12) "
                       "ON CONFLICT (id) DO UPDATE SET status = excluded.status, state = excluded.state, "
                       "roster = excluded.roster, logs_packed = excluded.logs_packed, game_seed = excluded.game_seed, "
                       "needs_bots = excluded.needs_bots, version = excluded.version, "
                       "round_epoch = excluded.round_epoch, created_at = excluded.created_at, "
                       "updated_at = excluded.updated_at, last_commit_at = excluded.last_commit_at, "
                       "bot_lease_token = NULL, bot_lease_until = NULL",
};

typedef struct {
    sqlite3        *db;
    pthread_mutex_t mu;
    sqlite3_stmt   *st[ST__COUNT];
} sq;

#define Q(s) ((sq *)(s)->impl)

// SQLite's answer as a store code. A unique or primary key is a conflict, a
// foreign key names a row that is not there; anything else is logged.
static int fail(sq *q, const char *what, int rc) {
    int ext = sqlite3_extended_errcode(q->db);
    if (ext == SQLITE_CONSTRAINT_UNIQUE || ext == SQLITE_CONSTRAINT_PRIMARYKEY) return CB_E_CONFLICT;
    if (ext == SQLITE_CONSTRAINT_FOREIGNKEY) return CB_E_NOT_FOUND;
    CB_ERROR(MOD, "%s: %s (%d)", what, sqlite3_errmsg(q->db), rc);
    return CB_E_DB;
}

// The statement, reset and unbound, prepared on first use. NULL after logging.
static sqlite3_stmt *stmt(sq *q, int id) {
    sqlite3_stmt *s = q->st[id];
    if (!s) {
        int rc = sqlite3_prepare_v3(q->db, SQL[id], -1, SQLITE_PREPARE_PERSISTENT, &q->st[id], NULL);
        if (rc != SQLITE_OK) {
            CB_ERROR(MOD, "prepare %d: %s", id, sqlite3_errmsg(q->db));
            return NULL;
        }
        return q->st[id];
    }
    sqlite3_reset(s);
    sqlite3_clear_bindings(s);
    return s;
}

static void done(sqlite3_stmt *s) {
    if (s) sqlite3_reset(s);
}

static int exec(sq *q, const char *sql) {
    char *err = NULL;
    int rc = sqlite3_exec(q->db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        CB_ERROR(MOD, "%.40s: %s", sql, err ? err : sqlite3_errstr(rc));
        sqlite3_free(err);
        return CB_E_DB;
    }
    return CB_OK;
}

static int begin(sq *q) { return exec(q, "BEGIN IMMEDIATE"); }
static int commit(sq *q) { return exec(q, "COMMIT"); }
static void rollback(sq *q) {
    if (!sqlite3_get_autocommit(q->db)) exec(q, "ROLLBACK");
}

static void lock(sq *q) { pthread_mutex_lock(&q->mu); }
static void unlock(sq *q) { pthread_mutex_unlock(&q->mu); }

// ---- binding and reading -------------------------------------------------------------

static void b_text(sqlite3_stmt *s, int i, const char *v) { sqlite3_bind_text(s, i, v, -1, SQLITE_TRANSIENT); }
// "" and NULL bind SQL NULL (the nullable text columns).
static void b_text_null(sqlite3_stmt *s, int i, const char *v) {
    if (v && *v) b_text(s, i, v);
    else sqlite3_bind_null(s, i);
}
static void b_i64(sqlite3_stmt *s, int i, int64_t v) { sqlite3_bind_int64(s, i, v); }
static void b_blob(sqlite3_stmt *s, int i, const uint8_t *p, size_t n) {
    if (p && n) sqlite3_bind_blob64(s, i, p, n, SQLITE_TRANSIENT);
    else sqlite3_bind_null(s, i);
}
// A NOT NULL blob column: zero bytes are an empty blob, not NULL.
static void b_blob_nn(sqlite3_stmt *s, int i, const uint8_t *p, size_t n) {
    sqlite3_bind_blob64(s, i, n ? (const void *)p : "", n, SQLITE_TRANSIENT);
}

static void c_text(sqlite3_stmt *s, int i, char *out, size_t cap) {
    const unsigned char *t = sqlite3_column_text(s, i);
    cb_strlcpy(out, t ? (const char *)t : "", cap);
}
static int64_t c_i64(sqlite3_stmt *s, int i) { return sqlite3_column_int64(s, i); }
static int32_t c_i32_or(sqlite3_stmt *s, int i, int32_t none) {
    return sqlite3_column_type(s, i) == SQLITE_NULL ? none : (int32_t)sqlite3_column_int64(s, i);
}
// A malloc'd copy of a blob column (NULL/0 for SQL NULL or empty). false: no memory.
static bool c_blob(sqlite3_stmt *s, int i, uint8_t **out, size_t *len) {
    int n = sqlite3_column_bytes(s, i);
    const void *p = sqlite3_column_blob(s, i);
    *out = NULL;
    *len = 0;
    if (!p || n <= 0) return true;
    *out = malloc((size_t)n);
    if (!*out) return false;
    memcpy(*out, p, (size_t)n);
    *len = (size_t)n;
    return true;
}

static void read_user(sqlite3_stmt *s, cb_user_row *u) {
    memset(u, 0, sizeof *u);
    c_text(s, 0, u->id, sizeof u->id);
    c_text(s, 1, u->email, sizeof u->email);
    c_text(s, 2, u->password_hash, sizeof u->password_hash);
    c_text(s, 3, u->username, sizeof u->username);
    u->created_at = c_i64(s, 4);
    u->last_sign_in_at = c_i64(s, 5);
}

static void bind_user(sqlite3_stmt *s, const cb_user_row *u) {
    b_text(s, 1, u->id);
    b_text(s, 2, u->email);
    b_text(s, 3, u->password_hash);
    b_text_null(s, 4, u->username);
    b_i64(s, 5, u->created_at);
    if (u->last_sign_in_at) b_i64(s, 6, u->last_sign_in_at);
    else sqlite3_bind_null(s, 6);
}

static bool read_game(sqlite3_stmt *s, cb_game_row *g) {
    memset(g, 0, sizeof *g);
    c_text(s, 0, g->id, sizeof g->id);
    g->status = (int)c_i64(s, 1);
    bool ok = c_blob(s, 2, &g->state, &g->state_len) && c_blob(s, 3, &g->roster, &g->roster_len) &&
              c_blob(s, 4, &g->logs, &g->logs_len);
    if (sqlite3_column_bytes(s, 5) == CB_SEED_LEN) {
        g->has_seed = true;
        memcpy(g->seed, sqlite3_column_blob(s, 5), CB_SEED_LEN);
    }
    g->needs_bots = c_i64(s, 6) != 0;
    g->version = c_i64(s, 7);
    g->round_epoch = c_i64(s, 8);
    g->created_at = c_i64(s, 9);
    g->updated_at = c_i64(s, 10);
    g->last_commit_at = c_i64(s, 11);
    if (!ok) cb_game_row_free(g);
    return ok;
}

// The twelve GAME_COLS, in order. last_commit_at is the row's.
static void bind_game(sqlite3_stmt *s, const cb_game_row *g) {
    b_text(s, 1, g->id);
    b_i64(s, 2, g->status);
    b_blob_nn(s, 3, g->state, g->state_len);
    b_blob_nn(s, 4, g->roster, g->roster_len);
    b_blob(s, 5, g->logs, g->logs_len);
    b_blob(s, 6, g->has_seed ? g->seed : NULL, g->has_seed ? CB_SEED_LEN : 0);
    b_i64(s, 7, g->needs_bots);
    b_i64(s, 8, g->version);
    b_i64(s, 9, g->round_epoch);
    b_i64(s, 10, g->created_at);
    b_i64(s, 11, g->updated_at);
    b_i64(s, 12, g->last_commit_at);
}

static void read_bot(sqlite3_stmt *s, cb_bot_row *b) {
    memset(b, 0, sizeof *b);
    c_text(s, 0, b->id, sizeof b->id);
    c_text(s, 1, b->nickname, sizeof b->nickname);
    c_text(s, 2, b->strategy_key, sizeof b->strategy_key);
    b->elo_rating = (int32_t)c_i64(s, 3);
    b->previous_elo = c_i32_or(s, 4, -1);
    b->games_played = (int32_t)c_i64(s, 5);
    b->created_at = c_i64(s, 6);
    b->updated_at = c_i64(s, 7);
}

static void read_rating(sqlite3_stmt *s, cb_rating_row *r) {
    memset(r, 0, sizeof *r);
    c_text(s, 0, r->user_id, sizeof r->user_id);
    c_text(s, 1, r->username, sizeof r->username);
    r->elo_rating = (int32_t)c_i64(s, 2);
    r->previous_elo = c_i32_or(s, 3, -1);
    r->games_played = (int32_t)c_i64(s, 4);
    r->updated_at = c_i64(s, 5);
}

// player_ids is "a,b,c": the seated humans in seat order.
static bool read_snapshot(sqlite3_stmt *s, cb_snapshot_row *r) {
    memset(r, 0, sizeof *r);
    c_text(s, 0, r->id, sizeof r->id);
    c_text(s, 1, r->game_id, sizeof r->game_id);
    const char *p = (const char *)sqlite3_column_text(s, 2);
    while (p && *p && r->n_players < CB_MAX_SEATS) {
        const char *e = strchr(p, ',');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        cb_strncpy0(r->player_ids[r->n_players++], sizeof r->player_ids[0], p, n);
        p = e ? e + 1 : NULL;
    }
    bool ok = c_blob(s, 3, &r->moves, &r->moves_len) && c_blob(s, 4, &r->extras, &r->extras_len);
    r->created_at = c_i64(s, 5);
    if (!ok) cb_snapshot_row_free(r);
    return ok;
}

// ---- growable row arrays (rows are read under the lock, handed out after) --------------

typedef struct {
    void  *p;
    int    n, cap;
    size_t size;
} vec;

static void *vec_push(vec *v) {
    if (v->n == v->cap) {
        int cap = v->cap ? v->cap * 2 : 16;
        void *p = realloc(v->p, (size_t)cap * v->size);
        if (!p) return NULL;
        v->p = p;
        v->cap = cap;
    }
    void *e = (char *)v->p + (size_t)v->n++ * v->size;
    memset(e, 0, v->size);
    return e;
}

// ---- open and close --------------------------------------------------------------------

static const cb_store_ops SQLITE_OPS;

static void sq_close(cb_store *s) {
    sq *q = Q(s);
    for (int i = 0; i < ST__COUNT; i++) sqlite3_finalize(q->st[i]);
    sqlite3_close(q->db);
    pthread_mutex_destroy(&q->mu);
    free(q);
    free(s);
}

static int pragma_int(sqlite3 *db, const char *sql, int64_t *out) {
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK) return CB_E_DB;
    int rc = sqlite3_step(s);
    if (rc == SQLITE_ROW) *out = sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);
    return rc == SQLITE_ROW ? CB_OK : CB_E_DB;
}

// The schema, applied in one transaction, then the version stamp. Steps for a
// future CB_SCHEMA_VERSION that are not idempotent go here, each guarded by
// the user_version it upgrades from.
static int apply_schema(sq *q) {
    int64_t have = 0;
    if (pragma_int(q->db, "PRAGMA user_version", &have) != CB_OK) return fail(q, "user_version", 0);
    if (have > CB_SCHEMA_VERSION) {
        CB_ERROR(MOD, "the database is at schema %lld, this binary knows %d: refusing to open it", (long long)have,
                 CB_SCHEMA_VERSION);
        return CB_E_STATE;
    }
    int rc = begin(q);
    if (rc != CB_OK) return rc;
    if ((rc = exec(q, cbase_schema_sql)) != CB_OK) goto out;
    char v[64];
    snprintf(v, sizeof v, "PRAGMA user_version = %d", CB_SCHEMA_VERSION);
    if ((rc = exec(q, v)) != CB_OK) goto out;
    rc = commit(q);
out:
    if (rc != CB_OK) rollback(q);
    return rc;
}

int cb_store_open_sqlite(const char *path, cb_store **out) {
    *out = NULL;
    cb_store *s = calloc(1, sizeof *s);
    sq *q = calloc(1, sizeof *q);
    if (!s || !q) {
        free(s);
        free(q);
        return CB_E_NOMEM;
    }
    pthread_mutex_init(&q->mu, NULL);
    s->ops = &SQLITE_OPS;
    s->impl = q;
    int rc = sqlite3_open_v2(path, &q->db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, NULL);
    if (rc != SQLITE_OK) {
        CB_ERROR(MOD, "open %s: %s", path, q->db ? sqlite3_errmsg(q->db) : sqlite3_errstr(rc));
        sq_close(s);
        return CB_E_DB;
    }
    sqlite3_extended_result_codes(q->db, 1);
    sqlite3_busy_timeout(q->db, 5000);
    // journal_mode answers a row; exec ignores it. ":memory:" stays "memory".
    if ((rc = exec(q, "PRAGMA journal_mode = WAL")) != CB_OK || (rc = exec(q, "PRAGMA synchronous = NORMAL")) != CB_OK ||
        (rc = exec(q, "PRAGMA foreign_keys = ON")) != CB_OK || (rc = apply_schema(q)) != CB_OK) {
        sq_close(s);
        return rc;
    }
    *out = s;
    return CB_OK;
}

// ---- users -----------------------------------------------------------------------------

static int get_user_by(sq *q, int st_id, const char *key, cb_user_row *out) {
    lock(q);
    sqlite3_stmt *s = stmt(q, st_id);
    int rc = CB_E_DB;
    if (s) {
        b_text(s, 1, key);
        int r = sqlite3_step(s);
        if (r == SQLITE_ROW) {
            read_user(s, out);
            rc = CB_OK;
        } else rc = r == SQLITE_DONE ? CB_E_NOT_FOUND : fail(q, "user get", r);
    }
    done(s);
    unlock(q);
    return rc;
}

static int sq_user_get(cb_store *st, const char *id, cb_user_row *out) { return get_user_by(Q(st), ST_USER_GET, id, out); }
static int sq_user_by_email(cb_store *st, const char *email, cb_user_row *out) {
    return get_user_by(Q(st), ST_USER_BY_EMAIL, email, out);
}

// One statement that changes rows; CB_OK, or CB_E_NOT_FOUND when it changed none
// and `need_row`. Called with the lock held.
static int step_write(sq *q, sqlite3_stmt *s, const char *what, bool need_row) {
    if (!s) return CB_E_DB;
    int r = sqlite3_step(s);
    int rc = r == SQLITE_DONE ? CB_OK : fail(q, what, r);
    if (rc == CB_OK && need_row && sqlite3_changes(q->db) == 0) rc = CB_E_NOT_FOUND;
    done(s);
    return rc;
}

static int sq_user_create(cb_store *st, const cb_user_row *u) {
    sq *q = Q(st);
    lock(q);
    int rc = begin(q);
    if (rc == CB_OK) {
        sqlite3_stmt *s = stmt(q, ST_USER_INSERT);
        if (s) bind_user(s, u);
        rc = step_write(q, s, "user create", false);
    }
    if (rc == CB_OK) {
        sqlite3_stmt *s = stmt(q, ST_RATING_FOR_NEW_USER);
        if (s) {
            b_text(s, 1, u->id);
            b_text_null(s, 2, u->username);
            b_i64(s, 3, TABLE_RATING_START);
            b_i64(s, 4, u->created_at);
        }
        rc = step_write(q, s, "user rating", false);
    }
    if (rc == CB_OK) rc = commit(q);
    if (rc != CB_OK) rollback(q);
    unlock(q);
    return rc;
}

static int write1(sq *q, int st_id, const char *what, bool need_row, const char *a, const char *b, const int64_t *i2) {
    lock(q);
    sqlite3_stmt *s = stmt(q, st_id);
    if (s) {
        b_text(s, 1, a);
        if (b) b_text_null(s, 2, b);
        if (i2) b_i64(s, 2, *i2);
    }
    int rc = step_write(q, s, what, need_row);
    unlock(q);
    return rc;
}

static int sq_user_set_password(cb_store *st, const char *id, const char *hash) {
    return write1(Q(st), ST_USER_SET_PASSWORD, "set password", true, id, hash, NULL);
}
static int sq_user_touch_sign_in(cb_store *st, const char *id, int64_t at) {
    return write1(Q(st), ST_USER_TOUCH, "touch sign-in", true, id, NULL, &at);
}
// The cascade does the rest: refresh_tokens, game_members, ratings and
// chat_messages reference users ON DELETE CASCADE.
static int sq_user_delete(cb_store *st, const char *id) {
    return write1(Q(st), ST_USER_DELETE, "user delete", true, id, NULL, NULL);
}

static int sq_users_list(cb_store *st, const char *search, int offset, int limit, cb_user_fn fn, void *ud) {
    sq *q = Q(st);
    vec v = { .size = sizeof(cb_user_row) };
    int rc = CB_OK, skip = offset > 0 ? offset : 0;
    lock(q);
    sqlite3_stmt *s = stmt(q, ST_USERS_ALL);
    if (!s) rc = CB_E_DB;
    int r = SQLITE_DONE;
    while (rc == CB_OK && (limit <= 0 || v.n < limit) && (r = sqlite3_step(s)) == SQLITE_ROW) {
        cb_user_row u;
        read_user(s, &u);
        if (!cb_store_user_matches(&u, search)) continue;
        if (skip > 0) { skip--; continue; }
        cb_user_row *e = vec_push(&v);
        if (!e) rc = CB_E_NOMEM;
        else *e = u;
    }
    if (rc == CB_OK && (limit <= 0 || v.n < limit) && r != SQLITE_DONE) rc = fail(q, "users list", r);
    done(s);
    unlock(q);
    int n = 0;
    for (int i = 0; rc == CB_OK && i < v.n; i++) {
        n++;
        if (!fn(&((cb_user_row *)v.p)[i], ud)) break;
    }
    free(v.p);
    return rc == CB_OK ? n : rc;
}

// ---- refresh tokens --------------------------------------------------------------------

static int sq_refresh_put(cb_store *st, const cb_refresh_row *r) {
    sq *q = Q(st);
    lock(q);
    sqlite3_stmt *s = stmt(q, ST_REFRESH_PUT);
    if (s) {
        b_text(s, 1, r->token_hash);
        b_text(s, 2, r->user_id);
        b_text(s, 3, r->session_id);
        b_i64(s, 4, r->created_at);
        b_i64(s, 5, r->revoked);
    }
    int rc = step_write(q, s, "refresh put", false);
    unlock(q);
    return rc;
}

static int sq_refresh_take(cb_store *st, const char *hash, cb_refresh_row *out) {
    sq *q = Q(st);
    lock(q);
    int rc = begin(q);
    if (rc == CB_OK) {
        sqlite3_stmt *s = stmt(q, ST_REFRESH_GET);
        if (!s) rc = CB_E_DB;
        else {
            b_text(s, 1, hash);
            int r = sqlite3_step(s);
            if (r == SQLITE_ROW) {
                memset(out, 0, sizeof *out);
                c_text(s, 0, out->token_hash, sizeof out->token_hash);
                c_text(s, 1, out->user_id, sizeof out->user_id);
                c_text(s, 2, out->session_id, sizeof out->session_id);
                out->created_at = c_i64(s, 3);
                out->revoked = c_i64(s, 4) != 0;
            } else rc = r == SQLITE_DONE ? CB_E_NOT_FOUND : fail(q, "refresh get", r);
            done(s);
        }
    }
    if (rc == CB_OK) {
        sqlite3_stmt *s = stmt(q, ST_REFRESH_REVOKE);
        if (s) b_text(s, 1, hash);
        rc = step_write(q, s, "refresh revoke", false);
    }
    if (rc == CB_OK) rc = commit(q);
    if (rc != CB_OK) rollback(q);
    unlock(q);
    return rc;
}

static int sq_refresh_revoke_session(cb_store *st, const char *session_id) {
    return write1(Q(st), ST_REFRESH_REVOKE_SESSION, "revoke session", false, session_id, NULL, NULL);
}

// ---- games -----------------------------------------------------------------------------

static int sq_game_get(cb_store *st, const char *id, cb_game_row *out) {
    sq *q = Q(st);
    lock(q);
    sqlite3_stmt *s = stmt(q, ST_GAME_GET);
    int rc = CB_E_DB;
    if (s) {
        b_text(s, 1, id);
        int r = sqlite3_step(s);
        if (r == SQLITE_ROW) rc = read_game(s, out) ? CB_OK : CB_E_NOMEM;
        else rc = r == SQLITE_DONE ? CB_E_NOT_FOUND : fail(q, "game get", r);
    }
    done(s);
    unlock(q);
    return rc;
}

static int member_insert(sq *q, const char *game, const char *user, int64_t at) {
    sqlite3_stmt *s = stmt(q, ST_MEMBER_INSERT);
    if (s) {
        b_text(s, 1, game);
        b_text(s, 2, user);
        b_i64(s, 3, at);
    }
    return step_write(q, s, "member insert", false);
}

static int sq_game_create(cb_store *st, const cb_game_row *g, const char *creator) {
    sq *q = Q(st);
    lock(q);
    int rc = begin(q);
    if (rc == CB_OK) {
        sqlite3_stmt *s = stmt(q, ST_GAME_INSERT);
        if (s) {
            bind_game(s, g);
            b_i64(s, 12, g->updated_at);   // a new row's last commit is its creation
        }
        rc = step_write(q, s, "game create", false);
    }
    if (rc == CB_OK) rc = member_insert(q, g->id, creator, g->created_at);
    if (rc == CB_OK) rc = commit(q);
    if (rc != CB_OK) rollback(q);
    unlock(q);
    return rc;
}

static bool listed(const cb_uuid *ids, int n, const char *id) {
    for (int i = 0; i < n; i++)
        if (strcmp(ids[i], id) == 0) return true;
    return false;
}

// Rewrites one membership table for `game` to exactly `ids` (commit_table's
// upsert + "DELETE ... <> ALL"). Lock held, inside a transaction.
static int sync_seats(sq *q, const char *game, const cb_uuid *ids, int n, bool bots, int64_t at) {
    cb_uuid have[64];
    int nh = 0, rc = CB_OK;
    sqlite3_stmt *s = stmt(q, bots ? ST_BOTSEATS_OF_GAME : ST_MEMBERS_OF_GAME);
    if (!s) return CB_E_DB;
    b_text(s, 1, game);
    int r = SQLITE_DONE;
    while ((r = sqlite3_step(s)) == SQLITE_ROW && nh < 64) c_text(s, 0, have[nh++], sizeof have[0]);
    if (r != SQLITE_ROW && r != SQLITE_DONE) rc = fail(q, "seats read", r);
    done(s);
    for (int i = 0; rc == CB_OK && i < nh; i++) {
        if (listed(ids, n, have[i])) continue;
        s = stmt(q, bots ? ST_BOTSEAT_DELETE : ST_MEMBER_DELETE);
        if (s) {
            b_text(s, 1, game);
            b_text(s, 2, have[i]);
        }
        rc = step_write(q, s, "seat delete", false);
    }
    for (int i = 0; rc == CB_OK && i < n; i++) {
        if (bots) {
            s = stmt(q, ST_BOTSEAT_INSERT);
            if (s) {
                b_text(s, 1, game);
                b_text(s, 2, ids[i]);
            }
            rc = step_write(q, s, "bot seat insert", false);
        } else rc = member_insert(q, game, ids[i], at);
    }
    return rc;
}

// commit_table's rating block: a human with no row gets one if the account
// exists, then every rated seat's change is ADDED to its row as it stands.
static int apply_ratings(sq *q, const cb_game_commit *c, int64_t at) {
    int rc = CB_OK;
    for (int i = 0; rc == CB_OK && i < c->n_rated; i++) {
        sqlite3_stmt *s;
        if (!c->rated_is_bot[i]) {
            s = stmt(q, ST_RATING_INSERT_MISSING);
            if (s) {
                b_text(s, 1, c->rated_ids[i]);
                b_text_null(s, 2, c->rated_names[i]);
                b_i64(s, 3, TABLE_RATING_START);
                b_i64(s, 4, at);
            }
            if ((rc = step_write(q, s, "rating insert", false)) != CB_OK) break;
        }
        s = stmt(q, c->rated_is_bot[i] ? ST_BOT_RATING_APPLY : ST_RATING_APPLY);
        if (s) {
            b_text(s, 1, c->rated_ids[i]);
            b_i64(s, 2, c->rating_delta[i]);
            b_i64(s, 3, at);
        }
        rc = step_write(q, s, "rating apply", false);
    }
    return rc;
}

static bool commit_valid(const cb_game_commit *c) {
    return c && c->row && c->n_humans >= 0 && c->n_humans <= CB_MAX_SEATS && c->n_bots >= 0 &&
           c->n_bots <= CB_MAX_SEATS && c->n_rated >= 0 && c->n_rated <= CB_MAX_SEATS;
}

// 1 when the game row exists, 0 when not, or a CB_E_*. Lock held.
static int game_exists(sq *q, const char *id) {
    sqlite3_stmt *s = stmt(q, ST_GAME_EXISTS);
    if (!s) return CB_E_DB;
    b_text(s, 1, id);
    int r = sqlite3_step(s);
    int rc = r == SQLITE_ROW ? 1 : r == SQLITE_DONE ? 0 : fail(q, "game exists", r);
    done(s);
    return rc;
}

// THE CAS. The row's last_commit_at is the commit's time: every commit moves
// the version, and games_stamp_last_commit stamped now() whenever it moved.
static int sq_game_commit(cb_store *st, const cb_game_commit *c) {
    if (!commit_valid(c)) return CB_E_INVALID;
    sq *q = Q(st);
    const cb_game_row *g = c->row;
    lock(q);
    int rc = begin(q);
    if (rc == CB_OK) {
        sqlite3_stmt *s = stmt(q, ST_GAME_UPDATE_CAS);
        if (!s) rc = CB_E_DB;
        else {
            b_text(s, 1, g->id);
            b_i64(s, 2, g->status);
            b_blob_nn(s, 3, g->state, g->state_len);
            b_blob_nn(s, 4, g->roster, g->roster_len);
            b_blob(s, 5, g->logs, g->logs_len);
            b_blob(s, 6, g->has_seed ? g->seed : NULL, g->has_seed ? CB_SEED_LEN : 0);
            b_i64(s, 7, g->needs_bots);
            b_i64(s, 8, g->version);
            b_i64(s, 9, g->round_epoch);
            b_i64(s, 10, g->updated_at);
            b_i64(s, 11, g->updated_at);
            b_i64(s, 12, c->expected_version);
            rc = step_write(q, s, "game commit", true);
            if (rc == CB_E_NOT_FOUND) {   // the fence refused: moved on, or gone
                int e = game_exists(q, g->id);
                rc = e == 1 ? CB_E_CONFLICT : e == 0 ? CB_E_NOT_FOUND : e;
            }
        }
    }
    if (rc == CB_OK) rc = sync_seats(q, g->id, c->humans, c->n_humans, false, g->updated_at);
    if (rc == CB_OK) rc = sync_seats(q, g->id, c->bots, c->n_bots, true, g->updated_at);
    if (rc == CB_OK) rc = apply_ratings(q, c, g->updated_at);
    if (rc == CB_OK) rc = commit(q);
    if (rc != CB_OK) rollback(q);
    unlock(q);
    return rc;
}

static int sq_game_delete(cb_store *st, const char *id, int64_t expected) {
    sq *q = Q(st);
    lock(q);
    sqlite3_stmt *s = stmt(q, ST_GAME_DELETE_CAS);
    if (s) {
        b_text(s, 1, id);
        b_i64(s, 2, expected);
    }
    int rc = step_write(q, s, "game delete", true);
    if (rc == CB_E_NOT_FOUND) {
        int e = game_exists(q, id);
        rc = e == 1 ? CB_E_CONFLICT : e == 0 ? CB_E_NOT_FOUND : e;
    }
    unlock(q);
    return rc;
}

static int sq_games_of_user(cb_store *st, const char *user, int limit, cb_member_fn fn, void *ud) {
    sq *q = Q(st);
    vec v = { .size = sizeof(cb_member_row) };
    int rc = CB_OK;
    lock(q);
    sqlite3_stmt *s = stmt(q, ST_GAMES_OF_USER);
    if (!s) rc = CB_E_DB;
    else {
        b_text(s, 1, user);
        b_i64(s, 2, limit > 0 ? limit : -1);
        int r = SQLITE_DONE;
        while (rc == CB_OK && (r = sqlite3_step(s)) == SQLITE_ROW) {
            cb_member_row *m = vec_push(&v);
            if (!m) { rc = CB_E_NOMEM; break; }
            c_text(s, 0, m->game_id, sizeof m->game_id);
            m->updated_at = c_i64(s, 1);
        }
        if (rc == CB_OK && r != SQLITE_DONE) rc = fail(q, "games of user", r);
    }
    done(s);
    unlock(q);
    int n = 0;
    for (int i = 0; rc == CB_OK && i < v.n; i++) {
        n++;
        if (!fn(&((cb_member_row *)v.p)[i], ud)) break;
    }
    free(v.p);
    return rc == CB_OK ? n : rc;
}

static int sq_is_member(cb_store *st, const char *game, const char *user) {
    sq *q = Q(st);
    lock(q);
    sqlite3_stmt *s = stmt(q, ST_IS_MEMBER);
    int rc = CB_E_DB;
    if (s) {
        b_text(s, 1, game);
        b_text(s, 2, user);
        int r = sqlite3_step(s);
        rc = r == SQLITE_ROW ? 1 : r == SQLITE_DONE ? 0 : fail(q, "is member", r);
    }
    done(s);
    unlock(q);
    return rc;
}

// A list of short names (game ids, collection names) read under the lock and
// handed out after it.
typedef struct {
    char (*names)[CB_TOPIC_MAX];
    int n, cap;
} namelist;

static bool names_push(namelist *l, const char *name) {
    if (l->n == l->cap) {
        int cap = l->cap ? l->cap * 2 : 16;
        void *p = realloc(l->names, (size_t)cap * sizeof *l->names);
        if (!p) return false;
        l->names = p;
        l->cap = cap;
    }
    cb_strlcpy(l->names[l->n++], name, sizeof *l->names);
    return true;
}

static int names_emit(namelist *l, int rc, cb_name_fn fn, void *ud) {
    int n = 0;
    for (int i = 0; rc == CB_OK && i < l->n; i++) {
        n++;
        if (!fn(l->names[i], ud)) break;
    }
    free(l->names);
    return rc == CB_OK ? n : rc;
}

static int sq_games_needing_bots(cb_store *st, int64_t stale_before, int64_t active_after, int limit, cb_name_fn fn,
                                 void *ud) {
    sq *q = Q(st);
    namelist l = { 0 };
    int rc = CB_OK;
    lock(q);
    sqlite3_stmt *s = stmt(q, ST_GAMES_NEEDING_BOTS);
    if (!s) rc = CB_E_DB;
    else {
        b_i64(s, 1, stale_before);
        b_i64(s, 2, active_after);
        b_i64(s, 3, limit > 0 ? limit : -1);
        int r = SQLITE_DONE;
        while (rc == CB_OK && (r = sqlite3_step(s)) == SQLITE_ROW)
            if (!names_push(&l, (const char *)sqlite3_column_text(s, 0))) rc = CB_E_NOMEM;
        if (rc == CB_OK && r != SQLITE_DONE) rc = fail(q, "games needing bots", r);
    }
    done(s);
    unlock(q);
    return names_emit(&l, rc, fn, ud);
}

// ---- the bot lease ---------------------------------------------------------------------

static int sq_lease_acquire(cb_store *st, const char *game, int64_t now, int64_t ttl, cb_uuid token_out) {
    sq *q = Q(st);
    cb_uuid tok;
    cb_uuid_v4(tok);
    lock(q);
    sqlite3_stmt *s = stmt(q, ST_LEASE_ACQUIRE);
    if (s) {
        b_text(s, 1, game);
        b_text(s, 2, tok);
        b_i64(s, 3, now);
        b_i64(s, 4, ttl);
    }
    int rc = step_write(q, s, "lease acquire", true);
    if (rc == CB_E_NOT_FOUND) {
        int e = game_exists(q, game);
        rc = e == 1 ? CB_E_CONFLICT : e == 0 ? CB_E_NOT_FOUND : e;
    }
    unlock(q);
    if (rc == CB_OK) memcpy(token_out, tok, sizeof tok);
    return rc;
}

static int sq_lease_renew(cb_store *st, const char *game, const char *token, int64_t now, int64_t ttl) {
    sq *q = Q(st);
    lock(q);
    sqlite3_stmt *s = stmt(q, ST_LEASE_RENEW);
    if (s) {
        b_text(s, 1, game);
        b_text(s, 2, token);
        b_i64(s, 3, now);
        b_i64(s, 4, ttl);
    }
    int rc = step_write(q, s, "lease renew", true);
    unlock(q);
    return rc == CB_E_NOT_FOUND ? CB_E_CONFLICT : rc;
}

// Frees the lease if `token` still holds it. Always CB_OK: a release that finds
// someone else's lease (or none) has nothing to do.
static int sq_lease_release(cb_store *st, const char *game, const char *token) {
    int rc = write1(Q(st), ST_LEASE_RELEASE, "lease release", false, game, token, NULL);
    return rc == CB_E_DB ? rc : CB_OK;
}

// ---- bots and ratings ------------------------------------------------------------------

static int sq_bots_list(cb_store *st, cb_bot_fn fn, void *ud) {
    sq *q = Q(st);
    vec v = { .size = sizeof(cb_bot_row) };
    int rc = CB_OK;
    lock(q);
    sqlite3_stmt *s = stmt(q, ST_BOTS_ALL);
    if (!s) rc = CB_E_DB;
    int r = SQLITE_DONE;
    while (rc == CB_OK && (r = sqlite3_step(s)) == SQLITE_ROW) {
        cb_bot_row *b = vec_push(&v);
        if (!b) rc = CB_E_NOMEM;
        else read_bot(s, b);
    }
    if (rc == CB_OK && r != SQLITE_DONE) rc = fail(q, "bots list", r);
    done(s);
    unlock(q);
    if (rc == CB_OK) cb_store_bots_sort(v.p, v.n);
    int n = 0;
    for (int i = 0; rc == CB_OK && i < v.n; i++) {
        n++;
        if (!fn(&((cb_bot_row *)v.p)[i], ud)) break;
    }
    free(v.p);
    return rc == CB_OK ? n : rc;
}

static int sq_ratings_list(cb_store *st, cb_rating_fn fn, void *ud) {
    sq *q = Q(st);
    vec v = { .size = sizeof(cb_rating_row) };
    int rc = CB_OK;
    lock(q);
    sqlite3_stmt *s = stmt(q, ST_RATINGS_ALL);
    if (!s) rc = CB_E_DB;
    int r = SQLITE_DONE;
    while (rc == CB_OK && (r = sqlite3_step(s)) == SQLITE_ROW) {
        cb_rating_row *e = vec_push(&v);
        if (!e) rc = CB_E_NOMEM;
        else read_rating(s, e);
    }
    if (rc == CB_OK && r != SQLITE_DONE) rc = fail(q, "ratings list", r);
    done(s);
    unlock(q);
    if (rc == CB_OK) cb_store_ratings_sort(v.p, v.n);
    int n = 0;
    for (int i = 0; rc == CB_OK && i < v.n; i++) {
        n++;
        if (!fn(&((cb_rating_row *)v.p)[i], ud)) break;
    }
    free(v.p);
    return rc == CB_OK ? n : rc;
}

// seat_ratings: a bot's row, else the user's, else TABLE_RATING_NONE.
static int sq_ratings_for(cb_store *st, const char *const *ids, int n, int32_t *out) {
    sq *q = Q(st);
    int rc = CB_OK;
    lock(q);
    for (int i = 0; rc == CB_OK && i < n; i++) {
        out[i] = TABLE_RATING_NONE;
        for (int k = 0; k < 2 && rc == CB_OK; k++) {
            sqlite3_stmt *s = stmt(q, k == 0 ? ST_BOT_ELO : ST_USER_ELO);
            if (!s) { rc = CB_E_DB; break; }
            b_text(s, 1, ids[i] ? ids[i] : "");
            int r = sqlite3_step(s);
            if (r == SQLITE_ROW) out[i] = (int32_t)sqlite3_column_int64(s, 0);
            else if (r != SQLITE_DONE) rc = fail(q, "ratings for", r);
            done(s);
            if (r == SQLITE_ROW) break;
        }
    }
    unlock(q);
    return rc;
}

static int sq_rating_set_username(cb_store *st, const char *user, const char *username) {
    return write1(Q(st), ST_RATING_SET_USERNAME, "rating username", false, user, username ? username : "", NULL);
}

// ---- snapshots and chat ----------------------------------------------------------------

static int sq_snapshot_put(cb_store *st, const cb_snapshot_row *r) {
    if (r->n_players < 0 || r->n_players > CB_MAX_SEATS) return CB_E_INVALID;
    char ids[CB_MAX_SEATS * (CB_UUID_LEN + 1) + 1] = "";
    for (int i = 0; i < r->n_players; i++) {
        if (i) strcat(ids, ",");
        strncat(ids, r->player_ids[i], CB_UUID_LEN);
    }
    sq *q = Q(st);
    lock(q);
    sqlite3_stmt *s = stmt(q, ST_SNAPSHOT_PUT);
    if (s) {
        b_text(s, 1, r->id);
        b_text_null(s, 2, r->game_id);
        b_text(s, 3, ids);
        b_blob_nn(s, 4, r->moves, r->moves_len);
        b_blob(s, 5, r->extras, r->extras_len);
        b_i64(s, 6, r->created_at);
    }
    int rc = step_write(q, s, "snapshot put", false);
    unlock(q);
    return rc;
}

static int sq_snapshots_query(cb_store *st, const cb_snapshot_query *qy, cb_snapshot_fn fn, void *ud) {
    if (!qy || !qy->viewer_id || !*qy->viewer_id) return CB_E_INVALID;
    sq *q = Q(st);
    vec v = { .size = sizeof(cb_snapshot_row) };
    int rc = CB_OK;
    lock(q);
    sqlite3_stmt *s = stmt(q, qy->newest_first ? ST_SNAPSHOTS_DESC : ST_SNAPSHOTS_ASC);
    if (!s) rc = CB_E_DB;
    else {
        b_text(s, 1, qy->viewer_id);
        if (qy->game_id) b_text(s, 2, qy->game_id);
        if (qy->id) b_text(s, 3, qy->id);
        b_i64(s, 4, qy->limit > 0 ? qy->limit : -1);
        int r = SQLITE_DONE;
        while (rc == CB_OK && (r = sqlite3_step(s)) == SQLITE_ROW) {
            cb_snapshot_row *e = vec_push(&v);
            if (!e || !read_snapshot(s, e)) {
                if (e) v.n--;
                rc = CB_E_NOMEM;
            }
        }
        if (rc == CB_OK && r != SQLITE_DONE) rc = fail(q, "snapshots", r);
    }
    done(s);
    unlock(q);
    cb_snapshot_row *rows = v.p;
    int n = 0;
    bool more = rc == CB_OK;
    for (int i = 0; i < v.n; i++) {
        if (more) {
            n++;
            more = fn(&rows[i], ud);
        }
        cb_snapshot_row_free(&rows[i]);
    }
    free(v.p);
    return rc == CB_OK ? n : rc;
}

static int sq_chat_put(cb_store *st, const cb_chat_row *c) {
    sq *q = Q(st);
    lock(q);
    sqlite3_stmt *s = stmt(q, ST_CHAT_PUT);
    if (s) {
        b_text(s, 1, c->id);
        b_text(s, 2, c->game_id);
        b_text(s, 3, c->user_id);
        b_text(s, 4, c->message ? c->message : "");
        b_i64(s, 5, c->is_system);
        b_i64(s, 6, c->created_at);
    }
    int rc = step_write(q, s, "chat put", false);
    unlock(q);
    return rc;
}

static int sq_chat_list(cb_store *st, const char *game, int limit, cb_chat_fn fn, void *ud) {
    sq *q = Q(st);
    vec v = { .size = sizeof(cb_chat_row) };
    int rc = CB_OK;
    lock(q);
    sqlite3_stmt *s = stmt(q, ST_CHAT_LIST);
    if (!s) rc = CB_E_DB;
    else {
        b_text(s, 1, game);
        b_i64(s, 2, limit > 0 ? limit : -1);
        int r = SQLITE_DONE;
        while (rc == CB_OK && (r = sqlite3_step(s)) == SQLITE_ROW) {
            cb_chat_row *c = vec_push(&v);
            if (!c) { rc = CB_E_NOMEM; break; }
            c_text(s, 0, c->id, sizeof c->id);
            c_text(s, 1, c->game_id, sizeof c->game_id);
            c_text(s, 2, c->user_id, sizeof c->user_id);
            const unsigned char *m = sqlite3_column_text(s, 3);
            c->message = strdup(m ? (const char *)m : "");
            if (!c->message) rc = CB_E_NOMEM;
            c->is_system = c_i64(s, 4) != 0;
            c->created_at = c_i64(s, 5);
        }
        if (rc == CB_OK && r != SQLITE_DONE) rc = fail(q, "chat list", r);
    }
    done(s);
    unlock(q);
    cb_chat_row *rows = v.p;
    int n = 0;
    bool more = rc == CB_OK;
    for (int i = 0; i < v.n; i++) {
        if (more) {
            n++;
            more = fn(&rows[i], ud);
        }
        cb_chat_row_free(&rows[i]);
    }
    free(v.p);
    return rc == CB_OK ? n : rc;
}

// ---- realtime connections --------------------------------------------------------------

static int sq_conn_put(cb_store *st, const cb_conn_row *c) {
    if (c->n_topics < 0 || c->n_topics > CB_CONN_TOPICS) return CB_E_INVALID;
    sq *q = Q(st);
    lock(q);
    int rc = begin(q);
    sqlite3_stmt *s;
    if (rc == CB_OK) {   // TTL: what has expired goes, as DynamoDB's TTL sweeps it
        s = stmt(q, ST_CONN_PRUNE);
        if (s) b_i64(s, 1, cb_now_ms());
        rc = step_write(q, s, "conn prune", false);
    }
    if (rc == CB_OK) {
        s = stmt(q, ST_CONN_UPSERT);
        if (s) {
            b_text(s, 1, c->conn_id);
            b_text(s, 2, c->user_id);
            b_i64(s, 3, c->vsn);
            b_i64(s, 4, c->expires_at);
        }
        rc = step_write(q, s, "conn put", false);
    }
    if (rc == CB_OK) {
        s = stmt(q, ST_CONN_TOPICS_CLEAR);
        if (s) b_text(s, 1, c->conn_id);
        rc = step_write(q, s, "conn topics", false);
    }
    for (int i = 0; rc == CB_OK && i < c->n_topics; i++) {
        s = stmt(q, ST_CONN_TOPIC_INSERT);
        if (s) {
            b_text(s, 1, c->conn_id);
            b_i64(s, 2, i);
            b_text(s, 3, c->topics[i]);
            b_text(s, 4, c->join_refs[i]);
        }
        rc = step_write(q, s, "conn topic", false);
    }
    if (rc == CB_OK) rc = commit(q);
    if (rc != CB_OK) rollback(q);
    unlock(q);
    return rc;
}

// Lock held.
static int conn_read(sq *q, const char *id, int64_t now, cb_conn_row *out) {
    memset(out, 0, sizeof *out);
    sqlite3_stmt *s = stmt(q, ST_CONN_GET);
    if (!s) return CB_E_DB;
    b_text(s, 1, id);
    b_i64(s, 2, now);
    int r = sqlite3_step(s), rc = CB_OK;
    if (r == SQLITE_ROW) {
        c_text(s, 0, out->conn_id, sizeof out->conn_id);
        c_text(s, 1, out->user_id, sizeof out->user_id);
        out->vsn = (int)c_i64(s, 2);
        out->expires_at = c_i64(s, 3);
    } else rc = r == SQLITE_DONE ? CB_E_NOT_FOUND : fail(q, "conn get", r);
    done(s);
    if (rc != CB_OK) return rc;
    s = stmt(q, ST_CONN_TOPICS_OF);
    if (!s) return CB_E_DB;
    b_text(s, 1, id);
    while ((r = sqlite3_step(s)) == SQLITE_ROW && out->n_topics < CB_CONN_TOPICS) {
        c_text(s, 0, out->topics[out->n_topics], sizeof out->topics[0]);
        c_text(s, 1, out->join_refs[out->n_topics], sizeof out->join_refs[0]);
        out->n_topics++;
    }
    if (r != SQLITE_ROW && r != SQLITE_DONE) rc = fail(q, "conn topics", r);
    done(s);
    return rc;
}

static int sq_conn_get(cb_store *st, const char *id, cb_conn_row *out) {
    sq *q = Q(st);
    lock(q);
    int rc = conn_read(q, id, cb_now_ms(), out);
    unlock(q);
    return rc;
}

static int sq_conn_delete(cb_store *st, const char *id) {
    return write1(Q(st), ST_CONN_DELETE, "conn delete", false, id, NULL, NULL);
}

// Every live connection that has joined `topic`, each as its whole row.
static int sq_conns_by_topic(cb_store *st, const char *topic, cb_conn_fn fn, void *ud) {
    sq *q = Q(st);
    namelist ids = { 0 };
    vec v = { .size = sizeof(cb_conn_row) };
    int64_t now = cb_now_ms();
    int rc = CB_OK;
    lock(q);
    sqlite3_stmt *s = stmt(q, ST_CONNS_BY_TOPIC);
    if (!s) rc = CB_E_DB;
    else {
        b_text(s, 1, topic);
        b_i64(s, 2, now);
        int r = SQLITE_DONE;
        while (rc == CB_OK && (r = sqlite3_step(s)) == SQLITE_ROW)
            if (!names_push(&ids, (const char *)sqlite3_column_text(s, 0))) rc = CB_E_NOMEM;
        if (rc == CB_OK && r != SQLITE_DONE) rc = fail(q, "conns by topic", r);
    }
    done(s);
    for (int i = 0; rc == CB_OK && i < ids.n; i++) {
        cb_conn_row *c = vec_push(&v);
        if (!c) rc = CB_E_NOMEM;
        else if ((rc = conn_read(q, ids.names[i], now, c)) == CB_E_NOT_FOUND) {
            v.n--;
            rc = CB_OK;
        }
    }
    unlock(q);
    free(ids.names);
    int n = 0;
    for (int i = 0; rc == CB_OK && i < v.n; i++) {
        n++;
        if (!fn(&((cb_conn_row *)v.p)[i], ud)) break;
    }
    free(v.p);
    return rc == CB_OK ? n : rc;
}

// ---- admin -----------------------------------------------------------------------------

static int sq_admin_collections(cb_store *st, cb_name_fn fn, void *ud) {
    sq *q = Q(st);
    namelist l = { 0 };
    int rc = CB_OK;
    lock(q);
    sqlite3_stmt *s = stmt(q, ST_COLLECTIONS);
    if (!s) rc = CB_E_DB;
    int r = SQLITE_DONE;
    while (rc == CB_OK && (r = sqlite3_step(s)) == SQLITE_ROW)
        if (!names_push(&l, (const char *)sqlite3_column_text(s, 0))) rc = CB_E_NOMEM;
    if (rc == CB_OK && r != SQLITE_DONE) rc = fail(q, "collections", r);
    done(s);
    unlock(q);
    return names_emit(&l, rc, fn, ud);
}

// A row of any statement as text: NULL stays NULL, a blob is "\x" hex.
typedef struct {
    int    n;
    char **names;
    char **values;
} textrow;

static void textrow_free(textrow *t) {
    for (int i = 0; i < t->n; i++) {
        free(t->names[i]);
        free(t->values[i]);
    }
    free(t->names);
    free(t->values);
}

static bool textrow_read(sqlite3_stmt *s, int first, textrow *t) {
    int n = sqlite3_column_count(s) - first;
    t->n = n > 0 ? n : 0;
    t->names = calloc((size_t)t->n + 1, sizeof *t->names);
    t->values = calloc((size_t)t->n + 1, sizeof *t->values);
    if (!t->names || !t->values) return false;
    for (int i = 0; i < t->n; i++) {
        int c = i + first;
        const char *nm = sqlite3_column_name(s, c);
        t->names[i] = strdup(nm ? nm : "?");
        if (!t->names[i]) return false;
        int type = sqlite3_column_type(s, c);
        if (type == SQLITE_NULL) continue;
        if (type == SQLITE_BLOB) {
            size_t len = (size_t)sqlite3_column_bytes(s, c);
            t->values[i] = malloc(2 * len + 3);
            if (!t->values[i]) return false;
            cb_bytea_encode(sqlite3_column_blob(s, c), len, t->values[i], 2 * len + 3);
        } else {
            const unsigned char *v = sqlite3_column_text(s, c);
            t->values[i] = strdup(v ? (const char *)v : "");
            if (!t->values[i]) return false;
        }
    }
    return true;
}

// Steps `s` to the end (or `limit` rows), reading every row as text under the
// lock. Rows are handed to fn by the caller after unlocking.
typedef struct {
    textrow *rows;
    int      n, cap;
} textrows;

static int textrows_read(sq *q, sqlite3_stmt *s, int first, int limit, textrows *out, int64_t *last_rowid) {
    int r = SQLITE_DONE;
    while ((limit <= 0 || out->n < limit) && (r = sqlite3_step(s)) == SQLITE_ROW) {
        if (out->n == out->cap) {
            int cap = out->cap ? out->cap * 2 : 32;
            void *p = realloc(out->rows, (size_t)cap * sizeof *out->rows);
            if (!p) return CB_E_NOMEM;
            out->rows = p;
            out->cap = cap;
        }
        textrow *t = &out->rows[out->n++];
        memset(t, 0, sizeof *t);
        if (last_rowid) *last_rowid = sqlite3_column_int64(s, 0);
        if (!textrow_read(s, first, t)) return CB_E_NOMEM;
    }
    if (limit > 0 && out->n >= limit) return CB_OK;
    return r == SQLITE_DONE ? CB_OK : fail(q, "admin read", r);
}

static int textrows_emit(textrows *t, int rc, cb_admin_row_fn fn, void *ud) {
    int n = 0;
    bool more = rc == CB_OK;
    for (int i = 0; i < t->n; i++) {
        if (more) {
            n++;
            more = fn(t->rows[i].n, (const char *const *)t->rows[i].names, (const char *const *)t->rows[i].values, ud);
        }
        textrow_free(&t->rows[i]);
    }
    free(t->rows);
    return rc == CB_OK ? n : rc;
}

static bool ident_ok(const char *s) {
    if (!s || !*s || strlen(s) > 64) return false;
    for (; *s; s++)
        if (!(isalnum((unsigned char)*s) || *s == '_')) return false;
    return true;
}

// 1 when `col` is a column of `table` (both already ident_ok), 0, or CB_E_DB.
static int has_column(sq *q, const char *table, const char *col, char *first_col, size_t cap) {
    char sql[160];
    snprintf(sql, sizeof sql, "SELECT name FROM pragma_table_info('%s') ORDER BY cid", table);
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(q->db, sql, -1, &s, NULL) != SQLITE_OK) return fail(q, "table info", 0);
    int found = 0, r, i = 0;
    while ((r = sqlite3_step(s)) == SQLITE_ROW) {
        const char *nm = (const char *)sqlite3_column_text(s, 0);
        if (i++ == 0 && first_col) cb_strlcpy(first_col, nm, cap);
        if (col && strcmp(nm, col) == 0) found = 1;
    }
    sqlite3_finalize(s);
    return r == SQLITE_DONE ? found : CB_E_DB;
}

// Pages of one table in rowid order. `prefix`: NULL or "" for every row;
// "column=value" for the rows whose column equals value (the console's eq
// filter); any other text for the rows whose first column starts with it.
// `cursor` is the previous page's next_cursor ("" for the first page);
// next_cursor comes back "" after the last page.
static int sq_admin_browse(cb_store *st, const char *collection, const char *prefix, const char *cursor, int limit,
                           cb_admin_row_fn fn, void *ud, char *next_cursor, size_t next_cap) {
    if (next_cursor && next_cap) next_cursor[0] = '\0';
    if (!ident_ok(collection)) return CB_E_INVALID;
    if (limit <= 0 || limit > 1000) limit = 100;
    sq *q = Q(st);
    char col[72] = "", first[72] = "";
    const char *value = NULL;
    const char *eq = prefix ? strchr(prefix, '=') : NULL;
    if (eq) {
        if (!cb_strncpy0(col, sizeof col, prefix, (size_t)(eq - prefix)) || !ident_ok(col)) return CB_E_INVALID;
        value = eq + 1;
    } else if (prefix && *prefix) value = prefix;
    int64_t after = cursor && *cursor ? strtoll(cursor, NULL, 10) : 0;

    textrows rows = { 0 };
    int64_t last = 0;
    lock(q);
    int rc = has_column(q, collection, eq ? col : NULL, first, sizeof first);
    if (rc >= 0 && !first[0]) rc = CB_E_NOT_FOUND;   // no such table
    else if (rc == 0 && eq) rc = CB_E_INVALID;          // no such column
    if (rc >= 0) {
        char sql[400];
        if (!value) snprintf(sql, sizeof sql, "SELECT rowid, * FROM \"%s\" WHERE rowid > ?1 ORDER BY rowid LIMIT ?2", collection);
        else if (eq)
            snprintf(sql, sizeof sql, "SELECT rowid, * FROM \"%s\" WHERE rowid > ?1 AND \"%s\" = ?3 ORDER BY rowid LIMIT ?2",
                     collection, col);
        else
            snprintf(sql, sizeof sql,
                     "SELECT rowid, * FROM \"%s\" WHERE rowid > ?1 AND substr(\"%s\", 1, length(?3)) = ?3 "
                     "ORDER BY rowid LIMIT ?2",
                     collection, first);
        sqlite3_stmt *s;
        if (sqlite3_prepare_v2(q->db, sql, -1, &s, NULL) != SQLITE_OK) rc = fail(q, "browse", 0);
        else {
            b_i64(s, 1, after);
            b_i64(s, 2, limit);
            if (value) b_text(s, 3, value);
            rc = textrows_read(q, s, 1, limit, &rows, &last);
            sqlite3_finalize(s);
        }
    }
    unlock(q);
    if (rc >= 0) rc = CB_OK;
    if (rc == CB_OK && rows.n == limit && next_cursor && next_cap) snprintf(next_cursor, next_cap, "%lld", (long long)last);
    return textrows_emit(&rows, rc, fn, ud);
}

// The statement's first keyword, upper-cased, comments and whitespace skipped.
static void first_keyword(const char *s, char *out, size_t cap) {
    for (;;) {
        while (isspace((unsigned char)*s)) s++;
        if (s[0] == '-' && s[1] == '-') {
            while (*s && *s != '\n') s++;
        } else if (s[0] == '/' && s[1] == '*') {
            const char *e = strstr(s + 2, "*/");
            s = e ? e + 2 : s + strlen(s);
        } else break;
    }
    size_t n = 0;
    while (n + 1 < cap && isalpha((unsigned char)s[n])) {
        out[n] = (char)toupper((unsigned char)s[n]);
        n++;
    }
    out[n] = '\0';
}

// One statement. Without allow_write it must be a SELECT (or WITH ... SELECT,
// or EXPLAIN) that SQLite itself calls read-only. Returns the rows given to fn.
static int sq_admin_query(cb_store *st, const char *statement, bool allow_write, cb_admin_row_fn fn, void *ud) {
    if (!statement) return CB_E_INVALID;
    sq *q = Q(st);
    textrows rows = { 0 };
    lock(q);
    sqlite3_stmt *s = NULL;
    const char *tail = NULL;
    int rc = CB_OK;
    if (sqlite3_prepare_v2(q->db, statement, -1, &s, &tail) != SQLITE_OK) {
        CB_WARN(MOD, "admin query: %s", sqlite3_errmsg(q->db));
        rc = CB_E_INVALID;
    } else if (!s) rc = CB_E_INVALID;   // only whitespace or comments
    else {
        while (tail && isspace((unsigned char)*tail)) tail++;
        if (tail && *tail && !(tail[0] == ';' && tail[1 + strspn(tail + 1, " \t\r\n")] == '\0')) rc = CB_E_INVALID;
        char kw[16];
        first_keyword(statement, kw, sizeof kw);
        bool reads = sqlite3_stmt_readonly(s) &&
                     (strcmp(kw, "SELECT") == 0 || strcmp(kw, "WITH") == 0 || strcmp(kw, "EXPLAIN") == 0);
        if (rc == CB_OK && !allow_write && !reads) rc = CB_E_FORBIDDEN;
        if (rc == CB_OK) rc = textrows_read(q, s, 0, 0, &rows, NULL);
    }
    sqlite3_finalize(s);
    unlock(q);
    return textrows_emit(&rows, rc, fn, ud);
}

// ---- migration -------------------------------------------------------------------------

static int sq_user_import(cb_store *st, const cb_user_row *u) {
    sq *q = Q(st);
    lock(q);
    sqlite3_stmt *s = stmt(q, ST_USER_IMPORT);
    if (s) bind_user(s, u);
    int rc = step_write(q, s, "user import", false);
    unlock(q);
    return rc;
}

static int sq_rating_import(cb_store *st, const cb_rating_row *r) {
    sq *q = Q(st);
    lock(q);
    sqlite3_stmt *s = stmt(q, ST_RATING_IMPORT);
    if (s) {
        b_text(s, 1, r->user_id);
        b_text_null(s, 2, r->username);
        b_i64(s, 3, r->elo_rating);
        if (r->previous_elo >= 0) b_i64(s, 4, r->previous_elo);
        else sqlite3_bind_null(s, 4);
        b_i64(s, 5, r->games_played);
        b_i64(s, 6, r->updated_at);
    }
    int rc = step_write(q, s, "rating import", false);
    unlock(q);
    return rc;
}

static int sq_bots_replace(cb_store *st, const cb_bot_row *rows, int n) {
    sq *q = Q(st);
    lock(q);
    int rc = begin(q);
    if (rc == CB_OK) rc = step_write(q, stmt(q, ST_BOTS_CLEAR), "bots clear", false);
    for (int i = 0; rc == CB_OK && i < n; i++) {
        sqlite3_stmt *s = stmt(q, ST_BOT_INSERT);
        if (s) {
            const cb_bot_row *b = &rows[i];
            b_text(s, 1, b->id);
            b_text(s, 2, b->nickname);
            b_text(s, 3, b->strategy_key);
            b_i64(s, 4, b->elo_rating);
            if (b->previous_elo >= 0) b_i64(s, 5, b->previous_elo);
            else sqlite3_bind_null(s, 5);
            b_i64(s, 6, b->games_played);
            b_i64(s, 7, b->created_at);
            b_i64(s, 8, b->updated_at);
        }
        rc = step_write(q, s, "bot insert", false);
    }
    if (rc == CB_OK) rc = commit(q);
    if (rc != CB_OK) rollback(q);
    unlock(q);
    return rc;
}

static int sq_game_import(cb_store *st, const cb_game_row *g, const cb_uuid *humans, int n_humans, const cb_uuid *bots,
                          int n_bots) {
    sq *q = Q(st);
    lock(q);
    int rc = begin(q);
    if (rc == CB_OK) {
        sqlite3_stmt *s = stmt(q, ST_GAME_IMPORT);
        if (s) bind_game(s, g);
        rc = step_write(q, s, "game import", false);
    }
    if (rc == CB_OK) {
        sqlite3_stmt *s = stmt(q, ST_MEMBERS_CLEAR);
        if (s) b_text(s, 1, g->id);
        rc = step_write(q, s, "members clear", false);
    }
    if (rc == CB_OK) {
        sqlite3_stmt *s = stmt(q, ST_BOTSEATS_CLEAR);
        if (s) b_text(s, 1, g->id);
        rc = step_write(q, s, "bot seats clear", false);
    }
    if (rc == CB_OK) rc = sync_seats(q, g->id, humans, n_humans, false, g->updated_at);
    if (rc == CB_OK) rc = sync_seats(q, g->id, bots, n_bots, true, g->updated_at);
    if (rc == CB_OK) rc = commit(q);
    if (rc != CB_OK) rollback(q);
    unlock(q);
    return rc;
}

static const cb_store_ops SQLITE_OPS = {
    .name = "sqlite",
    .close = sq_close,
    .user_get = sq_user_get,
    .user_by_email = sq_user_by_email,
    .user_create = sq_user_create,
    .user_set_password = sq_user_set_password,
    .user_touch_sign_in = sq_user_touch_sign_in,
    .user_delete = sq_user_delete,
    .users_list = sq_users_list,
    .refresh_put = sq_refresh_put,
    .refresh_take = sq_refresh_take,
    .refresh_revoke_session = sq_refresh_revoke_session,
    .game_get = sq_game_get,
    .game_create = sq_game_create,
    .game_commit = sq_game_commit,
    .game_delete = sq_game_delete,
    .games_of_user = sq_games_of_user,
    .is_member = sq_is_member,
    .games_needing_bots = sq_games_needing_bots,
    .lease_acquire = sq_lease_acquire,
    .lease_renew = sq_lease_renew,
    .lease_release = sq_lease_release,
    .bots_list = sq_bots_list,
    .ratings_list = sq_ratings_list,
    .ratings_for = sq_ratings_for,
    .rating_set_username = sq_rating_set_username,
    .snapshot_put = sq_snapshot_put,
    .snapshots_query = sq_snapshots_query,
    .chat_put = sq_chat_put,
    .chat_list = sq_chat_list,
    .conn_put = sq_conn_put,
    .conn_get = sq_conn_get,
    .conn_delete = sq_conn_delete,
    .conns_by_topic = sq_conns_by_topic,
    .admin_collections = sq_admin_collections,
    .admin_browse = sq_admin_browse,
    .admin_query = sq_admin_query,
    .user_import = sq_user_import,
    .rating_import = sq_rating_import,
    .bots_replace = sq_bots_replace,
    .game_import = sq_game_import,
};
