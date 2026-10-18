// store_sqlite.c - the SQLite backend (store.h): `cbase serve`, the tests, the
// import rehearsal. It is the item layer (items.h) on SQLite, and the admin
// console's view of it; the rows above it are written once for both backends
// (rows_auth.c, rows_foolish.c), so SQLite behaves as DynamoDB does by
// construction (docs/PLATFORM.md 8.3.2: local is the reference provider).
//
// One connection (SQLITE_OPEN_FULLMUTEX), WAL, synchronous=NORMAL, a busy
// timeout for a second process (cbase-import beside a running serve).
// sql/schema.sql (cbase_schema_sql) is applied in one transaction at every
// open, then PRAGMA user_version = CB_SCHEMA_VERSION; a database whose
// user_version is newer is refused.
//
// THREADS. serve calls the store from a pool, and one SQLite connection runs
// one transaction at a time, so every operation holds `mu` from its first
// statement to its last: a transaction can never pick up another thread's
// statements. A query reads its items under the lock and hands them out after
// releasing it, so a callback may call the store again.
#include "store.h"
#include "store_priv.h"
#include "rows_auth.h"
#include "rows_foolish.h"

#include "table.h"
#include "util/bytea.h"
#include "util/clock.h"
#include "util/log.h"
#include "util/str.h"
#include "../../../shared/c/num_parse.h"

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
    ST_BROWSE,
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

static bool any_bot(const cb_item *it, void *ud) {
    *(bool *)ud = true;
    return false;
}

// A database with no BOT# items gets the schema's bot_seed rows as them, as
// a deploy's `cbase-import --seed-bots` gives DynamoDB its bots.
static int seed_bots(cb_store *s) {
    sq *q = Q(s);
    bool have = false;
    cb_iquery bq = { .index = 2, .pk = "BOTS", .forward = true, .page = 1 };
    int rc = s->ops->item_query(s, &bq, any_bot, &have);
    if (rc != CB_OK || have) return rc;
    cb_bot_row rows[64];
    int n = 0;
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(q->db, "SELECT id, nickname, strategy_key FROM bot_seed ORDER BY rowid", -1, &st, NULL) != SQLITE_OK)
        return fail(q, "bot seed", 0);
    while (n < 64 && sqlite3_step(st) == SQLITE_ROW) {
        cb_bot_row *b = &rows[n++];
        memset(b, 0, sizeof *b);
        cb_strlcpy(b->id, (const char *)sqlite3_column_text(st, 0), sizeof b->id);
        cb_strlcpy(b->nickname, (const char *)sqlite3_column_text(st, 1), sizeof b->nickname);
        cb_strlcpy(b->strategy_key, (const char *)sqlite3_column_text(st, 2), sizeof b->strategy_key);
        b->elo_rating = TABLE_RATING_START;
        b->previous_elo = -1;
    }
    sqlite3_finalize(st);
    return n ? cb_rows_bots_replace(s, rows, n) : CB_OK;
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
        (rc = apply_schema(q)) != CB_OK || (rc = seed_bots(s)) != CB_OK) {
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

static bool set_named(const cb_item *it, const char *name) {
    cb_av v;
    return cb_item_get(it, name, &v);
}

static bool av_equal(const cb_av *a, const cb_av *b) {
    if (a->kind != b->kind) return false;
    switch (a->kind) {
    case CB_AV_S: return strcmp(a->s ? a->s : "", b->s ? b->s : "") == 0;
    case CB_AV_N: return a->n == b->n;
    case CB_AV_BOOL: return a->b == b->b;
    case CB_AV_B: return a->len == b->len && (!a->len || memcmp(a->bytes, b->bytes, a->len) == 0);
    case CB_AV_SL:
        if (a->n_list != b->n_list) return false;
        for (int i = 0; i < a->n_list; i++)
            if (strcmp(a->list[i], b->list[i]) != 0) return false;
        return true;
    }
    return false;
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
        case CB_C_EQ: ok = has && av_equal(&have, &c->v); break;
        case CB_C_NE: ok = !has || !av_equal(&have, &c->v); break;
        case CB_C_LT: ok = has && have.kind == CB_AV_N && c->v.kind == CB_AV_N && have.n < c->v.n; break;
        case CB_C_ATTR_EXISTS: ok = has; break;
        case CB_C_ABSENT_OR_EQ: ok = !has || av_equal(&have, &c->v); break;
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
static int item_apply(sq *q, const cb_iwrite *w, cb_item *old) {
    cb_item cur, next;
    cb_item_init(&cur);
    cb_item_init(&next);
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
        cb_iset sets[CB_ITEM_ATTRS + 4];
        int n = 0;
        sets[n++] = (cb_iset){ CB_AVS(CB_ITEM_PK, w->key.pk), CB_SET_TO };
        sets[n++] = (cb_iset){ CB_AVS(CB_ITEM_SK, w->key.sk), CB_SET_TO };
        if (w->op == CB_IW_PUT)
            for (int i = 0; i < w->n_attrs && n < CB_ITEM_ATTRS; i++) sets[n++] = (cb_iset){ w->attrs[i], CB_SET_TO };
        else
            for (int i = 0; i < w->n_sets && n < CB_ITEM_ATTRS; i++) sets[n++] = w->sets[i];
        if (w->ttl_ms > 0) {
            cb_set_how how = w->op == CB_IW_PUT ? CB_SET_TO : w->ttl_how;
            sets[n++] = (cb_iset){ ttl_ms, how };
            sets[n++] = (cb_iset){ ttl_s, how };
        }
        // An update keeps what it does not set; a put keeps nothing.
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
            if (set_named(&next, v.name)) continue;   // never: each name is set once
            if (!cb_item_add(&next, &v)) rc = CB_E_NOMEM;
        }
        if (rc == CB_OK) rc = item_store(q, &next);
    }
    cb_item_free(&cur);
    cb_item_free(&next);
    return rc;
}

// Each key read in turn under the lock (a repeated one once), projected as
// DynamoDB would, and handed out after it.
static int sq_item_batch_get(cb_store *st, const cb_ikey *keys, int n, const char *const *proj, int n_proj,
                             cb_item_at_fn fn, void *ud) {
    sq *q = Q(st);
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

static int sq_item_get(cb_store *st, cb_ikey key, bool consistent, cb_item *out) {
    sq *q = Q(st);
    lock(q);
    int rc = item_load(q, key, out);
    unlock(q);
    return rc;
}

static int sq_item_write(cb_store *st, const cb_iwrite *w, cb_item *old) {
    sq *q = Q(st);
    if (w->op == CB_IW_CHECK) return CB_E_INVALID;
    if (old) cb_item_reset(old);
    lock(q);
    int rc = begin(q);
    if (rc == CB_OK) rc = item_apply(q, w, old);
    if (rc == CB_OK) rc = commit(q);
    if (rc != CB_OK) rollback(q);
    unlock(q);
    return rc;
}

static int sq_item_txn(cb_store *st, const cb_iwrite *w, int n, int *failed, bool *failed_had_item) {
    sq *q = Q(st);
    if (failed) *failed = -1;
    if (failed_had_item) *failed_had_item = false;
    cb_item probe;
    cb_item_init(&probe);
    if (n < 1 || n > CB_ITEM_TXN_MAX) return CB_E_INVALID;
    lock(q);
    int rc = begin(q);
    for (int i = 0; rc == CB_OK && i < n; i++) {
        rc = item_apply(q, &w[i], w[i].old_on_fail ? &probe : NULL);
        if (rc == CB_E_CONFLICT) {
            if (failed) *failed = i;
            if (failed_had_item) *failed_had_item = probe.n > 0;
        }
    }
    cb_item_free(&probe);
    if (rc == CB_OK) rc = commit(q);
    if (rc != CB_OK) rollback(q);
    unlock(q);
    return rc;
}

// The matching items are read under the lock and handed out after it, so fn
// may write (a delete of each, as an account's cascade does).
static int sq_item_query(cb_store *st, const cb_iquery *iq, cb_item_fn fn, void *ud) {
    sq *q = Q(st);
    static const int ST[3][2] = { { ST_ITEM_Q0D, ST_ITEM_Q0A }, { ST_ITEM_Q1D, ST_ITEM_Q1A }, { ST_ITEM_Q2D, ST_ITEM_Q2A } };
    if (iq->index < 0 || iq->index > 2) return CB_E_INVALID;
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
    if (!cb_store_collection_pk(collection, prefix, pk, sizeof pk)) return CB_E_NOT_FOUND;
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
    .admin_collections = sq_admin_collections,
    .admin_browse = sq_admin_browse,
    .admin_query = sq_admin_query,
    CB_ROWS_AUTH_OPS
    CB_ROWS_FOOLISH_OPS
};
