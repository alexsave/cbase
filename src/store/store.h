// store.h - everything c_base keeps, behind one interface with two backends.
//
//   store_sqlite.c   `cbase serve`, the tests, and the import rehearsal: one
//                    SQLite file, WAL, the schema in sql/schema.sql.
//   store_dynamo.c   production: one DynamoDB table "cbase" (single-table,
//                    pk/sk, DESIGN.md "Store"), reached over HTTPS with SigV4
//                    (src/aws). DynamoDB's JSON never leaves that file.
//
// The interface is a table of function pointers (cb_store_ops) plus the inline
// wrappers below; callers use the wrappers. Every function is safe to call from
// several threads at once (serve runs handlers on a pool).
//
// CONSISTENCY is the same "fake consistent server" the Supabase backend had,
// rebuilt on a compare-and-swap (DESIGN.md "Why Lambda"):
//   * a game commit carries the version it read, and loses (CB_E_CONFLICT) if
//     the row moved on; the caller reloads and re-runs (app/table_ops.h);
//   * a bot drive holds a lease (lease_acquire/renew/release, 25 s TTL);
//   * nothing else needs ordering: users, chat, snapshots and connections are
//     single writes.
//
// CONVENTIONS. Times are ms since the Unix epoch. Ids are canonical lowercase
// UUID text (CB_UUID_LEN), game ids 6 hex characters. Byte fields are owned by
// the row struct that holds them (malloc'd by the store, freed by the matching
// *_free). A list function calls `fn` once per row, in the documented order,
// with a row the callee may read but not keep (copy what you keep); fn returns
// false to stop early. Every function returns CB_OK, a count, or a CB_E_* code:
// CB_E_NOT_FOUND, CB_E_CONFLICT (a unique key or a CAS lost), CB_E_DB (the
// backend failed; it has logged why), CB_E_NOTIMPL (a stub).
#ifndef CB_STORE_H
#define CB_STORE_H

#include "util/err.h"
#include "util/uuid.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CB_GAME_ID_MAX   16    // games.id is 6 hex today; room for growth
#define CB_EMAIL_MAX     320
#define CB_USERNAME_MAX  64    // == kernel ROSTER_NAME_MAX
#define CB_HASH_MAX      128   // a bcrypt hash is 60
#define CB_SEED_LEN      32    // == kernel FOOLISH_SEED_LEN
#define CB_MAX_SEATS     8     // == kernel MAX_PLAYERS
#define CB_TOPIC_MAX     128

typedef char cb_uuid[CB_UUID_LEN + 1];

// ---- rows ------------------------------------------------------------------------

typedef struct {
    cb_uuid id;
    char    email[CB_EMAIL_MAX + 1];       // lowercase
    char    password_hash[CB_HASH_MAX];    // bcrypt $2a$ / $2b$
    char    username[CB_USERNAME_MAX + 1]; // "" for none
    int64_t created_at;
    int64_t last_sign_in_at;               // 0 for never
} cb_user_row;

typedef struct {
    char    token_hash[65];   // lowercase hex SHA-256 of the refresh token
    cb_uuid user_id;
    cb_uuid session_id;
    int64_t created_at;
    bool    revoked;
    // When refresh_take rotated it (the FIRST take; later takes keep it), so
    // auth can give a retry of a lost refresh response GoTrue's 10 s grace.
    // 0 when never taken, and 0 again after refresh_revoke_session or
    // refresh_revoke_user: a logged-out token never gets the grace.
    int64_t revoked_at;
} cb_refresh_row;

// A game as committed: the kernel's bytes plus the scalar columns.
typedef struct {
    char     id[CB_GAME_ID_MAX + 1];
    int      status;            // kernel GAME_STATUS_*
    uint8_t *state;   size_t state_len;    // kernel state blob
    uint8_t *roster;  size_t roster_len;   // kernel durable roster (ROSTER_BYTES)
    uint8_t *logs;    size_t logs_len;     // session log (games.logs_packed); NULL/0 for none
    bool     has_seed;
    uint8_t  seed[CB_SEED_LEN];            // the deal seed, when has_seed
    bool     needs_bots;
    int64_t  version;           // the CAS token; clients gate on it
    int64_t  round_epoch;
    int64_t  created_at, updated_at, last_commit_at;
} cb_game_row;

void cb_game_row_free(cb_game_row *g);   // frees the byte fields; the struct is the caller's

// One commit of a game: the row as it is to become, written only if the stored
// version is still `expected_version` (the CAS). With it, atomically:
// membership (the humans and bots the roster seats now; anyone not listed is
// removed) and the rating changes the kernel computed (TableCommit.n_rated).
typedef struct {
    const cb_game_row *row;     // row->version is the NEW version (expected + 1)
    int64_t  expected_version;
    int      n_humans;
    cb_uuid  humans[CB_MAX_SEATS];
    int      n_bots;
    cb_uuid  bots[CB_MAX_SEATS];
    // Ratings: by seat id, applied as rating = max(0, rating + delta),
    // games_played + 1, previous_elo = the old rating. A human with no rating
    // row gets one (from TABLE_RATING_START) if the user exists. n_rated 0: none.
    int      n_rated;
    cb_uuid  rated_ids[CB_MAX_SEATS];
    bool     rated_is_bot[CB_MAX_SEATS];
    int32_t  rating_delta[CB_MAX_SEATS];
    // The names the leaderboard shows for the rated humans, by rated index.
    char     rated_names[CB_MAX_SEATS][CB_USERNAME_MAX + 1];
} cb_game_commit;

// A dashboard entry: a game a user sits at.
typedef struct {
    char    game_id[CB_GAME_ID_MAX + 1];
    int64_t updated_at;
} cb_member_row;

typedef struct {
    cb_uuid id;
    char    nickname[CB_USERNAME_MAX + 1];   // '%'-prefixed
    char    strategy_key[24];                // kernel bot_roster key
    int32_t elo_rating;
    int32_t previous_elo;                    // -1 for none
    int32_t games_played;
    int64_t created_at, updated_at;
} cb_bot_row;

typedef struct {
    cb_uuid user_id;
    char    username[CB_USERNAME_MAX + 1];   // "" after an account is deleted
    int32_t elo_rating;
    int32_t previous_elo;                    // -1 for none
    int32_t games_played;
    int64_t updated_at;
} cb_rating_row;

typedef struct {
    cb_uuid  id;
    char     game_id[CB_GAME_ID_MAX + 1];    // "" once the game is deleted
    int      n_players;
    cb_uuid  player_ids[CB_MAX_SEATS];       // the seated humans, seat order
    uint8_t *moves;  size_t moves_len;       // kernel v6 replay code
    uint8_t *extras; size_t extras_len;      // kernel replay extras
    int64_t  created_at;
} cb_snapshot_row;

void cb_snapshot_row_free(cb_snapshot_row *s);

typedef struct {
    cb_uuid id;
    char    game_id[CB_GAME_ID_MAX + 1];
    cb_uuid user_id;
    char   *message;      // owned, NUL-terminated
    bool    is_system;
    int64_t created_at;
} cb_chat_row;

void cb_chat_row_free(cb_chat_row *c);

// A realtime connection under API Gateway (realtime/rt_apigw.h): who it is and
// what it has joined. serve keeps these in memory and never stores them.
#define CB_CONN_TOPICS 16
typedef struct {
    char    conn_id[64];               // API Gateway connectionId
    cb_uuid user_id;                   // "" until a join presents a token
    int     vsn;                       // Phoenix serializer: 1 (objects) or 2 (arrays)
    int     n_topics;
    char    topics[CB_CONN_TOPICS][CB_TOPIC_MAX];   // bare topics ("gu-ab12cd-<uid>")
    char    join_refs[CB_CONN_TOPICS][24];          // the join_ref each topic was joined on
    int64_t expires_at;                // TTL, ms like every time here: a connection not seen for 2 h
                                       // is dropped. DynamoDB keeps it as expires_at in SECONDS (its
                                       // TTL unit) and the exact ms as expires_ms.
} cb_conn_row;

// What a snapshot query asks (PostgREST's game_snapshots shapes).
typedef struct {
    const char *viewer_id;   // REQUIRED: only snapshots whose player_ids contain it
    const char *game_id;     // NULL: any
    const char *id;          // NULL: any
    int         limit;       // 0: no limit
    bool        newest_first;
} cb_snapshot_query;

typedef bool (*cb_member_fn)(const cb_member_row *r, void *ud);
typedef bool (*cb_bot_fn)(const cb_bot_row *r, void *ud);
typedef bool (*cb_rating_fn)(const cb_rating_row *r, void *ud);
typedef bool (*cb_snapshot_fn)(const cb_snapshot_row *r, void *ud);
typedef bool (*cb_chat_fn)(const cb_chat_row *r, void *ud);
typedef bool (*cb_conn_fn)(const cb_conn_row *r, void *ud);
typedef bool (*cb_name_fn)(const char *name, void *ud);   // a game id, a collection name
typedef bool (*cb_user_fn)(const cb_user_row *r, void *ud);
// The admin console's raw browse: one row as name/value text pairs.
#define CB_ADMIN_GAMES "games"   // see admin_collections
typedef bool (*cb_admin_row_fn)(int n, const char *const *names, const char *const *values, void *ud);

// ---- the interface ------------------------------------------------------------------

typedef struct cb_store cb_store;

typedef struct cb_store_ops {
    const char *name;   // "sqlite", "dynamo"
    void (*close)(cb_store *s);

    // users
    int (*user_get)(cb_store *s, const char *id, cb_user_row *out);
    int (*user_by_email)(cb_store *s, const char *email, cb_user_row *out);
    // CB_E_CONFLICT when the email is taken. Also creates the rating row
    // (username, TABLE_RATING_START), as the auth.users trigger did.
    int (*user_create)(cb_store *s, const cb_user_row *u);
    int (*user_set_password)(cb_store *s, const char *id, const char *password_hash);
    int (*user_touch_sign_in)(cb_store *s, const char *id, int64_t at);
    // Deletes the user, their refresh tokens, memberships, rating row and chat
    // messages (the cascade auth.admin.deleteUser had). Snapshots keep their
    // player_ids, as today (DESIGN.md "Open defects carried over").
    int (*user_delete)(cb_store *s, const char *id);
    // Admin: users whose email or username contains `search` ("" for all),
    // newest first, from `offset`.
    int (*users_list)(cb_store *s, const char *search, int offset, int limit, cb_user_fn fn, void *ud);

    // refresh tokens
    int (*refresh_put)(cb_store *s, const cb_refresh_row *r);
    // Rotation: atomically marks the token revoked, stamps revoked_at = now_ms
    // unless a take already stamped it, and returns its row as it was before.
    // CB_E_NOT_FOUND for an unknown hash; a row with revoked = true means the
    // token was already used or logged out (revoked_at tells the two apart).
    int (*refresh_take)(cb_store *s, const char *token_hash, int64_t now_ms, cb_refresh_row *out);
    // Logout: revoke every token of the session (scope local), or of the user
    // (scope global), setting revoked_at to 0 on each.
    int (*refresh_revoke_session)(cb_store *s, const char *session_id);
    int (*refresh_revoke_user)(cb_store *s, const char *user_id);

    // games
    // CB_E_NOT_FOUND, or the row (caller frees with cb_game_row_free).
    int (*game_get)(cb_store *s, const char *id, cb_game_row *out);
    // A new game at version 0 with its creator as member. CB_E_CONFLICT when
    // the id is taken (the caller draws another, up to 5 times).
    int (*game_create)(cb_store *s, const cb_game_row *g, const char *creator_id);
    // THE CAS. CB_OK when written, CB_E_CONFLICT when the stored version is not
    // c->expected_version (nothing written), CB_E_NOT_FOUND when the game is
    // gone. The store stamps last_commit_at with row->updated_at (every commit
    // moves the version); row->last_commit_at is not read. A seated human with
    // no account (deleted; the roster keeps the id) is skipped, not refused.
    int (*game_commit)(cb_store *s, const cb_game_commit *c);
    // Deletes the game (the last seat left: TABLE_EMPTY) if still at
    // expected_version: CB_E_CONFLICT when it moved, CB_E_NOT_FOUND when gone.
    // Its seats and chat go with it; its replays stay with game_id "".
    int (*game_delete)(cb_store *s, const char *id, int64_t expected_version);
    // The games a user sits at, most recently updated first (the dashboard);
    // limit 0: all. DynamoDB answers from an index, so a seat taken a moment
    // ago may be missing for a moment.
    int (*games_of_user)(cb_store *s, const char *user_id, int limit, cb_member_fn fn, void *ud);
    // 1 a member, 0 not, or a CB_E_*.
    int (*is_member)(cb_store *s, const char *game_id, const char *user_id);
    // The heartbeat's scan: needs_bots, updated_at < stale_before,
    // last_commit_at > active_after, at most `limit`.
    int (*games_needing_bots)(cb_store *s, int64_t stale_before, int64_t active_after, int limit, cb_name_fn fn, void *ud);
    // The bot lease (today's try_acquire/renew/release_bot_lease): a random
    // token valid until now + ttl_ms. acquire: CB_OK and the token, or
    // CB_E_CONFLICT while another holder's lease is live (CB_E_NOT_FOUND: no
    // such game). renew: CB_OK, or CB_E_CONFLICT when the token no longer
    // holds it. release: CB_OK (a lease that is not the token's is left alone).
    // acquire and renew move updated_at to `now` (never last_commit_at), as
    // the update_games_updated_at trigger did, so the scan's staleness window
    // sees a drive in flight.
    int (*lease_acquire)(cb_store *s, const char *game_id, int64_t now, int64_t ttl_ms, cb_uuid token_out);
    int (*lease_renew)(cb_store *s, const char *game_id, const char *token, int64_t now, int64_t ttl_ms);
    int (*lease_release)(cb_store *s, const char *game_id, const char *token);

    // bots and ratings
    int (*bots_list)(cb_store *s, cb_bot_fn fn, void *ud);                 // ladder order
    int (*ratings_list)(cb_store *s, cb_rating_fn fn, void *ud);           // highest first
    // seat_ratings: ids[i]'s rating (a bot's, else a user's), or -1
    // (TABLE_RATING_NONE) when it has none, into out[i].
    int (*ratings_for)(cb_store *s, const char *const *ids, int n, int32_t *out);
    // The leaderboard name; NULL clears it (delete-account).
    int (*rating_set_username)(cb_store *s, const char *user_id, const char *username);

    // snapshots
    int (*snapshot_put)(cb_store *s, const cb_snapshot_row *r);
    int (*snapshots_query)(cb_store *s, const cb_snapshot_query *q, cb_snapshot_fn fn, void *ud);

    // chat
    int (*chat_put)(cb_store *s, const cb_chat_row *r);
    // Oldest first, the newest `limit` (0: all).
    int (*chat_list)(cb_store *s, const char *game_id, int limit, cb_chat_fn fn, void *ud);

    // realtime connections (API Gateway only)
    int (*conn_put)(cb_store *s, const cb_conn_row *c);       // insert or replace
    int (*conn_get)(cb_store *s, const char *conn_id, cb_conn_row *out);
    int (*conn_delete)(cb_store *s, const char *conn_id);
    // Every live connection that joined `topic`, each as its whole row. A row
    // past expires_at reads as absent (conn_get: CB_E_NOT_FOUND).
    int (*conns_by_topic)(cb_store *s, const char *topic, cb_conn_fn fn, void *ud);

    // admin console: list the collections, browse one in pages, and run one
    // read-only statement (SQL for SQLite, PartiQL for DynamoDB; writes only
    // with allow_write: CB_E_FORBIDDEN otherwise). Collections are SQLite's
    // tables, or DynamoDB's key families ("USER", "GAME", ...). browse's
    // `prefix`: NULL or "" for every row; on SQLite "column=value" is an eq
    // filter and any other text matches the start of the first column; on
    // DynamoDB it matches the pk after "<FAMILY>#". `cursor` is "" for the
    // first page and next_cursor comes back "" after the last. A value is
    // text, a blob "\x" hex, SQL NULL a NULL pointer. fn may be NULL.
    // CB_ADMIN_GAMES names the same thing on both backends: browse rows that
    // each name one game (SQLite's games table, column "id"; DynamoDB's GAME
    // family, where only the pk GAME#<id> / sk ROW item carries "id"), so the
    // console's Games panel needs no per-backend knowledge.
    int (*admin_collections)(cb_store *s, cb_name_fn fn, void *ud);
    int (*admin_browse)(cb_store *s, const char *collection, const char *prefix, const char *cursor, int limit,
                        cb_admin_row_fn fn, void *ud, char *next_cursor, size_t next_cap);
    int (*admin_query)(cb_store *s, const char *statement, bool allow_write, cb_admin_row_fn fn, void *ud);

    // migration (migrate/import.c): rows written as they are, with no side
    // effects. Nothing but the import calls these.
    // The user as given, insert or replace; unlike user_create, no rating row.
    int (*user_import)(cb_store *s, const cb_user_row *u);
    // The rating row as given (username "" stores none), insert or replace.
    int (*rating_import)(cb_store *s, const cb_rating_row *r);
    // Every bot row replaced by these n (the seeded ones included), atomically.
    int (*bots_replace)(cb_store *s, const cb_bot_row *rows, int n);
    // The game at its own version, with exactly these members, insert or
    // replace. The lease is left free.
    int (*game_import)(cb_store *s, const cb_game_row *g, const cb_uuid *humans, int n_humans, const cb_uuid *bots,
                       int n_bots);
} cb_store_ops;

struct cb_store {
    const cb_store_ops *ops;
    void *impl;
};

// Opens SQLite at `path`, applying sql/schema.sql (embedded) under a
// PRAGMA user_version check. CB_OK and *out, or a CB_E_*.
int cb_store_open_sqlite(const char *path, cb_store **out);
// Opens the DynamoDB table `table` in the region and with the credentials the
// Lambda environment provides (aws/sigv4.h cb_aws_creds_from_env).
int cb_store_open_dynamo(const char *table, cb_store **out);
void cb_store_close(cb_store *s);

// What the calling thread has sent DynamoDB since its last take: every request
// (each attempt of a retried one, as DynamoDB bills it), reads and writes, and
// "GetItem 1 Query 3 TransactWriteItems 1" in `detail`. The handler and the
// bot drive log it per request and per cycle at debug level, the measure of
// what an action costs in requests. Zero on SQLite. Resets the tally.
typedef struct {
    int  reads, writes;
    char detail[192];
} cb_store_tally;
void cb_store_tally_take(cb_store_tally *out);

// The SQLite schema version this binary applies (store_sqlite.c), and the
// embedded schema text (build/gen/schema_sql.c, from sql/schema.sql).
#define CB_SCHEMA_VERSION 1
extern const char cbase_schema_sql[];
extern const unsigned long cbase_schema_sql_len;

// ---- wrappers: call these, not s->ops->... ------------------------------------------
//
//   int rc = CB_STORE_CALL(ctx->store, game_get, id, &row);
//
// A backend that leaves an entry NULL answers CB_E_NOTIMPL for it.
#define CB_STORE_CALL(s, fn, ...) ((s)->ops->fn ? (s)->ops->fn((s), __VA_ARGS__) : CB_E_NOTIMPL)

#endif
