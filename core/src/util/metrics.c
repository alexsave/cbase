// metrics.c - util/metrics.h.
#include "metrics.h"

#include "../../../../shared/c/num_parse.h"

#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>
#ifdef __APPLE__
#include <libproc.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#endif

// ---- the histogram -------------------------------------------------------------------------

int cb_hist_bucket(uint64_t v) {
    if (v < 2) return 0;
    int lg = 63 - __builtin_clzll(v);                      // floor(log2 v)
    uint64_t frac = (v - (1ull << lg)) * 4 / (1ull << lg); // 0..3 within the octave
    int b = lg * 4 + (int)frac;
    return b >= CB_HIST_BUCKETS ? CB_HIST_BUCKETS - 1 : b;
}

uint32_t cb_hist_floor(int b) {
    if (b <= 0) return 0;   // bucket 0 holds 0 and 1: its floor is 0 (the fuzzer: a p99 of 1 over nothing but zeros)
    if (b >= CB_HIST_BUCKETS) b = CB_HIST_BUCKETS - 1;
    int lg = b / 4, frac = b % 4;
    // 2^lg * (1 + frac/4) = 2^(lg-2) * (4 + frac), exact in integers from lg 2 up.
    uint64_t v = lg >= 2 ? ((uint64_t)(4 + frac) << (lg - 2)) : ((uint64_t)(4 + frac) >> (2 - lg));
    return v > UINT32_MAX ? UINT32_MAX : (uint32_t)v;
}

void cb_hist_add(cb_hist *h, uint64_t v) {
    atomic_fetch_add_explicit(&h->b[cb_hist_bucket(v)], 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&h->n, 1, memory_order_relaxed);
    uint64_t m = atomic_load_explicit(&h->max, memory_order_relaxed);
    while (v > m && !atomic_compare_exchange_weak_explicit(&h->max, &m, v, memory_order_relaxed, memory_order_relaxed)) {}
}

uint32_t cb_hist_pct(const cb_hist *h, double pct) {
    uint64_t n = atomic_load_explicit((atomic_ullong *)&h->n, memory_order_relaxed);
    if (!n) return 0;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    // The pct-th sample, 1-based, rounded up: p50 of 3 samples is the 2nd, p99 of 100 the 99th.
    uint64_t want = (uint64_t)((n * pct + 99.999999) / 100.0);
    if (want < 1) want = 1;
    if (want > n) want = n;
    uint64_t acc = 0;
    for (int b = 0; b < CB_HIST_BUCKETS; b++) {
        acc += atomic_load_explicit((atomic_ullong *)&h->b[b], memory_order_relaxed);
        if (acc >= want) return cb_hist_floor(b);
    }
    return cb_hist_floor(CB_HIST_BUCKETS - 1);
}

uint64_t cb_hist_max(const cb_hist *h) { return atomic_load_explicit((atomic_ullong *)&h->max, memory_order_relaxed); }
uint64_t cb_hist_count(const cb_hist *h) { return atomic_load_explicit((atomic_ullong *)&h->n, memory_order_relaxed); }

void cb_hist_read(const cb_hist *h, uint64_t out[CB_HIST_BUCKETS], uint64_t *n, uint64_t *max) {
    for (int b = 0; b < CB_HIST_BUCKETS; b++) out[b] = atomic_load_explicit((atomic_ullong *)&h->b[b], memory_order_relaxed);
    if (n) *n = cb_hist_count(h);
    if (max) *max = cb_hist_max(h);
}

// ---- the rate ring -------------------------------------------------------------------------

// The slot of a second: a true modulo, so a second before the epoch (a test's small clock, a
// clock set wrong) lands in a slot and not before the array (the fuzzer found the plain '%').
static int slot_of(int64_t s) {
    int64_t i = s % CB_RATE_SLOTS;
    return (int)(i < 0 ? i + CB_RATE_SLOTS : i);
}

void cb_rate_add(cb_rate *r, int64_t now_ms, uint64_t n) {
    int64_t s = now_ms / 1000;
    int i = slot_of(s);
    int64_t have = atomic_load_explicit(&r->sec[i], memory_order_relaxed);
    if (have != s) {
        // A new second in this slot: whoever gets here first resets it. A count another thread
        // adds between the exchange and the store is lost: a rounding at the boundary.
        if (atomic_compare_exchange_strong_explicit(&r->sec[i], &have, s, memory_order_relaxed, memory_order_relaxed))
            atomic_store_explicit(&r->count[i], 0, memory_order_relaxed);
    }
    atomic_fetch_add_explicit(&r->count[i], n, memory_order_relaxed);
    atomic_fetch_add_explicit(&r->total, n, memory_order_relaxed);
}

float cb_rate_per_s(const cb_rate *r, int64_t now_ms) {
    int64_t s = now_ms / 1000;
    uint64_t total = 0;
    for (int k = 1; k < CB_RATE_SLOTS; k++) {
        int64_t want = s - k;
        int i = slot_of(want);
        if (atomic_load_explicit((atomic_llong *)&r->sec[i], memory_order_relaxed) == want)
            total += atomic_load_explicit((atomic_ullong *)&r->count[i], memory_order_relaxed);
    }
    return (float)total / (float)(CB_RATE_SLOTS - 1);
}

uint64_t cb_rate_total(const cb_rate *r) { return atomic_load_explicit((atomic_ullong *)&r->total, memory_order_relaxed); }

// ---- the process ---------------------------------------------------------------------------

#ifndef __APPLE__
// The first cap-1 bytes of a small file (a /proc entry), NUL-terminated: how many were read.
static size_t read_small(const char *path, char *buf, size_t cap) {
    FILE *f = fopen(path, "r");
    if (!f) { buf[0] = 0; return 0; }
    size_t n = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[n] = 0;
    return n;
}

// Field `want` (0-based) of a space-separated line, as a non-negative integer; -1 when it is not
// there or not a number (shared/c/num_parse.h: the one number reader).
static int64_t field_i64(const char *s, size_t n, int want) {
    size_t i = 0;
    for (int f = 0;; f++) {
        while (i < n && s[i] == ' ') i++;
        size_t start = i;
        while (i < n && s[i] != ' ' && s[i] != '\n') i++;
        if (i == start) return -1;
        if (f == want) {
            int64_t v;
            return num_parse_i64(s + start, i - start, 0, INT64_MAX, &v) ? v : -1;
        }
    }
}

static const char *memrchr_c(const char *s, char c, size_t n) {
    while (n--) if (s[n] == c) return s + n;
    return NULL;
}
#endif

uint32_t cb_rss_mb(int pid) {
#ifdef __APPLE__
    if (pid == 0) {
        struct mach_task_basic_info info;
        mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
        if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info, &count) != KERN_SUCCESS) return 0;
        return (uint32_t)(info.resident_size >> 20);
    }
    struct proc_taskinfo ti;
    if (proc_pidinfo(pid, PROC_PIDTASKINFO, 0, &ti, sizeof ti) != (int)sizeof ti) return 0;
    return (uint32_t)(ti.pti_resident_size >> 20);
#else
    char path[64], buf[256];
    snprintf(path, sizeof path, "/proc/%d/statm", pid == 0 ? (int)getpid() : pid);
    size_t n = read_small(path, buf, sizeof buf);
    // "size resident shared ...": the second field, in pages.
    int64_t resident = field_i64(buf, n, 1);
    if (resident < 0) return 0;
    long page = sysconf(_SC_PAGESIZE);
    return (uint32_t)(((uint64_t)resident * (uint64_t)(page > 0 ? page : 4096)) >> 20);
#endif
}

uint64_t cb_cpu_us(int pid) {
#ifdef __APPLE__
    if (pid == 0) {
        struct rusage ru;
        if (getrusage(RUSAGE_SELF, &ru) != 0) return 0;
        return (uint64_t)ru.ru_utime.tv_sec * 1000000 + (uint64_t)ru.ru_utime.tv_usec + (uint64_t)ru.ru_stime.tv_sec * 1000000 +
               (uint64_t)ru.ru_stime.tv_usec;
    }
    struct proc_taskinfo ti;
    if (proc_pidinfo(pid, PROC_PIDTASKINFO, 0, &ti, sizeof ti) != (int)sizeof ti) return 0;
    // Mach absolute time: nanoseconds on Apple silicon (timebase 1/1); the timebase is applied
    // for an Intel Mac, where it is not.
    mach_timebase_info_data_t tb;
    if (mach_timebase_info(&tb) != KERN_SUCCESS || tb.denom == 0) return 0;
    uint64_t ns = (ti.pti_total_user + ti.pti_total_system) / tb.denom * tb.numer;
    return ns / 1000;
#else
    if (pid == 0) {
        struct rusage ru;
        if (getrusage(RUSAGE_SELF, &ru) != 0) return 0;
        return (uint64_t)ru.ru_utime.tv_sec * 1000000 + (uint64_t)ru.ru_utime.tv_usec + (uint64_t)ru.ru_stime.tv_sec * 1000000 +
               (uint64_t)ru.ru_stime.tv_usec;
    }
    char path[64], buf[1024];
    snprintf(path, sizeof path, "/proc/%d/stat", pid);
    size_t n = read_small(path, buf, sizeof buf);
    // "pid (comm) state ppid ...": the comm may hold spaces and parentheses, so the fields are
    // counted from the last ')'. After it: state ppid pgrp session tty tpgid flags minflt cminflt
    // majflt cmajflt utime stime: utime is the 12th field after the ')', stime the 13th.
    const char *p = n ? memrchr_c(buf, ')', n) : NULL;
    if (!p) return 0;
    size_t off = (size_t)(p + 1 - buf);
    int64_t ut = field_i64(buf + off, n - off, 11), st = field_i64(buf + off, n - off, 12);
    if (ut < 0 || st < 0) return 0;
    long hz = sysconf(_SC_CLK_TCK);
    return (uint64_t)(ut + st) * 1000000 / (uint64_t)(hz > 0 ? hz : 100);
#endif
}
