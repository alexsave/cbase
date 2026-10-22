// metrics.h - the machinery of a process's own numbers, for every app on the platform and the
// platform itself (metrics.h one level up is the registry that names them and serves them):
//
//   cb_hist     a fixed log-scale histogram, four buckets per octave of an unsigned value
//               (microseconds, bytes), so a percentile is read in one pass with no sorting and
//               no allocation, and the max beside it
//   cb_rate     a per-second ring: events in each of the last CB_RATE_SLOTS seconds, so a rate
//               over the last whole seconds is read without a clock thread
//   cb_counter  one relaxed atomic word
//   cb_rss_mb, cb_cpu_us   a process's resident set and CPU time, by pid
//
// Every writer is a relaxed atomic: worker threads, the loop and a timer all add, and the
// report reads once a second. A read that lands between two adds of one sample sees one
// sample's count a step early, which is a display rounding, not a fact anyone acts on. Nothing
// here allocates or locks.
//
// Two apps each wrote this for themselves before it was lifted; this is the one copy.
#ifndef CB_UTIL_METRICS_H
#define CB_UTIL_METRICS_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

// ---- the histogram -------------------------------------------------------------------------
// Bucket b holds the values in [2^(b/4) * (1 + (b%4)/4), the next bucket's floor): 1, 1.25, 1.5,
// 1.75, 2, 2.5, 3, 3.5, 4, ... (0 and 1 share bucket 0, whose floor reads as 0; integers under
// 4 land exactly). 128 buckets reach 2^32: a value past 2^32 - 1 is counted in the last one.
#define CB_HIST_BUCKETS 128

typedef struct {
    atomic_ullong b[CB_HIST_BUCKETS];
    atomic_ullong n;     // samples ever
    atomic_ullong max;   // the largest sample ever
} cb_hist;

void     cb_hist_add(cb_hist *h, uint64_t v);
// The bucket a value lands in (0..CB_HIST_BUCKETS - 1), and a bucket's floor: the two sides of
// the same table, for a reader that wants the bands themselves.
int      cb_hist_bucket(uint64_t v);
uint32_t cb_hist_floor(int bucket);
// The value at the given percentile (0..100; clamped): the floor of the bucket the pct-th
// sample falls in (so a p99 reads a little under the true value, never over its bucket's top).
// 0 for an empty histogram.
uint32_t cb_hist_pct(const cb_hist *h, double pct);
uint64_t cb_hist_max(const cb_hist *h);
uint64_t cb_hist_count(const cb_hist *h);
// The bucket counts and n and max, at once, as of now (each a relaxed load: the counts can be
// off by the samples added meanwhile). For a report that wants the whole shape.
void     cb_hist_read(const cb_hist *h, uint64_t out[CB_HIST_BUCKETS], uint64_t *n, uint64_t *max);

// ---- the rate ring -------------------------------------------------------------------------
#define CB_RATE_SLOTS 10   // seconds the ring remembers: this one, and the nine before it

typedef struct {
    atomic_ullong count[CB_RATE_SLOTS];
    atomic_llong  sec[CB_RATE_SLOTS];   // the second (now_ms / 1000) each slot counts; 0 never written
    atomic_ullong total;                // events ever
} cb_rate;

// Counts n events at now_ms. A slot whose second has passed is taken over by the first writer of
// the new second (one count at the boundary may be lost to the reset: a rounding).
void  cb_rate_add(cb_rate *r, int64_t now_ms, uint64_t n);
// Events per second over the last CB_RATE_SLOTS - 1 WHOLE seconds (the current, partial second
// is left out; a second nobody wrote counts as zero), as of now_ms.
float cb_rate_per_s(const cb_rate *r, int64_t now_ms);
uint64_t cb_rate_total(const cb_rate *r);

// ---- the counter ---------------------------------------------------------------------------
typedef atomic_ullong cb_counter;
static inline void     cb_counter_add(cb_counter *c, uint64_t n) { atomic_fetch_add_explicit(c, n, memory_order_relaxed); }
static inline void     cb_counter_sub(cb_counter *c, uint64_t n) { atomic_fetch_sub_explicit(c, n, memory_order_relaxed); }
static inline void     cb_counter_set(cb_counter *c, uint64_t v) { atomic_store_explicit(c, v, memory_order_relaxed); }
static inline uint64_t cb_counter_get(const cb_counter *c) { return atomic_load_explicit((cb_counter *)c, memory_order_relaxed); }

// ---- the process ---------------------------------------------------------------------------
// A process's resident set now, in MB, and its CPU time so far (user + system), in microseconds:
// pid 0 is this process; 0 when it cannot be read (no such process, or no right to ask).
uint32_t cb_rss_mb(int pid);
uint64_t cb_cpu_us(int pid);

#endif
