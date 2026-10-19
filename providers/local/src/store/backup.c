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
    atomic_long ok, failed;
};

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
    char tmp[1100];
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
    if (rename(tmp, db_path) != 0) {
        CB_ERROR(MOD, "rename %s: cannot place the restored database", tmp);
        (void)unlink(tmp);
        return CB_E_IO;
    }
    CB_INFO(MOD, "restored %s from %s", db_path, key);
    return CB_OK;
}

// ---- upload --------------------------------------------------------------------------------

int cb_backup_once(cb_backup *b) {
    unsigned long seen = cb_store_local_changes(b->ctx->store);
    if (seen == b->synced) return 0;
    int64_t t0 = cb_mono_us();
    int rc = cb_backup_copy(b->ctx->cfg->db_path, b->tmp);
    if (rc == CB_OK) rc = b->blob.put(b->blob.ud, b->key, b->tmp);
    (void)unlink(b->tmp);
    if (rc != CB_OK) {
        atomic_fetch_add(&b->failed, 1);
        CB_ERROR(MOD, "backup failed (%s): the next tick retries", cb_strerror(rc));
        return rc < 0 ? rc : CB_E_IO;
    }
    b->synced = seen;
    atomic_fetch_add(&b->ok, 1);
    CB_INFO(MOD, "backed up to %s in %lld ms", b->key, (long long)((cb_mono_us() - t0) / 1000));
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
    if (cb_store_local_changes(b->ctx->store) == b->synced) { atomic_store(&b->running, false); return; }
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
    b->synced = cb_store_local_changes(ctx->store);   // what is in the file now is what the last start restored or seeded
    if (interval_ms > 0 && ctx->loop) cb_loop_timer(ctx->loop, interval_ms, on_timer, b);
    return b;
}

void cb_backup_stop(cb_backup *b) { free(b); }

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
