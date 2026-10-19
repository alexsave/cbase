// unit_checkpoint.c - the SQLite store's checkpointer (store_sqlite.c "the checkpointer").
// The writer's commits never checkpoint; a thread of the store does, off the commit path, and a WAL
// that keeps growing is rewound once it holds CK_CAP frames (the next group waits for a full pass). The disk is made slow: a VFS over the
// default one sleeps in every sync, as the nano's disk does under load (a checkpoint pass then always
// overlaps commits, and synchronous=NORMAL commits do not sync, so the writer is not slowed).
// Held here:
//   - the WAL stays bounded under a writer that never pauses: a PASSIVE pass alone never lets the WAL
//     rewind, so without the group's wait at CK_CAP the file holds every frame ever written;
//   - the meter counts the frames the commits appended;
//   - the commits do not sync, but for the new WAL header after each rewind;
//   - everything written is there after a reopen.
#include "store/store.h"
#include "store/store_sql.h"
#include "t.h"
#include "util/err.h"

#include <pthread.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CAP_FRAMES  8192               // store_sqlite.c CK_CAP
#define FRAME       (4096 + 24)        // a page and its frame header
#define ROWS        64
#define SYNC_US     20000

// ---- a VFS whose syncs take SYNC_US ----

typedef struct {
    sqlite3_file base;
    sqlite3_file *real;   // the default VFS's file, allocated after this struct
} slow_file;

static sqlite3_vfs *g_real;
static sqlite3_vfs g_slow;
static sqlite3_io_methods g_slow_io;
static int g_syncs, g_writer_syncs;
static pthread_t g_writer;

#define REAL(f) (((slow_file *)(f))->real)
static int s_close(sqlite3_file *f) { return REAL(f)->pMethods->xClose(REAL(f)); }
static int s_read(sqlite3_file *f, void *b, int n, sqlite3_int64 o) { return REAL(f)->pMethods->xRead(REAL(f), b, n, o); }
static int s_write(sqlite3_file *f, const void *b, int n, sqlite3_int64 o) { return REAL(f)->pMethods->xWrite(REAL(f), b, n, o); }
static int s_truncate(sqlite3_file *f, sqlite3_int64 n) { return REAL(f)->pMethods->xTruncate(REAL(f), n); }
static int s_sync(sqlite3_file *f, int fl) {
    __atomic_add_fetch(&g_syncs, 1, __ATOMIC_RELAXED);
    if (pthread_equal(pthread_self(), g_writer)) __atomic_add_fetch(&g_writer_syncs, 1, __ATOMIC_RELAXED);
    usleep(SYNC_US);
    return REAL(f)->pMethods->xSync(REAL(f), fl);
}
static int s_size(sqlite3_file *f, sqlite3_int64 *n) { return REAL(f)->pMethods->xFileSize(REAL(f), n); }
static int s_lock(sqlite3_file *f, int l) { return REAL(f)->pMethods->xLock(REAL(f), l); }
static int s_unlock(sqlite3_file *f, int l) { return REAL(f)->pMethods->xUnlock(REAL(f), l); }
static int s_check(sqlite3_file *f, int *o) { return REAL(f)->pMethods->xCheckReservedLock(REAL(f), o); }
static int s_fctl(sqlite3_file *f, int op, void *a) { return REAL(f)->pMethods->xFileControl(REAL(f), op, a); }
static int s_sector(sqlite3_file *f) { return REAL(f)->pMethods->xSectorSize(REAL(f)); }
static int s_dev(sqlite3_file *f) { return REAL(f)->pMethods->xDeviceCharacteristics(REAL(f)); }
static int s_shm_map(sqlite3_file *f, int i, int sz, int ext, void volatile **p) { return REAL(f)->pMethods->xShmMap(REAL(f), i, sz, ext, p); }
static int s_shm_lock(sqlite3_file *f, int o, int n, int fl) { return REAL(f)->pMethods->xShmLock(REAL(f), o, n, fl); }
static void s_shm_barrier(sqlite3_file *f) { REAL(f)->pMethods->xShmBarrier(REAL(f)); }
static int s_shm_unmap(sqlite3_file *f, int d) { return REAL(f)->pMethods->xShmUnmap(REAL(f), d); }

static int s_open(sqlite3_vfs *v, const char *name, sqlite3_file *f, int flags, int *out) {
    (void)v;
    slow_file *sf = (slow_file *)f;
    sf->real = (sqlite3_file *)(sf + 1);
    int rc = g_real->xOpen(g_real, name, sf->real, flags, out);
    sf->base.pMethods = rc == SQLITE_OK ? &g_slow_io : NULL;
    return rc;
}
static int s_delete(sqlite3_vfs *v, const char *n, int d) { (void)v; return g_real->xDelete(g_real, n, d); }
static int s_access(sqlite3_vfs *v, const char *n, int fl, int *o) { (void)v; return g_real->xAccess(g_real, n, fl, o); }
static int s_full(sqlite3_vfs *v, const char *n, int k, char *o) { (void)v; return g_real->xFullPathname(g_real, n, k, o); }
static int s_rand(sqlite3_vfs *v, int n, char *o) { (void)v; return g_real->xRandomness(g_real, n, o); }
static int s_sleep(sqlite3_vfs *v, int us) { (void)v; return g_real->xSleep(g_real, us); }
static int s_now(sqlite3_vfs *v, double *o) { (void)v; return g_real->xCurrentTime(g_real, o); }
static int s_err(sqlite3_vfs *v, int n, char *o) { (void)v; return g_real->xGetLastError(g_real, n, o); }

// The slow VFS becomes the default (the store opens with the default), or the real one again.
static void slow_disk(bool on) {
    if (!g_real) {
        g_real = sqlite3_vfs_find(NULL);
        g_slow = (sqlite3_vfs){ .iVersion = 1, .szOsFile = (int)sizeof(slow_file) + g_real->szOsFile, .mxPathname = g_real->mxPathname,
                                .zName = "slow-sync", .xOpen = s_open, .xDelete = s_delete, .xAccess = s_access,
                                .xFullPathname = s_full, .xRandomness = s_rand, .xSleep = s_sleep, .xCurrentTime = s_now,
                                .xGetLastError = s_err };
        g_slow_io = (sqlite3_io_methods){ .iVersion = 2, .xClose = s_close, .xRead = s_read, .xWrite = s_write, .xTruncate = s_truncate,
                                          .xSync = s_sync, .xFileSize = s_size, .xLock = s_lock, .xUnlock = s_unlock,
                                          .xCheckReservedLock = s_check, .xFileControl = s_fctl, .xSectorSize = s_sector,
                                          .xDeviceCharacteristics = s_dev, .xShmMap = s_shm_map, .xShmLock = s_shm_lock,
                                          .xShmBarrier = s_shm_barrier, .xShmUnmap = s_shm_unmap };
    }
    sqlite3_vfs_register(on ? &g_slow : g_real, 1);
}

// ---- the store ----

static int make_table(cb_sql *x, void *ud) {
    (void)ud;
    return cb_sql_exec(x, "CREATE TABLE IF NOT EXISTS t_ck(k INTEGER PRIMARY KEY, v BLOB NOT NULL)");
}

// One commit of about five frames: a 16 KB value overflows into four pages beside its leaf.
static int put(cb_sql *x, void *ud) {
    char sql[96];
    snprintf(sql, sizeof sql, "INSERT OR REPLACE INTO t_ck VALUES (%d, randomblob(16000))", *(int *)ud % ROWS);
    return cb_sql_exec(x, sql);
}

// Inserts a marker row only when all ROWS keys are there; *ud = rows changed.
static int all_rows(cb_sql *x, void *ud) {
    char sql[120];
    snprintf(sql, sizeof sql, "INSERT INTO t_ck SELECT -1, x'00' WHERE (SELECT count(*) FROM t_ck) = %d", ROWS);
    int rc = cb_sql_exec(x, sql);
    *(int *)ud = cb_sql_changes(x);
    return rc;
}

static long file_size(const char *path) {
    struct stat sb;
    return stat(path, &sb) == 0 ? (long)sb.st_size : -1;
}

static void a_busy_writer_on_a_slow_disk_keeps_the_wal_bounded(void) {
    char path[128], wal[140], rm[400];
    snprintf(path, sizeof path, "/tmp/unit_ck_%d.db", (int)getpid());
    snprintf(wal, sizeof wal, "%s-wal", path);
    snprintf(rm, sizeof rm, "rm -f %s %s-wal %s-shm", path, path, path);
    T_CHECK(system(rm) == 0);
    slow_disk(true);
    cb_store *st = NULL;
    T_EQ_INT(cb_store_open_sqlite(path, &st), CB_OK);
    T_EQ_INT(cb_sql_write(st, make_table, NULL), CB_OK);
    unsigned long before = cb_store_local_wal_frames(st);
    // Four times the cap, written with no pause.
    int commits = 0;
    long max_wal = 0;
    int syncs0 = __atomic_load_n(&g_syncs, __ATOMIC_RELAXED);
    g_writer = pthread_self();
    while (cb_store_local_wal_frames(st) - before < 4UL * CAP_FRAMES && commits < 4 * CAP_FRAMES / 5 * 2) {
        T_EQ_INT(cb_sql_write(st, put, &commits), CB_OK);
        commits++;
        long sz = file_size(wal);
        if (sz > max_wal) max_wal = sz;
    }
    int syncs = __atomic_load_n(&g_syncs, __ATOMIC_RELAXED) - syncs0;
    unsigned long frames = cb_store_local_wal_frames(st) - before;
    T_CHECK(frames >= 4UL * CAP_FRAMES);
    T_CHECK(frames >= (unsigned long)commits * 6);   // each commit: its leaf, four overflow pages and the header page, at least
    T_CHECK(frames <= (unsigned long)commits * 8);
    T_CHECK(max_wal > 0);
    T_CHECK(max_wal < (CAP_FRAMES + 64L) * FRAME);   // the cap where a group waits, and one group past it
    // The writer syncs once per rewind (SQLite syncs a new WAL header), nothing else.
    T_CHECK(__atomic_load_n(&g_writer_syncs, __ATOMIC_RELAXED) <= (int)(frames / CAP_FRAMES) + 1);
    // Passes ran between the waits too (one per CK_KICK frames while the checkpointer keeps up), not only
    // the one each wait asks for (one per CK_CAP frames, three syncs each: 12 here without the others,
    // about 24 with them).
    T_CHECK(syncs > 4 * (int)(frames / CAP_FRAMES));
    cb_store_close(st);
    slow_disk(false);
    g_writer = (pthread_t){0};
    // Reopened: every key is there.
    T_EQ_INT(cb_store_open_sqlite(path, &st), CB_OK);
    int changed = 0;
    T_EQ_INT(cb_sql_write(st, all_rows, &changed), CB_OK);
    T_EQ_INT(changed, 1);
    cb_store_close(st);
    T_CHECK(system(rm) == 0);
}

int main(void) {
    T_RUN(a_busy_writer_on_a_slow_disk_keeps_the_wal_bounded);
    return T_DONE();
}
