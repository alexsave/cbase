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
// CONSISTENCY is the same "fake consistent server" the TypeScript backend had,
// rebuilt on a compare-and-swap (DESIGN.md "Why Lambda"):
//   * a game commit carries the version it read, and loses (CB_E_CONFLICT) if
//     the row moved on; the caller reloads and re-runs (app/table_ops.h);
//   * a bot drive holds a lease (lease_acquire/renew/release, 25 s TTL);
//   * nothing else needs ordering: users, chat and snapshots are single
//     writes.
//
// CONVENTIONS. Times are ms since the Unix epoch. Ids are canonical lowercase
// UUID text (CB_UUID_LEN), game ids 12 hex characters (6 before). Byte fields are owned by
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

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CB_GAME_ID_MAX   16    // games.id is 12 hex (app_internal.h CB_GAME_ID_HEX), 6 for older games
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
    // The generation of the user's sessions: one begun at a lower one is
    // over, but kept_session. A global sign-out and a password change (auth.h)
    // each move it up by one, in one write; a count, not a time, so a session
    // begun in the same millisecond is neither missed nor caught.
    int64_t session_gen;                   // 0 until sessions are first ended
    cb_uuid kept_session;                  // "" for none
} cb_user_row;

// A session: what a sign-in or sign-up begins and every refresh continues.
// Ending it is one write (revoked), read back strongly consistent by every
// refresh, so a revoked session never refreshes again (security finding M3).
// It ends by itself at expires_at, the absolute lifetime (auth.h
// CB_AUTH_SESSION_MS); DynamoDB's TTL then removes the item.
typedef struct {
    cb_uuid id;
    cb_uuid user_id;
    int64_t created_at;
    int64_t expires_at;
    int64_t gen;        // the user's session_gen when it began
    bool    revoked;
} cb_session_row;

typedef struct {
    char    token_hash[65];   // lowercase hex SHA-256 of the refresh token
    cb_uuid user_id;
    cb_uuid session_id;
    int64_t created_at;
    bool    revoked;
    // When refresh_take rotated it (the FIRST take; later takes keep it), so
    // auth can give a retry of a lost refresh response GoTrue's 10 s grace.
    // 0 when never taken.
    int64_t revoked_at;
    // When the row may go (DynamoDB's TTL; SQLite deletes it at the next
    // write past it): its session's end while live, CB_AUTH_SPENT_TOKEN_MS
    // after its first use once spent, so the rows a session leaves behind are
    // bounded (security finding M2).
    int64_t expires_at;
} cb_refresh_row;

// Failed sign-ins at one account (auth.h CB_AUTH_FAIL_*): how many, when the
// last one was, and until when the account is locked. Kept by email,
// expiring by itself an hour after its last write (DynamoDB's TTL).
typedef struct {
    int32_t n;
    int64_t last_at;
    int64_t locked_until;   // 0: not locked
} cb_signin_fails;

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
    // DynamoDB's: the game's seat items are known to be within this row's
    // roster (written by the create or the commit that wrote the row; a seat
    // whose account is gone may be missing, none is extra). A commit fenced on
    // this row may then take the roster as the stored seats instead of reading
    // them (cb_game_commit prev_known). game_create and game_commit leave it
    // true; an imported row, a row written before 2026-10-18 and every SQLite
    // row read as false.
    bool     seats_exact;
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
    // The seats the row at expected_version holds (its roster's humans and
    // bots), given when that row's seats_exact was true: the fence makes them
    // the stored seats whenever the commit lands, and DynamoDB writes the
    // difference without first reading them (one Query less, under the
    // game's order lock). prev_known false: the store reads them.
    // prev_needs_bots is that row's needs_bots, read only with prev_known:
    // false there means the game has no SCAN marker, so on DynamoDB a commit
    // that writes no seat, no rating and needs_bots false is one conditional
    // UpdateItem instead of a transaction (store_dynamo.c, DESIGN.md "Store").
    bool     prev_known;
    bool     prev_needs_bots;
    int      n_prev_humans;
    cb_uuid  prev_humans[CB_MAX_SEATS];
    int      n_prev_bots;
    cb_uuid  prev_bots[CB_MAX_SEATS];
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

// What a snapshot query asks (PostgREST's game_snapshots shapes).
typedef struct {
    const char *viewer_id;   // REQUIRED: only snapshots whose player_ids contain it
    const char *game_id;     // NULL: any
    const char *id;          // NULL: any
    int         limit;       // 0: no limit
    bool        newest_first;
} cb_snapshot_query;

typedef bool (*cb_member_fn)(const cb_member_row *r, void *ud);
typedef bool (*cb_game_fn)(const cb_game_row *r, void *ud);
typedef bool (*cb_bot_fn)(const cb_bot_row *r, void *ud);
typedef bool (*cb_rating_fn)(const cb_rating_row *r, void *ud);
typedef bool (*cb_snapshot_fn)(const cb_snapshot_row *r, void *ud);
typedef bool (*cb_chat_fn)(const cb_chat_row *r, void *ud);
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
    // The new hash, and in the same write session_gen one up (every session
    // so far over) but keep_session (store.h cb_user_row).
    int (*user_set_password)(cb_store *s, const char *id, const char *password_hash, const char *keep_session);
    // session_gen one up, none kept (a global sign-out).
    int (*user_end_sessions)(cb_store *s, const char *id);
    int (*user_touch_sign_in)(cb_store *s, const char *id, int64_t at);
    // Deletes the user, their refresh tokens, memberships, rating row and chat
    // messages (the cascade auth.admin.deleteUser had). Snapshots keep their
    // player_ids, as today (DESIGN.md "Open defects carried over"); the app
    // has already redacted the name in their extras (snapshot_set_extras).
    int (*user_delete)(cb_store *s, const char *id);
    // Admin: users whose email or username contains `search` ("" for all),
    // newest first, from `offset`.
    int (*users_list)(cb_store *s, const char *search, int offset, int limit, cb_user_fn fn, void *ud);

    // failed sign-ins (auth.h): CB_E_NOT_FOUND when none are counted (or the
    // count has expired); put replaces the count, clear removes it (CB_OK
    // when there was none). Last writer wins: two failures at the same moment
    // may count once, which only ever errs by one.
    int (*signin_fails_get)(cb_store *s, const char *email, int64_t now_ms, cb_signin_fails *out);
    int (*signin_fails_put)(cb_store *s, const char *email, const cb_signin_fails *f, int64_t now_ms);
    int (*signin_fails_clear)(cb_store *s, const char *email);

    // refresh tokens
    int (*refresh_put)(cb_store *s, const cb_refresh_row *r);
    // Rotation: atomically marks the token revoked, stamps revoked_at = now_ms
    // unless a take already stamped it, moves its expires_at to now_ms +
    // spent_ms (a spent token is kept only that long, for reuse detection),
    // and returns its row as it was before. CB_E_NOT_FOUND for an unknown (or
    // expired) hash; a row with revoked = true was already used.
    int (*refresh_take)(cb_store *s, const char *token_hash, int64_t now_ms, int64_t spent_ms, cb_refresh_row *out);

    // sessions (cb_session_row). get: CB_E_NOT_FOUND when there is none or it
    // has expired, read strongly consistent. put: the row as given. revoke:
    // ended, in one write; a session with no row yet (one begun before
    // sessions were stored) gets one, revoked, for user_id until expires_at.
    int (*session_put)(cb_store *s, const cb_session_row *r);
    int (*session_get)(cb_store *s, const char *id, int64_t now_ms, cb_session_row *out);
    int (*session_revoke)(cb_store *s, const char *id, const char *user_id, int64_t expires_at);

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
    // ago may be missing for a moment, and "updated" there is when the seat
    // was taken (a commit writes only the seats that change): a caller that
    // needs the games' own order sorts the rows it loads, as PostgREST does.
    int (*games_of_user)(cb_store *s, const char *user_id, int limit, cb_member_fn fn, void *ud);
    // The rows of `ids` for a list of views (the dashboard): every column but
    // the session log (logs NULL, logs_len 0), read consistently. fn once per
    // game that exists, in the order of ids (a missing id is skipped); false
    // from fn stops. The number of rows handed to fn, or a CB_E_*. DynamoDB
    // reads them in BatchGetItems of up to 100 keys, not one GetItem each.
    int (*games_get_views)(cb_store *s, const char *const *ids, int n, cb_game_fn fn, void *ud);
    // 1 a member, 0 not, or a CB_E_*.
    int (*is_member)(cb_store *s, const char *game_id, const char *user_id);
    // The humans seated at the game (bots are not members here), read
    // consistently: fn once with each user id, in no particular order; false
    // from fn stops. The number handed to fn, or a CB_E_* (no game is 0). A
    // game's chat is published to each of them (realtime.h rt_publish_chat).
    int (*game_members)(cb_store *s, const char *game_id, cb_name_fn fn, void *ud);
    // The heartbeat's scan: needs_bots, updated_at < stale_before,
    // last_commit_at > active_after, at most `limit`.
    int (*games_needing_bots)(cb_store *s, int64_t stale_before, int64_t active_after, int limit, cb_name_fn fn, void *ud);
    // The bot lease (today's try_acquire/renew/release_bot_lease): a random
    // token valid until now + ttl_ms. acquire: CB_OK and the token, or
    // CB_E_CONFLICT while another holder's lease is live (CB_E_NOT_FOUND: no
    // such game, or a game with no bot work, which has no lease to take).
    // renew: CB_OK, or CB_E_CONFLICT when the token no longer holds it.
    // release: CB_OK (a lease that is not the token's is left alone). A
    // commit that leaves bot work keeps the lease. acquire and renew move the
    // scan's staleness clock (games_needing_bots' updated_at) to `now`, never
    // last_commit_at, as the update_games_updated_at trigger did, so the scan
    // sees a drive in flight. SQLite keeps that clock on the row, so game_get
    // sees it move; DynamoDB keeps it, and the lease, on the game's small
    // SCAN marker (1 WCU a write), so the row's updated_at is its last commit's.
    int (*lease_acquire)(cb_store *s, const char *game_id, int64_t now, int64_t ttl_ms, cb_uuid token_out);
    int (*lease_renew)(cb_store *s, const char *game_id, const char *token, int64_t now, int64_t ttl_ms);
    int (*lease_release)(cb_store *s, const char *game_id, const char *token);
    // A drive found that NO BOT CAN ACT on the row at `version` (table.h
    // table_bots_due) although the row is in the scan, which a row written
    // before the scan asked that question can be: takes the game out of the
    // scan, unless a commit has moved the row past `version` (that commit
    // wrote the scan's answer itself). SQLite clears the row's needs_bots;
    // DynamoDB deletes the SCAN marker (and the lease on it), fenced on the
    // row version the marker carries, and leaves the row's own needs_bots to
    // its next commit (the row is 4 to 8 KB a write, the marker 1 WCU).
    // CB_OK, or CB_E_CONFLICT when the row has moved on (SQLite: or is gone).
    int (*bots_idle)(cb_store *s, const char *game_id, int64_t version);
    // 1 while some drive's lease on the game is live at `now` (acquire would
    // answer CB_E_CONFLICT), 0 when it is free or there is none (no game, or
    // no bot work), or a CB_E_*. A consistent read: a kick asks it after its
    // commit, so it sees any lease taken before (app/bots.c kick_wanted).
    int (*lease_held)(cb_store *s, const char *game_id, int64_t now);

    // bots and ratings
    int (*bots_list)(cb_store *s, cb_bot_fn fn, void *ud);                 // ladder order
    // EVERY rating row the leaderboard index holds, highest first: the played
    // ones, and on DynamoDB any row still in the layout before the sparse key
    // (which cbase-import --reindex-ratings takes out). A read that grows with
    // the players, for that tool and tests, never for a request.
    int (*ratings_list)(cb_store *s, cb_rating_fn fn, void *ud);
    // The leaderboard's read: the players who have played (games_played > 0),
    // highest rating first, then user id, at most `limit` of them. Bounded
    // whatever the number of sign-ups: on DynamoDB it is one Query of `limit`
    // items on GSI2's rating-ordered key, which only played rows carry
    // (json_surface.h "THE ITEM LAYOUT", DESIGN.md "Edge cache").
    int (*ratings_top)(cb_store *s, int limit, cb_rating_fn fn, void *ud);
    // seat_ratings: ids[i]'s rating (a bot's, else a user's), or -1
    // (TABLE_RATING_NONE) when it has none, into out[i].
    int (*ratings_for)(cb_store *s, const char *const *ids, int n, int32_t *out);
    // The leaderboard name; NULL clears it (delete-account).
    int (*rating_set_username)(cb_store *s, const char *user_id, const char *username);

    // snapshots
    int (*snapshot_put)(cb_store *s, const cb_snapshot_row *r);
    int (*snapshots_query)(cb_store *s, const cb_snapshot_query *q, cb_snapshot_fn fn, void *ud);
    // Account deletion: one replay's extras replaced (the kernel's
    // replay_extras_redact), on every copy of the row; the moves, player_ids,
    // game and date stay. extras_len 0 clears them. CB_E_NOT_FOUND when the
    // snapshot is gone. A user's snapshots are snapshots_query by viewer_id.
    int (*snapshot_set_extras)(cb_store *s, const char *id, const uint8_t *extras, size_t extras_len);

    // chat
    int (*chat_put)(cb_store *s, const cb_chat_row *r);
    // Oldest first, the newest `limit` (0: all).
    int (*chat_list)(cb_store *s, const char *game_id, int limit, cb_chat_fn fn, void *ud);

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
    // The row's leaderboard index key brought to the current layout, the
    // values untouched and only while they are still r's (a commit that
    // raced it already wrote the new key): CB_OK, or CB_E_CONFLICT when the
    // row changed, is gone, or already has that key. SQLite has no index
    // key, so every row already has it: CB_E_CONFLICT.
    int (*rating_reindex)(cb_store *s, const cb_rating_row *r);
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
// The store CBASE_STORE names ("dynamo", else SQLite), or `fallback` when it
// names none: what each provider's store_open is (cbase/provider.h). The
// return of the opener it picked.
int cb_store_open_named(const char *store, const char *fallback, const char *db_path, const char *table, cb_store **out);
void cb_store_close(cb_store *s);

// What the calling thread has sent DynamoDB since its last take: every request
// (each attempt of a retried one, as DynamoDB bills it), reads and writes, and
// "GetItem 1 Query 3 TransactWriteItems 1" in `detail`, and the read and
// write capacity units DynamoDB says they consumed. The handler and the
// bot drive log it per request and per cycle at debug level, the measure of
// what an action costs in requests. Zero on SQLite. Resets the tally.
typedef struct {
    int    reads, writes;
    double rcu, wcu;   // the capacity units DynamoDB reported (ReturnConsumedCapacity)
    char   detail[192];
} cb_store_tally;
void cb_store_tally_take(cb_store_tally *out);

// Throttling (DynamoDB; DESIGN.md "The free tier, provisioned"). The table's
// capacity is provisioned, so a burst past it is throttled rather than
// billed: a request answered ProvisionedThroughputExceededException,
// ThrottlingException or RequestLimitExceeded, a transaction cancelled for
// throttling, and a batch left partly unprocessed are sent again after a
// jittered exponential pause, while the calling thread's deadline allows.
// The deadline is the Lambda invocation's (main_lambda.c sets it around each
// invocation); with none, each request gets CB_STORE_RETRY_WALL_MS of pauses.
// A request still throttled at its deadline fails as CB_E_BUSY: the HTTP
// routes answer it 503 with Retry-After (ASK_WHY_BUSY), and the clients send a
// read again (netclient.h nc_retry_ms). The backend marks the give-up on the
// calling thread (cb_store_mark_busy) and CB_STORE_CALL turns the store
// function's CB_E_DB into CB_E_BUSY, so no backend function maps it itself.
#define CB_STORE_RETRY_WALL_MS 5000
void cb_store_set_deadline(int64_t deadline_ms);   // epoch ms; 0: none
int64_t cb_store_deadline(void);                    // this thread's, as set

// Two independent reads at once (a route that needs both and neither needs
// the other's answer): cb_store_side_start runs fn(ud) on a thread of its own,
// with this thread's store deadline, while the caller makes its own read;
// cb_store_side_join waits for it. A thread that cannot be started runs fn at
// once, in the caller: the same answers, one after the other. fn must touch
// only what ud gives it, and the store, which is safe from any thread.
typedef struct {
    pthread_t th;
    bool      started;
    void    (*fn)(void *ud);
    void     *ud;
    int64_t   deadline;
} cb_store_side;
void cb_store_side_start(cb_store_side *s, void (*fn)(void *ud), void *ud);
void cb_store_side_join(cb_store_side *s);
// This process's throttling since it started (the admin overview shows it;
// under Lambda it is the warm container's, and every throttle is also a WARN
// in the log): responses that were throttled, and requests given up on.
typedef struct {
    uint64_t throttled, gave_up;
} cb_store_throttles;
void cb_store_throttles_get(cb_store_throttles *out);

// The SQLite schema version this binary applies (store_sqlite.c), and the
// embedded schema text (build/gen/schema_sql.c, from sql/schema.sql).
#define CB_SCHEMA_VERSION 4
extern const char cbase_schema_sql[];
extern const unsigned long cbase_schema_sql_len;

// games_get_views for a store with no round trip to save (SQLite, the tests'
// memory store): game_get for each id, its log dropped.
int cb_store_games_get_each(cb_store *s, const char *const *ids, int n, cb_game_fn fn, void *ud);

// ---- wrappers: call these, not s->ops->... ------------------------------------------
//
//   int rc = CB_STORE_CALL(ctx->store, game_get, id, &row);
//
// A backend that leaves an entry NULL answers CB_E_NOTIMPL for it.
#define CB_STORE_CALL(s, fn, ...) \
    (cb_store_begin_(), cb_store_end_((s)->ops->fn ? (s)->ops->fn((s), __VA_ARGS__) : CB_E_NOTIMPL))

// CB_STORE_CALL's two halves (store.c), the comma sequencing begin before the
// call: the outermost begin clears this thread's busy mark, and end answers
// rc, or CB_E_BUSY for a CB_E_DB that came with the mark (a store function
// that calls another through CB_STORE_CALL nests; the mark stays until the
// outermost call ends). cb_store_mark_busy is the backend's: store_dynamo.c
// calls it when it gives up on a throttled request.
void cb_store_begin_(void);
int  cb_store_end_(int rc);
void cb_store_mark_busy(void);

#endif
