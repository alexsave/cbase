// unit_backup.c - the database backup (store/backup.h): what it sends and when, what a restore accepts.
// No real object store: a cb_blob that keeps files in a directory, with failures on demand.
#include "store/backup.h"
#include "store/store.h"
#include "store/items.h"
#include "t.h"
#include "util/err.h"
#include "util/log.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char g_dir[256];
static int g_puts, g_gets, g_fail_put, g_fail_get, g_corrupt_get, g_empty_get;

static void cp(const char *a, const char *b) {
    FILE *f = fopen(a, "rb"), *o = fopen(b, "wb");
    char buf[4096];
    size_t n;
    while (f && o && (n = fread(buf, 1, sizeof buf, f)) > 0) fwrite(buf, 1, n, o);
    if (f) fclose(f);
    if (o) fclose(o);
}

static void obj_path(char *out, size_t cap) { snprintf(out, cap, "%s/object", g_dir); }

static int fake_put(void *ud, const char *key, const char *path) {
    (void)ud; (void)key;
    g_puts++;
    if (g_fail_put) return CB_E_IO;
    char o[300];
    obj_path(o, sizeof o);
    cp(path, o);
    return CB_OK;
}

static int fake_get(void *ud, const char *key, const char *path) {
    (void)ud; (void)key;
    g_gets++;
    if (g_fail_get) return CB_E_IO;
    char o[300];
    obj_path(o, sizeof o);
    if (g_empty_get || access(o, F_OK) != 0) return CB_E_NOT_FOUND;
    if (g_corrupt_get) {
        FILE *f = fopen(path, "wb");
        fputs("this is not a database, it is a long enough string to look like a header and then some", f);
        fclose(f);
        return CB_OK;
    }
    cp(o, path);
    return CB_OK;
}

static void fresh(void) {
    snprintf(g_dir, sizeof g_dir, "/tmp/unit_backup_%d", (int)getpid());
    char cmd[400];
    snprintf(cmd, sizeof cmd, "rm -rf %s && mkdir -p %s", g_dir, g_dir);
    T_CHECK(system(cmd) == 0);
    g_puts = g_gets = g_fail_put = g_fail_get = g_corrupt_get = g_empty_get = 0;
}

static int count_rows(const char *path, const char *sql) {
    sqlite3 *db = NULL;
    int n = -1;
    if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK) {
        sqlite3_stmt *s = NULL;
        if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK && sqlite3_step(s) == SQLITE_ROW) n = sqlite3_column_int(s, 0);
        sqlite3_finalize(s);
    }
    sqlite3_close(db);
    return n;
}

static void put_item(cb_store *st, const char *pk) {
    cb_iwrite w = { .op = CB_IW_PUT, .key = { pk, "sk" } };
    T_EQ_INT(st->ops->item_write(st, &w, NULL), CB_OK);
}

static void the_copy_is_a_valid_database_with_the_same_rows(void) {
    fresh();
    char db[300], snap[300];
    snprintf(db, sizeof db, "%s/db", g_dir);
    snprintf(snap, sizeof snap, "%s/snap", g_dir);
    cb_store *st = NULL;
    T_EQ_INT(cb_store_open_sqlite(db, &st), CB_OK);
    for (int i = 0; i < 5; i++) { char pk[16]; snprintf(pk, sizeof pk, "p%d", i); put_item(st, pk); }
    T_EQ_INT(cb_backup_copy(db, snap), CB_OK);   // the store is still open and has written
    T_EQ_INT(count_rows(snap, "SELECT count(*) FROM items WHERE pk LIKE 'p%'"), 5);
    T_EQ_INT(count_rows(snap, "SELECT count(*) FROM pragma_integrity_check WHERE integrity_check = 'ok'"), 1);
    T_EQ_INT(cb_backup_copy(db, snap), CB_OK);   // a second one replaces the first
    cb_store_close(st);
}

static void only_a_change_sends_and_a_failed_upload_retries(void) {
    fresh();
    char db[300];
    snprintf(db, sizeof db, "%s/db", g_dir);
    cb_store *st = NULL;
    T_EQ_INT(cb_store_open_sqlite(db, &st), CB_OK);
    cb_config cfg;
    memset(&cfg, 0, sizeof cfg);
    snprintf(cfg.db_path, sizeof cfg.db_path, "%s", db);
    cb_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.cfg = &cfg;
    ctx.store = st;
    cb_blob blob = { fake_put, fake_get, NULL };
    cb_backup *b = cb_backup_start(&ctx, &blob, "k", 0);
    T_CHECK(b != NULL);
    T_EQ_INT(cb_backup_once(b), 0);   // nothing changed since the start
    T_EQ_INT(g_puts, 0);
    put_item(st, "a");
    g_fail_put = 1;
    T_CHECK(cb_backup_once(b) < 0);   // the upload fails: nothing is marked sent, the process is alive
    g_fail_put = 0;
    T_EQ_INT(cb_backup_once(b), 1);   // the next tick sends it
    T_EQ_INT(g_puts, 2);
    T_EQ_INT(cb_backup_once(b), 0);   // and an unchanged store sends nothing more
    T_EQ_INT(g_puts, 2);
    char o[300];
    obj_path(o, sizeof o);
    T_EQ_INT(count_rows(o, "SELECT count(*) FROM items WHERE pk = 'a'"), 1);
    long ok, failed;
    cb_backup_counts(b, &ok, &failed);
    T_EQ_INT((int)ok, 1);
    T_EQ_INT((int)failed, 1);
    cb_backup_stop(b);
    cb_store_close(st);
}

static void restore_puts_a_sound_backup_in_place_and_refuses_the_rest(void) {
    fresh();
    char db[300], src[300], o[300];
    snprintf(db, sizeof db, "%s/db", g_dir);
    snprintf(src, sizeof src, "%s/src", g_dir);
    obj_path(o, sizeof o);
    cb_store *st = NULL;
    T_EQ_INT(cb_store_open_sqlite(src, &st), CB_OK);
    put_item(st, "kept");
    cb_store_close(st);
    T_EQ_INT(cb_backup_copy(src, o), CB_OK);
    cb_blob blob = { fake_put, fake_get, NULL };
    // corrupt download: refused, nothing in place
    g_corrupt_get = 1;
    T_EQ_INT(cb_backup_restore(&blob, "k", db), CB_E_DB);
    T_CHECK(access(db, F_OK) != 0);
    g_corrupt_get = 0;
    // a failed download: refused too, so an empty database does not replace the good backup
    g_fail_get = 1;
    T_EQ_INT(cb_backup_restore(&blob, "k", db), CB_E_IO);
    T_CHECK(access(db, F_OK) != 0);
    g_fail_get = 0;
    // a good one
    T_EQ_INT(cb_backup_restore(&blob, "k", db), CB_OK);
    T_EQ_INT(count_rows(db, "SELECT count(*) FROM items WHERE pk = 'kept'"), 1);
    // an existing database is never touched
    int gets = g_gets;
    T_EQ_INT(cb_backup_restore(&blob, "k", db), CB_OK);
    T_EQ_INT(g_gets, gets);
    // nothing stored: an empty start
    unlink(db);
    g_empty_get = 1;
    T_EQ_INT(cb_backup_restore(&blob, "k", db), CB_OK);
    T_CHECK(access(db, F_OK) != 0);
}

int main(void) {
    cb_log_init(CB_LOG_ERROR + 1, 0);
    T_RUN(the_copy_is_a_valid_database_with_the_same_rows);
    T_RUN(only_a_change_sends_and_a_failed_upload_retries);
    T_RUN(restore_puts_a_sound_backup_in_place_and_refuses_the_rest);
    return T_DONE();
}
