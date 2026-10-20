/* calbench.c : CPU calibration suite for Graviton2 (c6g / t4g.nano) and the Docker arm64 rig.
 *
 * One static arm64 binary, built with cbase serve's flags (-std=gnu11 -D_GNU_SOURCE -O2 -g -pthread -static, musl).
 * Usage: calbench GROUP PASS   (GROUP: env clock core kernel net wake real str malloc mem stream ebs steal)
 * Output: TSV on stdout, one row per measurement:
 *   group name param k n min max mean std p50 p99 cyc insn brmiss l1dmiss llcmiss cpu pass
 * ns columns are ns per op; each of the n samples times k back-to-back ops and divides by k
 * (so p99 is the p99 of k-op averages; k=1 rows are true per-op distributions).
 * cyc..llcmiss are per op, means over all samples, user+kernel, -1 when the PMU is unavailable.
 */
#include <errno.h>
#include <fcntl.h>
#include <linux/futex.h>
#include <linux/io_uring.h>
#include <linux/perf_event.h>
#include <math.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <sqlite3.h>

#define NS(ts) ((int64_t)(ts).tv_sec * 1000000000LL + (ts).tv_nsec)
static inline int64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return NS(t); }
static inline uint64_t cntvct(void) { uint64_t v; __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(v)); return v; }
#define OPAQUE(x) __asm__ volatile("" : "+r"(x))
#define CLOBBER() __asm__ volatile("" ::: "memory")

static int g_pass = 0;
static const char *g_group = "";
static int g_cpu = 1;            /* the measuring thread's pinned cpu */
static double g_budget_ms = 40;  /* default per-measurement wall budget */
static int g_samples = 300;

/* ---------------- PMU ---------------- */
#define NEV 5
static int pfd[NEV] = {-1, -1, -1, -1, -1};
static int pmu_ok = 0;
static long peo(struct perf_event_attr *a, int group) { return syscall(SYS_perf_event_open, a, 0, -1, group, 0); }
/* cycles + instructions are ONE group (the leader reads both atomically, so insn/cyc is exact); brmiss, l1dmiss and
 * llcmiss are single events. Every read carries time_enabled/time_running and the value is scaled by enabled/running,
 * so a counter multiplexed off the PMU for part of the run is not under-counted (the first run opened five
 * un-grouped events with no scaling and the c6g columns were wrong). */
static void pmu_open(void) {
  for (int i = 0; i < NEV; i++) { if (pfd[i] >= 0) close(pfd[i]); pfd[i] = -1; }
  uint32_t types[NEV] = {PERF_TYPE_HARDWARE, PERF_TYPE_HARDWARE, PERF_TYPE_HARDWARE, PERF_TYPE_HW_CACHE, PERF_TYPE_HARDWARE};
  uint64_t cfg[NEV] = {PERF_COUNT_HW_CPU_CYCLES, PERF_COUNT_HW_INSTRUCTIONS, PERF_COUNT_HW_BRANCH_MISSES,
                       PERF_COUNT_HW_CACHE_L1D | (PERF_COUNT_HW_CACHE_OP_READ << 8) | (PERF_COUNT_HW_CACHE_RESULT_MISS << 16),
                       PERF_COUNT_HW_CACHE_MISSES};
  pmu_ok = 0;
  for (int i = 0; i < NEV; i++) {
    struct perf_event_attr a; memset(&a, 0, sizeof a);
    a.size = sizeof a; a.type = types[i]; a.config = cfg[i];
    a.disabled = 0; a.exclude_hv = 1; a.inherit = 0;
    a.read_format = PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING | (i == 0 ? PERF_FORMAT_GROUP : 0);
    pfd[i] = (int)peo(&a, i == 1 ? pfd[0] : -1);
    if (i == 0 && pfd[i] >= 0) pmu_ok = 1;
  }
}
static double pmu_scale(uint64_t v, uint64_t en, uint64_t run) { return run ? (double)v * ((double)en / (double)run) : -1; }
static void pmu_read(int64_t v[NEV]) {
  for (int i = 0; i < NEV; i++) v[i] = -1;
  if (pfd[0] >= 0) { /* group layout: nr, time_enabled, time_running, then one value per member in open order */
    uint64_t g[5];
    ssize_t want = (ssize_t)((3 + (pfd[1] >= 0 ? 2 : 1)) * 8);
    if (read(pfd[0], g, sizeof g) >= want) {
      v[0] = (int64_t)pmu_scale(g[3], g[1], g[2]);
      if (pfd[1] >= 0) v[1] = (int64_t)pmu_scale(g[4], g[1], g[2]);
    }
  }
  for (int i = 2; i < NEV; i++) if (pfd[i] >= 0) { /* single event: value, time_enabled, time_running */
    uint64_t x[3];
    if (read(pfd[i], x, sizeof x) == (ssize_t)sizeof x) v[i] = (int64_t)pmu_scale(x[0], x[1], x[2]);
  }
}

/* ---------------- stats + driver ---------------- */
typedef void (*bfn)(void *ctx, long k);
static int dcmp(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }
static void emit(const char *name, const char *param, long k, double *s, int n, const double ev[NEV]) {
  if (n <= 0) { printf("%s\t%s\t%s\t%ld\t0\t-1\t-1\t-1\t-1\t-1\t-1\t-1\t-1\t-1\t-1\t-1\t%d\t%d\n", g_group, name, param, k, g_cpu, g_pass); fflush(stdout); return; }
  double sum = 0, sq = 0;
  for (int i = 0; i < n; i++) sum += s[i];
  double mean = sum / n;
  for (int i = 0; i < n; i++) sq += (s[i] - mean) * (s[i] - mean);
  qsort(s, n, sizeof *s, dcmp);
  double std = n > 1 ? sqrt(sq / (n - 1)) : 0;
  int i50 = (int)(0.5 * (n - 1)), i99 = (int)(0.99 * (n - 1) + 0.5);
  printf("%s\t%s\t%s\t%ld\t%d\t%.2f\t%.2f\t%.2f\t%.2f\t%.2f\t%.2f\t%.3f\t%.3f\t%.4f\t%.4f\t%.4f\t%d\t%d\n", g_group, name, param, k, n,
         s[0], s[n - 1], mean, std, s[i50], s[i99], ev[0], ev[1], ev[2], ev[3], ev[4], g_cpu, g_pass);
  fflush(stdout);
}
/* run fn(ctx,k) n times; k is calibrated so one sample is ~target_us unless k_fixed>0 */
static double *sbuf;
static void measure(const char *name, const char *param, bfn fn, void *ctx, long k_fixed, double target_us, int nsamp) {
  long k = k_fixed;
  if (k <= 0) {
    k = 1;
    for (;;) {
      int64_t t0 = now_ns(); fn(ctx, k); int64_t dt = now_ns() - t0;
      if (dt >= target_us * 1000 * 0.5 || k >= (1L << 30)) { if (dt > 0) k = (long)(k * (target_us * 1000.0 / dt)); if (k < 1) k = 1; break; }
      k *= 4;
    }
  }
  if (nsamp <= 0) nsamp = g_samples;
  /* cap total time by budget */
  double est_ms = nsamp * (double)k_fixed; (void)est_ms;
  int64_t start = now_ns();
  int n = 0;
  int64_t e0[NEV], e1[NEV]; double esum[NEV] = {0}; long eops = 0;
  fn(ctx, k); /* warm */
  for (int i = 0; i < nsamp; i++) {
    pmu_read(e0);
    int64_t t0 = now_ns();
    fn(ctx, k);
    int64_t t1 = now_ns();
    pmu_read(e1);
    sbuf[n++] = (double)(t1 - t0) / k;
    for (int j = 0; j < NEV; j++) if (e0[j] >= 0 && e1[j] >= 0) esum[j] += (double)(e1[j] - e0[j]);
    eops += k;
    if (n >= 20 && (now_ns() - start) > g_budget_ms * 1e6 * (target_us > 500 ? 20 : 1)) break;
  }
  double ev[NEV];
  for (int j = 0; j < NEV; j++) ev[j] = (pfd[j] >= 0) ? esum[j] / eops : -1;
  emit(name, param, k, sbuf, n, ev);
}
static void pin(int cpu) { cpu_set_t s; CPU_ZERO(&s); CPU_SET(cpu, &s); sched_setaffinity(0, sizeof s, &s); }
/* musl counts the affinity mask, so read it once before pinning */
static int g_ncpu = 0;
static int ncpu(void) { if (!g_ncpu) g_ncpu = (int)sysconf(_SC_NPROCESSORS_ONLN); return g_ncpu; }
static uint64_t rng = 88172645463325252ULL;
static inline uint64_t xr(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }
static char P[64];
#define PRM(...) (snprintf(P, sizeof P, __VA_ARGS__), P)

/* ================= CORE ================= */
static void b_add_dep(void *c, long k) { uint64_t x = 1, y = 3; OPAQUE(y);
  for (long i = 0; i < k; i++) { __asm__ volatile("add %0,%0,%1\n add %0,%0,%1\n add %0,%0,%1\n add %0,%0,%1\n add %0,%0,%1\n add %0,%0,%1\n add %0,%0,%1\n add %0,%0,%1" : "+r"(x) : "r"(y)); }
  OPAQUE(x); }
static void b_add_ind(void *c, long k) { uint64_t a = 1, b = 2, d = 3, e = 4, y = 3; OPAQUE(y);
  for (long i = 0; i < k; i++) { __asm__ volatile("add %0,%0,%4\n add %1,%1,%4\n add %2,%2,%4\n add %3,%3,%4\n add %0,%0,%4\n add %1,%1,%4\n add %2,%2,%4\n add %3,%3,%4" : "+r"(a), "+r"(b), "+r"(d), "+r"(e) : "r"(y)); }
  OPAQUE(a); OPAQUE(b); OPAQUE(d); OPAQUE(e); }
static void b_mul_dep(void *c, long k) { uint64_t x = 1, y = 3; OPAQUE(y);
  for (long i = 0; i < k; i++) { __asm__ volatile("mul %0,%0,%1\n mul %0,%0,%1\n mul %0,%0,%1\n mul %0,%0,%1\n mul %0,%0,%1\n mul %0,%0,%1\n mul %0,%0,%1\n mul %0,%0,%1" : "+r"(x) : "r"(y)); }
  OPAQUE(x); }
static void b_mul_ind(void *c, long k) { uint64_t a = 1, b = 2, d = 3, e = 4, y = 3; OPAQUE(y);
  for (long i = 0; i < k; i++) { __asm__ volatile("mul %0,%0,%4\n mul %1,%1,%4\n mul %2,%2,%4\n mul %3,%3,%4\n mul %0,%0,%4\n mul %1,%1,%4\n mul %2,%2,%4\n mul %3,%3,%4" : "+r"(a), "+r"(b), "+r"(d), "+r"(e) : "r"(y)); }
  OPAQUE(a); OPAQUE(b); OPAQUE(d); OPAQUE(e); }
/* division: the dividend is re-formed each step as (q & 0) | n, a 2-op dependent link (add_dep gives its cost) */
typedef struct { uint64_t n, d; } divc;
static void b_udiv64_dep(void *c, long k) { divc *v = c; uint64_t n = v->n, d = v->d;
  for (long i = 0; i < k; i++) { uint64_t q; __asm__ volatile("udiv %0,%1,%2\n and %0,%0,xzr\n orr %1,%0,%3" : "=&r"(q), "+r"(n) : "r"(d), "r"(v->n)); }
  OPAQUE(n); }
static void b_udiv32_dep(void *c, long k) { divc *v = c; uint32_t n = (uint32_t)v->n, d = (uint32_t)v->d, b = (uint32_t)v->n;
  for (long i = 0; i < k; i++) { uint32_t q; __asm__ volatile("udiv %w0,%w1,%w2\n and %w0,%w0,wzr\n orr %w1,%w0,%w3" : "=&r"(q), "+r"(n) : "r"(d), "r"(b)); }
  OPAQUE(n); }
static void b_udiv64_ind(void *c, long k) { divc *v = c; uint64_t n = v->n, d = v->d, q0, q1, q2, q3;
  for (long i = 0; i < k; i++) { __asm__ volatile("udiv %0,%4,%5\n udiv %1,%4,%5\n udiv %2,%4,%5\n udiv %3,%4,%5" : "=&r"(q0), "=&r"(q1), "=&r"(q2), "=&r"(q3) : "r"(n), "r"(d)); }
  OPAQUE(q0); }
static void b_fdiv_dep(void *c, long k) { double x = 1e300, y = 1.0000001; OPAQUE(y);
  for (long i = 0; i < k; i++) { __asm__ volatile("fdiv %d0,%d0,%d1" : "+w"(x) : "w"(y)); }
  OPAQUE(x); }
/* branches: data-dependent cbz on a byte array, either all-zero (predictable) or random */
typedef struct { uint8_t *a; long n; } brc;
static void b_branch(void *c, long k) { brc *v = c; uint64_t s = 0; long n = v->n; const uint8_t *a = v->a;
  for (long i = 0; i < k; i++) { uint32_t b = a[i & (n - 1)]; __asm__ volatile("cbz %w1, 1f\n add %0,%0,#1\n1:" : "+r"(s) : "r"(b)); }
  OPAQUE(s); }
static void b_empty(void *c, long k) { for (long i = 0; i < k; i++) CLOBBER(); }
/* atomics / mutex */
static _Atomic uint64_t at;
static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static void b_atomic_add(void *c, long k) { for (long i = 0; i < k; i++) atomic_fetch_add_explicit(&at, 1, memory_order_seq_cst); }
static void b_atomic_add_relaxed(void *c, long k) { for (long i = 0; i < k; i++) atomic_fetch_add_explicit(&at, 1, memory_order_relaxed); }
static void b_cas(void *c, long k) { for (long i = 0; i < k; i++) { uint64_t e = atomic_load_explicit(&at, memory_order_relaxed); atomic_compare_exchange_strong(&at, &e, e + 1); } }
static void b_mutex(void *c, long k) { for (long i = 0; i < k; i++) { pthread_mutex_lock(&mtx); CLOBBER(); pthread_mutex_unlock(&mtx); } }
static atomic_int bg_stop;
static void *bg_atomic(void *a) { pin((int)(intptr_t)a); while (!atomic_load(&bg_stop)) atomic_fetch_add_explicit(&at, 1, memory_order_seq_cst); return 0; }
static void *bg_mutex(void *a) { pin((int)(intptr_t)a); while (!atomic_load(&bg_stop)) { pthread_mutex_lock(&mtx); CLOBBER(); pthread_mutex_unlock(&mtx); } return 0; }

static void g_core(void) {
  measure("empty_loop", "-", b_empty, 0, 0, 20, 0);
  measure("add_dep", "x8", b_add_dep, 0, 0, 20, 0);
  measure("add_ind4", "x8", b_add_ind, 0, 0, 20, 0);
  measure("mul_dep", "x8", b_mul_dep, 0, 0, 20, 0);
  measure("mul_ind4", "x8", b_mul_ind, 0, 0, 20, 0);
  measure("fdiv_dep", "-", b_fdiv_dep, 0, 0, 20, 0);
  /* divide curve: dividend significant bits x divisor bits; quotient bits drive N1's early-out */
  int nb[] = {1, 8, 16, 24, 32, 40, 48, 56, 64};
  int db[] = {2, 8, 16, 32};
  for (unsigned i = 0; i < sizeof nb / sizeof *nb; i++) for (unsigned j = 0; j < sizeof db / sizeof *db; j++) {
    divc v; v.n = nb[i] == 64 ? ~0ULL : ((1ULL << nb[i]) - 1); v.d = (1ULL << (db[j] - 1)) | 1;
    measure("udiv64_dep", PRM("n%d_d%d", nb[i], db[j]), b_udiv64_dep, &v, 0, 20, 0);
    if (nb[i] <= 32) measure("udiv32_dep", PRM("n%d_d%d", nb[i], db[j]), b_udiv32_dep, &v, 0, 20, 0);
    if (db[j] == 2 || db[j] == 16) measure("udiv64_ind4", PRM("n%d_d%d", nb[i], db[j]), b_udiv64_ind, &v, 0, 20, 0);
  }
  long n = 1 << 16; uint8_t *a = malloc(n); brc bc = {a, n};
  memset(a, 0, n); measure("branch", "predictable", b_branch, &bc, 0, 50, 0);
  for (long i = 0; i < n; i++) a[i] = (uint8_t)(i & 1); measure("branch", "alternating", b_branch, &bc, 0, 50, 0);
  for (long i = 0; i < n; i++) a[i] = (uint8_t)((i % 7) == 0); measure("branch", "period7", b_branch, &bc, 0, 50, 0);
  for (long i = 0; i < n; i++) a[i] = (uint8_t)(xr() & 1); measure("branch", "random50", b_branch, &bc, 0, 50, 0);
  for (long i = 0; i < n; i++) a[i] = (uint8_t)((xr() & 7) == 0); measure("branch", "random12", b_branch, &bc, 0, 50, 0);
  free(a);
  measure("atomic_add_seqcst", "uncontended", b_atomic_add, 0, 0, 20, 0);
  measure("atomic_add_relaxed", "uncontended", b_atomic_add_relaxed, 0, 0, 20, 0);
  measure("atomic_cas", "uncontended", b_cas, 0, 0, 20, 0);
  measure("mutex_lock_unlock", "uncontended", b_mutex, 0, 0, 20, 0);
  if (ncpu() >= 2) {
    pthread_t t; int other = g_cpu == 0 ? 1 : 0;
    atomic_store(&bg_stop, 0); pthread_create(&t, 0, bg_atomic, (void *)(intptr_t)other);
    usleep(1000); measure("atomic_add_seqcst", "contended2", b_atomic_add, 0, 0, 20, 0);
    atomic_store(&bg_stop, 1); pthread_join(t, 0);
    atomic_store(&bg_stop, 0); pthread_create(&t, 0, bg_mutex, (void *)(intptr_t)other);
    usleep(1000); measure("mutex_lock_unlock", "contended2", b_mutex, 0, 0, 20, 0);
    atomic_store(&bg_stop, 1); pthread_join(t, 0);
  }
}

/* ================= MEM: load latency by working set ================= */
typedef struct { void **p; } chc;
static void b_chase(void *c, long k) { chc *v = c; void **p = v->p;
  for (long i = 0; i < k; i++) { p = (void **)*p; p = (void **)*p; p = (void **)*p; p = (void **)*p; p = (void **)*p; p = (void **)*p; p = (void **)*p; p = (void **)*p; }
  v->p = p; }
static long mem_avail_kb(void) { FILE *f = fopen("/proc/meminfo", "r"); char l[256]; long v = -1; if (!f) return -1;
  while (fgets(l, sizeof l, f)) if (sscanf(l, "MemAvailable: %ld kB", &v) == 1) break; fclose(f); return v; }
static void chase_one(size_t bytes, const char *mode) {
  size_t stride = 64, nl = bytes / stride;
  char *buf = mmap(0, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (buf == MAP_FAILED) return;
  if (!strcmp(mode, "nothp")) madvise(buf, bytes, MADV_NOHUGEPAGE);
  memset(buf, 1, bytes);
  size_t *perm = malloc(nl * sizeof *perm);
  for (size_t i = 0; i < nl; i++) perm[i] = i;
  for (size_t i = nl - 1; i > 0; i--) { size_t j = xr() % (i + 1); size_t t = perm[i]; perm[i] = perm[j]; perm[j] = t; }
  for (size_t i = 0; i < nl; i++) *(void **)(buf + perm[i] * stride) = buf + perm[(i + 1) % nl] * stride;
  free(perm);
  chc v = {(void **)buf};
  /* each sample walks >= the whole set once when it is big; ~100us for small sets */
  long k = (long)(nl / 8); if (k < 2000) k = 2000;
  int ns = bytes >= (64u << 20) ? 12 : bytes >= (8u << 20) ? 30 : 100;
  measure("load_latency", PRM("%zuKB_%s", bytes >> 10, mode), b_chase, &v, k, 0, ns);
  munmap(buf, bytes);
}
static void g_mem(void) {
  long avail = mem_avail_kb();
  size_t sizes[] = {4, 8, 16, 32, 48, 64, 96, 128, 192, 256, 384, 512, 768, 1024, 1536, 2048, 3072, 4096, 6144, 8192, 16384, 32768, 65536, 131072, 262144};
  for (unsigned i = 0; i < sizeof sizes / sizeof *sizes; i++) {
    size_t b = sizes[i] << 10;
    if (avail > 0 && (long)(sizes[i] * 2) > avail) { printf("# skip %zuKB: MemAvailable %ld kB\n", sizes[i], avail); continue; }
    chase_one(b, "default");
    if (sizes[i] >= 1024 && sizes[i] <= 65536 && (sizes[i] & (sizes[i] - 1)) == 0) chase_one(b, "nothp");
  }
}

/* ================= STR: memcpy/memset/memcmp/strcmp/strlen curves ================= */
static char *sA, *sB;
typedef struct { size_t len; size_t oa, ob; } strc;
static void b_memcpy(void *c, long k) { strc *v = c; for (long i = 0; i < k; i++) { memcpy(sA + v->oa, sB + v->ob, v->len); CLOBBER(); } }
static void b_memset(void *c, long k) { strc *v = c; for (long i = 0; i < k; i++) { memset(sA + v->oa, (int)i, v->len); CLOBBER(); } }
static void b_memmove(void *c, long k) { strc *v = c; for (long i = 0; i < k; i++) { memmove(sA + v->oa + 8, sA + v->oa, v->len); CLOBBER(); } }
static volatile int sink;
static void b_memcmp(void *c, long k) { strc *v = c; int s = 0; for (long i = 0; i < k; i++) { s += memcmp(sA + v->oa, sB + v->ob, v->len); CLOBBER(); } sink = s; }
static void b_strcmp(void *c, long k) { strc *v = c; int s = 0; for (long i = 0; i < k; i++) { s += strcmp(sA + v->oa, sB + v->ob); CLOBBER(); } sink = s; }
static void b_strlen(void *c, long k) { strc *v = c; size_t s = 0; for (long i = 0; i < k; i++) { s += strlen(sA + v->oa); CLOBBER(); } sink = (int)s; }
static void str_setup(size_t len, size_t oa, size_t ob, size_t diffat) {
  memset(sA, 'a', 70000); memset(sB, 'a', 70000);
  sA[oa + len] = 0; sB[ob + len] = 0;
  if (diffat < len) sB[ob + diffat] = 'b';
}
static void g_str(void) {
  sA = aligned_alloc(4096, 1 << 17); sB = aligned_alloc(4096, 1 << 17);
  memset(sA, 'a', 1 << 17); memset(sB, 'a', 1 << 17);
  size_t L[] = {0, 1, 2, 3, 4, 7, 8, 12, 16, 24, 32, 48, 64, 80, 96, 128, 160, 192, 256, 320, 384, 512, 768, 1024, 1200, 1500, 2048, 3072, 4096, 8192, 16384, 32768, 65536};
  size_t al[][2] = {{0, 0}, {1, 0}, {0, 3}, {13, 7}};
  for (unsigned i = 0; i < sizeof L / sizeof *L; i++) for (unsigned a = 0; a < 4; a++) {
    strc v = {L[i], al[a][0], al[a][1]};
    char pr[64]; snprintf(pr, sizeof pr, "len%zu_a%zu_%zu", L[i], al[a][0], al[a][1]);
    memset(sA, 'a', 70000); memset(sB, 'a', 70000);
    measure("memcpy", pr, b_memcpy, &v, 0, 20, 150);
    measure("memset", pr, b_memset, &v, 0, 20, 150);
    if (a < 2) measure("memmove_overlap", pr, b_memmove, &v, 0, 20, 100);
    memset(sA, 'a', 70000); memset(sB, 'a', 70000);
    measure("memcmp_equal", pr, b_memcmp, &v, 0, 20, 150);
    str_setup(L[i], v.oa, v.ob, (size_t)-1);
    measure("strcmp_equal", pr, b_strcmp, &v, 0, 20, 150);
    measure("strlen", pr, b_strlen, &v, 0, 20, 150);
  }
  /* strcmp / memcmp by where the first difference falls, 4 KB strings */
  size_t D[] = {0, 1, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048};
  for (unsigned i = 0; i < sizeof D / sizeof *D; i++) {
    strc v = {4096, 0, 0};
    str_setup(4096, 0, 0, D[i]);
    measure("strcmp_diffat", PRM("diff%zu", D[i]), b_strcmp, &v, 0, 20, 150);
    measure("memcmp_diffat", PRM("diff%zu", D[i]), b_memcmp, &v, 0, 20, 150);
  }
}

/* ================= MALLOC ================= */
typedef struct { size_t sz; int batch; void **slots; } mc;
static void b_malloc_free(void *c, long k) { mc *v = c; for (long i = 0; i < k; i++) { void *p = malloc(v->sz); OPAQUE(p); *(volatile char *)p = 1; free(p); } }
static void b_malloc_batch(void *c, long k) { mc *v = c;
  for (long i = 0; i < k; i++) { for (int j = 0; j < v->batch; j++) { v->slots[j] = malloc(v->sz); *(volatile char *)v->slots[j] = 1; }
    for (int j = 0; j < v->batch; j++) free(v->slots[(j * 7) % v->batch]); } }
static size_t bg_sz;
static void *bg_malloc(void *a) { pin((int)(intptr_t)a); while (!atomic_load(&bg_stop)) { void *p = malloc(bg_sz); *(volatile char *)p = 1; free(p); } return 0; }
static void g_malloc(void) {
  size_t S[] = {8, 16, 24, 32, 48, 64, 96, 128, 192, 256, 384, 512, 768, 1024, 1200, 1536, 2048, 4096, 8192, 16384, 32768, 65536, 131072, 262144, 1048576};
  void *slots[64];
  for (unsigned i = 0; i < sizeof S / sizeof *S; i++) {
    mc v = {S[i], 64, slots};
    measure("malloc_free", PRM("sz%zu", S[i]), b_malloc_free, &v, 0, 20, 150);
    measure("malloc_batch64_per_pair", PRM("sz%zu", S[i]), b_malloc_batch, &v, 0, 50, 100);
    if (ncpu() >= 2) {
      pthread_t t; bg_sz = S[i]; atomic_store(&bg_stop, 0);
      pthread_create(&t, 0, bg_malloc, (void *)(intptr_t)(g_cpu == 0 ? 1 : 0));
      usleep(500);
      measure("malloc_free_2thr", PRM("sz%zu", S[i]), b_malloc_free, &v, 0, 20, 150);
      atomic_store(&bg_stop, 1); pthread_join(t, 0);
    }
  }
}

/* ================= KERNEL paths ================= */
static void b_getpid(void *c, long k) { for (long i = 0; i < k; i++) syscall(SYS_getpid); }
static void b_getppid(void *c, long k) { for (long i = 0; i < k; i++) getppid(); }
static void b_badsys(void *c, long k) { for (long i = 0; i < k; i++) syscall(4000); }
static int ppfd[2];
typedef struct { size_t len; } lenc;
static char iobuf[1 << 17];
static void b_pipe_rw(void *c, long k) { lenc *v = c; for (long i = 0; i < k; i++) { if (write(ppfd[1], iobuf, v->len) < 0) abort(); if (read(ppfd[0], iobuf, v->len) < 0) abort(); } }
static int efd;
static void b_eventfd(void *c, long k) { uint64_t x = 1; for (long i = 0; i < k; i++) { if (write(efd, &x, 8) != 8) abort(); if (read(efd, &x, 8) != 8) abort(); } }
static int epfd, epready, epidle;
static void b_epoll_ready(void *c, long k) { struct epoll_event e[4]; for (long i = 0; i < k; i++) if (epoll_wait(epfd, e, 4, 0) != 1) abort(); }
static int epfd0;
static void b_epoll_zero(void *c, long k) { struct epoll_event e[4]; for (long i = 0; i < k; i++) epoll_wait(epfd0, e, 4, 0); }
static void b_clock(void *c, long k) { clockid_t id = *(clockid_t *)c; struct timespec t; for (long i = 0; i < k; i++) clock_gettime(id, &t); }
static void b_cntvct(void *c, long k) { uint64_t s = 0; for (long i = 0; i < k; i++) s += cntvct(); OPAQUE(s); }
static void b_sched_yield(void *c, long k) { for (long i = 0; i < k; i++) sched_yield(); }
static void b_futex_wake_nowaiter(void *c, long k) { static int w; for (long i = 0; i < k; i++) syscall(SYS_futex, &w, FUTEX_WAKE_PRIVATE, 1, 0, 0, 0); }
/* page faults */
typedef struct { size_t pages; int mode; } pfc;
static void b_pagefault(void *c, long k) { pfc *v = c; size_t ps = 4096;
  for (long i = 0; i < k; i++) {
    char *m = mmap(0, v->pages * ps, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED) abort();
    if (v->mode == 1) madvise(m, v->pages * ps, MADV_NOHUGEPAGE);
    for (size_t p = 0; p < v->pages; p++) m[p * ps] = 1;
    munmap(m, v->pages * ps);
  } }
static void b_mmap_munmap(void *c, long k) { pfc *v = c; for (long i = 0; i < k; i++) { char *m = mmap(0, v->pages * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); munmap(m, v->pages * 4096); } }
static char *touchbuf;
static void b_retouch(void *c, long k) { pfc *v = c; for (long i = 0; i < k; i++) for (size_t p = 0; p < v->pages; p++) ((volatile char *)touchbuf)[p * 4096] = 1; }
static void b_dontneed_touch(void *c, long k) { pfc *v = c; for (long i = 0; i < k; i++) { madvise(touchbuf, v->pages * 4096, MADV_DONTNEED); for (size_t p = 0; p < v->pages; p++) touchbuf[p * 4096] = 1; } }
/* file I/O */
static int ffd; static off_t foff;
static void b_pwrite(void *c, long k) { lenc *v = c; for (long i = 0; i < k; i++) { if (pwrite(ffd, iobuf, v->len, foff) < 0) abort(); foff = (foff + 4096) % (16 << 20); } }
static void b_pwrite_fdatasync(void *c, long k) { lenc *v = c; for (long i = 0; i < k; i++) { if (pwrite(ffd, iobuf, v->len, foff) < 0) abort(); foff = (foff + 4096) % (1 << 20); fdatasync(ffd); } }
static void b_pwrite_append_fdatasync(void *c, long k) { lenc *v = c; for (long i = 0; i < k; i++) { if (pwrite(ffd, iobuf, v->len, foff) < 0) abort(); foff += v->len; fdatasync(ffd); } }
static char *mfile;
static void b_msync(void *c, long k) { lenc *v = c; for (long i = 0; i < k; i++) { memset(mfile + (i % 64) * 4096, (int)i, v->len); msync(mfile + (i % 64) * 4096, 4096, MS_SYNC); } }
static void b_pread(void *c, long k) { lenc *v = c; for (long i = 0; i < k; i++) { if (pread(ffd, iobuf, v->len, (i * 4096) % (16 << 20)) < 0) abort(); } }

/* io_uring raw */
struct uring { int fd; unsigned *sq_tail, *sq_mask, *sq_array, *cq_head, *cq_tail, *cq_mask; struct io_uring_sqe *sqes; struct io_uring_cqe *cqes; };
static struct uring ur; static int ur_ok;
static int uring_init(unsigned entries) {
  struct io_uring_params p; memset(&p, 0, sizeof p);
  int fd = (int)syscall(__NR_io_uring_setup, entries, &p); if (fd < 0) return -errno;
  size_t sqsz = p.sq_off.array + p.sq_entries * sizeof(unsigned), cqsz = p.cq_off.cqes + p.cq_entries * sizeof(struct io_uring_cqe);
  char *sq = mmap(0, sqsz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQ_RING);
  char *cq = mmap(0, cqsz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_CQ_RING);
  ur.sqes = mmap(0, p.sq_entries * sizeof(struct io_uring_sqe), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQES);
  if (sq == MAP_FAILED || cq == MAP_FAILED || ur.sqes == MAP_FAILED) return -1;
  ur.fd = fd; ur.sq_tail = (unsigned *)(sq + p.sq_off.tail); ur.sq_mask = (unsigned *)(sq + p.sq_off.ring_mask); ur.sq_array = (unsigned *)(sq + p.sq_off.array);
  ur.cq_head = (unsigned *)(cq + p.cq_off.head); ur.cq_tail = (unsigned *)(cq + p.cq_off.tail); ur.cq_mask = (unsigned *)(cq + p.cq_off.ring_mask);
  ur.cqes = (struct io_uring_cqe *)(cq + p.cq_off.cqes);
  return 0;
}
typedef struct { int n; int op; int fd; size_t len; } urc;
static void b_uring(void *c, long k) { urc *v = c;
  for (long i = 0; i < k; i++) {
    unsigned tail = *ur.sq_tail;
    for (int j = 0; j < v->n; j++) {
      unsigned idx = tail & *ur.sq_mask; struct io_uring_sqe *s = &ur.sqes[idx]; memset(s, 0, sizeof *s);
      s->opcode = (uint8_t)v->op; s->fd = v->fd; s->addr = (uint64_t)(uintptr_t)iobuf; s->len = (uint32_t)v->len; s->off = 0;
      ur.sq_array[idx] = idx; tail++;
    }
    atomic_store_explicit((_Atomic unsigned *)ur.sq_tail, tail, memory_order_release);
    if (syscall(__NR_io_uring_enter, ur.fd, v->n, v->n, IORING_ENTER_GETEVENTS, 0, 0) < 0) abort();
    unsigned h = *ur.cq_head, t = atomic_load_explicit((_Atomic unsigned *)ur.cq_tail, memory_order_acquire);
    atomic_store_explicit((_Atomic unsigned *)ur.cq_head, t, memory_order_release); (void)h;
  } }

static void g_kernel(void) {
  measure("syscall_getpid", "-", b_getpid, 0, 0, 20, 0);
  measure("syscall_getppid", "-", b_getppid, 0, 0, 20, 0);
  measure("syscall_enosys", "-", b_badsys, 0, 0, 20, 0);
  measure("sched_yield", "-", b_sched_yield, 0, 0, 20, 0);
  measure("futex_wake_nowaiter", "-", b_futex_wake_nowaiter, 0, 0, 20, 0);
  clockid_t ids[] = {CLOCK_MONOTONIC, CLOCK_REALTIME, CLOCK_MONOTONIC_COARSE, CLOCK_BOOTTIME, CLOCK_THREAD_CPUTIME_ID};
  const char *in[] = {"MONOTONIC", "REALTIME", "MONOTONIC_COARSE", "BOOTTIME", "THREAD_CPUTIME"};
  for (int i = 0; i < 5; i++) measure("clock_gettime", in[i], b_clock, &ids[i], 0, 20, 0);
  measure("cntvct_read", "-", b_cntvct, 0, 0, 20, 0);
  if (pipe(ppfd)) abort();
  size_t PL[] = {1, 8, 64, 256, 512, 1200, 1500, 4096, 8192, 16384, 32768, 65536};
  for (unsigned i = 0; i < sizeof PL / sizeof *PL; i++) { lenc v = {PL[i]}; measure("pipe_write_read_samethread", PRM("len%zu", PL[i]), b_pipe_rw, &v, 0, 30, 0); }
  close(ppfd[0]); close(ppfd[1]);
  efd = eventfd(0, 0); measure("eventfd_write_read", "-", b_eventfd, 0, 0, 20, 0);
  epfd = epoll_create1(0); epready = eventfd(1, 0); struct epoll_event ev = {.events = EPOLLIN, .data.u64 = 1};
  epoll_ctl(epfd, EPOLL_CTL_ADD, epready, &ev);
  measure("epoll_wait", "1ready_lt", b_epoll_ready, 0, 0, 20, 0);
  epfd0 = epoll_create1(0); epidle = eventfd(0, 0); epoll_ctl(epfd0, EPOLL_CTL_ADD, epidle, &ev);
  for (int j = 0; j < 63; j++) { int f = eventfd(0, 0); struct epoll_event e2 = {.events = EPOLLIN, .data.u64 = 2}; epoll_ctl(epfd0, EPOLL_CTL_ADD, f, &e2); }
  measure("epoll_wait", "timeout0_none_ready_64fds", b_epoll_zero, 0, 0, 20, 0);
  /* page faults by first touch vs reuse */
  size_t PG[] = {1, 4, 16, 64, 256, 1024};
  for (unsigned i = 0; i < sizeof PG / sizeof *PG; i++) {
    pfc v = {PG[i], 1};
    measure("mmap_touch_munmap_nothp", PRM("pages%zu", PG[i]), b_pagefault, &v, 0, 100, 100);
    measure("mmap_munmap_notouch", PRM("pages%zu", PG[i]), b_mmap_munmap, &v, 0, 30, 100);
    touchbuf = mmap(0, PG[i] * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); madvise(touchbuf, PG[i] * 4096, MADV_NOHUGEPAGE);
    memset(touchbuf, 1, PG[i] * 4096);
    measure("retouch_resident", PRM("pages%zu", PG[i]), b_retouch, &v, 0, 30, 100);
    measure("madv_dontneed_refault", PRM("pages%zu", PG[i]), b_dontneed_touch, &v, 0, 100, 100);
    munmap(touchbuf, PG[i] * 4096);
  }
  /* file I/O under /var/tmp (the root EBS volume on EC2; the container's overlay on the rig) */
  const char *dir = getenv("CAL_DIR") ? getenv("CAL_DIR") : "/var/tmp";
  char path[256]; snprintf(path, sizeof path, "%s/calbench.dat", dir);
  ffd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
  if (ffd >= 0) {
    if (ftruncate(ffd, 16 << 20)) {}
    for (off_t o = 0; o < (16 << 20); o += 65536) if (pwrite(ffd, iobuf, 65536, o) < 0) break;
    fsync(ffd);
    size_t WL[] = {64, 512, 1200, 4096, 16384, 65536};
    for (unsigned i = 0; i < sizeof WL / sizeof *WL; i++) { lenc v = {WL[i]}; measure("pwrite_pagecache", PRM("len%zu", WL[i]), b_pwrite, &v, 0, 30, 150); }
    for (unsigned i = 0; i < sizeof WL / sizeof *WL; i++) { lenc v = {WL[i]}; measure("pread_pagecache", PRM("len%zu", WL[i]), b_pread, &v, 0, 30, 150); }
    size_t SL[] = {64, 1200, 4096, 16384};
    for (unsigned i = 0; i < sizeof SL / sizeof *SL; i++) { lenc v = {SL[i]}; foff = 0; measure("pwrite_fdatasync_overwrite", PRM("len%zu", SL[i]), b_pwrite_fdatasync, &v, 1, 0, 60); }
    { lenc v = {4096}; foff = 16 << 20; measure("pwrite_fdatasync_append", "len4096", b_pwrite_append_fdatasync, &v, 1, 0, 60); }
    mfile = mmap(0, 64 * 4096, PROT_READ | PROT_WRITE, MAP_SHARED, ffd, 0);
    if (mfile != MAP_FAILED) { lenc v = {256}; measure("msync_4k_page", "dirty256", b_msync, &v, 1, 0, 60); munmap(mfile, 64 * 4096); }
    /* io_uring on the file */
    if (uring_init(64) == 0) {
      ur_ok = 1;
      int NS[] = {1, 2, 4, 11, 32};
      for (int i = 0; i < 5; i++) { urc v = {NS[i], IORING_OP_NOP, -1, 0}; measure("io_uring_enter_nop", PRM("sqes%d", NS[i]), b_uring, &v, 0, 30, 0); }
      for (int i = 0; i < 5; i++) { urc v = {NS[i], IORING_OP_WRITE, ffd, 4096}; measure("io_uring_enter_write4k", PRM("sqes%d", NS[i]), b_uring, &v, 0, 30, 0); }
    } else printf("# io_uring_setup unavailable\n");
    close(ffd); unlink(path);
  }
  if (!ur_ok) emit("io_uring_enter_nop", "unavailable", 0, sbuf, 0, 0);
}

/* ---- cross-thread ping-pong: futex, pipe, eventfd ---- */
static _Atomic int fx_a, fx_b; static int pp_mode; static atomic_int pp_stop;
static int pa[2], pb[2], ea, eb;
static void fwait(_Atomic int *f, int v) { while (atomic_load(f) == v) syscall(SYS_futex, f, FUTEX_WAIT_PRIVATE, v, 0, 0, 0); }
static void fwake(_Atomic int *f) { atomic_fetch_add(f, 1); syscall(SYS_futex, f, FUTEX_WAKE_PRIVATE, 1, 0, 0, 0); }
static void *pong(void *arg) { pin((int)(intptr_t)arg); char c; uint64_t x; int seen = atomic_load(&fx_a);
  for (;;) {
    if (pp_mode == 0) { fwait(&fx_a, seen); seen = atomic_load(&fx_a); if (atomic_load(&pp_stop)) break; fwake(&fx_b); }
    else if (pp_mode == 1) { if (read(pa[0], &c, 1) != 1 || c == 'q') break; if (write(pb[1], &c, 1) != 1) break; }
    else { if (read(ea, &x, 8) != 8 || x >= 1000) break; x = 1; if (write(eb, &x, 8) != 8) break; }
  } return 0; }
static void b_ping(void *c, long k) { char ch = 'p'; uint64_t x = 1;
  for (long i = 0; i < k; i++) {
    if (pp_mode == 0) { int v = atomic_load(&fx_b); fwake(&fx_a); fwait(&fx_b, v); }
    else if (pp_mode == 1) { if (write(pa[1], &ch, 1) != 1 || read(pb[0], &ch, 1) != 1) abort(); }
    else { if (write(ea, &x, 8) != 8 || read(eb, &x, 8) != 8) abort(); }
  } }
static void pingpong(const char *name, int mode, int other) {
  pp_mode = mode; atomic_store(&pp_stop, 0); pthread_t t;
  /* the futex pong waits for fx_a to change from the value it read; give it a sentinel start */
  pthread_create(&t, 0, pong, (void *)(intptr_t)other); usleep(2000);
  measure(name, other == g_cpu ? "samecore_roundtrip" : "crosscore_roundtrip", b_ping, 0, 0, 200, 200);
  atomic_store(&pp_stop, 1);
  if (mode == 0) fwake(&fx_a);
  else if (mode == 1) { char q = 'q'; if (write(pa[1], &q, 1)) {} }
  else { uint64_t x = 5000; if (write(ea, &x, 8)) {} }
  pthread_join(t, 0);
}

/* ================= NET: TCP loopback ================= */
static int tcp_pair(int *cli, int *srv) {
  int l = socket(AF_INET, SOCK_STREAM, 0); struct sockaddr_in a = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
  int one = 1; setsockopt(l, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  if (bind(l, (void *)&a, sizeof a) || listen(l, 64)) return -1;
  socklen_t al = sizeof a; getsockname(l, (void *)&a, &al);
  *cli = socket(AF_INET, SOCK_STREAM, 0); if (connect(*cli, (void *)&a, sizeof a)) return -1;
  *srv = accept(l, 0, 0); close(l);
  setsockopt(*cli, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one); setsockopt(*srv, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
  int big = 4 << 20; setsockopt(*cli, SOL_SOCKET, SO_SNDBUF, &big, sizeof big); setsockopt(*srv, SOL_SOCKET, SO_RCVBUF, &big, sizeof big);
  return 0;
}
static int nsock; static int cl[16], sv[16]; static size_t net_len;
static void drain(int fd, size_t total) { while (total > 0) { ssize_t r = recv(fd, iobuf, total < sizeof iobuf ? total : sizeof iobuf, 0); if (r <= 0) abort(); total -= (size_t)r; } }
/* send timing: k sends of net_len on socket 0, then (untimed in the next call) drain. We fold drain cost out by timing it separately. */
static void b_tcp_send(void *c, long k) { for (long i = 0; i < k; i++) if (send(cl[0], iobuf, net_len, 0) != (ssize_t)net_len) abort(); drain(sv[0], (size_t)k * net_len); }
static void b_tcp_drain_only(void *c, long k) { (void)c; (void)k; }
static void b_tcp_sendrecv(void *c, long k) { for (long i = 0; i < k; i++) { if (send(cl[0], iobuf, net_len, 0) != (ssize_t)net_len) abort(); drain(sv[0], net_len); } }
static void b_tcp_fanout(void *c, long k) { for (long i = 0; i < k; i++) { for (int s = 0; s < nsock; s++) if (send(cl[s], iobuf, net_len, 0) != (ssize_t)net_len) abort(); for (int s = 0; s < nsock; s++) drain(sv[s], net_len); } }
/* send-only cost: time sends alone, drain outside the timer */
static void measure_send_only(size_t len) {
  int n = 0; long k = (long)((256 << 10) / len); if (k > 64) k = 64; if (k < 1) k = 1;
  int64_t e0[NEV], e1[NEV]; double es[NEV] = {0}, rs[NEV] = {0}; long ops = 0;
  double *rb = malloc(sizeof(double) * 400);
  int nr = 0;
  for (int i = 0; i < 300; i++) {
    pmu_read(e0); int64_t t0 = now_ns();
    for (long j = 0; j < k; j++) if (send(cl[0], iobuf, len, 0) != (ssize_t)len) abort();
    int64_t t1 = now_ns(); pmu_read(e1);
    for (int q = 0; q < NEV; q++) if (e0[q] >= 0) es[q] += (double)(e1[q] - e0[q]);
    sbuf[n++] = (double)(t1 - t0) / k;
    /* recv side: k recvs of len each */
    pmu_read(e0); t0 = now_ns();
    for (long j = 0; j < k; j++) drain(sv[0], len);
    t1 = now_ns(); pmu_read(e1);
    for (int q = 0; q < NEV; q++) if (e0[q] >= 0) rs[q] += (double)(e1[q] - e0[q]);
    rb[nr++] = (double)(t1 - t0) / k; ops += k;
  }
  double ev[NEV], rv[NEV];
  for (int q = 0; q < NEV; q++) { ev[q] = pfd[q] >= 0 ? es[q] / ops : -1; rv[q] = pfd[q] >= 0 ? rs[q] / ops : -1; }
  emit("tcp_send_only", PRM("len%zu", len), k, sbuf, n, ev);
  emit("tcp_recv_only", PRM("len%zu", len), k, rb, nr, rv);
  free(rb);
}
static void g_net(void) {
  for (int i = 0; i < 16; i++) if (tcp_pair(&cl[i], &sv[i])) { printf("# tcp_pair failed\n"); return; }
  size_t NL[] = {16, 64, 128, 256, 512, 1024, 1200, 1500, 2048, 4096, 8192, 16384, 65536};
  for (unsigned i = 0; i < sizeof NL / sizeof *NL; i++) {
    net_len = NL[i];
    measure_send_only(NL[i]);
    measure("tcp_send_then_recv_samethread", PRM("len%zu", NL[i]), b_tcp_sendrecv, 0, 0, 30, 0);
  }
  int NSK[] = {1, 2, 3, 4, 6, 8, 11, 16};
  size_t FL[] = {64, 512, 1500};
  for (unsigned j = 0; j < 3; j++) for (unsigned i = 0; i < 8; i++) {
    nsock = NSK[i]; net_len = FL[j];
    measure("tcp_fanout_send_then_drain", PRM("socks%d_len%zu", NSK[i], FL[j]), b_tcp_fanout, 0, 0, 50, 0);
  }
  /* cross-thread ping-pong over the same primitives the server uses */
  if (pipe(pa) || pipe(pb)) abort();
  ea = eventfd(0, 0); eb = eventfd(0, 0);
  int other = (g_cpu == 0) ? 1 : 0;
  int targets[2] = {other, g_cpu};
  for (int t = 0; t < (ncpu() >= 2 ? 2 : 1); t++) {
    int o = ncpu() >= 2 ? targets[t] : g_cpu;
    pingpong("futex_pingpong", 0, o);
    pingpong("pipe_pingpong", 1, o);
    pingpong("eventfd_pingpong", 2, o);
  }
  for (int i = 0; i < 16; i++) { close(cl[i]); close(sv[i]); }
}

/* ================= WAKE: wake-up tax after idle ================= */
/* a waiter thread blocks (futex or epoll); the main thread sleeps X ms, stamps t0, wakes it; the waiter stamps t1.
 * latency = t1 - t0. waker_cost = duration of the wake syscall. wakee_cpu = waiter's thread CPU time per cycle. */
static _Atomic int wk_f; static _Atomic int64_t wk_t1; static _Atomic int64_t wk_cpu; static int wk_mode; static int wk_efd, wk_ep; static atomic_int wk_stop;
static int64_t thr_cpu(void) { struct timespec t; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t); return NS(t); }
static void *waiter(void *arg) { pin((int)(intptr_t)arg); int seen = atomic_load(&wk_f);
  for (;;) {
    int64_t c0 = thr_cpu();
    if (wk_mode == 0) { while (atomic_load(&wk_f) == seen) syscall(SYS_futex, &wk_f, FUTEX_WAIT_PRIVATE, seen, 0, 0, 0); seen = atomic_load(&wk_f); }
    else { struct epoll_event e; while (epoll_wait(wk_ep, &e, 1, -1) != 1) {} uint64_t x; if (read(wk_efd, &x, 8)) {} }
    int64_t t1 = now_ns();
    if (atomic_load(&wk_stop)) break;
    atomic_store(&wk_cpu, thr_cpu() - c0);
    atomic_store(&wk_t1, t1);
  } return 0; }
static void wake_series(const char *mode_name, int mode, int other, double sleep_ms, int n) {
  wk_mode = mode; atomic_store(&wk_stop, 0); atomic_store(&wk_t1, 0);
  pthread_t t; pthread_create(&t, 0, waiter, (void *)(intptr_t)other); usleep(5000);
  double *lat = malloc(sizeof(double) * n), *wc = malloc(sizeof(double) * n), *cc = malloc(sizeof(double) * n), *os = malloc(sizeof(double) * n);
  int got = 0;
  for (int i = 0; i < n; i++) {
    int64_t s0 = now_ns();
    if (sleep_ms > 0) { struct timespec ts = {(time_t)(sleep_ms / 1000), (long)(fmod(sleep_ms, 1000) * 1e6)}; nanosleep(&ts, 0); }
    else { int64_t e = now_ns() + 20000; while (now_ns() < e) {} }
    int64_t t0 = now_ns();
    os[i] = (double)(t0 - s0) - sleep_ms * 1e6;
    atomic_store(&wk_t1, 0);
    if (mode == 0) { atomic_fetch_add(&wk_f, 1); syscall(SYS_futex, &wk_f, FUTEX_WAKE_PRIVATE, 1, 0, 0, 0); }
    else { uint64_t x = 1; if (write(wk_efd, &x, 8)) {} }
    int64_t tw = now_ns();
    int64_t t1; int spins = 0;
    while ((t1 = atomic_load(&wk_t1)) == 0) { if (++spins > 1000) { struct timespec q = {0, 20000}; nanosleep(&q, 0); } if (now_ns() - t0 > 2000000000LL) break; }
    if (t1 == 0) continue;
    lat[got] = (double)(t1 - t0); wc[got] = (double)(tw - t0); cc[got] = (double)atomic_load(&wk_cpu); os[got] = os[i]; got++;
  }
  double ev[NEV] = {-1, -1, -1, -1, -1};
  char pr[96]; snprintf(pr, sizeof pr, "%s_idle%.0fms_%s", mode_name, sleep_ms, other == g_cpu ? "samecore" : "crosscore");
  g_group = "wake";
  emit("wake_latency", pr, 1, lat, got, ev);
  emit("waker_syscall", pr, 1, wc, got, ev);
  emit("wakee_cpu_per_cycle", pr, 1, cc, got, ev);
  emit("sleep_overshoot", pr, 1, os, got, ev);
  atomic_store(&wk_stop, 1);
  if (mode == 0) { atomic_fetch_add(&wk_f, 1); syscall(SYS_futex, &wk_f, FUTEX_WAKE_PRIVATE, 1, 0, 0, 0); } else { uint64_t x = 1; if (write(wk_efd, &x, 8)) {} }
  pthread_join(t, 0);
  free(lat); free(wc); free(cc); free(os);
}
static void g_wake(void) {
  wk_efd = eventfd(0, 0); wk_ep = epoll_create1(0); struct epoll_event e = {.events = EPOLLIN}; epoll_ctl(wk_ep, EPOLL_CTL_ADD, wk_efd, &e);
  double S[] = {0, 1, 10, 100};
  int N[] = {400, 300, 100, 25};
  int other = ncpu() >= 2 ? (g_cpu == 0 ? 1 : 0) : g_cpu;
  for (int i = 0; i < 4; i++) {
    wake_series("futex", 0, other, S[i], N[i]);
    wake_series("epoll_eventfd", 1, other, S[i], N[i]);
    if (other != g_cpu) wake_series("futex", 0, g_cpu, S[i], N[i] / 2 + 1);
  }
}

/* ================= REAL: OpenSSL HMAC/SHA, SQLite ================= */
typedef struct { size_t len; } hc;
static unsigned char key[32] = "0123456789abcdef0123456789abcdef";
static void b_hmac(void *c, long k) { hc *v = c; unsigned char out[32]; unsigned ol;
  for (long i = 0; i < k; i++) HMAC(EVP_sha256(), key, 32, (unsigned char *)iobuf, v->len, out, &ol); }
static void b_sha256(void *c, long k) { hc *v = c; unsigned char out[32]; for (long i = 0; i < k; i++) SHA256((unsigned char *)iobuf, v->len, out); }
static EVP_MD_CTX *mdctx;
static void b_evp_sha256(void *c, long k) { hc *v = c; unsigned char out[32]; unsigned ol;
  for (long i = 0; i < k; i++) { EVP_DigestInit_ex(mdctx, EVP_sha256(), 0); EVP_DigestUpdate(mdctx, iobuf, v->len); EVP_DigestFinal_ex(mdctx, out, &ol); } }
static sqlite3 *db; static sqlite3_stmt *ins, *sel, *upd; static long rowid_next = 1; static long sql_rows;
static void b_sql_insert(void *c, long k) { hc *v = c;
  for (long i = 0; i < k; i++) { sqlite3_bind_int64(ins, 1, rowid_next++); sqlite3_bind_blob(ins, 2, iobuf, (int)v->len, SQLITE_STATIC);
    if (sqlite3_step(ins) != SQLITE_DONE) abort(); sqlite3_reset(ins); } }
static void b_sql_update(void *c, long k) { hc *v = c;
  for (long i = 0; i < k; i++) { sqlite3_bind_blob(upd, 1, iobuf, (int)v->len, SQLITE_STATIC); sqlite3_bind_int64(upd, 2, 1 + (long)(xr() % sql_rows));
    if (sqlite3_step(upd) != SQLITE_DONE) abort(); sqlite3_reset(upd); } }
static void b_sql_select(void *c, long k) {
  for (long i = 0; i < k; i++) { sqlite3_bind_int64(sel, 1, 1 + (long)(xr() % sql_rows)); if (sqlite3_step(sel) != SQLITE_ROW) abort();
    const void *b = sqlite3_column_blob(sel, 0); OPAQUE(b); sqlite3_reset(sel); } }
static void b_sql_txn_insert(void *c, long k) { hc *v = c;
  for (long i = 0; i < k; i++) { sqlite3_exec(db, "BEGIN", 0, 0, 0);
    for (int j = 0; j < 4; j++) { sqlite3_bind_int64(ins, 1, rowid_next++); sqlite3_bind_blob(ins, 2, iobuf, (int)v->len, SQLITE_STATIC); if (sqlite3_step(ins) != SQLITE_DONE) abort(); sqlite3_reset(ins); }
    sqlite3_exec(db, "COMMIT", 0, 0, 0); } }
static void g_real(void) {
  for (size_t i = 0; i < sizeof iobuf; i++) iobuf[i] = (char)(xr() & 0x7f);
  size_t HL[] = {0, 16, 32, 55, 64, 100, 200, 300, 400, 512, 1024, 1200, 2048, 4096, 8192, 16384};
  mdctx = EVP_MD_CTX_new();
  for (unsigned i = 0; i < sizeof HL / sizeof *HL; i++) { hc v = {HL[i]};
    measure("hmac_sha256_oneshot", PRM("len%zu", HL[i]), b_hmac, &v, 0, 30, 0);
    measure("sha256_oneshot", PRM("len%zu", HL[i]), b_sha256, &v, 0, 30, 0);
    measure("evp_sha256_reuse_ctx", PRM("len%zu", HL[i]), b_evp_sha256, &v, 0, 30, 0); }
  const char *dir = getenv("CAL_DIR") ? getenv("CAL_DIR") : "/var/tmp";
  size_t RL[] = {64, 256, 512, 1200, 2048, 4096, 8192};
  for (unsigned r = 0; r < sizeof RL / sizeof *RL; r++) {
    char path[256]; snprintf(path, sizeof path, "%s/calbench.sqlite", dir);
    unlink(path); char w[300]; snprintf(w, sizeof w, "%s-wal", path); unlink(w); snprintf(w, sizeof w, "%s-shm", path); unlink(w);
    if (sqlite3_open(path, &db) != SQLITE_OK) { printf("# sqlite open failed\n"); return; }
    sqlite3_exec(db, "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; CREATE TABLE t(id INTEGER PRIMARY KEY, v BLOB);", 0, 0, 0);
    sqlite3_prepare_v2(db, "INSERT INTO t(id,v) VALUES(?,?)", -1, &ins, 0);
    sqlite3_prepare_v2(db, "SELECT v FROM t WHERE id=?", -1, &sel, 0);
    sqlite3_prepare_v2(db, "UPDATE t SET v=? WHERE id=?", -1, &upd, 0);
    rowid_next = 1; hc v = {RL[r]};
    measure("sqlite_insert_autocommit_wal_normal", PRM("row%zu", RL[r]), b_sql_insert, &v, 0, 200, 150);
    sql_rows = rowid_next - 1;
    measure("sqlite_update_autocommit_wal_normal", PRM("row%zu", RL[r]), b_sql_update, &v, 0, 200, 150);
    measure("sqlite_select_pk", PRM("row%zu", RL[r]), b_sql_select, &v, 0, 30, 150);
    measure("sqlite_txn4_insert", PRM("row%zu", RL[r]), b_sql_txn_insert, &v, 0, 200, 100);
    sqlite3_finalize(ins); sqlite3_finalize(sel); sqlite3_finalize(upd); sqlite3_close(db);
    unlink(path);
  }
}


/* ================= CLOCK / FP / STREAM / EBS / STEAL (timing-only cycle model) ================= */
/* 256 dependent 1-cycle adds per iteration: ns per add == ns per cycle on N1 (ADD latency 1) */
static void b_add_chain256(void *c, long k) { uint64_t x = 1, y = 3; OPAQUE(y);
  for (long i = 0; i < k; i++) { __asm__ volatile(".rept 256\n add %0,%0,%1\n .endr" : "+r"(x) : "r"(y)); } OPAQUE(x); }
#define FPB(nm, ins, reg, rep) \
static void nm##_dep(void *c, long k) { double x = 1.0000001, y = 0.9999999; OPAQUE(y); \
  for (long i = 0; i < k; i++) { __asm__ volatile(".rept " #rep "\n " ins " %" reg "0,%" reg "0,%" reg "1\n .endr" : "+w"(x) : "w"(y)); } OPAQUE(x); } \
static void nm##_ind(void *c, long k) { double a = 1.0000001, b = 1.0000002, d = 1.0000003, e = 1.0000004, f = 1.0000005, g = 1.0000006, h = 1.0000007, j = 1.0000008, y = 0.9999999; OPAQUE(y); \
  for (long i = 0; i < k; i++) { __asm__ volatile(".rept " #rep "\n " ins " %" reg "0,%" reg "0,%" reg "8\n " ins " %" reg "1,%" reg "1,%" reg "8\n " ins " %" reg "2,%" reg "2,%" reg "8\n " ins " %" reg "3,%" reg "3,%" reg "8\n " \
    ins " %" reg "4,%" reg "4,%" reg "8\n " ins " %" reg "5,%" reg "5,%" reg "8\n " ins " %" reg "6,%" reg "6,%" reg "8\n " ins " %" reg "7,%" reg "7,%" reg "8\n .endr" \
    : "+w"(a), "+w"(b), "+w"(d), "+w"(e), "+w"(f), "+w"(g), "+w"(h), "+w"(j) : "w"(y)); } OPAQUE(a); OPAQUE(j); }
FPB(fadd_d, "fadd", "d", 16) FPB(fmul_d, "fmul", "d", 16) FPB(fdiv_d, "fdiv", "d", 16)
FPB(fadd_s, "fadd", "s", 16) FPB(fmul_s, "fmul", "s", 16) FPB(fdiv_s, "fdiv", "s", 16)
static void fsqrt_d_dep(void *c, long k) { double x = 1e10; for (long i = 0; i < k; i++) { __asm__ volatile(".rept 16\n fsqrt %d0,%d0\n .endr" : "+w"(x)); } OPAQUE(x); }
static void fsqrt_s_dep(void *c, long k) { float x = 1e10f; for (long i = 0; i < k; i++) { __asm__ volatile(".rept 16\n fsqrt %s0,%s0\n .endr" : "+w"(x)); } OPAQUE(x); }
static void b_sdiv64_dep(void *c, long k) { divc *v = c; int64_t n = (int64_t)v->n, d = (int64_t)v->d, base = (int64_t)v->n;
  for (long i = 0; i < k; i++) { int64_t q; __asm__ volatile("sdiv %0,%1,%2\n and %0,%0,xzr\n orr %1,%0,%3" : "=&r"(q), "+r"(n) : "r"(d), "r"(base)); } OPAQUE(n); }
static void b_andorr_dep(void *c, long k) { uint64_t n = 5, b = 5; OPAQUE(b);
  for (long i = 0; i < k; i++) { uint64_t q = n; __asm__ volatile("and %0,%0,xzr\n orr %1,%0,%2" : "+r"(q), "+r"(n) : "r"(b)); } OPAQUE(n); }
static void g_clock(void) {
  measure("empty_loop", "-", b_empty, 0, 0, 50, 0);
  measure("add_chain256", "ns_per_iter_of_256_cycles", b_add_chain256, 0, 0, 100, 0);
  measure("divlink_and_orr_dep", "the_2op_link_in_div_chains", b_andorr_dep, 0, 0, 20, 0);
  struct { const char *n; bfn d, i; } F[] = {{"fadd_d", fadd_d_dep, fadd_d_ind}, {"fmul_d", fmul_d_dep, fmul_d_ind}, {"fdiv_d", fdiv_d_dep, fdiv_d_ind},
    {"fadd_s", fadd_s_dep, fadd_s_ind}, {"fmul_s", fmul_s_dep, fmul_s_ind}, {"fdiv_s", fdiv_s_dep, fdiv_s_ind}};
  for (int i = 0; i < 6; i++) { measure(F[i].n, "dep_x16", F[i].d, 0, 0, 50, 0); measure(F[i].n, "ind8_x16", F[i].i, 0, 0, 50, 0); }
  measure("fsqrt_d", "dep_x16", fsqrt_d_dep, 0, 0, 50, 0);
  measure("fsqrt_s", "dep_x16", fsqrt_s_dep, 0, 0, 50, 0);
  int nb[] = {1, 8, 16, 24, 32, 40, 48, 56, 62};
  for (unsigned i = 0; i < 9; i++) { divc v; v.n = (1ULL << nb[i]) - 1; v.d = 3; measure("sdiv64_dep", PRM("n%d_d2", nb[i]), b_sdiv64_dep, &v, 0, 20, 0); }
  measure("add_chain256", "ns_per_iter_of_256_cycles_again", b_add_chain256, 0, 0, 100, 0);
}
/* frequency + steal over time: every 250 ms, a 2 ms add-chain sample on the pinned core, and /proc/stat steal deltas */
static void read_stat(uint64_t *tot, uint64_t *steal) { FILE *f = fopen("/proc/stat", "r"); unsigned long long u, n, s, id, io, irq, sirq, st; *tot = *steal = 0;
  if (f && fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &u, &n, &s, &id, &io, &irq, &sirq, &st) == 8) { *tot = u + n + s + id + io + irq + sirq + st; *steal = st; } if (f) fclose(f); }
static atomic_int burn_stop;
static void *burner(void *a) { pin((int)(intptr_t)a); volatile uint64_t x = 0; while (!atomic_load(&burn_stop)) x++; return 0; }
static void freq_series(const char *label, int seconds) {
  uint64_t t0, s0, t1, s1; read_stat(&t0, &s0);
  for (int i = 0; i < seconds * 4; i++) {
    int64_t a = now_ns(); long k = 0; int64_t b;
    do { b_add_chain256(0, 32); k += 32; b = now_ns(); } while (b - a < 2000000);
    double ns_per_cycle = (double)(b - a) / (k * 256.0);
    read_stat(&t1, &s1);
    printf("%s\tfreq_series\t%s_t%.2f\t%ld\t1\t%.5f\t%.5f\t%.5f\t0\t%.5f\t%.5f\t%.4f\t-1\t-1\t-1\t%.4f\t%d\t%d\n", g_group, label, i / 4.0, k,
           ns_per_cycle, ns_per_cycle, ns_per_cycle, ns_per_cycle, ns_per_cycle, 1.0 / ns_per_cycle, t1 > t0 ? (double)(s1 - s0) / (double)(t1 - t0) : 0, g_cpu, g_pass);
    fflush(stdout); t0 = t1; s0 = s1;
    usleep(248000);
  }
}
static void g_steal(void) {
  /* cols in freq_series rows: mean_ns = ns per cycle, cyc column = GHz, llcmiss column = steal fraction over the last 250 ms (all cpus) */
  freq_series("idle_other_cpus", 30);
  pthread_t th[8]; int nc = ncpu(); atomic_store(&burn_stop, 0);
  for (int c = 0; c < nc && c < 8; c++) if (c != g_cpu) pthread_create(&th[c], 0, burner, (void *)(intptr_t)c);
  freq_series("all_cpus_busy", 90);
  atomic_store(&burn_stop, 1);
  for (int c = 0; c < nc && c < 8; c++) if (c != g_cpu) pthread_join(th[c], 0);
}
/* STREAM-like: copy/scale/add/triad on 3 arrays, ns per element (8 B doubles); GB/s = bytes_per_elem / ns */
static double *SA, *SB, *SC; static size_t SN;
static void b_copy(void *c, long k) { for (long r = 0; r < k; r++) { for (size_t i = 0; i < SN; i++) SC[i] = SA[i]; CLOBBER(); } }
static void b_scale(void *c, long k) { for (long r = 0; r < k; r++) { for (size_t i = 0; i < SN; i++) SB[i] = 3.0 * SC[i]; CLOBBER(); } }
static void b_sadd(void *c, long k) { for (long r = 0; r < k; r++) { for (size_t i = 0; i < SN; i++) SC[i] = SA[i] + SB[i]; CLOBBER(); } }
static void b_triad(void *c, long k) { for (long r = 0; r < k; r++) { for (size_t i = 0; i < SN; i++) SA[i] = SB[i] + 3.0 * SC[i]; CLOBBER(); } }
static void g_stream(void) {
  long avail = mem_avail_kb();
  size_t MB[] = {1, 8, 32, 64, 128};
  for (int m = 0; m < 5; m++) {
    size_t bytes = MB[m] << 20;
    if (avail > 0 && (long)(3 * bytes / 1024) > avail / 2) { printf("# stream skip %zuMB per array\n", MB[m]); continue; }
    SN = bytes / 8; SA = malloc(bytes); SB = malloc(bytes); SC = malloc(bytes);
    for (size_t i = 0; i < SN; i++) { SA[i] = 1; SB[i] = 2; SC[i] = 0; }
    int ns = MB[m] >= 32 ? 12 : 40;
    /* ns per element: per-call k=1 sweeps of SN elements; the row's k column is 1 and mean is ns per SWEEP; divide by elems */
    double t[4]; bfn fs[4] = {b_copy, b_scale, b_sadd, b_triad}; const char *nm[4] = {"stream_copy", "stream_scale", "stream_add", "stream_triad"}; int by[4] = {16, 16, 24, 24};
    for (int f = 0; f < 4; f++) {
      double best = 1e30; fs[f](0, 1);
      for (int r = 0; r < ns; r++) { int64_t a = now_ns(); fs[f](0, 1); int64_t b = now_ns(); sbuf[r] = (double)(b - a) / SN; if (sbuf[r] < best) best = sbuf[r]; }
      double ev[NEV] = {-1, -1, -1, -1, -1};
      emit(nm[f], PRM("%zuMB_per_array_ns_per_elem_%dB", MB[m], by[f]), (long)SN, sbuf, ns, ev);
      printf("# %s %zuMB best %.2f GB/s\n", nm[f], MB[m], by[f] / best); t[f] = best;
    }
    (void)t; free(SA); free(SB); free(SC);
  }
}
/* EBS: O_DIRECT latency at QD1 and IOPS at QD32 via io_uring, sequential 1 MB throughput */
static int dfd; static char *dbuf; static size_t dfile = 256u << 20;
static void b_dio_randread(void *c, long k) { lenc *v = c; for (long i = 0; i < k; i++) { off_t o = (off_t)((xr() % (dfile / v->len)) * v->len); if (pread(dfd, dbuf, v->len, o) != (ssize_t)v->len) abort(); } }
static void b_dio_randwrite_dsync(void *c, long k) { lenc *v = c; for (long i = 0; i < k; i++) { off_t o = (off_t)((xr() % (dfile / v->len)) * v->len); if (pwrite(dfd, dbuf, v->len, o) != (ssize_t)v->len) abort(); fdatasync(dfd); } }
static void b_dio_seqwrite(void *c, long k) { lenc *v = c; static off_t o; for (long i = 0; i < k; i++) { if (pwrite(dfd, dbuf, v->len, o) != (ssize_t)v->len) abort(); o = (o + (off_t)v->len) % (off_t)dfile; } }
static void b_dio_seqread(void *c, long k) { lenc *v = c; static off_t o; for (long i = 0; i < k; i++) { if (pread(dfd, dbuf, v->len, o) != (ssize_t)v->len) abort(); o = (o + (off_t)v->len) % (off_t)dfile; } }
static void b_uring_dio(void *c, long k) { urc *v = c;
  for (long i = 0; i < k; i++) {
    unsigned tail = *ur.sq_tail;
    for (int j = 0; j < v->n; j++) { unsigned idx = tail & *ur.sq_mask; struct io_uring_sqe *s = &ur.sqes[idx]; memset(s, 0, sizeof *s);
      s->opcode = (uint8_t)v->op; s->fd = v->fd; s->addr = (uint64_t)(uintptr_t)(dbuf + (size_t)j * v->len); s->len = (uint32_t)v->len;
      s->off = (xr() % (dfile / v->len)) * v->len; ur.sq_array[idx] = idx; tail++; }
    atomic_store_explicit((_Atomic unsigned *)ur.sq_tail, tail, memory_order_release);
    int need = v->n;
    while (need > 0) { if (syscall(__NR_io_uring_enter, ur.fd, need == v->n ? v->n : 0, 1, IORING_ENTER_GETEVENTS, 0, 0) < 0) abort();
      unsigned h = *ur.cq_head, t = atomic_load_explicit((_Atomic unsigned *)ur.cq_tail, memory_order_acquire);
      need -= (int)(t - h); atomic_store_explicit((_Atomic unsigned *)ur.cq_head, t, memory_order_release); }
  } }
static void g_ebs(void) {
  const char *dir = getenv("CAL_DIR") ? getenv("CAL_DIR") : "/var/tmp";
  char path[256]; snprintf(path, sizeof path, "%s/calbench.direct", dir);
  dfd = open(path, O_RDWR | O_CREAT | O_TRUNC | O_DIRECT, 0600);
  if (dfd < 0) { printf("# O_DIRECT open failed: %s\n", strerror(errno)); return; }
  if (posix_memalign((void **)&dbuf, 4096, 32u << 20)) abort(); memset(dbuf, 7, 32u << 20);
  for (off_t o = 0; o < (off_t)dfile; o += 1 << 20) if (pwrite(dfd, dbuf, 1 << 20, o) != 1 << 20) { printf("# prefill failed\n"); break; }
  fsync(dfd);
  lenc v4 = {4096}, v16 = {16384}, v1m = {1 << 20};
  measure("ebs_direct_randread_qd1", "4k", b_dio_randread, &v4, 1, 0, 400);
  measure("ebs_direct_randread_qd1", "16k", b_dio_randread, &v16, 1, 0, 200);
  measure("ebs_direct_randwrite_fdatasync_qd1", "4k", b_dio_randwrite_dsync, &v4, 1, 0, 300);
  measure("ebs_direct_seqwrite", "1m", b_dio_seqwrite, &v1m, 1, 0, 100);
  measure("ebs_direct_seqread", "1m", b_dio_seqread, &v1m, 1, 0, 100);
  if (uring_init(64) == 0) {
    int QD[] = {4, 16, 32};
    for (int q = 0; q < 3; q++) { urc u = {QD[q], IORING_OP_READ, dfd, 4096}; measure("ebs_uring_randread_batch", PRM("qd%d_4k", QD[q]), b_uring_dio, &u, 1, 0, 200); }
    { urc u = {32, IORING_OP_WRITE, dfd, 4096}; measure("ebs_uring_randwrite_batch", "qd32_4k", b_uring_dio, &u, 1, 0, 200); }
    { urc u = {8, IORING_OP_WRITE, dfd, 1 << 20}; measure("ebs_uring_seqish_write_batch", "qd8_1m", b_uring_dio, &u, 1, 0, 40); }
  }
  close(dfd); unlink(path);
}

/* ================= ENV ================= */
static void cat(const char *p, int maxlines) { FILE *f = fopen(p, "r"); char l[512]; int n = 0; if (!f) { printf("# %s: n/a\n", p); return; }
  while (fgets(l, sizeof l, f) && n++ < maxlines) printf("# %s: %s", p, l); fclose(f); }
static void g_env(void) {
  struct utsname u; uname(&u); printf("# uname: %s %s %s %s\n", u.sysname, u.release, u.version, u.machine);
  printf("# ncpu: %d pmu: %d sqlite: %s openssl: %s\n", ncpu(), pmu_ok, sqlite3_libversion(), OpenSSL_version(OPENSSL_VERSION));
  uint64_t f; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f)); printf("# cntfrq_hz: %llu\n", (unsigned long long)f);
  cat("/proc/cpuinfo", 40); cat("/proc/meminfo", 3);
  cat("/sys/kernel/mm/transparent_hugepage/enabled", 1);
  cat("/sys/devices/system/clocksource/clocksource0/current_clocksource", 1);
  cat("/proc/sys/kernel/perf_event_paranoid", 1);
  cat("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", 1);
  cat("/sys/devices/system/cpu/cpu0/regs/identification/midr_el1", 1);
  cat("/sys/devices/system/cpu/vulnerabilities/spectre_v2", 1);
  cat("/sys/devices/system/cpu/vulnerabilities/spec_store_bypass", 1);
  cat("/proc/cmdline", 1);
  cat("/sys/devices/system/cpu/cpuidle/current_driver", 1);
  for (int c = 0; c < ncpu() && c < 8; c++) { char p[128]; snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/cpu_capacity", c); cat(p, 1); }
}

int main(int argc, char **argv) {
  if (argc < 2) { fprintf(stderr, "usage: calbench GROUP [PASS]\n"); return 2; }
  g_pass = argc > 2 ? atoi(argv[2]) : 0;
  ncpu();
  if (getenv("CAL_CPU")) g_cpu = atoi(getenv("CAL_CPU"));
  if (g_cpu >= ncpu()) g_cpu = 0;
  if (getenv("CAL_BUDGET_MS")) g_budget_ms = atof(getenv("CAL_BUDGET_MS"));
  pin(g_cpu);
  sbuf = malloc(sizeof(double) * 100000);
  pmu_open();
  const char *G = argv[1];
  int all = !strcmp(G, "all");
#define RUN(name, fn) if (all || !strcmp(G, name)) { g_group = name; fn(); }
  printf("#cols\tgroup\tname\tparam\tk\tn\tmin_ns\tmax_ns\tmean_ns\tstd_ns\tp50_ns\tp99_ns\tcyc\tinsn\tbrmiss\tl1dmiss\tllcmiss\tcpu\tpass\n");
  RUN("env", g_env);
  RUN("clock", g_clock);
  RUN("core", g_core);
  RUN("str", g_str);
  RUN("malloc", g_malloc);
  RUN("kernel", g_kernel);
  RUN("net", g_net);
  RUN("wake", g_wake);
  RUN("real", g_real);
  RUN("mem", g_mem);
  RUN("stream", g_stream);
  RUN("ebs", g_ebs);
  RUN("steal", g_steal);
  return 0;
}
