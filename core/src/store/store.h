// store.h - the platform's store, behind one interface with a backend per
// provider (providers/local: SQLite; providers/aws: DynamoDB).
//
// The interface is a table of function pointers (cb_store_ops): the accounts
// and the auth rows, and the admin console's reads. SQLite keeps them in
// plain tables (providers/local/sql/schema.sql, store_sqlite.c); DynamoDB in
// items (items.h, the item layer only that backend implements) written by
// rows_auth.c. A product's own rows are its own table (cb_store_app below):
// on SQLite its SQL tables (store_sql.h), on DynamoDB its items; foolish's
// are foolish/server/src/store/rows_sql.c and rows_foolish.c. Callers use the
// wrappers. Every function is safe to call from several threads at once
// (serve runs handlers on a pool).
//
// CONVENTIONS. Times are ms since the Unix epoch. Ids are canonical lowercase
// UUID text (CB_UUID_LEN). A list function calls `fn` once per row, in the documented order,
// with a row the callee may read but not keep (copy what you keep); fn returns
// false to stop early. Every function returns CB_OK, a count, or a CB_E_* code:
// CB_E_NOT_FOUND, CB_E_CONFLICT (a unique key or a CAS lost), CB_E_DB (the
// backend failed; it has logged why), CB_E_NOTIMPL (a stub).
#ifndef CB_STORE_H
#define CB_STORE_H

#include "items.h"
#include "util/err.h"
#include "util/uuid.h"

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CB_EMAIL_MAX     320
#define CB_USERNAME_MAX  64    // == kernel ROSTER_NAME_MAX
#define CB_HASH_MAX      128   // a bcrypt hash is 60
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

typedef bool (*cb_name_fn)(const char *name, void *ud);   // an id, a collection name
typedef bool (*cb_user_fn)(const cb_user_row *r, void *ud);
// The admin console's raw browse: one row as name/value text pairs.
#define CB_ADMIN_GAMES "games"   // see admin_collections
typedef bool (*cb_admin_row_fn)(int n, const char *const *names, const char *const *values, void *ud);

// ---- the interface ------------------------------------------------------------------

typedef struct cb_store cb_store;
struct cb_sql;

typedef struct cb_store_ops {
    const char *name;   // "sqlite", "dynamo"
    void (*close)(cb_store *s);
    // Readiness (GET /ready): one point read that touches the database. CB_OK when it answered.
    int (*ready)(cb_store *s);

    // users
    int (*user_get)(cb_store *s, const char *id, cb_user_row *out);
    int (*user_by_email)(cb_store *s, const char *email, cb_user_row *out);
    // CB_E_CONFLICT when the email is taken. The product's signup items go
    // in the same transaction (cb_store_app signup_txn).
    int (*user_create)(cb_store *s, const cb_user_row *u);
    // The new hash, and in the same write session_gen one up (every session
    // so far over) but keep_session (store.h cb_user_row).
    int (*user_set_password)(cb_store *s, const char *id, const char *password_hash, const char *keep_session);
    // session_gen one up, none kept (a global sign-out).
    int (*user_end_sessions)(cb_store *s, const char *id);
    int (*user_touch_sign_in)(cb_store *s, const char *id, int64_t at);
    // Deletes the user and their refresh tokens, and what the product hangs
    // off the account (cb_store_app delete_keys and delete_txn), the cascade
    // auth.admin.deleteUser had. The product has already redacted what it
    // keeps of the user elsewhere.
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

    // migration (foolish/server/migrate/import.c): the user as given, insert
    // or replace, with no side effects: unlike user_create, no product items.
    int (*user_import)(cb_store *s, const cb_user_row *u);

    // THE ITEM LAYER (items.h): DynamoDB's only (rows_auth.c, the product's
    // item rows, and cbase-export's walk of a table). NULL on SQLite.
    int (*item_get)(cb_store *s, cb_ikey key, bool consistent, cb_item *out);
    int (*item_write)(cb_store *s, const cb_iwrite *w, cb_item *old);
    int (*item_txn)(cb_store *s, const cb_iwrite *w, int n, int *failed, bool *failed_had_item);
    int (*item_query)(cb_store *s, const cb_iquery *q, cb_item_fn fn, void *ud);
    int (*item_batch_get)(cb_store *s, const cb_ikey *keys, int n, const char *const *proj, int n_proj,
                          cb_item_at_fn fn, void *ud);
    // The items at keys[0..n) deleted, unconditionally and not atomically
    // (DynamoDB: BatchWriteItem, 25 a request, again while some are left).
    int (*item_batch_delete)(cb_store *s, const cb_ikey *keys, int n);
    // Every item's pk and sk, consistently, in no set order: an export walk
    // (DynamoDB: a keys-only Scan in pages of 100, paced in bulk mode). fn
    // answers false to stop; it may call the store.
    int (*item_keys)(cb_store *s, cb_ikey_fn fn, void *ud);
} cb_store_ops;

// WHAT A PRODUCT ADDS TO EVERY STORE (docs/PLATFORM.md 2.2 and 2.5): its own
// rows, and its part of an account. The product's app names it
// (cbase/app.h cb_app store), and every backend installs it at open
// (cb_store_init), so a store opened anywhere (serve, the Lambda, a tool, a
// test) holds the product's rows; a test double installs its own. Any hook
// may be NULL.
typedef struct cb_store_app {
    // The product's operations table, opaque here: the product's own call
    // macro reads it (foolish: CB_ROWS_CALL).
    const void *rows;
    // Signup: acct[0..n) are the account's writes (EMAIL, PROFILE); the hook
    // sends them in ONE transaction with the product's own after them, as
    // item_txn does (*failed the refused write's index). NULL: the account's
    // alone.
    int (*signup_txn)(cb_store *s, const cb_user_row *u, const cb_iwrite *acct, int n, int *failed);
    // Delete, first: every item of the product's that hangs off the user,
    // each handed to key(it, ud) (it carries pk and sk), for the batch delete
    // the account's refresh tokens go in.
    int (*delete_keys)(cb_store *s, const char *user_id, cb_item_fn key, void *ud);
    // Delete, last: acct[0..n) are the account's deletes (PROFILE, EMAIL);
    // the hook sends them in ONE transaction with the product's own. NULL:
    // the account's alone.
    int (*delete_txn)(cb_store *s, const cb_user_row *u, const cb_iwrite *acct, int n);
    // The product's seed data, written when the store has none: at open by a
    // backend that seeds itself (SQLite), and by a deploy's import tool
    // (DynamoDB). The number written, 0 when there was some, or a CB_E_*.
    int (*seed)(cb_store *s);

    // ON THE SQL STORE (store_sql.h): the product's rows table there (what
    // cb_rows_of answers for a SQLite store), its tables (applied after the
    // platform's, in the same transaction, at every open), and its prepared
    // statements by index (cb_sql_stmt).
    const void         *sql_rows;
    const char         *sql_schema;
    const char *const  *sql_stmts;
    int                 n_sql_stmts;
    // Signup: the product's rows of a new account, in the account's
    // transaction (an error rolls the account back). Delete: every row of the
    // product's that hangs off the user, in the delete's transaction.
    int (*sql_signup)(struct cb_sql *x, const cb_user_row *u);
    int (*sql_delete)(struct cb_sql *x, const char *user_id);
} cb_store_app;

struct cb_store {
    const cb_store_ops *ops;
    void *impl;
    const cb_store_app *app;   // the product's (cb_store_init), NULL for none
    // A product layer that holds acknowledged writes in front of the store (a product's
    // move journal under `cbase serve`): its state, and how to make every write it
    // acknowledged readable from the store's own file (cb_store_settle). NULL: none.
    void *app_state;
    int (*app_settle)(struct cb_store *s);
};

// Every write acknowledged so far is in the store's file: what a copy of the file
// (a backup) and a reader of the file behind the product's back (the admin's SQL)
// must call first. CB_OK when nothing holds writes in front of the store.
int cb_store_settle(cb_store *s);

// A backend's open: ops and impl, and the linked product's cb_store_app.
void cb_store_init(cb_store *s, const cb_store_ops *ops, void *impl);

// Opens SQLite at `path`, applying providers/local/sql/schema.sql (embedded) under a
// PRAGMA user_version check. CB_OK and *out, or a CB_E_*.
int cb_store_open_sqlite(const char *path, cb_store **out);
// Gives back what the calling thread's local store keeps between requests (write-scratch items) and
// what the process-wide block pools hold. Nothing is needed for correctness; a leak-counting test
// calls it after a request, and a process may at exit.
void cb_store_local_drain(void);
// Finalizes the prepared statements a local store keeps (they are re-prepared on first use) and has
// the library give back its cached memory. A statement holds its compiled program and the library re-compiles a
// cached one whenever a bound value changes its plan, so what it keeps is neither a leak nor a
// constant: a leak-counting test calls this after a request. No-op for another backend.
void cb_store_local_release(cb_store *s);
// The bytes of page cache the local store's database holds (0 for another backend). A row written may
// take a new page, which the library allocates and keeps for as long as the data lives: a leak-counting test
// allows the blocks that growth explains (a page and its header), and not one more.
long cb_store_local_cache_bytes(cb_store *s);
// The blocks the local store's database library holds now, and in *fresh the ones handed out since the last
// cb_store_local_mark. A test marks when it arms the failing allocator: the allocator's own count then
// splits into the library's (page cache churn included) and the code under test's.
void cb_store_local_mark(void);
// Transactions the local store has committed since it opened (0 for another backend): a backup compares two
// readings to know whether anything changed.
unsigned long cb_store_local_changes(cb_store *s);
// WAL frames (a page and a 24-byte header each) the local store has appended since it opened (0 for another
// backend or a database without a WAL): the bytes a write costs on disk before any checkpoint.
unsigned long cb_store_local_wal_frames(cb_store *s);
long cb_store_local_blocks(long *fresh);
// Opens the DynamoDB table `table` in the region and with the credentials the
// Lambda environment provides (aws/sigv4.h cb_aws_creds_from_env).
int cb_store_open_dynamo(const char *table, cb_store **out);
// The store CBASE_STORE names ("dynamo", else SQLite), or `fallback` when it
// names none: what each provider's store_open is (cbase/provider.h). The
// return of the opener it picked.
int cb_store_open_named(const char *store, const char *fallback, const char *db_path, const char *table, cb_store **out);
void cb_store_close(cb_store *s);

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
// embedded schema text (build/gen/schema_sql.c, from providers/local/sql/schema.sql).
#define CB_SCHEMA_VERSION 8
extern const char cbase_schema_sql[];
extern const unsigned long cbase_schema_sql_len;

// ---- wrappers: call these, not s->ops->... ------------------------------------------
//
//   int rc = CB_STORE_CALL(ctx->store, user_get, id, &row);
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
