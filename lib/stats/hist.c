/* hist.c - hist.h. */
#include "hist.h"

#define PER STAT_HIST_PER_OCTAVE   /* 8: a power of two, so the arithmetic below is shifts */

int stat_hist_bucket(uint64_t v)
{
    if (v < 2) return 0;
    int lg = 63 - __builtin_clzll(v);                        /* floor(log2 v) */
    uint64_t frac = (v - (1ull << lg)) * PER / (1ull << lg); /* 0..PER-1 within the octave */
    int b = lg * PER + (int)frac;
    return b >= STAT_HIST_BUCKETS ? STAT_HIST_BUCKETS - 1 : b;
}

/* The floor of bucket b as a 64-bit value (the last bucket's top is 2^32): 2^lg * (1 + frac/PER)
 * = 2^(lg-3) * (PER + frac), exact in integers from lg 3 up. */
static uint64_t floor64(int b)
{
    if (b <= 0) return 0;   /* bucket 0 holds 0 and 1: its floor is 0 (a p99 over nothing but zeros is 0, not 1) */
    if (b >= STAT_HIST_BUCKETS) return 1ull << 32;
    int lg = b / PER, frac = b % PER;
    return lg >= 3 ? ((uint64_t)(PER + frac) << (lg - 3)) : ((uint64_t)(PER + frac) >> (3 - lg));
}

uint32_t stat_hist_floor(int b)
{
    uint64_t v = floor64(b);
    return v > UINT32_MAX ? UINT32_MAX : (uint32_t)v;
}

void stat_hist_add(StatHist *h, uint64_t v)
{
    atomic_fetch_add_explicit(&h->b[stat_hist_bucket(v)], 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&h->n, 1, memory_order_relaxed);
    uint64_t m = atomic_load_explicit(&h->max, memory_order_relaxed);
    while (v > m && !atomic_compare_exchange_weak_explicit(&h->max, &m, v, memory_order_relaxed, memory_order_relaxed)) {}
}

uint32_t stat_hist_pct(const StatHist *h, double pct)
{
    uint64_t n = atomic_load_explicit((atomic_ullong *)&h->n, memory_order_relaxed);
    if (!n) return 0;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    /* The pct-th sample, 1-based, rounded up: p50 of 3 samples is the 2nd, p99 of 100 the 99th. */
    uint64_t want = (uint64_t)((n * pct + 99.999999) / 100.0);
    if (want < 1) want = 1;
    uint64_t max = atomic_load_explicit((atomic_ullong *)&h->max, memory_order_relaxed);
    if (want >= n) return max > UINT32_MAX ? UINT32_MAX : (uint32_t)max;   /* p100: the max itself */
    uint64_t acc = 0;
    for (int b = 0; b < STAT_HIST_BUCKETS; b++) {
        uint64_t c = atomic_load_explicit((atomic_ullong *)&h->b[b], memory_order_relaxed);
        if (acc + c < want) { acc += c; continue; }
        /* The want-th sample is the k-th of this bucket's c: place it across the bucket's width
         * by rank, (k - 0.5) / c of the way from the floor to the next floor, and never past the
         * max seen (the top bucket's width is a guess the max corrects). */
        uint64_t k = want - acc;
        double lo = (double)floor64(b), hi = (double)floor64(b + 1);
        /* The bucket holding the max ends at the max, not at its own top: the samples cannot
         * spread past it, and a bucket a tenth full would otherwise read almost a bucket high. */
        if ((double)max + 1 < hi) hi = (double)max + 1;
        double v = lo + (hi - lo) * ((double)k - 0.5) / (double)c;
        if (v > (double)max) v = (double)max;
        if (v < lo) v = lo;
        return v > 4294967295.0 ? UINT32_MAX : (uint32_t)(v + 0.5);
    }
    return max > UINT32_MAX ? UINT32_MAX : (uint32_t)max;
}

uint64_t stat_hist_max(const StatHist *h) { return atomic_load_explicit((atomic_ullong *)&h->max, memory_order_relaxed); }
uint64_t stat_hist_count(const StatHist *h) { return atomic_load_explicit((atomic_ullong *)&h->n, memory_order_relaxed); }

void stat_hist_read(const StatHist *h, uint64_t out[STAT_HIST_BUCKETS], uint64_t *n, uint64_t *max)
{
    for (int b = 0; b < STAT_HIST_BUCKETS; b++) out[b] = atomic_load_explicit((atomic_ullong *)&h->b[b], memory_order_relaxed);
    if (n) *n = stat_hist_count(h);
    if (max) *max = stat_hist_max(h);
}

/* The slot of a second: a true modulo, so a second before the epoch (a test's small clock, a
 * clock set wrong) lands in a slot and not before the array (the fuzzer found the plain '%'). */
static int slot_of(int64_t s)
{
    int64_t i = s % STAT_RATE_SLOTS;
    return (int)(i < 0 ? i + STAT_RATE_SLOTS : i);
}

void stat_rate_add(StatRate *r, int64_t now_ms, uint64_t n)
{
    int64_t s = now_ms / 1000;
    int i = slot_of(s);
    int64_t have = atomic_load_explicit(&r->sec[i], memory_order_relaxed);
    if (have != s) {
        /* A new second in this slot: whoever gets here first resets it. A count another thread
         * adds between the exchange and the store is lost: a rounding at the boundary. */
        if (atomic_compare_exchange_strong_explicit(&r->sec[i], &have, s, memory_order_relaxed, memory_order_relaxed))
            atomic_store_explicit(&r->count[i], 0, memory_order_relaxed);
    }
    atomic_fetch_add_explicit(&r->count[i], n, memory_order_relaxed);
    atomic_fetch_add_explicit(&r->total, n, memory_order_relaxed);
}

float stat_rate_per_s(const StatRate *r, int64_t now_ms)
{
    int64_t s = now_ms / 1000;
    uint64_t total = 0;
    for (int k = 1; k < STAT_RATE_SLOTS; k++) {
        int64_t want = s - k;
        int i = slot_of(want);
        if (atomic_load_explicit((atomic_llong *)&r->sec[i], memory_order_relaxed) == want)
            total += atomic_load_explicit((atomic_ullong *)&r->count[i], memory_order_relaxed);
    }
    return (float)total / (float)(STAT_RATE_SLOTS - 1);
}

uint64_t stat_rate_total(const StatRate *r) { return atomic_load_explicit((atomic_ullong *)&r->total, memory_order_relaxed); }
