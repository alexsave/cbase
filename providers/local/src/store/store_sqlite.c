// store_sqlite.c - the SQLite backend (store.h): `cbase serve`, the tests, the
// import rehearsal. It is the item layer (items.h) on SQLite, and the admin
// console's view of it; the rows above it are written once for both backends
// (rows_auth.c, rows_foolish.c), so SQLite behaves as DynamoDB does by
// construction (docs/PLATFORM.md 8.3.2: local is the reference provider).
//
// One connection (SQLITE_OPEN_FULLMUTEX), WAL, synchronous=NORMAL, a busy
// timeout for a second process (cbase-import beside a running serve).
// providers/local/sql/schema.sql (cbase_schema_sql) is applied in one transaction at every
// open, then PRAGMA user_version = CB_SCHEMA_VERSION; a database whose
// user_version is newer is refused.
//
// THREADS. serve calls the store from a pool, and one SQLite connection runs
// one transaction at a time, so every operation holds `mu` from its first
// statement to its last: a transaction can never pick up another thread's
// statements. A query reads its items under the lock and hands them out after
// releasing it, so a callback may call the store again.
#include "store/store.h"
#include "store/store_priv.h"
#include "store/rows_auth.h"

#include "util/bytea.h"
#include "util/clock.h"
#include "util/log.h"
#include "util/str.h"
#include "../../../../../shared/c/num_parse.h"

#include <ctype.h>
#include <pthread.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MOD "store"

extern const char cbase_schema_sql[];

// ---- the statements ------------------------------------------------------------------

enum {
    ST_ITEM_GET, ST_ITEM_PUT, ST_ITEM_DELETE,
    ST_ITEM_Q0A, ST_ITEM_Q0D, ST_ITEM_Q1A, ST_ITEM_Q1D, ST_ITEM_Q2A, ST_ITEM_Q2D,
    ST_BROWSE, ST_KEYS,
    ST_BEGIN, ST_COMMIT, ST_ROLLBACK, ST_SAVEPOINT, ST_RELEASE, ST_ROLLBACK_TO,   // transaction control, prepared once
    ST__COUNT
};

// The attributes in one binary record, the keys an item is found by in
// columns. ?2 of a query is a sort key prefix or NULL, ?3 a bound below or NULL.
#define ITEM_Q(pkc, skc, dir) "SELECT attrs FROM items WHERE " pkc " = ?1 AND (?2 IS NULL OR substr(" skc ", 1, length(?2)) = ?2) " \
                              "AND (?3 IS NULL OR " skc " < ?3) ORDER BY " skc " " dir

static const char *const SQL[ST__COUNT] = {
    [ST_ITEM_GET] = "SELECT attrs FROM items WHERE pk = ?1 AND sk = ?2",
    [ST_ITEM_PUT] = "INSERT INTO items (pk, sk, gsi1pk, gsi1sk, gsi2pk, gsi2sk, expires_ms, attrs) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8) "
                    "ON CONFLICT (pk, sk) DO UPDATE SET gsi1pk = excluded.gsi1pk, gsi1sk = excluded.gsi1sk, "
                    "gsi2pk = excluded.gsi2pk, gsi2sk = excluded.gsi2sk, expires_ms = excluded.expires_ms, attrs = excluded.attrs",
    [ST_ITEM_DELETE] = "DELETE FROM items WHERE pk = ?1 AND sk = ?2",
    [ST_ITEM_Q0A] = ITEM_Q("pk", "sk", "ASC"),
    [ST_ITEM_Q0D] = ITEM_Q("pk", "sk", "DESC"),
    [ST_ITEM_Q1A] = ITEM_Q("gsi1pk", "gsi1sk", "ASC"),
    [ST_ITEM_Q1D] = ITEM_Q("gsi1pk", "gsi1sk", "DESC"),
    [ST_ITEM_Q2A] = ITEM_Q("gsi2pk", "gsi2sk", "ASC"),
    [ST_ITEM_Q2D] = ITEM_Q("gsi2pk", "gsi2sk", "DESC"),
    // A page of one collection (store_priv.h cb_store_collection_pk), in rowid order.
    [ST_BROWSE] = "SELECT rowid, attrs FROM items WHERE substr(pk, 1, length(?1)) = ?1 AND rowid > ?2 ORDER BY rowid LIMIT ?3",
    [ST_KEYS] = "SELECT pk, sk FROM items",
    [ST_BEGIN] = "BEGIN IMMEDIATE",
    [ST_COMMIT] = "COMMIT",
    [ST_ROLLBACK] = "ROLLBACK",
    [ST_SAVEPOINT] = "SAVEPOINT g",
    [ST_RELEASE] = "RELEASE g",
    [ST_ROLLBACK_TO] = "ROLLBACK TO g",
};

// A write waiting for its group (see "group commit" below).
typedef struct wreq {
    struct wreq     *next;
    const cb_iwrite *w;
    int              n;
    bool             txn;       // item_txn (all-or-nothing list, failed/had reported) or one item_write
    cb_item         *old;       // item_write: the stored item on a refused condition
    int             *failed;
    bool            *had;
    int              rc;
    bool             done;      // under qmu
} wreq;

typedef struct {
    sqlite3        *db;
    pthread_mutex_t mu;         // the connection: held from a transaction's first statement to its last
    sqlite3_stmt   *st[ST__COUNT];
    pthread_mutex_t qmu;        // the write queue only; never held across SQLite
    pthread_cond_t  cv;
    wreq           *qhead, *qtail;
    bool            leader;     // a thread is running a group
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

// A prepared control statement (BEGIN, COMMIT, ...): no parse per transaction.
static int ctl(sq *q, int id) {
    sqlite3_stmt *s = stmt(q, id);
    if (!s) return CB_E_DB;
    int r = sqlite3_step(s);
    int rc = r == SQLITE_DONE ? CB_OK : fail(q, SQL[id], r);
    done(s);
    return rc;
}

static int begin(sq *q) { return ctl(q, ST_BEGIN); }
static int commit(sq *q) { return ctl(q, ST_COMMIT); }
static void rollback(sq *q) {
    if (!sqlite3_get_autocommit(q->db)) ctl(q, ST_ROLLBACK);
}

static void lock(sq *q) { pthread_mutex_lock(&q->mu); }
static void unlock(sq *q) { pthread_mutex_unlock(&q->mu); }

// ---- binding ---------------------------------------------------------------------------

static void b_text(sqlite3_stmt *s, int i, const char *v) { sqlite3_bind_text(s, i, v, -1, SQLITE_TRANSIENT); }
static void b_text_null(sqlite3_stmt *s, int i, const char *v) {
    if (v && *v) b_text(s, i, v);
    else sqlite3_bind_null(s, i);
}
static void b_i64(sqlite3_stmt *s, int i, int64_t v) { sqlite3_bind_int64(s, i, v); }
static void b_blob_nn(sqlite3_stmt *s, int i, const uint8_t *p, size_t n) {
    sqlite3_bind_blob64(s, i, n ? (const void *)p : "", n, SQLITE_TRANSIENT);
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

// ---- open and close --------------------------------------------------------------------

static const cb_store_ops SQLITE_OPS;

static void sq_close(cb_store *s) {
    sq *q = Q(s);
    for (int i = 0; i < ST__COUNT; i++) sqlite3_finalize(q->st[i]);
    sqlite3_close(q->db);
    pthread_mutex_destroy(&q->mu);
    pthread_mutex_destroy(&q->qmu);
    pthread_cond_destroy(&q->cv);
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
    // Every step from schema 1 to 6 is in the schema itself: 6 keeps every
    // row as an item and drops the tables of 1 to 5 (SQLite holds no
    // production data), so an older database needs nothing first.
    if ((rc = exec(q, cbase_schema_sql)) != CB_OK) goto out;
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
// tlb_flush, the page faults after). Blocks of 4 KB to 64 KB come from per-class
// free lists here instead, kept up to POOL_KEEP each and never given back to
// malloc until then; everything else is plain malloc. A 16-byte header holds the
// block's usable size (xSize). One lock: SQLite already serialises the callers.
#define POOL_MIN_SHIFT 12
#define POOL_CLASSES   5   // 4K 8K 16K 32K 64K
#define POOL_KEEP      8
#define POOL_HDR       16
typedef struct pblock { struct pblock *next; } pblock;
static pthread_mutex_t g_pool_mu = PTHREAD_MUTEX_INITIALIZER;
static pblock *g_pool[POOL_CLASSES];
static int g_pool_n[POOL_CLASSES];

static int pool_class(size_t n) {   // n usable bytes -> class or -1
    for (int c = 0; c < POOL_CLASSES; c++)
        if (n <= ((size_t)1 << (POOL_MIN_SHIFT + c))) return c;
    return -1;
}
static void *pm_malloc(int n) {
    if (n <= 0) return NULL;
    size_t want = (size_t)n;
    int c = want > ((size_t)1 << (POOL_MIN_SHIFT - 1)) ? pool_class(want) : -1;   // only blocks over 2 KB
    size_t usable = c >= 0 ? ((size_t)1 << (POOL_MIN_SHIFT + c)) : (want + 7) & ~(size_t)7;
    unsigned char *b = NULL;
    if (c >= 0) {
        pthread_mutex_lock(&g_pool_mu);
        pblock *p = g_pool[c];
        if (p) { g_pool[c] = p->next; g_pool_n[c]--; b = (unsigned char *)p; }
        pthread_mutex_unlock(&g_pool_mu);
        if (b) return b;
    }
    b = malloc(usable + POOL_HDR);
    if (!b) return NULL;
    *(size_t *)b = usable;
    return b + POOL_HDR;
}
static void pm_free(void *v) {
    if (!v) return;
    unsigned char *b = (unsigned char *)v - POOL_HDR;
    size_t usable = *(size_t *)b;
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
        (rc = apply_schema(q)) != CB_OK || (rc = seed(s)) < 0) {
        sq_close(s);
        return rc;
    }
    *out = s;
    return CB_OK;
}

// ---- the item layer (items.h) ------------------------------------------------------------

// The item stored at key into it (reset first): CB_OK, CB_E_NOT_FOUND, CB_E_DB. Under the lock.
static int item_load(sq *q, cb_ikey key, cb_item *it) {
    cb_item_reset(it);
    sqlite3_stmt *s = stmt(q, ST_ITEM_GET);
    if (!s) return CB_E_DB;
    b_text(s, 1, key.pk);
    b_text(s, 2, key.sk);
    int r = sqlite3_step(s), rc;
    if (r == SQLITE_ROW)
        rc = cb_item_record_decode(sqlite3_column_blob(s, 0), (size_t)sqlite3_column_bytes(s, 0), it) ? CB_OK : CB_E_DB;
    else rc = r == SQLITE_DONE ? CB_E_NOT_FOUND : fail(q, "item get", r);
    done(s);
    return rc;
}

static const char *s_of(const cb_item *it, const char *name) {
    cb_av v;
    return cb_item_get(it, name, &v) && v.kind == CB_AV_S ? v.s : NULL;
}

// Writes the whole item (its keys and TTL from its own attributes). Under the lock.
static int item_store(sq *q, const cb_item *it) {
    cb_buf rec;
    cb_buf_init(&rec);
    if (!cb_item_record_encode(it, &rec)) { cb_buf_free(&rec); return CB_E_NOMEM; }
    sqlite3_stmt *s = stmt(q, ST_ITEM_PUT);
    if (s) {
        static const char *const COLS[6] = { CB_ITEM_PK, CB_ITEM_SK, CB_ITEM_GSI1PK, CB_ITEM_GSI1SK, CB_ITEM_GSI2PK, CB_ITEM_GSI2SK };
        for (int i = 0; i < 6; i++) b_text_null(s, i + 1, s_of(it, COLS[i]));
        int64_t ttl;
        if (cb_item_n(it, CB_ITEM_TTL_MS, &ttl)) b_i64(s, 7, ttl);
        else sqlite3_bind_null(s, 7);
        b_blob_nn(s, 8, rec.p, rec.len);
    }
    int rc = step_write(q, s, "item put", false);
    cb_buf_free(&rec);
    return rc;
}

// Every clause of the write's condition, against the stored item (DynamoDB's
// semantics: a comparison with an absent attribute is false).
static bool cond_holds(const cb_item *cur, bool exists, const cb_iwrite *w) {
    for (int i = 0; i < w->n_cond; i++) {
        const cb_iclause *c = &w->cond[i];
        cb_av have;
        bool has = exists && c->v.name && cb_item_get(cur, c->v.name, &have);
        bool ok;
        switch (c->check) {
        case CB_C_EXISTS: ok = exists; break;
        case CB_C_ABSENT: ok = !exists; break;
        case CB_C_EQ: ok = has && cb_av_same(&have, &c->v); break;
        case CB_C_NE: ok = !has || !cb_av_same(&have, &c->v); break;
        case CB_C_LT: ok = has && have.kind == CB_AV_N && c->v.kind == CB_AV_N && have.n < c->v.n; break;
        case CB_C_ATTR_EXISTS: ok = has; break;
        case CB_C_ABSENT_OR_EQ: ok = !has || cb_av_same(&have, &c->v); break;
        case CB_C_ABSENT_OR_LT: ok = !has || (have.kind == CB_AV_N && c->v.kind == CB_AV_N && have.n < c->v.n); break;
        default: ok = false;
        }
        if (!ok) return false;
    }
    return true;
}

// One write against the stored item, under the lock and inside a
// transaction. old (may be NULL) gets the item as it was, whether the write
// went through or its condition refused it.
// The two items one write works on (the stored one, the one it becomes) are the
// calling thread's, kept between writes: a stored row is several KB, and a
// fresh buffer grown by realloc every write was an 8 KB musl malloc group
// mmap'ed and munmap'ed per write (mallocng unmaps a group when its last slot is
// freed; perf showed 2 mmap + 2 munmap a move). item_apply never nests. A buffer
// that grew past SCRATCH_KEEP_MAX is let go so one huge item does not stay.
#define SCRATCH_KEEP_MAX (256u * 1024)
static _Thread_local cb_item t_cur, t_next;
static _Thread_local bool t_scratch_ready;

static void scratch_done(cb_item *it) {
    if (it->store.cap > SCRATCH_KEEP_MAX) cb_item_free(it);
    else cb_item_reset(it);
}

static int item_apply(sq *q, const cb_iwrite *w, cb_item *old) {
    if (!t_scratch_ready) {
        cb_item_init(&t_cur);
        cb_item_init(&t_next);
        t_scratch_ready = true;
    }
    cb_item *const cur_p = &t_cur, *const next_p = &t_next;
#define cur (*cur_p)
#define next (*next_p)
    int rc = item_load(q, w->key, &cur);
    bool exists = rc == CB_OK;
    if (rc == CB_E_NOT_FOUND) rc = CB_OK;
    if (rc == CB_OK && !cond_holds(&cur, exists, w)) rc = CB_E_CONFLICT;
    if ((rc == CB_OK || rc == CB_E_CONFLICT) && old) {
        int keep = rc;
        rc = CB_OK;
        cb_item_reset(old);
        for (int i = 0; i < cur.n && rc == CB_OK; i++) {
            cb_av v;
            cb_item_at(&cur, i, &v);
            if (!cb_item_add(old, &v)) rc = CB_E_NOMEM;
        }
        if (rc == CB_OK) rc = keep;
    }
    if (rc == CB_OK && w->op == CB_IW_DELETE) {
        sqlite3_stmt *s = stmt(q, ST_ITEM_DELETE);
        if (s) {
            b_text(s, 1, w->key.pk);
            b_text(s, 2, w->key.sk);
        }
        rc = step_write(q, s, "item delete", false);
    }
    if (rc == CB_OK && (w->op == CB_IW_PUT || w->op == CB_IW_UPDATE)) {
        cb_av ttl_ms = CB_AVN(CB_ITEM_TTL_MS, w->ttl_ms), ttl_s = CB_AVN(CB_ITEM_TTL_S, w->ttl_ms / 1000);
        // What this write sets, in order, and how.
        // cb_iwrite_check bounds a put's attributes by CB_ITEM_ATTRS and an
        // update's sets by CB_ITEM_UPDATE_NAMES, its TTL's two among them.
        cb_iset sets[CB_ITEM_ATTRS + 4];
        int n = 0;
        sets[n++] = (cb_iset){ CB_AVS(CB_ITEM_PK, w->key.pk), CB_SET_TO };
        sets[n++] = (cb_iset){ CB_AVS(CB_ITEM_SK, w->key.sk), CB_SET_TO };
        if (w->op == CB_IW_PUT)
            for (int i = 0; i < w->n_attrs; i++) sets[n++] = (cb_iset){ w->attrs[i], CB_SET_TO };
        else
            for (int i = 0; i < w->n_sets; i++) sets[n++] = w->sets[i];
        if (w->ttl_ms > 0) {
            cb_set_how how = w->op == CB_IW_PUT ? CB_SET_TO : w->ttl_how;
            sets[n++] = (cb_iset){ ttl_ms, how };
            sets[n++] = (cb_iset){ ttl_s, how };
        }
        // An update keeps what it does not set; a put keeps nothing. Each
        // name is set once (cb_iwrite_check), so nothing is added twice.
        if (w->op == CB_IW_UPDATE)
            for (int i = 0; i < cur.n && rc == CB_OK; i++) {
                cb_av v;
                cb_item_at(&cur, i, &v);
                bool replaced = false;
                for (int j = 0; j < n && !replaced; j++) replaced = strcmp(sets[j].v.name, v.name) == 0;
                if (!replaced && !cb_item_add(&next, &v)) rc = CB_E_NOMEM;
            }
        for (int j = 0; j < n && rc == CB_OK; j++) {
            cb_av v = sets[j].v, have;
            bool had = cb_item_get(&cur, v.name, &have);
            if (sets[j].how == CB_SET_REMOVE) continue;   // not carried over above, not set here
            if (sets[j].how == CB_SET_IF_ABSENT && had) v = have;
            else if (sets[j].how == CB_SET_INCR) {
                if (v.kind != CB_AV_N || (had && have.kind != CB_AV_N)) { rc = CB_E_INVALID; break; }
                v.n += had ? have.n : 0;
            }
            if (next.n >= CB_ITEM_ATTRS) rc = CB_E_INVALID;   // the update would grow the item past what one holds
            else if (!cb_item_add(&next, &v)) rc = CB_E_NOMEM;
        }
        // What the update makes, past the item limit: refused, as DynamoDB refuses it.
        if (rc == CB_OK && cb_item_size(&next) > CB_ITEM_SIZE_MAX) rc = CB_E_INVALID;
        if (rc == CB_OK) rc = item_store(q, &next);
    }
    scratch_done(&cur);
    scratch_done(&next);
#undef cur
#undef next
    return rc;
}

// Each key read in turn under the lock (a repeated one once), projected as
// DynamoDB would, and handed out after it.
static int sq_item_batch_get(cb_store *st, const cb_ikey *keys, int n, const char *const *proj, int n_proj,
                             cb_item_at_fn fn, void *ud) {
    sq *q = Q(st);
    for (int i = 0; i < n; i++)
        if (cb_ikey_check(keys[i]) != CB_OK) return CB_E_INVALID;
    cb_item *got = calloc((size_t)(n > 0 ? n : 1), sizeof *got);
    bool *have = calloc((size_t)(n > 0 ? n : 1), sizeof *have);
    if (!got || !have) { free(got); free(have); return CB_E_NOMEM; }
    int rc = CB_OK;
    cb_item full;
    cb_item_init(&full);
    lock(q);
    for (int i = 0; rc == CB_OK && i < n; i++) {
        cb_item_init(&got[i]);
        bool dup = false;
        for (int j = 0; j < i && !dup; j++) dup = strcmp(keys[j].pk, keys[i].pk) == 0 && strcmp(keys[j].sk, keys[i].sk) == 0;
        if (dup) continue;
        int r = item_load(q, keys[i], &full);
        if (r == CB_E_NOT_FOUND) continue;
        if (r != CB_OK) { rc = r; break; }
        for (int a = 0; a < full.n && rc == CB_OK; a++) {
            cb_av v;
            cb_item_at(&full, a, &v);
            bool want = !proj || n_proj == 0 || strcmp(v.name, CB_ITEM_PK) == 0 || strcmp(v.name, CB_ITEM_SK) == 0;
            for (int p = 0; !want && p < n_proj; p++) want = strcmp(proj[p], v.name) == 0;
            if (want && !cb_item_add(&got[i], &v)) rc = CB_E_NOMEM;
        }
        have[i] = true;
    }
    unlock(q);
    bool more = true;
    for (int i = 0; i < n; i++) {
        if (rc == CB_OK && more && have[i]) more = fn(i, &got[i], ud);
        cb_item_free(&got[i]);
    }
    cb_item_free(&full);
    free(got);
    free(have);
    return rc;
}

static int sq_item_batch_delete(cb_store *st, const cb_ikey *keys, int n) {
    sq *q = Q(st);
    for (int i = 0; i < n; i++)
        if (cb_ikey_check(keys[i]) != CB_OK) return CB_E_INVALID;
    lock(q);
    int rc = begin(q);
    for (int i = 0; rc == CB_OK && i < n; i++) {
        sqlite3_stmt *s = stmt(q, ST_ITEM_DELETE);
        if (s) {
            b_text(s, 1, keys[i].pk);
            b_text(s, 2, keys[i].sk);
        }
        rc = step_write(q, s, "item delete", false);
    }
    if (rc == CB_OK) rc = commit(q);
    if (rc != CB_OK) rollback(q);
    unlock(q);
    return rc;
}

// Every item's keys, read whole under the lock and handed out after it, so
// fn may call the store.
static int sq_item_keys(cb_store *st, cb_ikey_fn fn, void *ud) {
    sq *q = Q(st);
    cb_buf keys;   // pk NUL sk NUL, for every item
    cb_buf_init(&keys);
    lock(q);
    sqlite3_stmt *s = stmt(q, ST_KEYS);
    int rc = s ? CB_OK : CB_E_DB, r = SQLITE_DONE;
    while (s && (r = sqlite3_step(s)) == SQLITE_ROW) {
        cb_buf_puts(&keys, (const char *)sqlite3_column_text(s, 0));
        cb_buf_putc(&keys, 0);
        cb_buf_puts(&keys, (const char *)sqlite3_column_text(s, 1));
        cb_buf_putc(&keys, 0);
    }
    if (s && r != SQLITE_DONE) rc = fail(q, "keys", r);
    done(s);
    unlock(q);
    if (keys.oom) rc = CB_E_NOMEM;
    for (size_t off = 0; rc == CB_OK && off < keys.len;) {
        const char *pk = (const char *)keys.p + off;
        const char *sk = pk + strlen(pk) + 1;
        off = (size_t)(sk + strlen(sk) + 1 - (const char *)keys.p);
        if (!fn(pk, sk, ud)) break;
    }
    cb_buf_free(&keys);
    return rc;
}

static int sq_item_get(cb_store *st, cb_ikey key, bool consistent, cb_item *out) {
    sq *q = Q(st);
    cb_item_reset(out);
    if (cb_ikey_check(key) != CB_OK) return CB_E_INVALID;
    lock(q);
    int rc = item_load(q, key, out);
    unlock(q);
    return rc;
}

// ---- group commit ---------------------------------------------------------------------------
//
// Item writes (a move's CAS and its rows) queue instead of each taking the
// connection for a transaction of its own. The first thread to find no group
// running becomes the leader: it takes everything queued at that moment, runs
// it as ONE transaction (BEGIN, a SAVEPOINT per request so a refused condition
// undoes only that request, COMMIT) and hands each caller its own result. A
// caller returns only after the COMMIT, so a move the player saw accepted is in
// the WAL; and the connection stays locked from BEGIN to COMMIT, so a reader
// never sees rows of a group that has not committed. A lone request runs as
// the plain transaction it always was (no savepoint). If the COMMIT fails,
// every request of the group that had succeeded reports the failure.
// There is no dedicated thread: under no contention a write costs what it did
// (no hand-off), and under contention the waiters' transactions merge.

static int apply_req(sq *q, wreq *r) {
    if (!r->txn) return item_apply(q, r->w, r->old);
    cb_item probe;
    cb_item_init(&probe);
    int rc = CB_OK;
    for (int i = 0; rc == CB_OK && i < r->n; i++) {
        rc = item_apply(q, &r->w[i], r->w[i].old_on_fail ? &probe : NULL);
        if (rc == CB_E_CONFLICT) {
            if (r->failed) *r->failed = i;
            if (r->had) *r->had = probe.n > 0;
        }
    }
    cb_item_free(&probe);
    return rc;
}

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
        r->rc = apply_req(q, r);
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

static int submit(sq *q, wreq *r) {
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

static int sq_item_write(cb_store *st, const cb_iwrite *w, cb_item *old) {
    sq *q = Q(st);
    if (old) cb_item_reset(old);
    if (cb_iwrite_check(w, 1, false) != CB_OK) return CB_E_INVALID;
    wreq r = { .w = w, .n = 1, .old = old };
    return submit(q, &r);
}

static int sq_item_txn(cb_store *st, const cb_iwrite *w, int n, int *failed, bool *failed_had_item) {
    sq *q = Q(st);
    if (failed) *failed = -1;
    if (failed_had_item) *failed_had_item = false;
    if (cb_iwrite_check(w, n, true) != CB_OK) return CB_E_INVALID;
    wreq r = { .w = w, .n = n, .txn = true, .failed = failed, .had = failed_had_item };
    return submit(q, &r);
}

// The item cut down to its pk and sk, as DynamoDB's keys-only projection
// answers it (a reader of a keys-only query can depend on nothing more).
static bool keys_only(cb_item *it) {
    cb_item k;
    cb_item_init(&k);
    cb_av v;
    bool ok = cb_item_get(it, CB_ITEM_PK, &v) && cb_item_add(&k, &v) && cb_item_get(it, CB_ITEM_SK, &v) && cb_item_add(&k, &v);
    if (ok) {
        cb_item_free(it);
        *it = k;
    } else cb_item_free(&k);
    return ok;
}

// The matching items are read under the lock and handed out after it, so fn
// may write (a delete of each, as an account's cascade does).
static int sq_item_query(cb_store *st, const cb_iquery *iq, cb_item_fn fn, void *ud) {
    sq *q = Q(st);
    static const int ST[3][2] = { { ST_ITEM_Q0D, ST_ITEM_Q0A }, { ST_ITEM_Q1D, ST_ITEM_Q1A }, { ST_ITEM_Q2D, ST_ITEM_Q2A } };
    if (cb_iquery_check(iq) != CB_OK) return CB_E_INVALID;
    cb_buf recs;   // each record as u32 length, then its bytes
    cb_buf_init(&recs);
    lock(q);
    sqlite3_stmt *s = stmt(q, ST[iq->index][iq->forward ? 1 : 0]);
    int rc = s ? CB_OK : CB_E_DB, r = SQLITE_DONE;
    if (s) {
        b_text(s, 1, iq->pk);
        b_text_null(s, 2, iq->sk_prefix);
        b_text_null(s, 3, iq->sk_below);
        while ((r = sqlite3_step(s)) == SQLITE_ROW) {
            uint32_t len = (uint32_t)sqlite3_column_bytes(s, 0);
            cb_buf_append(&recs, &len, sizeof len);
            cb_buf_append(&recs, sqlite3_column_blob(s, 0), len);
        }
        if (r != SQLITE_DONE) rc = fail(q, "item query", r);
    }
    done(s);
    unlock(q);
    if (recs.oom) rc = CB_E_NOMEM;
    cb_item it;
    cb_item_init(&it);
    for (size_t off = 0; rc == CB_OK && off < recs.len;) {
        uint32_t len;
        memcpy(&len, recs.p + off, sizeof len);
        off += sizeof len;
        cb_item_reset(&it);
        if (!cb_item_record_decode(recs.p + off, len, &it)) rc = CB_E_DB;
        else if (iq->keys_only && !keys_only(&it)) rc = CB_E_NOMEM;
        else if (!fn(&it, ud)) break;
        off += len;
    }
    cb_item_free(&it);
    cb_buf_free(&recs);
    return rc;
}






// ---- admin ---------------------------------------------------------------------------

// The key families, as DynamoDB's (store_priv.h cb_store_collection).
static int sq_admin_collections(cb_store *st, cb_name_fn fn, void *ud) {
    int n = 0;
    for (const char *c; (c = cb_store_collection(n)) != NULL;) {
        n++;
        if (!fn(c, ud)) break;
    }
    return n;
}

// A page of one key family in rowid order, each item as the console's text
// row. `prefix` narrows the pk after "<FAMILY>#"; `cursor` is the previous
// page's last rowid ("" for the first page), and next_cursor comes back ""
// after the last page.
static int sq_admin_browse(cb_store *st, const char *collection, const char *prefix, const char *cursor, int limit,
                           cb_admin_row_fn fn, void *ud, char *next_cursor, size_t next_cap) {
    if (next_cursor && next_cap) next_cursor[0] = '\0';
    char pk[CB_ITEM_KEY_MAX];
    if (!cb_store_collection_pk(collection, prefix, pk, sizeof pk)) return CB_E_INVALID;   // as DynamoDB answers
    if (limit <= 0 || limit > 1000) limit = 100;
    int64_t after = 0;
    if (cursor && *cursor && (!num_parse_i64_cstr(cursor, 0, INT64_MAX, &after))) return CB_E_INVALID;
    sq *q = Q(st);
    cb_buf recs;   // each record as u32 length, then its bytes
    cb_buf_init(&recs);
    int64_t last = 0;
    int n = 0;
    lock(q);
    sqlite3_stmt *s = stmt(q, ST_BROWSE);
    int rc = s ? CB_OK : CB_E_DB, r = SQLITE_DONE;
    if (s) {
        b_text(s, 1, pk);
        b_i64(s, 2, after);
        b_i64(s, 3, limit);
        while ((r = sqlite3_step(s)) == SQLITE_ROW) {
            last = sqlite3_column_int64(s, 0);
            uint32_t len = (uint32_t)sqlite3_column_bytes(s, 1);
            cb_buf_append(&recs, &len, sizeof len);
            cb_buf_append(&recs, sqlite3_column_blob(s, 1), len);
            n++;
        }
        if (r != SQLITE_DONE) rc = fail(q, "browse", r);
    }
    done(s);
    unlock(q);
    if (recs.oom) rc = CB_E_NOMEM;
    cb_item it;
    cb_item_init(&it);
    cb_buf text;
    cb_buf_init(&text);
    const char *names[CB_ITEM_ATTRS], *values[CB_ITEM_ATTRS];
    int emitted = 0;
    bool more = true;
    for (size_t off = 0; rc == CB_OK && more && off < recs.len;) {
        uint32_t len;
        memcpy(&len, recs.p + off, sizeof len);
        off += sizeof len;
        cb_item_reset(&it);
        int k = cb_item_record_decode(recs.p + off, len, &it) ? cb_item_text(&it, &text, names, values, CB_ITEM_ATTRS) : -1;
        off += len;
        if (k < 0) continue;
        emitted++;
        more = !fn || fn(k, names, values, ud);
    }
    cb_item_free(&it);
    cb_buf_free(&text);
    cb_buf_free(&recs);
    if (rc == CB_OK && n == limit && next_cursor && next_cap) snprintf(next_cursor, next_cap, "%lld", (long long)last);
    return rc == CB_OK ? emitted : rc;
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

static const cb_store_ops SQLITE_OPS = {
    .name = "sqlite",
    .close = sq_close,
    .item_get = sq_item_get,
    .item_write = sq_item_write,
    .item_txn = sq_item_txn,
    .item_query = sq_item_query,
    .item_batch_get = sq_item_batch_get,
    .item_batch_delete = sq_item_batch_delete,
    .item_keys = sq_item_keys,
    .admin_collections = sq_admin_collections,
    .admin_browse = sq_admin_browse,
    .admin_query = sq_admin_query,
    CB_ROWS_AUTH_OPS
};
