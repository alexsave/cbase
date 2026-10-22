/* hist.h - running statistics that keep no samples: a fixed log-scale histogram and a
 * per-second rate ring, for a process's own numbers (a server's latencies, a simulator's
 * throughput, a load generator's round trips).
 *
 *   StatHist  four buckets per octave of an unsigned value (microseconds, bytes), so a
 *             percentile is read in one pass with no sorting and no allocation, and the max
 *             beside it
 *   StatRate  events in each of the last STAT_RATE_SLOTS seconds, so a rate over the last
 *             whole seconds is read without a clock thread
 *
 * Every writer is a relaxed atomic: worker threads, a loop and a timer all add, and a reader
 * reads once a second. A read that lands between two adds of one sample sees one sample's
 * count a step early, which is a display rounding, not a fact anyone acts on. Nothing here
 * allocates or locks. Every number is a plain integer; no libm.
 *
 * Two servers on c_base each wrote this for themselves before it was lifted; this is the one
 * copy, and c_base's metrics registry builds on it. */
#ifndef SHARED_STATS_HIST_H
#define SHARED_STATS_HIST_H

#include <stdatomic.h>
#include <stdint.h>

/* Bucket b holds the values in [2^(b/4) * (1 + (b%4)/4), the next bucket's floor): 1, 1.25,
 * 1.5, 1.75, 2, 2.5, 3, 3.5, 4, ... (0 and 1 share bucket 0, whose floor reads as 0; integers
 * under 4 land exactly). 128 buckets reach 2^32: a value past 2^32 - 1 is counted in the last
 * one. */
#define STAT_HIST_BUCKETS 128

typedef struct {
    atomic_ullong b[STAT_HIST_BUCKETS];
    atomic_ullong n;     /* samples ever */
    atomic_ullong max;   /* the largest sample ever */
} StatHist;

void     stat_hist_add(StatHist *h, uint64_t v);
/* The bucket a value lands in (0..STAT_HIST_BUCKETS - 1), and a bucket's floor: the two sides
 * of the same table, for a reader that wants the bands themselves. */
int      stat_hist_bucket(uint64_t v);
uint32_t stat_hist_floor(int bucket);
/* The value at the given percentile (0..100; clamped): the floor of the bucket the pct-th
 * sample falls in (so a p99 reads a little under the true value, never over its bucket's top).
 * 0 for an empty histogram. */
uint32_t stat_hist_pct(const StatHist *h, double pct);
uint64_t stat_hist_max(const StatHist *h);
uint64_t stat_hist_count(const StatHist *h);
/* The bucket counts and n and max, at once, as of now (each a relaxed load: the counts can be
 * off by the samples added meanwhile). For a report that wants the whole shape. n and max may
 * be NULL. */
void     stat_hist_read(const StatHist *h, uint64_t out[STAT_HIST_BUCKETS], uint64_t *n, uint64_t *max);

#define STAT_RATE_SLOTS 10   /* seconds the ring remembers: this one, and the nine before it */

typedef struct {
    atomic_ullong count[STAT_RATE_SLOTS];
    atomic_llong  sec[STAT_RATE_SLOTS];   /* the second (now_ms / 1000) each slot counts; 0 never written */
    atomic_ullong total;                  /* events ever */
} StatRate;

/* Counts n events at now_ms. A slot whose second has passed is taken over by the first writer
 * of the new second (one count at the boundary may be lost to the reset: a rounding). A second
 * before the epoch has a slot too (a test's small clock). */
void     stat_rate_add(StatRate *r, int64_t now_ms, uint64_t n);
/* Events per second over the last STAT_RATE_SLOTS - 1 WHOLE seconds (the current, partial
 * second is left out; a second nobody wrote counts as zero), as of now_ms. */
float    stat_rate_per_s(const StatRate *r, int64_t now_ms);
uint64_t stat_rate_total(const StatRate *r);

#endif
