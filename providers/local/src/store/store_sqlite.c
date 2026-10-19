// store_sqlite.c - the SQLite backend (store.h): `cbase serve`, the tests, the
// import rehearsal. Plain tables (providers/local/sql/schema.sql): the
// accounts and the auth rows are written here as SQL, the product's rows by
// the product against store_sql.h (its tables in the same database, applied
// after the platform's), and the admin console browses the tables.
//
// One connection (SQLITE_OPEN_FULLMUTEX), WAL, synchronous=NORMAL, a busy
// timeout for a second process (cbase-import beside a running serve).
// The schema is applied in one transaction at every open, then PRAGMA
// user_version = CB_SCHEMA_VERSION; a database whose user_version is newer is
// refused.
//
// THREADS. serve calls the store from a pool, and one SQLite connection runs
// one transaction at a time, so every operation holds `mu` from its first
// statement to its last: a transaction can never pick up another thread's
// statements. A read collects its rows under the lock and hands them out after
// releasing it, so a callback may call the store again. Writes go through the
// group commit (cb_sql_write, below).
#include "store/store.h"
#include "store/store_priv.h"
#include "store/store_sql.h"

#include "util/bytea.h"
#include "util/clock.h"
#include "util/log.h"
#include "util/str.h"
#include "../../../../../shared/c/hex/hex.h"
#include "../../../../../shared/c/num_parse.h"

#include <ctype.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MOD "store"

extern const char cbase_schema_sql[];

#define FAILS_TTL_MS (3600LL * 1000)   // store.h cb_signin_fails: an hour after the last write

// ---- the statements ------------------------------------------------------------------

enum {
    ST_USER_GET, ST_USER_BY_EMAIL, ST_USER_INSERT, ST_USER_IMPORT, ST_USER_PASSWORD, ST_USER_END, ST_USER_TOUCH,
    ST_USER_DELETE, ST_USER_DEL_REFRESH, ST_USER_DEL_SESSIONS, ST_USERS_ALL,
    ST_SESSION_PUT, ST_SESSION_SWEEP, ST_SESSION_GET, ST_SESSION_REVOKE,
    ST_REFRESH_PUT, ST_REFRESH_SWEEP, ST_REFRESH_GET, ST_REFRESH_TAKE,
    ST_FAILS_GET, ST_FAILS_PUT, ST_FAILS_CLEAR,
    ST_READY, ST_TABLES,
    ST_BEGIN, ST_COMMIT, ST_ROLLBACK, ST_SAVEPOINT, ST_RELEASE, ST_ROLLBACK_TO,   // transaction control, prepared once
    ST__COUNT
};

// A row type's columns are listed once with a name per position: the bind parameter of a statement that
// writes the list in order, and (less one, COL) the result column of one that selects it. A statement of
// one or two keys numbers them in place (?1, ?2).
#define XS_(x) #x
#define XS(x) XS_(x)
#define P(n) "?" XS(n)
#define COL(n) ((n) - 1)
#define USER_COLS "id, email, password_hash, username, created_at, last_sign_in_at, session_gen, kept_session"
#define U_ID          1
#define U_EMAIL       2
#define U_HASH        3
#define U_USERNAME    4
#define U_CREATED_AT  5
#define U_SIGNED_IN   6
#define U_SESSION_GEN 7
#define U_KEPT        8
#define USER_VALS P(U_ID) ", " P(U_EMAIL) ", " P(U_HASH) ", " P(U_USERNAME) ", " P(U_CREATED_AT) ", " P(U_SIGNED_IN) ", " \
                  P(U_SESSION_GEN) ", " P(U_KEPT)

static const char *const SQL[ST__COUNT] = {
    [ST_USER_GET] = "SELECT " USER_COLS " FROM users WHERE id = ?1",
    [ST_USER_BY_EMAIL] = "SELECT " USER_COLS " FROM users WHERE email = ?1",
    [ST_USER_INSERT] = "INSERT INTO users (" USER_COLS ") VALUES (" USER_VALS ")",
    // Replaced whole, but the larger session generation wins and kept_session goes with it: re-importing must
    // not turn a global sign-out back into live sessions. (SET's right-hand sides read the row as it was.)
    [ST_USER_IMPORT] = "INSERT INTO users (" USER_COLS ") VALUES (" USER_VALS ") "
                       "ON CONFLICT (id) DO UPDATE SET email = excluded.email, password_hash = excluded.password_hash, "
                       "username = excluded.username, created_at = excluded.created_at, "
                       "last_sign_in_at = excluded.last_sign_in_at, "
                       "kept_session = CASE WHEN users.session_gen >= excluded.session_gen THEN users.kept_session ELSE excluded.kept_session END, "
                       "session_gen = max(users.session_gen, excluded.session_gen)",
    [ST_USER_PASSWORD] = "UPDATE users SET password_hash = ?2, session_gen = session_gen + 1, kept_session = ?3 WHERE id = ?1",
    [ST_USER_END] = "UPDATE users SET session_gen = session_gen + 1, kept_session = NULL WHERE id = ?1",
    [ST_USER_TOUCH] = "UPDATE users SET last_sign_in_at = ?2 WHERE id = ?1",
    [ST_USER_DELETE] = "DELETE FROM users WHERE id = ?1",
    [ST_USER_DEL_REFRESH] = "DELETE FROM refresh_tokens WHERE user_id = ?1",
    [ST_USER_DEL_SESSIONS] = "DELETE FROM sessions WHERE user_id = ?1",
    [ST_USERS_ALL] = "SELECT " USER_COLS " FROM users ORDER BY created_at DESC, id DESC",
    [ST_SESSION_PUT] = "INSERT OR REPLACE INTO sessions (id, user_id, created_at, expires_at, gen, revoked) VALUES (?1, ?2, ?3, ?4, ?5, ?6)",
    [ST_SESSION_SWEEP] = "DELETE FROM sessions WHERE user_id = ?1 AND expires_at <= ?2",
    [ST_SESSION_GET] = "SELECT id, user_id, created_at, expires_at, gen, revoked FROM sessions WHERE id = ?1",
    [ST_SESSION_REVOKE] = "INSERT INTO sessions (id, user_id, created_at, expires_at, gen, revoked) VALUES (?1, ?2, 0, ?3, 0, 1) "
                          "ON CONFLICT (id) DO UPDATE SET revoked = 1",
    [ST_REFRESH_PUT] = "INSERT INTO refresh_tokens (token_hash, user_id, session_id, created_at, revoked, revoked_at, expires_at) "
                       "VALUES (?1, ?2, ?3, ?4, ?5, 0, ?6)",
    [ST_REFRESH_SWEEP] = "DELETE FROM refresh_tokens WHERE user_id = ?1 AND expires_at > 0 AND expires_at <= ?2",
    [ST_REFRESH_GET] = "SELECT token_hash, user_id, session_id, created_at, revoked, revoked_at, expires_at "
                       "FROM refresh_tokens WHERE token_hash = ?1",
    [ST_REFRESH_TAKE] = "UPDATE refresh_tokens SET revoked = 1, revoked_at = CASE WHEN revoked_at = 0 THEN ?2 ELSE revoked_at END, "
                        "expires_at = ?3 WHERE token_hash = ?1",
    [ST_FAILS_GET] = "SELECT n, last_at, locked_until, expires_at FROM signin_fails WHERE email = ?1",
    [ST_FAILS_PUT] = "INSERT OR REPLACE INTO signin_fails (email, n, last_at, locked_until, expires_at) VALUES (?1, ?2, ?3, ?4, ?5)",
    [ST_FAILS_CLEAR] = "DELETE FROM signin_fails WHERE email = ?1",
    [ST_READY] = "SELECT 1 FROM users WHERE id = zeroblob(" XS(CB_UUID_BYTES) ")",
    [ST_TABLES] = "SELECT name FROM sqlite_schema WHERE type = 'table' AND name NOT LIKE 'sqlite_%' ORDER BY name",
    [ST_BEGIN] = "BEGIN IMMEDIATE",
    [ST_COMMIT] = "COMMIT",
    [ST_ROLLBACK] = "ROLLBACK",
    [ST_SAVEPOINT] = "SAVEPOINT g",
    [ST_RELEASE] = "RELEASE g",
    [ST_ROLLBACK_TO] = "ROLLBACK TO g",
};

// A write waiting for its group (see "group commit" below).
typedef struct wreq {
    struct wreq *next;
    cb_sql_fn    fn;
    void        *ud;
    int          rc;
    bool         done;      // under qmu
} wreq;

struct cb_sql {
    sqlite3        *db;
    atomic_ulong    changes;    // transactions committed: the backup job's "did anything change"
    pthread_mutex_t mu;         // the connection: held from a transaction's first statement to its last
    sqlite3_stmt   *st[ST__COUNT];
    const char *const *app_sql; // the product's statements (cb_store_app sql_stmts)
    sqlite3_stmt  **app_st;
    int             n_app;
    pthread_mutex_t qmu;        // the write queue only; never held across SQLite
    pthread_cond_t  cv;
    wreq           *qhead, *qtail;
    bool            leader;     // a thread is running a group
};
typedef struct cb_sql sq;

#define Q(s) ((sq *)(s)->impl)

// SQLite's answer as a store code. A unique or primary key is a conflict, a
// foreign key names a row that is not there; anything else is logged.
static int fail(sq *q, const char *what, int rc) {
    int ext = sqlite3_extended_errcode(q->db);
    if (ext == SQLITE_CONSTRAINT_UNIQUE || ext == SQLITE_CONSTRAINT_PRIMARYKEY) return CB_E_CONFLICT;
    if (ext == SQLITE_CONSTRAINT_FOREIGNKEY) return CB_E_NOT_FOUND;
    if (rc == SQLITE_NOMEM || ext == SQLITE_NOMEM) return CB_E_NOMEM;
    CB_ERROR(MOD, "%s: %s (%d)", what, sqlite3_errmsg(q->db), rc);
    return CB_E_DB;
}

// The statement `sql` kept at *slot, reset and unbound, prepared on first use. NULL after logging.
static sqlite3_stmt *prep(sq *q, sqlite3_stmt **slot, const char *sql) {
    sqlite3_stmt *s = *slot;
    if (!s) {
        int rc = sqlite3_prepare_v3(q->db, sql, -1, SQLITE_PREPARE_PERSISTENT, slot, NULL);
        if (rc != SQLITE_OK) {
            if (rc != SQLITE_NOMEM) CB_ERROR(MOD, "prepare %.60s: %s", sql, sqlite3_errmsg(q->db));
            *slot = NULL;
            return NULL;
        }
        return *slot;
    }
    sqlite3_reset(s);
    sqlite3_clear_bindings(s);
    return s;
}

static sqlite3_stmt *stmt(sq *q, int id) { return prep(q, &q->st[id], SQL[id]); }

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

// Steps s to its end: CB_OK, or the store's code. Resets it.
static int run(sq *q, sqlite3_stmt *s, const char *what) {
    if (!s) return CB_E_DB;
    int r = sqlite3_step(s);
    int rc = r == SQLITE_DONE || r == SQLITE_ROW ? CB_OK : fail(q, what, r);
    done(s);
    return rc;
}

// A prepared control statement (BEGIN, COMMIT, ...): no parse per transaction.
static int ctl(sq *q, int id) { return run(q, stmt(q, id), SQL[id]); }

// BEGIN, and with it the statements that must run for a transaction to end: ROLLBACK, and the
// savepoint pair of a group. They are prepared lazily and re-prepared after a release, so under memory
// pressure the prepare could fail only once the transaction was open, leaving it open for good (every
// later BEGIN answered "within a transaction"). Preparing them here fails the whole transaction
// before it has changed anything.
static int begin(sq *q) {
    int rc = ctl(q, ST_BEGIN);
    if (rc != CB_OK) return rc;
    if (!stmt(q, ST_ROLLBACK) || !stmt(q, ST_ROLLBACK_TO) || !stmt(q, ST_RELEASE)) {
        if (!sqlite3_get_autocommit(q->db)) sqlite3_exec(q->db, "ROLLBACK", NULL, NULL, NULL);
        return CB_E_NOMEM;
    }
    return CB_OK;
}
static int commit(sq *q) {
    int rc = ctl(q, ST_COMMIT);
    if (rc == CB_OK) atomic_fetch_add_explicit(&q->changes, 1, memory_order_relaxed);
    return rc;
}
static void rollback(sq *q) {
    if (!sqlite3_get_autocommit(q->db)) ctl(q, ST_ROLLBACK);
}

static void lock(sq *q) { pthread_mutex_lock(&q->mu); }
static void unlock(sq *q) { pthread_mutex_unlock(&q->mu); }

// ---- store_sql.h: what a product's rows use ------------------------------------------------

static const cb_store_ops SQLITE_OPS;

bool cb_store_is_sql(const cb_store *s) { return s && s->ops == &SQLITE_OPS; }

sqlite3_stmt *cb_sql_stmt(cb_sql *x, int id) {
    if (id < 0 || id >= x->n_app || !x->app_sql[id]) {
        CB_ERROR(MOD, "no product statement %d", id);
        return NULL;
    }
    return prep(x, &x->app_st[id], x->app_sql[id]);
}
int cb_sql_run(cb_sql *x, sqlite3_stmt *st, const char *what) { return run(x, st, what); }
int cb_sql_fail(cb_sql *x, const char *what, int rc) { return fail(x, what, rc); }
int cb_sql_changes(cb_sql *x) { return sqlite3_changes(x->db); }
int cb_sql_exec(cb_sql *x, const char *sql) {
    int rc = sqlite3_exec(x->db, sql, NULL, NULL, NULL);
    return rc == SQLITE_OK ? CB_OK : fail(x, "exec", rc);
}

bool cb_sql_bind_uuid(sqlite3_stmt *st, int i, const char *id) {
    unsigned char b[CB_UUID_BYTES];
    if (!id || !cb_uuid_pack(id, b)) {
        sqlite3_bind_null(st, i);
        return false;
    }
    sqlite3_bind_blob(st, i, b, CB_UUID_BYTES, SQLITE_TRANSIENT);
    return true;
}
void cb_sql_bind_text_or_null(sqlite3_stmt *st, int i, const char *v) {
    if (v && *v) sqlite3_bind_text(st, i, v, -1, SQLITE_TRANSIENT);
    else sqlite3_bind_null(st, i);
}
void cb_sql_bind_bytes(sqlite3_stmt *st, int i, const void *p, size_t n) {
    sqlite3_bind_blob64(st, i, n ? p : (const void *)"", n, SQLITE_TRANSIENT);
}
void cb_sql_col_uuid(sqlite3_stmt *st, int c, char out[CB_UUID_LEN + 1]) {
    const void *b = sqlite3_column_blob(st, c);
    if (b && sqlite3_column_bytes(st, c) == CB_UUID_BYTES) cb_uuid_unpack(b, out);
    else out[0] = '\0';
}
void cb_sql_col_text(sqlite3_stmt *st, int c, char *out, size_t cap) {
    const unsigned char *t = sqlite3_column_text(st, c);
    cb_strlcpy(out, t ? (const char *)t : "", cap);
}
bool cb_sql_col_bytes(sqlite3_stmt *st, int c, uint8_t **out, size_t *len) {
    *out = NULL;
    *len = 0;
    const void *b = sqlite3_column_blob(st, c);
    size_t n = (size_t)sqlite3_column_bytes(st, c);
    if (!b || !n) return true;
    *out = malloc(n);
    if (!*out) return false;
    memcpy(*out, b, n);
    *len = n;
    return true;
}

int cb_sql_read(cb_store *s, cb_sql_fn fn, void *ud) {
    sq *q = Q(s);
    lock(q);
    int rc = fn(q, ud);
    unlock(q);
    return rc;
}

// ---- group commit ---------------------------------------------------------------------------
//
// Writes (a move's commit and its rows) queue instead of each taking the
// connection for a transaction of its own. The first thread to find no group
// running becomes the leader: it takes everything queued at that moment, runs
// it as ONE transaction (BEGIN, a SAVEPOINT per request so a refused write
// undoes only that request, COMMIT) and hands each caller its own result. A
// caller returns only after the COMMIT, so a move the player saw accepted is in
// the WAL; and the connection stays locked from BEGIN to COMMIT, so a reader
// never sees rows of a group that has not committed. A lone request runs as
// the plain transaction it always was (no savepoint). If the COMMIT fails,
// every request of the group that had succeeded reports the failure.
// There is no dedicated thread: under no contention a write costs what it did
// (no hand-off), and under contention the waiters' transactions merge.

// Called with `mu` held.
static void run_group(sq *q, wreq *list) {
    int rc = begin(q);
    if (rc != CB_OK) {
        for (wreq *r = list; r; r = r->next) r->rc = rc;
        return;
    }
    bool single = list->next == NULL;
    for (wreq *r = list; r; r = r->next) {
        if (!single && (r->rc = ctl(q, ST_SAVEPOINT)) != CB_OK) continue;
        r->rc = r->fn(q, r->ud);
        if (single) break;
        if (r->rc != CB_OK) ctl(q, ST_ROLLBACK_TO);
        ctl(q, ST_RELEASE);
    }
    if (single && list->rc != CB_OK) { rollback(q); return; }
    rc = commit(q);
    if (rc != CB_OK) {
        rollback(q);
        for (wreq *r = list; r; r = r->next)
            if (r->rc == CB_OK) r->rc = rc;
    }
}

int cb_sql_write(cb_store *s, cb_sql_fn fn, void *ud) {
    sq *q = Q(s);
    wreq req = { .fn = fn, .ud = ud }, *r = &req;
    pthread_mutex_lock(&q->qmu);
    if (q->qtail) q->qtail->next = r;
    else q->qhead = r;
    q->qtail = r;
    while (!r->done && q->leader) pthread_cond_wait(&q->cv, &q->qmu);
    if (r->done) {
        pthread_mutex_unlock(&q->qmu);
        return r->rc;
    }
    q->leader = true;   // this thread's request is still queued: it runs in the group it leads
    wreq *list = q->qhead;
    q->qhead = q->qtail = NULL;
    pthread_mutex_unlock(&q->qmu);

    lock(q);
    run_group(q, list);
    unlock(q);

    pthread_mutex_lock(&q->qmu);
    for (wreq *x = list, *next; x; x = next) {
        next = x->next;   // before done: a woken caller returns and its request is gone
        x->done = true;
    }
    q->leader = false;
    pthread_cond_broadcast(&q->cv);
    pthread_mutex_unlock(&q->qmu);
    return r->rc;
}

// ---- open and close --------------------------------------------------------------------

static void finalize_all(sq *q) {
    for (int i = 0; i < ST__COUNT; i++) {
        sqlite3_finalize(q->st[i]);
        q->st[i] = NULL;
    }
    for (int i = 0; i < q->n_app; i++) {
        sqlite3_finalize(q->app_st[i]);
        q->app_st[i] = NULL;
    }
}

static void sq_close(cb_store *s) {
    sq *q = Q(s);
    finalize_all(q);
    sqlite3_close(q->db);
    pthread_mutex_destroy(&q->mu);
    pthread_mutex_destroy(&q->qmu);
    pthread_cond_destroy(&q->cv);
    free(q->app_st);
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

// The schema (the platform's, then the product's), applied in one transaction, then the version stamp.
// Steps for a future CB_SCHEMA_VERSION that are not idempotent go here, each guarded by the user_version
// it upgrades from.
static int apply_schema(sq *q, const cb_store_app *app) {
    int64_t have = 0;
    if (pragma_int(q->db, "PRAGMA user_version", &have) != CB_OK) return fail(q, "user_version", 0);
    if (have > CB_SCHEMA_VERSION) {
        CB_ERROR(MOD, "the database is at schema %lld, this binary knows %d: refusing to open it", (long long)have,
                 CB_SCHEMA_VERSION);
        return CB_E_STATE;
    }
    int rc = begin(q);
    if (rc != CB_OK) return rc;
    // Every step from schema 1 to 7 is in the schema itself: 7 drops the
    // `items` table of 6 (SQLite held no production data), so an older
    // database needs nothing first.
    if ((rc = exec(q, cbase_schema_sql)) != CB_OK) goto out;
    if (app && app->sql_schema && (rc = exec(q, app->sql_schema)) != CB_OK) goto out;
    char v[64];
    snprintf(v, sizeof v, "PRAGMA user_version = %d", CB_SCHEMA_VERSION);
    if ((rc = exec(q, v)) != CB_OK) goto out;
    rc = commit(q);
out:
    if (rc != CB_OK) rollback(q);
    return rc;
}

// SQLite seeds itself: the product's seed data, written at open into a
// database that has none (store.h cb_store_app seed).
static int seed(cb_store *s) { return s->app && s->app->seed ? s->app->seed(s) : CB_OK; }

// ---- SQLite's allocator ---------------------------------------------------------------------
// SQLite's row buffers (a stored row is several KB: VdbeMemGrow for the
// record being written, the blob being read) took an 8 KB musl malloc group
// that was mmap'ed and munmap'ed on nearly every request, because mallocng
// unmaps a group when its last slot is freed (perf: 2 mmap + 2 munmap a move,
// tlb_flush, the page faults after). Blocks of 1 KB to 64 KB come from per-class
// free lists here instead, kept up to POOL_KEEP each and never given back to
// malloc until then; everything else is plain malloc. A 16-byte header holds the
// block's usable size (xSize). One lock: SQLite already serialises the callers.
#define POOL_MIN_SHIFT 11
#define POOL_CLASSES   6   // 2K 4K 8K 16K 32K 64K
#define POOL_KEEP      8
#define POOL_HDR       16
typedef struct pblock { struct pblock *next; } pblock;
static pthread_mutex_t g_pool_mu = PTHREAD_MUTEX_INITIALIZER;
static pblock *g_pool[POOL_CLASSES];
static int g_pool_n[POOL_CLASSES];
// Leak accounting for the tests (cb_store_local_blocks): the blocks SQLite holds, and of them the ones
// handed out since the last cb_store_local_mark. The header's second word is the epoch a block was
// handed out in. Three relaxed counters; nothing reads them in a running server.
static atomic_long g_pm_out, g_pm_fresh;
static atomic_ulong g_pm_epoch;
static void pm_take(unsigned char *user, size_t usable) {
    *(size_t *)(user - POOL_HDR) = usable;
    *(unsigned long *)(user - POOL_HDR + sizeof(size_t)) = atomic_load_explicit(&g_pm_epoch, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_pm_out, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_pm_fresh, 1, memory_order_relaxed);
}
static void pm_give(unsigned char *user) {
    atomic_fetch_sub_explicit(&g_pm_out, 1, memory_order_relaxed);
    if (*(unsigned long *)(user - POOL_HDR + sizeof(size_t)) == atomic_load_explicit(&g_pm_epoch, memory_order_relaxed))
        atomic_fetch_sub_explicit(&g_pm_fresh, 1, memory_order_relaxed);
}
void cb_store_local_mark(void) {
    atomic_fetch_add(&g_pm_epoch, 1);
    atomic_store(&g_pm_fresh, 0);
}
long cb_store_local_blocks(long *fresh) {
    if (fresh) *fresh = atomic_load(&g_pm_fresh);
    return atomic_load(&g_pm_out);
}

static int pool_class(size_t n) {   // n usable bytes -> class or -1
    for (int c = 0; c < POOL_CLASSES; c++)
        if (n <= ((size_t)1 << (POOL_MIN_SHIFT + c))) return c;
    return -1;
}
static void *pm_malloc(int n) {
    if (n <= 0) return NULL;
    size_t want = (size_t)n;
    int c = want > ((size_t)1 << (POOL_MIN_SHIFT - 1)) ? pool_class(want) : -1;   // only blocks over 1 KB
    size_t usable = c >= 0 ? ((size_t)1 << (POOL_MIN_SHIFT + c)) : (want + 7) & ~(size_t)7;
    unsigned char *b = NULL;
    if (c >= 0) {
        pthread_mutex_lock(&g_pool_mu);
        pblock *p = g_pool[c];
        if (p) { g_pool[c] = p->next; g_pool_n[c]--; b = (unsigned char *)p; }
        pthread_mutex_unlock(&g_pool_mu);
        if (b) { pm_take(b, usable); return b; }
    }
    b = malloc(usable + POOL_HDR);
    if (!b) return NULL;
    pm_take(b + POOL_HDR, usable);
    return b + POOL_HDR;
}
static void pm_free(void *v) {
    if (!v) return;
    unsigned char *b = (unsigned char *)v - POOL_HDR;
    size_t usable = *(size_t *)b;
    pm_give(v);
    int c = usable > ((size_t)1 << (POOL_MIN_SHIFT - 1)) ? pool_class(usable) : -1;
    if (c >= 0 && usable == ((size_t)1 << (POOL_MIN_SHIFT + c))) {
        pthread_mutex_lock(&g_pool_mu);
        if (g_pool_n[c] < POOL_KEEP) {
            pblock *p = v;
            p->next = g_pool[c];
            g_pool[c] = p;
            g_pool_n[c]++;
            pthread_mutex_unlock(&g_pool_mu);
            return;
        }
        pthread_mutex_unlock(&g_pool_mu);
    }
    free(b);
}
static int pm_size(void *v) { return v ? (int)*(size_t *)((unsigned char *)v - POOL_HDR) : 0; }
static void *pm_realloc(void *v, int n) {
    if (!v) return pm_malloc(n);
    if (n <= 0) { pm_free(v); return NULL; }
    if ((size_t)n <= *(size_t *)((unsigned char *)v - POOL_HDR)) return v;
    void *nv = pm_malloc(n);
    if (!nv) return NULL;
    memcpy(nv, v, *(size_t *)((unsigned char *)v - POOL_HDR));
    pm_free(v);
    return nv;
}
static int pm_roundup(int n) {
    if (n <= 0) return 0;
    int c = (size_t)n > ((size_t)1 << (POOL_MIN_SHIFT - 1)) ? pool_class((size_t)n) : -1;
    return c >= 0 ? (int)((size_t)1 << (POOL_MIN_SHIFT + c)) : (n + 7) & ~7;
}
static int pm_init(void *u) { (void)u; return SQLITE_OK; }
static void pm_shutdown(void *u) { (void)u; }
static const sqlite3_mem_methods POOL_MEM = { pm_malloc, pm_free, pm_realloc, pm_size, pm_roundup, pm_init, pm_shutdown, NULL };

static pthread_once_t g_sqlite_cfg_once = PTHREAD_ONCE_INIT;
// Before SQLite's first use; a later open in the same process (tests) finds it
// configured or initialised and carries on.
static void sqlite_configure(void) { (void)sqlite3_config(SQLITE_CONFIG_MALLOC, &POOL_MEM); }

// Every statement, the platform's and the product's, prepared once at open: a statement that does not
// compile fails the open instead of its first request, and what SQLite computes and keeps for the life
// of the schema the first time a plan reads an index (its column affinities) is in place before any
// request runs.
static int prepare_all(sq *q) {
    for (int i = 0; i < ST__COUNT; i++)
        if (!stmt(q, i)) return CB_E_DB;
    for (int i = 0; i < q->n_app; i++)
        if (q->app_sql[i] && !cb_sql_stmt(q, i)) return CB_E_DB;
    return CB_OK;
}

int cb_store_open_sqlite(const char *path, cb_store **out) {
    *out = NULL;
    pthread_once(&g_sqlite_cfg_once, sqlite_configure);
    cb_store *s = calloc(1, sizeof *s);
    sq *q = calloc(1, sizeof *q);
    if (!s || !q) {
        free(s);
        free(q);
        return CB_E_NOMEM;
    }
    pthread_mutex_init(&q->mu, NULL);
    pthread_mutex_init(&q->qmu, NULL);
    pthread_cond_init(&q->cv, NULL);
    cb_store_init(s, &SQLITE_OPS, q);
    if (s->app && s->app->n_sql_stmts > 0) {
        q->app_st = calloc((size_t)s->app->n_sql_stmts, sizeof *q->app_st);
        if (!q->app_st) {
            sq_close(s);
            return CB_E_NOMEM;
        }
        q->app_sql = s->app->sql_stmts;
        q->n_app = s->app->n_sql_stmts;
    }
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
        (rc = apply_schema(q, s->app)) != CB_OK || (rc = seed(s)) < 0 || (rc = prepare_all(q)) != CB_OK) {
        sq_close(s);
        return rc;
    }
    *out = s;
    return CB_OK;
}

void cb_store_local_drain(void) {
    pthread_mutex_lock(&g_pool_mu);
    for (int c = 0; c < POOL_CLASSES; c++) {
        while (g_pool[c]) {
            pblock *p = g_pool[c];
            g_pool[c] = p->next;
            free((unsigned char *)p - POOL_HDR);
        }
        g_pool_n[c] = 0;
    }
    pthread_mutex_unlock(&g_pool_mu);
}

void cb_store_local_release(cb_store *s) {
    if (!cb_store_is_sql(s)) return;
    sq *q = Q(s);
    lock(q);
    finalize_all(q);
    sqlite3_db_release_memory(q->db);
    unlock(q);
}

unsigned long cb_store_local_changes(cb_store *s) { return cb_store_is_sql(s) ? atomic_load(&Q(s)->changes) : 0; }

long cb_store_local_cache_bytes(cb_store *s) {
    if (!cb_store_is_sql(s)) return 0;
    sq *q = Q(s);
    int cur = 0, hi = 0;
    lock(q);
    (void)sqlite3_db_status(q->db, SQLITE_DBSTATUS_CACHE_USED, &cur, &hi, 0);
    unlock(q);
    return (long)cur;
}

static int sq_ready(cb_store *st) {
    sq *q = Q(st);
    lock(q);
    int rc = run(q, stmt(q, ST_READY), "ready");
    unlock(q);
    return rc;
}

// ---- accounts ---------------------------------------------------------------------------------

static void b_i64(sqlite3_stmt *s, int i, int64_t v) { sqlite3_bind_int64(s, i, v); }
static void b_text(sqlite3_stmt *s, int i, const char *v) { sqlite3_bind_text(s, i, v ? v : "", -1, SQLITE_TRANSIENT); }

// A refresh token's hash: its SHA-256, HASH_HEX lowercase hex characters <-> HASH_BYTES bytes.
#define HASH_BYTES 32
#define HASH_HEX   (2 * HASH_BYTES)
_Static_assert(sizeof(((cb_refresh_row *)0)->token_hash) == HASH_HEX + 1, "cb_refresh_row token_hash is the hex SHA-256");
static bool hash_raw(const char *hex, uint8_t raw[HASH_BYTES]) {
    return hex && strlen(hex) == HASH_HEX && hex_decode(hex, HASH_HEX, raw, HASH_BYTES) == HASH_BYTES;
}
// false (and NULL bound): not one.
static bool b_hash(sqlite3_stmt *s, int i, const char *hex) {
    uint8_t raw[HASH_BYTES];
    if (!hash_raw(hex, raw)) {
        sqlite3_bind_null(s, i);
        return false;
    }
    sqlite3_bind_blob(s, i, raw, HASH_BYTES, SQLITE_TRANSIENT);
    return true;
}

static void user_of(sqlite3_stmt *s, cb_user_row *u) {
    memset(u, 0, sizeof *u);
    cb_sql_col_uuid(s, COL(U_ID), u->id);
    cb_sql_col_text(s, COL(U_EMAIL), u->email, sizeof u->email);
    cb_sql_col_text(s, COL(U_HASH), u->password_hash, sizeof u->password_hash);
    cb_sql_col_text(s, COL(U_USERNAME), u->username, sizeof u->username);
    u->created_at = sqlite3_column_int64(s, COL(U_CREATED_AT));
    u->last_sign_in_at = sqlite3_column_int64(s, COL(U_SIGNED_IN));
    u->session_gen = sqlite3_column_int64(s, COL(U_SESSION_GEN));
    cb_sql_col_uuid(s, COL(U_KEPT), u->kept_session);
}

static void user_bind(sqlite3_stmt *s, const cb_user_row *u) {
    cb_sql_bind_uuid(s, U_ID, u->id);
    b_text(s, U_EMAIL, u->email);
    b_text(s, U_HASH, u->password_hash);
    cb_sql_bind_text_or_null(s, U_USERNAME, u->username);
    b_i64(s, U_CREATED_AT, u->created_at);
    b_i64(s, U_SIGNED_IN, u->last_sign_in_at);
    b_i64(s, U_SESSION_GEN, u->session_gen);
    cb_sql_bind_uuid(s, U_KEPT, u->kept_session);
}

// One row of statement `id` with its ?1 bound to `key` by bind, into out by of: CB_OK, CB_E_NOT_FOUND.
typedef struct {
    int         id;
    const char *key;
    bool (*bind)(sqlite3_stmt *s, int i, const char *key);
    void (*of)(sqlite3_stmt *s, void *out);
    void       *out;
} one_read;

static int one_step(sq *q, void *ud) {
    one_read *r = ud;
    sqlite3_stmt *s = stmt(q, r->id);
    if (!s) return CB_E_DB;
    r->bind(s, 1, r->key);
    int st = sqlite3_step(s), rc;
    if (st == SQLITE_ROW) { r->of(s, r->out); rc = CB_OK; }
    else rc = st == SQLITE_DONE ? CB_E_NOT_FOUND : fail(q, SQL[r->id], st);
    done(s);
    return rc;
}
static bool bind_text1(sqlite3_stmt *s, int i, const char *v) { b_text(s, i, v); return true; }
static void user_of_v(sqlite3_stmt *s, void *out) { user_of(s, out); }

static int sq_user_get(cb_store *st, const char *id, cb_user_row *out) {
    one_read r = { ST_USER_GET, id, cb_sql_bind_uuid, user_of_v, out };
    return cb_sql_read(st, one_step, &r);
}

static int sq_user_by_email(cb_store *st, const char *email, cb_user_row *out) {
    one_read r = { ST_USER_BY_EMAIL, email, bind_text1, user_of_v, out };
    return cb_sql_read(st, one_step, &r);
}

typedef struct {
    cb_store          *s;
    const cb_user_row *u;
    bool               import;
} user_put;

static int user_put_step(sq *q, void *ud) {
    user_put *p = ud;
    sqlite3_stmt *s = stmt(q, p->import ? ST_USER_IMPORT : ST_USER_INSERT);
    if (!s) return CB_E_DB;
    user_bind(s, p->u);
    int rc = run(q, s, "user put");
    if (rc == CB_OK && !p->import && p->s->app && p->s->app->sql_signup) rc = p->s->app->sql_signup(q, p->u);
    return rc;
}

// CB_E_CONFLICT when the email (or the id) is taken; the product's signup rows go in the same transaction.
static int sq_user_create(cb_store *st, const cb_user_row *u) {
    unsigned char b[CB_UUID_BYTES];
    if (!cb_uuid_pack(u->id, b)) return CB_E_INVALID;
    user_put p = { st, u, false };
    return cb_sql_write(st, user_put_step, &p);
}

static int sq_user_import(cb_store *st, const cb_user_row *u) {
    unsigned char b[CB_UUID_BYTES];
    if (!cb_uuid_pack(u->id, b)) return CB_E_INVALID;
    user_put p = { st, u, true };
    int rc = cb_sql_write(st, user_put_step, &p);
    return rc == CB_E_CONFLICT ? CB_E_DB : rc;   // the email is another account's
}

typedef struct {
    int         id;
    const char *user, *text, *keep;
    int64_t     at;
} profile_up;

static int profile_step(sq *q, void *ud) {
    profile_up *p = ud;
    sqlite3_stmt *s = stmt(q, p->id);
    if (!s) return CB_E_DB;
    cb_sql_bind_uuid(s, 1, p->user);
    if (p->id == ST_USER_PASSWORD) {
        b_text(s, 2, p->text);
        cb_sql_bind_uuid(s, 3, p->keep);
    } else if (p->id == ST_USER_TOUCH) {
        b_i64(s, 2, p->at);
    }
    int rc = run(q, s, SQL[p->id]);
    return rc == CB_OK && sqlite3_changes(q->db) == 0 ? CB_E_NOT_FOUND : rc;
}

static int sq_user_set_password(cb_store *st, const char *id, const char *hash, const char *keep) {
    profile_up p = { ST_USER_PASSWORD, id, hash, keep, 0 };
    return cb_sql_write(st, profile_step, &p);
}
static int sq_user_end_sessions(cb_store *st, const char *id) {
    profile_up p = { ST_USER_END, id, NULL, NULL, 0 };
    return cb_sql_write(st, profile_step, &p);
}
static int sq_user_touch_sign_in(cb_store *st, const char *id, int64_t at) {
    profile_up p = { ST_USER_TOUCH, id, NULL, NULL, at };
    return cb_sql_write(st, profile_step, &p);
}

typedef struct {
    cb_store   *s;
    const char *id;
} user_del;

static int user_del_step(sq *q, void *ud) {
    user_del *d = ud;
    static const int IDS[] = { ST_USER_DEL_REFRESH, ST_USER_DEL_SESSIONS };
    int rc = CB_OK;
    for (int i = 0; rc == CB_OK && i < 2; i++) {
        sqlite3_stmt *s = stmt(q, IDS[i]);
        if (s) cb_sql_bind_uuid(s, 1, d->id);
        rc = run(q, s, SQL[IDS[i]]);
    }
    if (rc == CB_OK && d->s->app && d->s->app->sql_delete) rc = d->s->app->sql_delete(q, d->id);
    if (rc == CB_OK) {
        sqlite3_stmt *s = stmt(q, ST_USER_DELETE);
        if (s) cb_sql_bind_uuid(s, 1, d->id);
        rc = run(q, s, "user delete");
        if (rc == CB_OK && sqlite3_changes(q->db) == 0) rc = CB_E_NOT_FOUND;
    }
    return rc;
}

// The cascade auth.admin.deleteUser had: the account, its sessions and refresh tokens, and every row of
// the product's that hangs off it (cb_store_app sql_delete), in ONE transaction.
static int sq_user_delete(cb_store *st, const char *id) {
    user_del d = { st, id };
    return cb_sql_write(st, user_del_step, &d);
}

typedef struct {
    const char  *search;
    int          skip, limit;
    cb_user_row *rows;
    int          n, cap;
} users_walk;

static int users_step(sq *q, void *ud) {
    users_walk *w = ud;
    sqlite3_stmt *s = stmt(q, ST_USERS_ALL);
    if (!s) return CB_E_DB;
    int r = SQLITE_DONE, rc = CB_OK;
    cb_user_row u;
    while ((w->limit <= 0 || w->n < w->limit) && (r = sqlite3_step(s)) == SQLITE_ROW) {
        user_of(s, &u);
        if (!cb_store_user_matches(&u, w->search)) continue;
        if (w->skip > 0) { w->skip--; continue; }
        if (w->n == w->cap) {
            int cap = w->cap ? w->cap * 2 : 32;
            cb_user_row *p = realloc(w->rows, (size_t)cap * sizeof *p);
            if (!p) { rc = CB_E_NOMEM; break; }
            w->rows = p;
            w->cap = cap;
        }
        w->rows[w->n++] = u;
    }
    if (rc == CB_OK && r != SQLITE_DONE && r != SQLITE_ROW) rc = fail(q, "users", r);
    done(s);
    return rc;
}

// Newest first.
static int sq_users_list(cb_store *st, const char *search, int offset, int limit, cb_user_fn fn, void *ud) {
    users_walk w = { .search = search, .skip = offset > 0 ? offset : 0, .limit = limit };
    int rc = cb_sql_read(st, users_step, &w), n = 0;
    for (int i = 0; rc == CB_OK && i < w.n; i++) {
        n++;
        if (!fn(&w.rows[i], ud)) break;
    }
    free(w.rows);
    return rc == CB_OK ? n : rc;
}

// ---- sessions --------------------------------------------------------------------------

typedef struct {
    const cb_session_row *r;
    const char           *id, *user;
    int64_t               now, expires;
    cb_session_row       *out;
} sess_op;

static int session_put_step(sq *q, void *ud) {
    sess_op *o = ud;
    // The user's sessions past their end go first (TTL deletion, as DynamoDB's).
    sqlite3_stmt *s = stmt(q, ST_SESSION_SWEEP);
    if (s) { cb_sql_bind_uuid(s, 1, o->r->user_id); b_i64(s, 2, o->now); }
    int rc = run(q, s, "session sweep");
    if (rc != CB_OK) return rc;
    if (!(s = stmt(q, ST_SESSION_PUT))) return CB_E_DB;
    cb_sql_bind_uuid(s, 1, o->r->id);
    cb_sql_bind_uuid(s, 2, o->r->user_id);
    b_i64(s, 3, o->r->created_at);
    b_i64(s, 4, o->r->expires_at);
    b_i64(s, 5, o->r->gen);
    b_i64(s, 6, o->r->revoked);
    return run(q, s, "session put");
}

static int sq_session_put(cb_store *st, const cb_session_row *r) {
    unsigned char b[CB_UUID_BYTES];
    if (!cb_uuid_pack(r->id, b) || !cb_uuid_pack(r->user_id, b)) return CB_E_INVALID;
    sess_op o = { .r = r, .now = cb_now_ms() };
    return cb_sql_write(st, session_put_step, &o);
}

static void session_of(sqlite3_stmt *s, void *out) {
    cb_session_row *r = out;
    cb_sql_col_uuid(s, 0, r->id);
    cb_sql_col_uuid(s, 1, r->user_id);
    r->created_at = sqlite3_column_int64(s, 2);
    r->expires_at = sqlite3_column_int64(s, 3);
    r->gen = sqlite3_column_int64(s, 4);
    r->revoked = sqlite3_column_int64(s, 5) != 0;
}

// Read consistently (a revoke just written is seen); past its end it is gone.
static int sq_session_get(cb_store *st, const char *id, int64_t now_ms, cb_session_row *out) {
    memset(out, 0, sizeof *out);
    one_read r = { ST_SESSION_GET, id, cb_sql_bind_uuid, session_of, out };
    int rc = cb_sql_read(st, one_step, &r);
    if (rc == CB_OK && out->expires_at <= now_ms) rc = CB_E_NOT_FOUND;
    if (rc != CB_OK) memset(out, 0, sizeof *out);
    return rc;
}

static int session_revoke_step(sq *q, void *ud) {
    sess_op *o = ud;
    sqlite3_stmt *s = stmt(q, ST_SESSION_REVOKE);
    if (!s) return CB_E_DB;
    cb_sql_bind_uuid(s, 1, o->id);
    cb_sql_bind_uuid(s, 2, o->user);
    b_i64(s, 3, o->expires);
    return run(q, s, "session revoke");
}

// Ended in one write; a session with no row yet gets one (store.h session_revoke).
static int sq_session_revoke(cb_store *st, const char *id, const char *user_id, int64_t expires_at) {
    unsigned char b[CB_UUID_BYTES];
    if (!cb_uuid_pack(id, b)) return CB_E_INVALID;
    sess_op o = { .id = id, .user = user_id, .expires = expires_at };
    int rc = cb_sql_write(st, session_revoke_step, &o);
    return rc == CB_E_CONFLICT ? CB_E_DB : rc;
}

// ---- refresh tokens --------------------------------------------------------------------------

typedef struct {
    const cb_refresh_row *r;
    const char           *hash;
    int64_t               now, spent;
    cb_refresh_row       *out;
} refresh_op;

static int refresh_put_step(sq *q, void *ud) {
    refresh_op *o = ud;
    sqlite3_stmt *s = stmt(q, ST_REFRESH_SWEEP);   // the user's tokens past their time go first
    if (s) { cb_sql_bind_uuid(s, 1, o->r->user_id); b_i64(s, 2, o->now); }
    int rc = run(q, s, "refresh sweep");
    if (rc != CB_OK) return rc;
    if (!(s = stmt(q, ST_REFRESH_PUT))) return CB_E_DB;
    b_hash(s, 1, o->r->token_hash);
    cb_sql_bind_uuid(s, 2, o->r->user_id);
    cb_sql_bind_uuid(s, 3, o->r->session_id);
    b_i64(s, 4, o->r->created_at);
    b_i64(s, 5, o->r->revoked);
    b_i64(s, 6, o->r->expires_at > 0 ? o->r->expires_at : 0);
    return run(q, s, "refresh put");
}

// CB_E_CONFLICT for a hash already stored.
static int sq_refresh_put(cb_store *st, const cb_refresh_row *r) {
    unsigned char b[CB_UUID_BYTES];
    uint8_t h[HASH_BYTES];
    if (!hash_raw(r->token_hash, h) || !cb_uuid_pack(r->user_id, b))
        return CB_E_INVALID;
    refresh_op o = { .r = r, .now = cb_now_ms() };
    return cb_sql_write(st, refresh_put_step, &o);
}

static void refresh_of(sqlite3_stmt *s, cb_refresh_row *r) {
    memset(r, 0, sizeof *r);
    const void *h = sqlite3_column_blob(s, 0);
    if (h && sqlite3_column_bytes(s, 0) == HASH_BYTES) hex_encode(h, HASH_BYTES, r->token_hash, sizeof r->token_hash);
    cb_sql_col_uuid(s, 1, r->user_id);
    cb_sql_col_uuid(s, 2, r->session_id);
    r->created_at = sqlite3_column_int64(s, 3);
    r->revoked = sqlite3_column_int64(s, 4) != 0;
    r->revoked_at = sqlite3_column_int64(s, 5);
    r->expires_at = sqlite3_column_int64(s, 6);
}

// Rotation (store.h refresh_take): the row as it was, then spent in the same transaction.
static int refresh_take_step(sq *q, void *ud) {
    refresh_op *o = ud;
    sqlite3_stmt *s = stmt(q, ST_REFRESH_GET);
    if (!s) return CB_E_DB;
    b_hash(s, 1, o->hash);
    int r = sqlite3_step(s), rc;
    if (r == SQLITE_ROW) { refresh_of(s, o->out); rc = CB_OK; }
    else rc = r == SQLITE_DONE ? CB_E_NOT_FOUND : fail(q, "refresh get", r);
    done(s);
    if (rc != CB_OK) return rc;
    if (!(s = stmt(q, ST_REFRESH_TAKE))) return CB_E_DB;
    b_hash(s, 1, o->hash);
    b_i64(s, 2, o->now);
    b_i64(s, 3, o->now + o->spent);
    return run(q, s, "refresh take");
}

static int sq_refresh_take(cb_store *st, const char *hash, int64_t now_ms, int64_t spent_ms, cb_refresh_row *out) {
    memset(out, 0, sizeof *out);
    refresh_op o = { .hash = hash, .now = now_ms, .spent = spent_ms, .out = out };
    int rc = cb_sql_write(st, refresh_take_step, &o);
    // Past its time but not yet swept: as good as gone.
    if (rc == CB_OK && out->expires_at > 0 && out->expires_at <= now_ms) rc = CB_E_NOT_FOUND;
    if (rc != CB_OK) memset(out, 0, sizeof *out);
    return rc;
}

// ---- failed sign-ins ------------------------------------------------------------------------

typedef struct {
    const char            *email;
    const cb_signin_fails *f;
    int64_t                now, expires;
    cb_signin_fails       *out;
} fails_op;

static int fails_get_step(sq *q, void *ud) {
    fails_op *o = ud;
    sqlite3_stmt *s = stmt(q, ST_FAILS_GET);
    if (!s) return CB_E_DB;
    b_text(s, 1, o->email);
    int r = sqlite3_step(s), rc;
    if (r == SQLITE_ROW) {
        int64_t n = sqlite3_column_int64(s, 0);
        o->out->n = n < 0 ? 0 : n > INT32_MAX ? INT32_MAX : (int32_t)n;
        o->out->last_at = sqlite3_column_int64(s, 1);
        o->out->locked_until = sqlite3_column_int64(s, 2);
        o->expires = sqlite3_column_int64(s, 3);
        rc = CB_OK;
    } else rc = r == SQLITE_DONE ? CB_E_NOT_FOUND : fail(q, "fails get", r);
    done(s);
    return rc;
}

static int sq_signin_fails_get(cb_store *st, const char *email, int64_t now_ms, cb_signin_fails *out) {
    memset(out, 0, sizeof *out);
    fails_op o = { .email = email, .out = out };
    int rc = cb_sql_read(st, fails_get_step, &o);
    if (rc == CB_OK && o.expires <= now_ms) rc = CB_E_NOT_FOUND;   // past its hour: none
    if (rc != CB_OK) memset(out, 0, sizeof *out);
    return rc;
}

static int fails_put_step(sq *q, void *ud) {
    fails_op *o = ud;
    sqlite3_stmt *s = stmt(q, o->f ? ST_FAILS_PUT : ST_FAILS_CLEAR);
    if (!s) return CB_E_DB;
    b_text(s, 1, o->email);
    if (o->f) {
        b_i64(s, 2, o->f->n);
        b_i64(s, 3, o->f->last_at);
        b_i64(s, 4, o->f->locked_until);
        b_i64(s, 5, o->now + FAILS_TTL_MS);
    }
    return run(q, s, "signin fails");
}

static int sq_signin_fails_put(cb_store *st, const char *email, const cb_signin_fails *f, int64_t now_ms) {
    fails_op o = { .email = email, .f = f, .now = now_ms };
    return cb_sql_write(st, fails_put_step, &o);
}

static int sq_signin_fails_clear(cb_store *st, const char *email) {
    fails_op o = { .email = email };
    return cb_sql_write(st, fails_put_step, &o);
}

// ---- admin ---------------------------------------------------------------------------

typedef struct {
    char (*names)[64];
    int    n, cap;
} name_list;

static int tables_step(sq *q, void *ud) {
    name_list *l = ud;
    sqlite3_stmt *s = stmt(q, ST_TABLES);
    if (!s) return CB_E_DB;
    int r, rc = CB_OK;
    while ((r = sqlite3_step(s)) == SQLITE_ROW) {
        if (l->n == l->cap) {
            int cap = l->cap ? l->cap * 2 : 16;
            char (*p)[64] = realloc(l->names, (size_t)cap * sizeof *p);
            if (!p) { rc = CB_E_NOMEM; break; }
            l->names = p;
            l->cap = cap;
        }
        cb_sql_col_text(s, 0, l->names[l->n++], 64);
    }
    if (rc == CB_OK && r != SQLITE_DONE) rc = fail(q, "tables", r);
    done(s);
    return rc;
}

// The tables, by name.
static int sq_admin_collections(cb_store *st, cb_name_fn fn, void *ud) {
    name_list l = { 0 };
    int rc = cb_sql_read(st, tables_step, &l), n = 0;
    for (int i = 0; rc == CB_OK && i < l.n; i++) {
        n++;
        if (!fn(l.names[i], ud)) break;
    }
    free(l.names);
    return rc == CB_OK ? n : rc;
}

// A row of any statement as text: NULL stays NULL, a UUID column its text, any other blob "\x" hex.
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

// On false, *t holds what was read so far and textrow_free frees it: t->n
// counts columns only once both arrays exist (unit_oom.c failed the second
// calloc and textrow_free walked a NULL names array).
static bool textrow_read(sqlite3_stmt *s, int first, textrow *t) {
    int n = sqlite3_column_count(s) - first;
    if (n < 0) n = 0;
    t->n = 0;
    t->names = calloc((size_t)n + 1, sizeof *t->names);
    t->values = calloc((size_t)n + 1, sizeof *t->values);
    if (!t->names || !t->values) return false;
    t->n = n;
    for (int i = 0; i < t->n; i++) {
        int c = i + first;
        const char *nm = sqlite3_column_name(s, c);
        t->names[i] = strdup(nm ? nm : "?");
        if (!t->names[i]) return false;
        int type = sqlite3_column_type(s, c);
        if (type == SQLITE_NULL) continue;
        if (type == SQLITE_BLOB) {
            size_t len = (size_t)sqlite3_column_bytes(s, c);
            // An id is shown as people see it: a UUID's 16 bytes as its text, a token hash as hex.
            size_t nl = strlen(t->names[i]);
            bool idcol = strcmp(t->names[i], "id") == 0 || strcmp(t->names[i], "kept_session") == 0 ||
                         (nl > 3 && strcmp(t->names[i] + nl - 3, "_id") == 0);
            if ((idcol && len == CB_UUID_BYTES) || (strcmp(t->names[i], "token_hash") == 0 && len == HASH_BYTES)) {
                t->values[i] = malloc(2 * len + 1);
                if (!t->values[i]) return false;
                if (len == CB_UUID_BYTES) cb_uuid_unpack(sqlite3_column_blob(s, c), t->values[i]);
                else hex_encode(sqlite3_column_blob(s, c), len, t->values[i], 2 * len + 1);
                continue;
            }
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
            more = !fn || fn(t->rows[i].n, (const char *const *)t->rows[i].names,
                             (const char *const *)t->rows[i].values, ud);
        }
        textrow_free(&t->rows[i]);
    }
    free(t->rows);
    return rc == CB_OK ? n : rc;
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


// ---- the admin browse --------------------------------------------------------------------------

typedef struct {
    const char *table, *prefix;
    int64_t     offset;
    int         limit;
    textrows    rows;
} browse_op;

// Is `name` one of the tables (never text spliced into SQL otherwise)? 1 yes, 0 no, or a CB_E_*.
static int is_table(sq *q, const char *name) {
    sqlite3_stmt *s = stmt(q, ST_TABLES);
    if (!s) return CB_E_DB;
    int r, yes = 0;
    while (!yes && (r = sqlite3_step(s)) == SQLITE_ROW) {
        const unsigned char *t = sqlite3_column_text(s, 0);
        if (!t) { r = SQLITE_NOMEM; break; }
        yes = strcmp((const char *)t, name) == 0;
    }
    int rc = yes ? 1 : r == SQLITE_DONE ? 0 : fail(q, "tables", r);
    done(s);
    return rc;
}

// A value for a BLOB column: a UUID's bytes, else hex's, else the text's own.
static void bind_blob_value(sqlite3_stmt *s, int i, const char *v) {
    size_t n = strlen(v);
    uint8_t *raw = malloc(n / 2 + 1);
    if (cb_sql_bind_uuid(s, i, v)) { free(raw); return; }
    if (raw && n % 2 == 0 && hex_decode(v, n, raw, n / 2 + 1) == (int)(n / 2)) sqlite3_bind_blob(s, i, raw, (int)(n / 2), SQLITE_TRANSIENT);
    else sqlite3_bind_blob(s, i, v, (int)n, SQLITE_TRANSIENT);
    free(raw);
}

static int browse_step(sq *q, void *ud) {
    browse_op *o = ud;
    int t = is_table(q, o->table);
    if (t <= 0) return t == 0 ? CB_E_INVALID : t;
    // The table's columns (names and declared types), from an empty select.
    char sql[512];
    snprintf(sql, sizeof sql, "SELECT * FROM \"%s\" LIMIT 0", o->table);
    sqlite3_stmt *meta = NULL;
    if (sqlite3_prepare_v2(q->db, sql, -1, &meta, NULL) != SQLITE_OK) return fail(q, "browse columns", 0);
    int nc = sqlite3_column_count(meta), col = 0;
    const char *value = NULL;
    const char *eq = o->prefix && *o->prefix ? strchr(o->prefix, '=') : NULL;
    char cname[64] = "";
    if (eq) {
        size_t k = (size_t)(eq - o->prefix);
        col = -1;
        for (int i = 0; i < nc; i++)
            if (strlen(sqlite3_column_name(meta, i)) == k && strncmp(sqlite3_column_name(meta, i), o->prefix, k) == 0) col = i;
        value = eq + 1;
    } else if (o->prefix && *o->prefix) value = o->prefix;
    if (col < 0) { sqlite3_finalize(meta); return CB_E_INVALID; }
    cb_strlcpy(cname, nc ? sqlite3_column_name(meta, col) : "", sizeof cname);
    const char *decl = nc ? sqlite3_column_decltype(meta, col) : NULL;
    bool blob = decl && strcmp(decl, "BLOB") == 0;
    sqlite3_finalize(meta);
    // The prefix with a UUID's dashes dropped, upper-cased, for a match on a BLOB column's hex.
    char hexp[160] = "";
    if (!eq && value && blob) {
        size_t k = 0;
        for (const char *p = value; *p && k + 2 < sizeof hexp; p++)
            if (*p != '-') hexp[k++] = (char)toupper((unsigned char)*p);
        hexp[k++] = '%';
        hexp[k] = '\0';
    }
    if (!value) snprintf(sql, sizeof sql, "SELECT * FROM \"%s\" LIMIT ?2 OFFSET ?3", o->table);
    else if (eq) snprintf(sql, sizeof sql, "SELECT * FROM \"%s\" WHERE \"%s\" = ?1 LIMIT ?2 OFFSET ?3", o->table, cname);
    else if (blob) snprintf(sql, sizeof sql, "SELECT * FROM \"%s\" WHERE hex(\"%s\") LIKE ?1 LIMIT ?2 OFFSET ?3", o->table, cname);
    else snprintf(sql, sizeof sql, "SELECT * FROM \"%s\" WHERE substr(\"%s\", 1, length(?1)) = ?1 LIMIT ?2 OFFSET ?3", o->table, cname);
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(q->db, sql, -1, &s, NULL) != SQLITE_OK) return fail(q, "browse", 0);
    if (value && eq && blob) bind_blob_value(s, 1, value);
    else if (value && blob) b_text(s, 1, hexp);
    else if (value) b_text(s, 1, value);
    b_i64(s, 2, o->limit);
    b_i64(s, 3, o->offset);
    int rc = textrows_read(q, s, 0, o->limit, &o->rows, NULL);
    sqlite3_finalize(s);
    return rc;
}

// A page of one table, each row as the console's text row. `prefix`: "column=value" is an eq filter, any
// other text matches the start of the first column (a UUID column's text). `cursor` is the row offset of
// the page ("" for the first), and next_cursor comes back "" after the last page.
static int sq_admin_browse(cb_store *st, const char *collection, const char *prefix, const char *cursor, int limit,
                           cb_admin_row_fn fn, void *ud, char *next_cursor, size_t next_cap) {
    if (next_cursor && next_cap) next_cursor[0] = '\0';
    if (!collection || !*collection || strlen(collection) > 60) return CB_E_INVALID;
    if (limit <= 0 || limit > 1000) limit = 100;
    int64_t after = 0;
    if (cursor && *cursor && (!num_parse_i64_cstr(cursor, 0, INT64_MAX, &after))) return CB_E_INVALID;
    browse_op o = { .table = collection, .prefix = prefix, .offset = after, .limit = limit };
    int rc = cb_sql_read(st, browse_step, &o);
    int got = o.rows.n;
    rc = textrows_emit(&o.rows, rc, fn, ud);
    if (rc >= 0 && got == limit && next_cursor && next_cap) snprintf(next_cursor, next_cap, "%lld", (long long)(after + got));
    return rc;
}

static const cb_store_ops SQLITE_OPS = {
    .name = "sqlite",
    .close = sq_close,
    .ready = sq_ready,
    .user_get = sq_user_get,
    .user_by_email = sq_user_by_email,
    .user_create = sq_user_create,
    .user_set_password = sq_user_set_password,
    .user_end_sessions = sq_user_end_sessions,
    .user_touch_sign_in = sq_user_touch_sign_in,
    .user_delete = sq_user_delete,
    .users_list = sq_users_list,
    .signin_fails_get = sq_signin_fails_get,
    .signin_fails_put = sq_signin_fails_put,
    .signin_fails_clear = sq_signin_fails_clear,
    .refresh_put = sq_refresh_put,
    .refresh_take = sq_refresh_take,
    .session_put = sq_session_put,
    .session_get = sq_session_get,
    .session_revoke = sq_session_revoke,
    .admin_collections = sq_admin_collections,
    .admin_browse = sq_admin_browse,
    .admin_query = sq_admin_query,
    .user_import = sq_user_import,
};
