// backup.c - see backup.h.
#include "store/backup.h"

#include "store/store.h"
#include "util/clock.h"
#include "util/err.h"
#include "util/log.h"
#include "util/pool.h"
#include "net/loop.h"
#include "../../../../../shared/c/num_parse.h"

#include <sqlite3.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MOD "backup"

struct cb_backup {
    cb_ctx  *ctx;
    cb_blob  blob;
    char     key[256];
    char     tmp[1100];
    int64_t  interval_ms;
    atomic_bool running;
    unsigned long synced;   // the store's change count at the last upload
    bool     db_up;         // a copy went up since this process started
    uint64_t db_mark;       // the write-behind layer's mark when it was taken
    bool     listed;        // the manifest's files below are what the bucket holds
    cb_store_file *up;      // the layer's files the last manifest named, at the sizes uploaded
    int      n_up;
    atomic_long ok, failed;
};

// ---- the manifest ----------------------------------------------------------------------
//
// <key>.manifest names the files a restore puts beside the copy under <key>; it is written
// after them, so the files it names are in the bucket. "CBMF", u32 LE format (1), u32 LE
// count, then per file: u8 suffix length, the suffix, u64 LE size (as uploaded).

#define MF_MAGIC "CBMF"
#define MF_MAX 4096

static void mf_key(const cb_backup *b, char *out, size_t cap) { snprintf(out, cap, "%s.manifest", b->key); }

static int mf_write(const char *path, const cb_store_file *f, int n) {
    FILE *o = fopen(path, "wb");
    if (!o) return CB_E_IO;
    uint8_t h[12] = { 'C', 'B', 'M', 'F', 1, 0, 0, 0 };
    for (int i = 0; i < 4; i++) h[8 + i] = (uint8_t)((uint32_t)n >> (8 * i));
    bool ok = fwrite(h, 1, sizeof h, o) == sizeof h;
    for (int i = 0; ok && i < n; i++) {
        uint8_t e[1 + sizeof f[i].suffix + 8];
        const size_t sl = strlen(f[i].suffix);
        e[0] = (uint8_t)sl;
        memcpy(e + 1, f[i].suffix, sl);
        for (int k = 0; k < 8; k++) e[1 + sl + k] = (uint8_t)((uint64_t)f[i].size >> (8 * k));
        ok = fwrite(e, 1, 1 + sl + 8, o) == 1 + sl + 8;
    }
    if (fclose(o) != 0) ok = false;
    return ok ? CB_OK : CB_E_IO;
}

// The files a manifest names (malloc'd), or CB_E_DB for one that does not parse.
static int mf_read(const char *path, cb_store_file **out, int *n) {
    *out = NULL;
    *n = 0;
    FILE *in = fopen(path, "rb");
    if (!in) return CB_E_IO;
    uint8_t h[12];
    int rc = fread(h, 1, sizeof h, in) == sizeof h && memcmp(h, MF_MAGIC, 4) == 0 && h[4] == 1 && !h[5] && !h[6] && !h[7]
                 ? CB_OK : CB_E_DB;
    const uint32_t cnt = (uint32_t)h[8] | (uint32_t)h[9] << 8 | (uint32_t)h[10] << 16 | (uint32_t)h[11] << 24;
    if (rc == CB_OK && cnt > MF_MAX) rc = CB_E_DB;
    cb_store_file *f = rc == CB_OK ? calloc(cnt ? cnt : 1, sizeof *f) : NULL;
    if (rc == CB_OK && !f) rc = CB_E_NOMEM;
    for (uint32_t i = 0; rc == CB_OK && i < cnt; i++) {
        int sl = fgetc(in);
        uint8_t sz[8];
        if (sl <= 0 || sl >= (int)sizeof f[i].suffix || fread(f[i].suffix, 1, (size_t)sl, in) != (size_t)sl ||
            fread(sz, 1, 8, in) != 8 || strchr(f[i].suffix, '/') || strstr(f[i].suffix, "..") || strlen(f[i].suffix) != (size_t)sl) {
            rc = CB_E_DB;
            break;
        }
        for (int k = 7; k >= 0; k--) f[i].size = f[i].size << 8 | sz[k];
    }
    if (rc == CB_OK && fgetc(in) != EOF) rc = CB_E_DB;
    fclose(in);
    if (rc != CB_OK) { free(f); return rc; }
    *out = f;
    *n = (int)cnt;
    return CB_OK;
}

// ---- the copy ------------------------------------------------------------------------------

int cb_backup_copy(const char *db_path, const char *dest) {
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        CB_ERROR(MOD, "open %s: %s", db_path, db ? sqlite3_errmsg(db) : "out of memory");
        sqlite3_close(db);
        return CB_E_DB;
    }
    sqlite3_busy_timeout(db, 5000);
    (void)unlink(dest);   // VACUUM INTO refuses an existing file
    sqlite3_stmt *s = NULL;
    int rc = CB_E_DB;
    if (sqlite3_prepare_v2(db, "VACUUM INTO ?1", -1, &s, NULL) == SQLITE_OK && sqlite3_bind_text(s, 1, dest, -1, SQLITE_STATIC) == SQLITE_OK &&
        sqlite3_step(s) == SQLITE_DONE)
        rc = CB_OK;
    else
        CB_ERROR(MOD, "copy: %s", sqlite3_errmsg(db));
    sqlite3_finalize(s);
    sqlite3_close(db);
    return rc;
}

// The file opens and PRAGMA integrity_check answers "ok".
static bool db_is_sound(const char *path) {
    sqlite3 *db = NULL;
    bool ok = false;
    if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK) {
        sqlite3_stmt *s = NULL;
        if (sqlite3_prepare_v2(db, "PRAGMA integrity_check", -1, &s, NULL) == SQLITE_OK && sqlite3_step(s) == SQLITE_ROW) {
            const unsigned char *t = sqlite3_column_text(s, 0);
            ok = t && strcmp((const char *)t, "ok") == 0;
        }
        sqlite3_finalize(s);
    }
    sqlite3_close(db);
    return ok;
}

// ---- restore -------------------------------------------------------------------------------

int cb_backup_restore(const cb_blob *blob, const char *key, const char *db_path) {
    if (access(db_path, F_OK) == 0) return CB_OK;
    char tmp[1100], mkey[300], mtmp[1100], fkey[400], fpath[1200], ftmp[1300];
    snprintf(tmp, sizeof tmp, "%s.restore", db_path);
    (void)unlink(tmp);
    int rc = blob->get(blob->ud, key, tmp);
    if (rc == CB_E_NOT_FOUND) {
        CB_WARN(MOD, "no database at %s and no backup stored under %s: starting empty", db_path, key);
        return CB_OK;
    }
    if (rc != CB_OK) {
        CB_ERROR(MOD, "no database at %s and the backup could not be fetched (%s): not starting", db_path, cb_strerror(rc));
        (void)unlink(tmp);
        return rc;
    }
    if (!db_is_sound(tmp)) {
        CB_ERROR(MOD, "the backup fetched from %s fails its integrity check: not starting", key);
        (void)unlink(tmp);
        return CB_E_DB;
    }
    // The write-behind layer's files the copy needs (a copy uploaded before there were any has no manifest).
    snprintf(mkey, sizeof mkey, "%s.manifest", key);
    snprintf(mtmp, sizeof mtmp, "%s.manifest", tmp);
    cb_store_file *f = NULL;
    int n = 0;
    rc = blob->get(blob->ud, mkey, mtmp);
    if (rc == CB_E_NOT_FOUND) rc = CB_OK;
    else if (rc == CB_OK && (rc = mf_read(mtmp, &f, &n)) != CB_OK) CB_ERROR(MOD, "the manifest %s does not parse: not starting", mkey);
    else if (rc != CB_OK) CB_ERROR(MOD, "the manifest %s could not be fetched (%s): not starting", mkey, cb_strerror(rc));
    (void)unlink(mtmp);
    for (int i = 0; rc == CB_OK && i < n; i++) {
        snprintf(fkey, sizeof fkey, "%s%s", key, f[i].suffix);
        snprintf(fpath, sizeof fpath, "%s%s", db_path, f[i].suffix);
        snprintf(ftmp, sizeof ftmp, "%s.restore", fpath);
        if ((rc = blob->get(blob->ud, fkey, ftmp)) != CB_OK || rename(ftmp, fpath) != 0) {
            CB_ERROR(MOD, "%s, named by the manifest, could not be fetched (%s): not starting", fkey, cb_strerror(rc));
            (void)unlink(ftmp);
            if (rc == CB_OK) rc = CB_E_IO;
        }
    }
    // The marker goes before the database: a start that finds the database finds it too.
    snprintf(fpath, sizeof fpath, "%s" CB_STORE_RESTORED_SUFFIX, db_path);
    FILE *mk = rc == CB_OK ? fopen(fpath, "wb") : NULL;
    if (rc == CB_OK && (!mk || fclose(mk) != 0)) rc = CB_E_IO;
    if (rc == CB_OK && rename(tmp, db_path) != 0) {
        CB_ERROR(MOD, "rename %s: cannot place the restored database", tmp);
        rc = CB_E_IO;
    }
    if (rc != CB_OK) {
        (void)unlink(tmp);
        for (int i = 0; i < n; i++) {
            snprintf(fpath, sizeof fpath, "%s%s", db_path, f[i].suffix);
            (void)unlink(fpath);
        }
        free(f);
        return rc;
    }
    free(f);
    CB_INFO(MOD, "restored %s from %s with %d file(s) beside it", db_path, key, n);
    return CB_OK;
}

// ---- upload --------------------------------------------------------------------------------

static const cb_store_file *find_file(const cb_store_file *f, int n, const char *suffix) {
    for (int i = 0; i < n; i++) if (strcmp(f[i].suffix, suffix) == 0) return &f[i];
    return NULL;
}

// The files the bucket's manifest names, for the deletes after the next one (a failure
// leaves the list empty: what it named stays in the bucket).
static void learn_listed(cb_backup *b) {
    char mkey[300];
    mf_key(b, mkey, sizeof mkey);
    if (b->blob.get && b->blob.get(b->blob.ud, mkey, b->tmp) == CB_OK) (void)mf_read(b->tmp, &b->up, &b->n_up);
    (void)unlink(b->tmp);
    for (int i = 0; i < b->n_up; i++) b->up[i].size = -1;   // sizes unknown: every one goes up again
    b->listed = true;
}

int cb_backup_once(cb_backup *b) {
    // The database is copied only when it changed; a write-behind layer's files (the move
    // journal's segments) go beside it, so no move waits for the tables to be copied.
    cb_store *st = b->ctx->store;
    const bool wb = st->app_backup_mark && st->app_backup_files && st->app_backup_release;
    uint64_t mark = 0;
    int rc = wb ? st->app_backup_mark(st, &mark) : CB_OK;   // before the copy: what it may lack is at or after
    if (wb && !b->listed && rc == CB_OK) learn_listed(b);
    unsigned long seen = cb_store_local_changes(st);
    int64_t t0 = cb_mono_us();
    bool sent = false;
    if (rc == CB_OK && (!b->db_up || seen != b->synced)) {
        rc = cb_backup_copy(b->ctx->cfg->db_path, b->tmp);
        if (rc == CB_OK) rc = b->blob.put(b->blob.ud, b->key, b->tmp);
        (void)unlink(b->tmp);
        if (rc == CB_OK) {
            b->synced = seen;
            b->db_up = true;
            b->db_mark = mark;
            sent = true;
        }
    }
    cb_store_file *f = NULL;
    int n = 0;
    if (rc == CB_OK && wb) rc = st->app_backup_files(st, b->db_mark, &f, &n);
    bool named = sent;   // a new copy, or a file new or longer: the manifest is written again
    char key[400], path[1200];
    for (int i = 0; rc == CB_OK && i < n; i++) {
        const cb_store_file *u = find_file(b->up, b->n_up, f[i].suffix);
        if (u && u->size == f[i].size) continue;
        snprintf(key, sizeof key, "%s%s", b->key, f[i].suffix);
        snprintf(path, sizeof path, "%s%s", b->ctx->cfg->db_path, f[i].suffix);
        rc = b->blob.put(b->blob.ud, key, path);
        named = true;
    }
    if (rc == CB_OK && wb && !named) named = n != b->n_up;
    if (rc == CB_OK && wb && named) {
        char mkey[300];
        mf_key(b, mkey, sizeof mkey);
        rc = mf_write(b->tmp, f, n);
        if (rc == CB_OK) rc = b->blob.put(b->blob.ud, mkey, b->tmp);
        (void)unlink(b->tmp);
        if (rc == CB_OK) {
            // What the last manifest named and this one does not is no restore's any more.
            for (int i = 0; b->blob.del && i < b->n_up; i++)
                if (!find_file(f, n, b->up[i].suffix)) {
                    snprintf(key, sizeof key, "%s%s", b->key, b->up[i].suffix);
                    if (b->blob.del(b->blob.ud, key) != CB_OK) CB_WARN(MOD, "%s could not be deleted: it stays", key);
                }
            free(b->up);
            b->up = f;
            b->n_up = n;
            f = NULL;
            sent = true;
        }
    }
    free(f);
    // The layer's files before the uploaded copy's mark are no restore's: they may go.
    if (rc == CB_OK && wb && b->db_up) st->app_backup_release(st, b->db_mark);
    if (rc != CB_OK) {
        atomic_fetch_add(&b->failed, 1);
        CB_ERROR(MOD, "backup failed (%s): the next tick retries", cb_strerror(rc));
        return rc < 0 ? rc : CB_E_IO;
    }
    if (!sent) return 0;
    atomic_fetch_add(&b->ok, 1);
    CB_INFO(MOD, "backed up to %s in %lld ms (%d file(s) beside it)", b->key, (long long)((cb_mono_us() - t0) / 1000), n);
    return 1;
}

static void backup_job(void *ud) {
    cb_backup *b = ud;
    (void)cb_backup_once(b);
    atomic_store(&b->running, false);
}

void cb_backup_tick(cb_backup *b) {
    bool expect = false;
    if (!atomic_compare_exchange_strong(&b->running, &expect, true)) return;   // one is still running
    // (No early "unchanged" check here: a write-behind layer's files change without the file.)
    if (!b->ctx->job_pool || cb_pool_submit(b->ctx->job_pool, backup_job, b) != CB_OK) atomic_store(&b->running, false);
}

static void on_timer(cb_loop *l, void *ud) {
    cb_backup *b = ud;
    cb_backup_tick(b);
    cb_loop_timer(l, b->interval_ms, on_timer, b);
}

cb_backup *cb_backup_start(cb_ctx *ctx, const cb_blob *blob, const char *key, int64_t interval_ms) {
    cb_backup *b = calloc(1, sizeof *b);
    if (!b) return NULL;
    b->ctx = ctx;
    b->blob = *blob;
    b->interval_ms = interval_ms;
    snprintf(b->key, sizeof b->key, "%s", key);
    snprintf(b->tmp, sizeof b->tmp, "%s.backup", ctx->cfg->db_path);
    // The first tick uploads a copy whatever it finds: a start may have replayed a
    // write-behind layer's files into the file, and its numbering starts again.
    if (interval_ms > 0 && ctx->loop) cb_loop_timer(ctx->loop, interval_ms, on_timer, b);
    return b;
}

void cb_backup_stop(cb_backup *b) {
    if (!b) return;
    free(b->up);
    free(b);
}

void cb_backup_counts(const cb_backup *b, long *ok, long *failed) {
    if (ok) *ok = atomic_load(&b->ok);
    if (failed) *failed = atomic_load(&b->failed);
}

// ---- serve ---------------------------------------------------------------------------------

static int env_blob(cb_blob *blob, char *key, size_t kcap, int64_t *interval_ms) {
    const char *bucket = getenv("CBASE_BACKUP_BUCKET");
    if (!bucket || !*bucket) return CB_E_NOT_FOUND;
    const char *region = getenv("CBASE_BACKUP_REGION");
    const char *k = getenv("CBASE_BACKUP_KEY");
    snprintf(key, kcap, "%s", k && *k ? k : "cbase/latest.sqlite");
    int64_t secs = 60;
    const char *iv = getenv("CBASE_BACKUP_INTERVAL_S");
    if (iv && *iv && !num_parse_i64(iv, strlen(iv), 1, 86400, &secs)) secs = 60;
    *interval_ms = secs * 1000;
    return cb_blob_open_s3(bucket, region && *region ? region : "us-east-1", blob);
}

int cb_backup_serve_prepare(const cb_config *cfg) {
    cb_blob blob;
    char key[256];
    int64_t iv;
    int rc = env_blob(&blob, key, sizeof key, &iv);
    if (rc == CB_E_NOT_FOUND) return CB_OK;   // no bucket configured: no backups
    if (rc != CB_OK) return rc;
    return cb_backup_restore(&blob, key, cfg->db_path);
}

void cb_backup_serve_start(cb_ctx *ctx) {
    cb_blob blob;
    char key[256];
    int64_t iv;
    if (env_blob(&blob, key, sizeof key, &iv) != CB_OK) return;
    if (cb_backup_start(ctx, &blob, key, iv)) CB_INFO(MOD, "backing up to %s every %lld s when the database changed", key, (long long)(iv / 1000));
}
