/* sqlite_group.inc : the `sqlite` group of calbench.c (included by it, not built on its own).
 *
 * What SQLite costs per syscall and per transaction with production's settings (c_base/providers/local/src/store/
 * store_sqlite.c): SQLITE_OPEN_FULLMUTEX, busy_timeout 5000, journal_mode=WAL, synchronous=NORMAL, the default page_size
 * and cache_size, the writer's automatic checkpoint replaced by a PASSIVE checkpoint every 1000 frames off the request
 * path (here: untimed, between samples), and the store's pooled allocator for 1 KB to 64 KB blocks. The SQLite is the
 * one serve links: alpine 3.24's sqlite-static, built in the same pinned image.
 *
 * Syscall counting: build.sh links with -Wl,--wrap=<fn> for every libc entry point below, so each call anything in the
 * binary (SQLite included) makes to one of them passes a counter, which is on only while a timed sample runs. In musl
 * each of these is one syscall. It does not see musl's own internal calls (malloc's mmap/munmap) or the vdso clock;
 * the runner's strace pass (sqlitecount) sees everything. /proc/self/io syscr and syscw are a kernel-side cross-check.
 * Each timed transaction row is followed by a `#sys` comment line: name, param, samples, then per-sample means. */

/* ---- counted libc entry points ---- */
enum { W_open, W_close, W_access, W_stat, W_lstat, W_fstat, W_ftruncate, W_read, W_write, W_pread, W_pwrite, W_lseek, W_fsync,
  W_fdatasync, W_getpid, W_mmap, W_munmap, W_mremap, W_unlink, W_ioctl, W_geteuid, W_fchown, W_fchmod, W_readlink, W_getcwd,
  W_posix_fallocate, W_fcntl_setlk_rd, W_fcntl_setlk_wr, W_fcntl_setlk_un, W_fcntl_getlk, W_fcntl_setlkw, W_fcntl_other, W_N };
static const char *WN[W_N] = {"open", "close", "access", "stat", "lstat", "fstat", "ftruncate", "read", "write", "pread", "pwrite",
  "lseek", "fsync", "fdatasync", "getpid", "mmap", "munmap", "mremap", "unlink", "ioctl", "geteuid", "fchown", "fchmod", "readlink",
  "getcwd", "posix_fallocate", "fcntl_setlk_rd", "fcntl_setlk_wr", "fcntl_setlk_un", "fcntl_getlk", "fcntl_setlkw", "fcntl_other"};
static int w_on;
static long wc[W_N];
#define WC(i) do { if (w_on) wc[i]++; } while (0)
int __real_open(const char *, int, ...);
int __wrap_open(const char *p, int f, ...) { mode_t m = 0; if (f & O_CREAT) { va_list a; va_start(a, f); m = va_arg(a, mode_t); va_end(a); } WC(W_open); return __real_open(p, f, m); }
int __real_close(int); int __wrap_close(int fd) { WC(W_close); return __real_close(fd); }
int __real_access(const char *, int); int __wrap_access(const char *p, int m) { WC(W_access); return __real_access(p, m); }
int __real_stat(const char *restrict, struct stat *restrict); int __wrap_stat(const char *restrict p, struct stat *restrict s) { WC(W_stat); return __real_stat(p, s); }
int __real_lstat(const char *restrict, struct stat *restrict); int __wrap_lstat(const char *restrict p, struct stat *restrict s) { WC(W_lstat); return __real_lstat(p, s); }
int __real_fstat(int, struct stat *); int __wrap_fstat(int fd, struct stat *s) { WC(W_fstat); return __real_fstat(fd, s); }
int __real_ftruncate(int, off_t); int __wrap_ftruncate(int fd, off_t l) { WC(W_ftruncate); return __real_ftruncate(fd, l); }
ssize_t __real_read(int, void *, size_t); ssize_t __wrap_read(int fd, void *b, size_t n) { WC(W_read); return __real_read(fd, b, n); }
ssize_t __real_write(int, const void *, size_t); ssize_t __wrap_write(int fd, const void *b, size_t n) { WC(W_write); return __real_write(fd, b, n); }
ssize_t __real_pread(int, void *, size_t, off_t); ssize_t __wrap_pread(int fd, void *b, size_t n, off_t o) { WC(W_pread); return __real_pread(fd, b, n, o); }
ssize_t __real_pwrite(int, const void *, size_t, off_t); ssize_t __wrap_pwrite(int fd, const void *b, size_t n, off_t o) { WC(W_pwrite); return __real_pwrite(fd, b, n, o); }
off_t __real_lseek(int, off_t, int); off_t __wrap_lseek(int fd, off_t o, int w) { WC(W_lseek); return __real_lseek(fd, o, w); }
int __real_fsync(int); int __wrap_fsync(int fd) { WC(W_fsync); return __real_fsync(fd); }
int __real_fdatasync(int); int __wrap_fdatasync(int fd) { WC(W_fdatasync); return __real_fdatasync(fd); }
pid_t __real_getpid(void); pid_t __wrap_getpid(void) { WC(W_getpid); return __real_getpid(); }
void *__real_mmap(void *, size_t, int, int, int, off_t); void *__wrap_mmap(void *a, size_t n, int p, int f, int fd, off_t o) { WC(W_mmap); return __real_mmap(a, n, p, f, fd, o); }
int __real_munmap(void *, size_t); int __wrap_munmap(void *a, size_t n) { WC(W_munmap); return __real_munmap(a, n); }
void *__real_mremap(void *, size_t, size_t, int, ...);
void *__wrap_mremap(void *a, size_t o, size_t n, int f, ...) { void *na = 0; if (f & MREMAP_FIXED) { va_list v; va_start(v, f); na = va_arg(v, void *); va_end(v); } WC(W_mremap); return __real_mremap(a, o, n, f, na); }
int __real_unlink(const char *); int __wrap_unlink(const char *p) { WC(W_unlink); return __real_unlink(p); }
int __real_ioctl(int, int, ...);
int __wrap_ioctl(int fd, int r, ...) { va_list v; va_start(v, r); void *x = va_arg(v, void *); va_end(v); WC(W_ioctl); return __real_ioctl(fd, r, x); }
uid_t __real_geteuid(void); uid_t __wrap_geteuid(void) { WC(W_geteuid); return __real_geteuid(); }
int __real_fchown(int, uid_t, gid_t); int __wrap_fchown(int fd, uid_t u, gid_t g) { WC(W_fchown); return __real_fchown(fd, u, g); }
int __real_fchmod(int, mode_t); int __wrap_fchmod(int fd, mode_t m) { WC(W_fchmod); return __real_fchmod(fd, m); }
ssize_t __real_readlink(const char *restrict, char *restrict, size_t);
ssize_t __wrap_readlink(const char *restrict p, char *restrict b, size_t n) { WC(W_readlink); return __real_readlink(p, b, n); }
char *__real_getcwd(char *, size_t); char *__wrap_getcwd(char *b, size_t n) { WC(W_getcwd); return __real_getcwd(b, n); }
int __real_posix_fallocate(int, off_t, off_t); int __wrap_posix_fallocate(int fd, off_t o, off_t l) { WC(W_posix_fallocate); return __real_posix_fallocate(fd, o, l); }
int __real_fcntl(int, int, ...);
int __wrap_fcntl(int fd, int cmd, ...) {
  va_list a; va_start(a, cmd);
  if (cmd == F_SETLK || cmd == F_SETLKW || cmd == F_GETLK || cmd == F_OFD_SETLK || cmd == F_OFD_SETLKW || cmd == F_OFD_GETLK) {
    struct flock *l = va_arg(a, struct flock *); va_end(a);
    if (w_on) {
      if (cmd == F_GETLK || cmd == F_OFD_GETLK) wc[W_fcntl_getlk]++;
      else if (cmd == F_SETLKW || cmd == F_OFD_SETLKW) wc[W_fcntl_setlkw]++;
      else wc[l->l_type == F_RDLCK ? W_fcntl_setlk_rd : l->l_type == F_WRLCK ? W_fcntl_setlk_wr : W_fcntl_setlk_un]++;
    }
    return __real_fcntl(fd, cmd, l);
  }
  unsigned long x = va_arg(a, unsigned long); va_end(a);
  WC(W_fcntl_other);
  return __real_fcntl(fd, cmd, x);
}
/* syscr/syscw of this process; the read that takes the first snapshot is counted in the second (subtracted by the caller) */
static void pio(long *r, long *w) {
  char b[512]; *r = *w = -1;
  int fd = __real_open("/proc/self/io", O_RDONLY);
  if (fd < 0) return;
  ssize_t n = __real_read(fd, b, sizeof b - 1); __real_close(fd);
  if (n <= 0) return;
  b[n] = 0;
  char *p = strstr(b, "syscr:"), *q = strstr(b, "syscw:");
  if (p) *r = atol(p + 6);
  if (q) *w = atol(q + 6);
}

/* ---- the store's allocator (store_sqlite.c "SQLite's allocator"): 1 KB to 64 KB blocks from per-class free lists ---- */
#define PM_MIN 11
#define PM_CL 6
#define PM_KEEP 8
#define PM_HDR 16
typedef struct pmb { struct pmb *next; } pmb;
static pmb *pm_pool[PM_CL];
static int pm_n[PM_CL];
static pthread_mutex_t pm_mu = PTHREAD_MUTEX_INITIALIZER;
static int pm_class(size_t n) {
  if (n <= ((size_t)1 << (PM_MIN - 1))) return -1;
  for (int c = 0; c < PM_CL; c++) if (n <= ((size_t)1 << (PM_MIN + c))) return c;
  return -1;
}
static void *pm_malloc(int n) {
  if (n <= 0) return 0;
  int c = pm_class((size_t)n);
  size_t u = c >= 0 ? ((size_t)1 << (PM_MIN + c)) : ((size_t)n + 7) & ~(size_t)7;
  unsigned char *b = 0;
  if (c >= 0) {
    pthread_mutex_lock(&pm_mu);
    if (pm_pool[c]) { b = (unsigned char *)pm_pool[c]; pm_pool[c] = pm_pool[c]->next; pm_n[c]--; }
    pthread_mutex_unlock(&pm_mu);
    if (b) { *(size_t *)(b - PM_HDR) = u; return b; }
  }
  b = malloc(u + PM_HDR);
  if (!b) return 0;
  *(size_t *)b = u;
  return b + PM_HDR;
}
static void pm_free(void *v) {
  if (!v) return;
  unsigned char *b = (unsigned char *)v - PM_HDR;
  size_t u = *(size_t *)b;
  int c = pm_class(u);
  if (c >= 0 && u == ((size_t)1 << (PM_MIN + c))) {
    pthread_mutex_lock(&pm_mu);
    if (pm_n[c] < PM_KEEP) { ((pmb *)v)->next = pm_pool[c]; pm_pool[c] = v; pm_n[c]++; pthread_mutex_unlock(&pm_mu); return; }
    pthread_mutex_unlock(&pm_mu);
  }
  free(b);
}
static int pm_size(void *v) { return v ? (int)*(size_t *)((unsigned char *)v - PM_HDR) : 0; }
static void *pm_realloc(void *v, int n) {
  if (!v) return pm_malloc(n);
  if (n <= 0) { pm_free(v); return 0; }
  if ((size_t)n <= *(size_t *)((unsigned char *)v - PM_HDR)) return v;
  void *nv = pm_malloc(n);
  if (!nv) return 0;
  memcpy(nv, v, *(size_t *)((unsigned char *)v - PM_HDR));
  pm_free(v);
  return nv;
}
static int pm_roundup(int n) { if (n <= 0) return 0; int c = pm_class((size_t)n); return c >= 0 ? (1 << (PM_MIN + c)) : (n + 7) & ~7; }
static int pm_init(void *u) { (void)u; return SQLITE_OK; }
static void pm_shutdown(void *u) { (void)u; }
static const sqlite3_mem_methods PM = {pm_malloc, pm_free, pm_realloc, pm_size, pm_roundup, pm_init, pm_shutdown, 0};

/* ---- the database: foolish's games table (foolish/server/sql/schema.sql) and a page-per-row table for checkpoints ---- */
static char sq_path[256], sq_scr[256];
static sqlite3 *sdb;
static sqlite3_stmt *s_begin, *s_commit, *s_ins, *s_upd, *s_sel, *s_ckupd;
static int sq_rofd = -1;           /* the db file, read-only, for fstat and fadvise. Never closed while SQLite has the file open:
                                      closing any fd of a file drops every POSIX lock this process holds on it */
static long sq_fill = 20000;       /* rows in games before the timed rows */
static long sq_next;               /* the next new row's index */
static int sq_walframes;           /* the WAL's length in frames after the last commit (the WAL hook) */
static long sq_ckpts;              /* untimed checkpoints run between samples */
static uint64_t sq_mix(uint64_t x) { x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL; x ^= x >> 33; return x; }
static void sq_id(char out[17], long i) { snprintf(out, 17, "%016llx", (unsigned long long)sq_mix((uint64_t)i + 1)); }
static int sq_hook(void *u, sqlite3 *d, const char *name, int n) { (void)u; (void)d; (void)name; sq_walframes = n; return SQLITE_OK; }
static void sq_exec(sqlite3 *d, const char *sql) {
  char *e = 0;
  if (sqlite3_exec(d, sql, 0, 0, &e) != SQLITE_OK) { printf("# sqlite exec failed: %s: %s\n", sql, e ? e : "?"); fflush(stdout); exit(1); }
}
static sqlite3 *sq_open(void) {
  sqlite3 *d = 0;
  if (sqlite3_open_v2(sq_path, &d, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, 0) != SQLITE_OK) {
    printf("# sqlite open failed: %s\n", d ? sqlite3_errmsg(d) : "no memory"); fflush(stdout); exit(1);
  }
  sqlite3_extended_result_codes(d, 1);
  sqlite3_busy_timeout(d, 5000);
  sq_exec(d, "PRAGMA journal_mode = WAL");
  sq_exec(d, "PRAGMA synchronous = NORMAL");
  return d;
}
static sqlite3_stmt *sq_prep(const char *sql) {
  sqlite3_stmt *s = 0;
  if (sqlite3_prepare_v2(sdb, sql, -1, &s, 0) != SQLITE_OK) { printf("# prepare failed: %s: %s\n", sql, sqlite3_errmsg(sdb)); fflush(stdout); exit(1); }
  return s;
}
static void sq_step(sqlite3_stmt *s, int want) {
  int rc = sqlite3_step(s);
  if (rc != want) { printf("# step %d (want %d): %s\n", rc, want, sqlite3_errmsg(sdb)); fflush(stdout); exit(1); }
  sqlite3_reset(s);
}
static char sq_blob[65536];
static void sq_insert_row(int blob) {
  char id[17]; sq_id(id, sq_next++);
  int64_t now = now_ns() / 1000000;
  sqlite3_bind_text(s_ins, 1, id, 16, SQLITE_TRANSIENT);
  sqlite3_bind_blob(s_ins, 2, sq_blob, blob, SQLITE_STATIC);
  sqlite3_bind_blob(s_ins, 3, sq_blob + 7000, 64, SQLITE_STATIC);   /* roster */
  sqlite3_bind_blob(s_ins, 4, sq_blob + 9000, 32, SQLITE_STATIC);   /* seed */
  sqlite3_bind_int64(s_ins, 5, now);
  sq_step(s_ins, SQLITE_DONE);
}
/* untimed, between samples: the checkpointer's PASSIVE pass every 1000 frames */
static void bt_ckpt(void) {
  if (sq_walframes >= 1000) { sqlite3_wal_checkpoint_v2(sdb, 0, SQLITE_CHECKPOINT_PASSIVE, 0, 0); sq_walframes = 0; sq_ckpts++; }
}
static void sq_setup(void) {
  const char *dir = getenv("CAL_DIR") ? getenv("CAL_DIR") : "/var/tmp";
  snprintf(sq_path, sizeof sq_path, "%s/calbench_sq.db", dir);
  snprintf(sq_scr, sizeof sq_scr, "%s/calbench_sq.scratch", dir);
  char w[300];
  unlink(sq_path); snprintf(w, sizeof w, "%s-wal", sq_path); unlink(w); snprintf(w, sizeof w, "%s-shm", sq_path); unlink(w);
  sqlite3_config(SQLITE_CONFIG_MALLOC, &PM);  /* before SQLite's first use, as the store does */
  for (size_t i = 0; i < sizeof sq_blob; i++) sq_blob[i] = (char)(xr() & 0xff);
  sdb = sq_open();
  sqlite3_wal_hook(sdb, sq_hook, 0);   /* replaces the writer's automatic checkpoint, as the store's checkpointer does */
  sq_exec(sdb, "CREATE TABLE games (id TEXT PRIMARY KEY, status INTEGER NOT NULL, version INTEGER NOT NULL, state BLOB NOT NULL, "
               "roster BLOB NOT NULL, seed BLOB, needs_bots INTEGER NOT NULL, round_epoch INTEGER NOT NULL, log_from INTEGER NOT NULL, "
               "created_at INTEGER NOT NULL, updated_at INTEGER NOT NULL, last_commit_at INTEGER NOT NULL)");
  sq_exec(sdb, "CREATE TABLE ck (id INTEGER PRIMARY KEY, v BLOB NOT NULL)");
  s_begin = sq_prep("BEGIN IMMEDIATE");
  s_commit = sq_prep("COMMIT");
  s_ins = sq_prep("INSERT INTO games VALUES (?1, 0, 1, ?2, ?3, ?4, 0, 0, 0, ?5, ?5, ?5)");
  s_upd = sq_prep("UPDATE games SET state = ?2, version = version + 1, updated_at = ?3, last_commit_at = ?3 WHERE id = ?1");
  s_sel = sq_prep("SELECT state, roster, version FROM games WHERE id = ?1");
  s_ckupd = sq_prep("UPDATE ck SET v = ?2 WHERE id = ?1");
  sq_next = 0;
  for (long i = 0; i < sq_fill; i += 1000) {
    sq_step(s_begin, SQLITE_DONE);
    for (long j = 0; j < 1000; j++) sq_insert_row(1500);
    sq_step(s_commit, SQLITE_DONE);
  }
  sqlite3_stmt *ci = sq_prep("INSERT INTO ck VALUES (?1, ?2)");
  sq_step(s_begin, SQLITE_DONE);
  for (int i = 1; i <= 1000; i++) { sqlite3_bind_int(ci, 1, i); sqlite3_bind_blob(ci, 2, sq_blob, 3000, SQLITE_STATIC); sq_step(ci, SQLITE_DONE); }
  sq_step(s_commit, SQLITE_DONE);
  sqlite3_finalize(ci);
  sqlite3_wal_checkpoint_v2(sdb, 0, SQLITE_CHECKPOINT_TRUNCATE, 0, 0);
  sq_walframes = 0;
  sq_rofd = open(sq_path, O_RDONLY);
}

/* ---- transaction ops: one call is one transaction (or one autocommit SELECT) ---- */
typedef struct { int n, blob; long lo, range; } txc;
static void op_txn(void *c) { txc *v = c; sq_step(s_begin, SQLITE_DONE); for (int i = 0; i < v->n; i++) sq_insert_row(v->blob); sq_step(s_commit, SQLITE_DONE); }
static void op_upd(void *c) {
  txc *v = c; char id[17];
  sq_step(s_begin, SQLITE_DONE);
  for (int i = 0; i < v->n; i++) {
    sq_id(id, (long)(xr() % (uint64_t)sq_fill));
    sqlite3_bind_text(s_upd, 1, id, 16, SQLITE_TRANSIENT);
    sqlite3_bind_blob(s_upd, 2, sq_blob, v->blob, SQLITE_STATIC);
    sqlite3_bind_int64(s_upd, 3, now_ns() / 1000000);
    sq_step(s_upd, SQLITE_DONE);
  }
  sq_step(s_commit, SQLITE_DONE);
}
static void op_sel(void *c) {
  txc *v = c; char id[17];
  sq_id(id, v->lo + (long)(xr() % (uint64_t)v->range));
  sqlite3_bind_text(s_sel, 1, id, 16, SQLITE_TRANSIENT);
  if (sqlite3_step(s_sel) != SQLITE_ROW) { printf("# select: no row\n"); exit(1); }
  const void *b = sqlite3_column_blob(s_sel, 0); OPAQUE(b);
  sqlite3_reset(s_sel);
}
static int ck_pages; static long ck_frames, ck_log, ck_calls;
static void bt_ckprep(void) {   /* a clean WAL, then ck_pages rows (one page each) dirtied in one transaction */
  sqlite3_wal_checkpoint_v2(sdb, 0, SQLITE_CHECKPOINT_PASSIVE, 0, 0);
  sq_step(s_begin, SQLITE_DONE);
  for (int i = 1; i <= ck_pages; i++) { sqlite3_bind_int(s_ckupd, 1, i); sqlite3_bind_blob(s_ckupd, 2, sq_blob + (xr() & 1023), 3000, SQLITE_STATIC); sq_step(s_ckupd, SQLITE_DONE); }
  sq_step(s_commit, SQLITE_DONE);
}
static void op_ckpt(void *c) { int lg = 0, ck = 0; sqlite3_wal_checkpoint_v2(sdb, 0, SQLITE_CHECKPOINT_PASSIVE, &lg, &ck); ck_log += lg; ck_frames += ck; ck_calls++; }
static void sq_warm(void) {   /* the db file into the page cache: the cold rows drop it, and a small box may have evicted it */
  static char b[65536]; off_t o = 0; ssize_t n;
  while ((n = pread(sq_rofd, b, sizeof b, o)) > 0) o += n;
}
static void bt_cold(void) {   /* SQLite's page cache and the OS page cache of the db and the WAL dropped */
  sqlite3_db_release_memory(sdb);
  posix_fadvise(sq_rofd, 0, 0, POSIX_FADV_DONTNEED);
}

/* ---- the sampler: k = 1, an untimed `between` before every sample, libc-call counts per sample ---- */
static void msamp(const char *name, const char *param, void (*fn)(void *), void *ctx, void (*between)(void), int nsamp, double max_s) {
  long tot[W_N] = {0}, c0[W_N], r0, w0, r1, w1, rs = 0, ws = 0;
  double ev[NEV]; for (int j = 0; j < NEV; j++) ev[j] = -1;
  if (between) between();
  fn(ctx); /* warm */
  int n = 0; int64_t start = now_ns();
  for (int i = 0; i < nsamp; i++) {
    if (between) between();
    pio(&r0, &w0); memcpy(c0, wc, sizeof c0); w_on = 1;
    int64_t t0 = now_ns(); fn(ctx); int64_t t1 = now_ns();
    w_on = 0; pio(&r1, &w1);
    for (int j = 0; j < W_N; j++) tot[j] += wc[j] - c0[j];
    rs += r1 - r0 - 1; ws += w1 - w0;
    sbuf[n++] = (double)(t1 - t0);
    if (n >= 10 && (now_ns() - start) > max_s * 1e9) break;
  }
  emit(name, param, 1, sbuf, n, ev);
  long all = 0; for (int j = 0; j < W_N; j++) all += tot[j];
  printf("#sys\t%s\t%s\tsamples=%d\tlibc_total=%.2f\tsyscr=%.2f\tsyscw=%.2f", name, param, n, (double)all / n, (double)rs / n, (double)ws / n);
  for (int j = 0; j < W_N; j++) if (tot[j]) printf("\t%s=%.2f", WN[j], (double)tot[j] / n);
  printf("\n"); fflush(stdout);
}

/* ---- direct syscalls on scratch files of the same filesystem (never the live database's files: a lock taken and
 * released by this process on SQLite's own file would drop SQLite's lock) ---- */
#define SQ_SHARED_FIRST (0x40000000 + 2)   /* SQLite's unix VFS: PENDING_BYTE + 2, the 510-byte shared range */
static int sfd = -1, swfd = -1, shfd = -1;
static unsigned char *shm;
static long sq_frame;
static void lk(int fd, int cmd, short type, off_t start, off_t len) { struct flock l = {0}; l.l_type = type; l.l_whence = SEEK_SET; l.l_start = start; l.l_len = len; fcntl(fd, cmd, &l); }
static void b_lk_shared(void *c, long k) { for (long i = 0; i < k; i++) { lk(sfd, F_SETLK, F_RDLCK, SQ_SHARED_FIRST, 510); lk(sfd, F_SETLK, F_UNLCK, SQ_SHARED_FIRST, 510); } }
static void b_lk_shmbyte(void *c, long k) { for (long i = 0; i < k; i++) { lk(shfd, F_SETLK, F_WRLCK, 120, 1); lk(shfd, F_SETLK, F_UNLCK, 120, 1); } }
static void b_lk_shmread(void *c, long k) { for (long i = 0; i < k; i++) { lk(shfd, F_SETLK, F_RDLCK, 124, 1); lk(shfd, F_SETLK, F_UNLCK, 124, 1); } }
static void b_getlk(void *c, long k) { for (long i = 0; i < k; i++) lk(sfd, F_GETLK, F_WRLCK, SQ_SHARED_FIRST, 510); }
static void b_lk_conflict(void *c, long k) { for (long i = 0; i < k; i++) lk(sfd, F_SETLK, F_WRLCK, SQ_SHARED_FIRST, 510); }
static void b_fstat(void *c, long k) { struct stat st; for (long i = 0; i < k; i++) fstat(sq_rofd, &st); }
static void b_wal_pwrite(void *c, long k) {
  lenc *v = c;
  for (long i = 0; i < k; i++) { off_t o = 32 + (sq_frame++ % 900) * 4120 + (v->len == 4096 ? 24 : 0); if (pwrite(swfd, sq_blob, v->len, o) < 0) abort(); }
}
static void b_pread_page(void *c, long k) { for (long i = 0; i < k; i++) if (pread(sfd, iobuf, 4096, (off_t)(xr() % 16384) * 4096) != 4096) abort(); }
static void op_pread_page(void *c) { b_pread_page(c, 1); }
static void bt_dropscr(void) { posix_fadvise(sfd, 0, 0, POSIX_FADV_DONTNEED); }
static void op_wal_frame_fdatasync(void *c) { b_wal_pwrite(&(lenc){4120}, 1); fdatasync(swfd); }
static void op_wal_frame_fsync(void *c) { b_wal_pwrite(&(lenc){4120}, 1); fsync(swfd); }
static void b_shm_hdr(void *c, long k) {   /* walIndexTryHdr: two copies of the 48-byte header, compared */
  unsigned char a[48], b[48]; int s = 0;
  for (long i = 0; i < k; i++) { memcpy(a, shm, 48); __atomic_thread_fence(__ATOMIC_SEQ_CST); memcpy(b, shm + 48, 48); s += memcmp(a, b, 48); CLOBBER(); }
  sink = s;
}
static void b_shm_mark(void *c, long k) { for (long i = 0; i < k; i++) { __atomic_store_n((uint32_t *)(shm + 100), (uint32_t)i, __ATOMIC_SEQ_CST); CLOBBER(); } }
static void b_shm_map(void *c, long k) {   /* a connection's first walIndexPage: map the 32 KB region, touch it, unmap */
  for (long i = 0; i < k; i++) {
    volatile unsigned char *m = mmap(0, 32768, PROT_READ | PROT_WRITE, MAP_SHARED, shfd, 0);
    if (m == MAP_FAILED) abort();
    for (int p = 0; p < 32768; p += 4096) (void)m[p];
    munmap((void *)m, 32768);
  }
}

/* ---- the other process: holds POSIX locks the way a reading connection does, or an open SQLite read transaction ---- */
static pid_t sq_child = -1;
static void child_start(const char *mode, const char *path) {
  int p[2]; if (pipe(p)) abort();
  pid_t pid = fork();
  if (pid == 0) {
    close(p[0]); char fds[16]; snprintf(fds, sizeof fds, "%d", p[1]);
    execl("/proc/self/exe", "calbench", mode, path, fds, (char *)0);
    _exit(127);
  }
  close(p[1]); char x;
  if (read(p[0], &x, 1) != 1) { printf("# child %s did not start\n", mode); fflush(stdout); exit(1); }
  close(p[0]);
  sq_child = pid;
}
static void child_stop(void) { if (sq_child > 0) { kill(sq_child, SIGKILL); waitpid(sq_child, 0, 0); sq_child = -1; } }
static int child_main(const char *mode, const char *path, int fd) {
  if (!strcmp(mode, "__hold")) {   /* the scratch "db" and "shm": SHARED range, DMS byte 128, read mark 123 */
    char s[300]; snprintf(s, sizeof s, "%s-shm", path);
    int a = open(path, O_RDWR), b = open(s, O_RDWR);
    if (a < 0 || b < 0) return 1;
    lk(a, F_SETLK, F_RDLCK, SQ_SHARED_FIRST, 510);
    lk(b, F_SETLK, F_RDLCK, 128, 1);
    lk(b, F_SETLK, F_RDLCK, 123, 1);
  } else {                          /* __sqlreader: a connection with a read transaction open on the WAL */
    snprintf(sq_path, sizeof sq_path, "%s", path);
    sqlite3 *d = sq_open(); sqlite3_stmt *s = 0;
    if (sqlite3_prepare_v2(d, "SELECT state FROM games LIMIT 1", -1, &s, 0) != SQLITE_OK || sqlite3_step(s) != SQLITE_ROW) return 1;
    /* the statement stays un-reset: the read transaction (and its WAL read mark) stays open */
  }
  if (write(fd, "r", 1) != 1) return 1;
  for (;;) pause();
}

/* ---- the transaction rows, also run under the strace pass by name (sqlitecount) ---- */
typedef struct { const char *name; void (*fn)(void *); txc c; } sqop;
static const sqop SQOPS[] = {
  {"ins1200", op_txn, {1, 1200, 0, 0}}, {"ins1500", op_txn, {1, 1500, 0, 0}},
  {"upd1200", op_upd, {1, 1200, 0, 0}}, {"upd1500", op_upd, {1, 1500, 0, 0}},
  {"txn10x1500", op_txn, {10, 1500, 0, 0}}, {"txn100x1500", op_txn, {100, 1500, 0, 0}}, {"txn2000x1500", op_txn, {2000, 1500, 0, 0}},
  {"selwarm", op_sel, {1, 0, 0, 50}}, {"selcache_miss", op_sel, {1, 0, 0, 20000}},
};

static void g_sqlite(void) {
  printf("# sqlite %s (%s)\n", sqlite3_libversion(), sqlite3_sourceid());
  sq_setup();
  {
    sqlite3_stmt *s = sq_prep("PRAGMA compile_options");
    printf("# sqlite compile_options:");
    while (sqlite3_step(s) == SQLITE_ROW) printf(" %s", (const char *)sqlite3_column_text(s, 0));
    printf("\n"); sqlite3_finalize(s);
    const char *pr[] = {"page_size", "cache_size", "journal_mode", "synchronous", "mmap_size", "wal_autocheckpoint", "locking_mode"};
    for (unsigned i = 0; i < sizeof pr / sizeof *pr; i++) {
      char q[64]; snprintf(q, sizeof q, "PRAGMA %s", pr[i]); s = sq_prep(q);
      if (sqlite3_step(s) == SQLITE_ROW) printf("# pragma %s = %s\n", pr[i], (const char *)sqlite3_column_text(s, 0));
      sqlite3_finalize(s);
    }
    struct stat st; fstat(sq_rofd, &st);
    printf("# db file %lld bytes after the fill (%ld game rows of 1500 B state, 1000 ck rows of 3000 B)\n", (long long)st.st_size, sq_fill);
    fflush(stdout);
  }

  /* A. the syscalls, on scratch files beside the database */
  char s[300];
  sfd = open(sq_scr, O_RDWR | O_CREAT | O_TRUNC, 0600);
  snprintf(s, sizeof s, "%s-wal", sq_scr); swfd = open(s, O_RDWR | O_CREAT | O_TRUNC, 0600);
  snprintf(s, sizeof s, "%s-shm", sq_scr); shfd = open(s, O_RDWR | O_CREAT | O_TRUNC, 0600);
  if (sfd < 0 || swfd < 0 || shfd < 0) { printf("# scratch open failed\n"); return; }
  for (off_t o = 0; o < (64 << 20); o += 65536) if (pwrite(sfd, sq_blob, 65536, o) != 65536) abort();
  for (off_t o = 0; o < 900 * 4120 + 32; o += 4120) if (pwrite(swfd, sq_blob, 4120, o) != 4120) abort();
  if (ftruncate(shfd, 32768)) abort();
  fsync(sfd); fsync(swfd); fsync(shfd);
  shm = mmap(0, 32768, PROT_READ | PROT_WRITE, MAP_SHARED, shfd, 0);
  if (shm == MAP_FAILED) abort();
  memset(shm, 0, 32768);
  for (int pass = 0; pass < 2; pass++) {
    const char *who = pass ? "other_process_holds" : "uncontended";
    if (pass) child_start("__hold", sq_scr);
    measure("fcntl_setlk_shared_rd+un", who, b_lk_shared, 0, 0, 30, 0);
    measure("fcntl_setlk_shm_wr+un", who, b_lk_shmbyte, 0, 0, 30, 0);
    measure("fcntl_setlk_shm_readmark_rd+un", who, b_lk_shmread, 0, 0, 30, 0);
    measure("fcntl_getlk", who, b_getlk, 0, 0, 30, 0);
    if (pass) measure("fcntl_setlk_wr_conflict_eagain", who, b_lk_conflict, 0, 0, 30, 0);
    if (pass) child_stop();
  }
  measure("syscall_getpid", "-", b_getpid, 0, 0, 20, 0);
  measure("fstat_db", "-", b_fstat, 0, 0, 20, 0);
  { size_t L[] = {24, 4096, 4120}; for (unsigned i = 0; i < 3; i++) { lenc v = {L[i]}; measure("pwrite_wal_overwrite", PRM("len%zu", L[i]), b_wal_pwrite, &v, 0, 30, 150); } }
  for (off_t o = 0; o < (64 << 20); o += 65536) if (pread(sfd, iobuf, 65536, o) != 65536) abort();
  measure("pread_page_pagecache", "len4096", b_pread_page, 0, 0, 30, 150);
  msamp("pread_page_cold", "len4096", op_pread_page, 0, bt_dropscr, 100, 20);
  msamp("wal_frame_pwrite+fdatasync", "len4120", op_wal_frame_fdatasync, 0, 0, 60, 20);
  msamp("wal_frame_pwrite+fsync", "len4120", op_wal_frame_fsync, 0, 0, 60, 20);
  measure("shm_hdr_read", "-", b_shm_hdr, 0, 0, 20, 0);
  measure("shm_word_store_seqcst", "-", b_shm_mark, 0, 0, 20, 0);
  measure("shm_mmap_touch_munmap", "32KB", b_shm_map, 0, 0, 30, 100);
  munmap(shm, 32768);

  /* B. transactions on the live database, uncontended */
  sq_warm();
  for (unsigned i = 0; i < sizeof SQOPS / sizeof *SQOPS; i++) {
    txc v = SQOPS[i].c;
    char pm[48]; snprintf(pm, sizeof pm, "rows%d_blob%d", v.n, v.blob);
    const char *nm = SQOPS[i].fn == op_sel ? (v.range > 1000 ? "sqlite_select_pk_cachemiss" : "sqlite_select_pk_warm")
                   : SQOPS[i].fn == op_upd ? "sqlite_update_txn" : "sqlite_insert_txn";
    msamp(nm, pm, SQOPS[i].fn, &v, bt_ckpt, v.n >= 2000 ? 20 : v.n >= 100 ? 100 : 300, 8);
  }
  { int N[] = {1, 10, 100, 1000}, B[] = {128, 600};
    for (int b = 0; b < 2; b++) for (int i = 0; i < 4; i++) {   /* the grid for the formula (1500 B is above) */
      txc v = {N[i], B[b], 0, 0}; char pm[48]; snprintf(pm, sizeof pm, "rows%d_blob%d", N[i], B[b]);
      msamp("sqlite_insert_txn", pm, op_txn, &v, bt_ckpt, N[i] >= 1000 ? 20 : N[i] >= 100 ? 100 : 300, 8);
    } }
  { txc v = {1, 0, 0, sq_fill}; msamp("sqlite_select_pk_cold", "rows1", op_sel, &v, bt_cold, 100, 20); }
  { int N[] = {1, 10, 100, 1000};
    for (int i = 0; i < 4; i++) {
      ck_pages = N[i]; ck_frames = ck_log = ck_calls = 0;
      char pm[32]; snprintf(pm, sizeof pm, "pages%d", N[i]);
      msamp("sqlite_checkpoint_passive", pm, op_ckpt, 0, bt_ckprep, N[i] >= 1000 ? 10 : 30, 20);
      printf("#ckpt\t%s\twal_frames=%.1f\tbackfilled=%.1f\n", pm, (double)ck_log / ck_calls, (double)ck_frames / ck_calls);
    } }
  printf("# untimed checkpoints between samples so far: %ld\n", sq_ckpts);

  /* C. the same with another process: POSIX locks held on the scratch files (above), and a reader on the live WAL */
  sqlite3_wal_checkpoint_v2(sdb, 0, SQLITE_CHECKPOINT_TRUNCATE, 0, 0);
  sq_warm();   /* select_pk_cold and the checkpoints above left it cold: without this, C measured disk reads */
  child_start("__sqlreader", sq_path);
  { const char *ops[] = {"ins1500", "upd1500", "txn100x1500", "selwarm"};
    for (unsigned j = 0; j < 4; j++) for (unsigned i = 0; i < sizeof SQOPS / sizeof *SQOPS; i++) if (!strcmp(SQOPS[i].name, ops[j])) {
      txc v = SQOPS[i].c; char pm[64]; snprintf(pm, sizeof pm, "rows%d_blob%d_reader", v.n, v.blob);
      const char *nm = SQOPS[i].fn == op_sel ? "sqlite_select_pk_warm" : SQOPS[i].fn == op_upd ? "sqlite_update_txn" : "sqlite_insert_txn";
      msamp(nm, pm, SQOPS[i].fn, &v, 0, v.n >= 100 ? 50 : 200, 8);
    } }
  child_stop();

  sqlite3_finalize(s_begin); sqlite3_finalize(s_commit); sqlite3_finalize(s_ins); sqlite3_finalize(s_upd); sqlite3_finalize(s_sel); sqlite3_finalize(s_ckupd);
  sqlite3_close(sdb);
  close(sq_rofd); close(sfd); close(swfd); close(shfd);
  char w[300];
  unlink(sq_path); snprintf(w, sizeof w, "%s-wal", sq_path); unlink(w); snprintf(w, sizeof w, "%s-shm", sq_path); unlink(w);
  unlink(sq_scr); snprintf(w, sizeof w, "%s-wal", sq_scr); unlink(w); snprintf(w, sizeof w, "%s-shm", sq_scr); unlink(w);
}

/* calbench sqlitecount OP N : the setup of the sqlite group, then OP N times (the runner runs it under strace -f -c
 * with N = 0 and N = 500 and takes the difference over 500: every syscall per op, musl's internal ones included) */
static int sqlitecount_main(const char *op, int n) {
  sq_setup();
  for (unsigned i = 0; i < sizeof SQOPS / sizeof *SQOPS; i++) if (!strcmp(SQOPS[i].name, op)) {
    txc v = SQOPS[i].c;
    for (int j = 0; j < n; j++) { bt_ckpt(); SQOPS[i].fn(&v); }
    sqlite3_close(sdb);
    return 0;
  }
  fprintf(stderr, "unknown op %s\n", op);
  return 2;
}
