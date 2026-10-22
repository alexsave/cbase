/* hist_test.c - hist.h: the bucket table, percentiles and the max, four threads adding at
 * once, and the rate ring across seconds, slot reuse and a clock before the epoch.
 *
 * Exits 1 on any failure. */
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../test/check.h"
#include "hist.h"

static void buckets(void)
{
    TEST("hist: four buckets per octave");
    /* 1 (and 0), then 2, 2.5, 3, 3.5, 4, 5, 6, 7, 8, ... */
    CHECK(stat_hist_bucket(0) == 0, "0");
    CHECK(stat_hist_bucket(1) == 0, "1");
    CHECK(stat_hist_bucket(2) == 4, "2");
    CHECK(stat_hist_bucket(3) == 6, "3");
    CHECK(stat_hist_bucket(4) == 8, "4");
    CHECK(stat_hist_bucket(5) == 9, "5");
    CHECK(stat_hist_bucket(7) == 11, "7");
    CHECK(stat_hist_bucket(8) == 12, "8");
    CHECK(stat_hist_bucket(1000) == 39, "1000 (2^9 * 1.75 = 896 <= 1000 < 1024)");
    CHECK(stat_hist_bucket(1ull << 31) == 124, "2^31");
    CHECK(stat_hist_bucket(UINT64_MAX) == STAT_HIST_BUCKETS - 1, "the largest value lands in the last bucket");
    TEST("hist: floors");
    CHECK(stat_hist_floor(0) == 0, "bucket 0 holds 0 and 1: its floor is 0");
    CHECK(stat_hist_floor(1) == 1, "bucket 1");
    CHECK(stat_hist_floor(4) == 2, "bucket 4");
    CHECK(stat_hist_floor(8) == 4, "bucket 8");
    CHECK(stat_hist_floor(9) == 5, "bucket 9");
    CHECK(stat_hist_floor(39) == 896, "bucket 39");
    CHECK(stat_hist_floor(STAT_HIST_BUCKETS - 1) == 3758096384u, "the last bucket: 2^31 * 1.75");
    CHECK(stat_hist_floor(-5) == 0, "a bucket below 0 reads as 0");
    CHECK(stat_hist_floor(999) == 3758096384u, "a bucket past the end reads as the last");
    /* From 4 up every bucket's floor is an integer that lands in that bucket, and the floors
     * rise; below 4 the quarter steps round down (1.25 -> 1, 2.5 -> 2) and only never fall. */
    for (int b = 0; b < STAT_HIST_BUCKETS; b++) {
        uint32_t f = stat_hist_floor(b);
        if (b >= 8) {
            CHECK(stat_hist_bucket(f) == b, "a floor lands in its bucket");
            CHECK(f > stat_hist_floor(b - 1), "floors rise");
        } else if (b > 0) {
            CHECK(f >= stat_hist_floor(b - 1), "floors never fall");
        }
    }
}

static void percentiles(void)
{
    TEST("hist: percentiles and the max");
    StatHist h;
    memset(&h, 0, sizeof h);
    CHECK(stat_hist_pct(&h, 50) == 0, "empty: p50 0");
    CHECK(stat_hist_max(&h) == 0, "empty: max 0");
    CHECK(stat_hist_count(&h) == 0, "empty: count 0");
    /* 1..100 once each: p50 is the 50th sample (50, in bucket [48, 56)), p99 the 99th (99 in
     * [96, 112)), p100 the 100th, p0 the first. */
    for (uint64_t v = 1; v <= 100; v++) stat_hist_add(&h, v);
    CHECK(stat_hist_count(&h) == 100, "100 samples");
    CHECK(stat_hist_max(&h) == 100, "max 100");
    CHECK(stat_hist_pct(&h, 50) == 48, "p50 of 1..100 is bucket [48, 56)");
    CHECK(stat_hist_pct(&h, 99) == 96, "p99 of 1..100 is bucket [96, 112)");
    CHECK(stat_hist_pct(&h, 100) == 96, "p100 too");
    CHECK(stat_hist_pct(&h, 0) == 0, "p0: the first sample, 1, in bucket 0");
    CHECK(stat_hist_pct(&h, 1) == 0, "p1 too");
    CHECK(stat_hist_pct(&h, 2) == 2, "p2: the 2nd sample");
    CHECK(stat_hist_pct(&h, 150) == 96, "pct over 100 is clamped");
    CHECK(stat_hist_pct(&h, -1) == 0, "pct under 0 is clamped");
    /* One huge outlier moves the max and the p100, not the p99. */
    stat_hist_add(&h, 5000000);
    CHECK(stat_hist_max(&h) == 5000000, "the outlier is the max");
    CHECK(stat_hist_pct(&h, 100) == stat_hist_floor(stat_hist_bucket(5000000)), "p100 is the outlier's bucket");
    CHECK(stat_hist_pct(&h, 99) == 96, "p99 unmoved");
    /* A smaller sample never lowers the max. */
    stat_hist_add(&h, 3);
    CHECK(stat_hist_max(&h) == 5000000, "max stays");
    uint64_t counts[STAT_HIST_BUCKETS], n = 0, max = 0, sum = 0;
    stat_hist_read(&h, counts, &n, &max);
    CHECK(n == 102 && max == 5000000, "read: n and max");
    for (int b = 0; b < STAT_HIST_BUCKETS; b++) sum += counts[b];
    CHECK(sum == 102, "read: the counts sum to n");
    CHECK(counts[6] == 2, "3 twice, in bucket 6");
    stat_hist_read(&h, counts, NULL, NULL);
    TEST("hist: nothing but zeros reads 0");
    StatHist z;
    memset(&z, 0, sizeof z);
    stat_hist_add(&z, 0);
    stat_hist_add(&z, 0);
    CHECK(stat_hist_pct(&z, 99) == 0, "p99 of zeros is 0, not bucket 0's old floor of 1");
    CHECK(stat_hist_max(&z) == 0, "max 0");
}

typedef struct { StatHist *h; } job;
static void *adder(void *ud)
{
    job *j = ud;
    for (int i = 0; i < 20000; i++) stat_hist_add(j->h, (uint64_t)(i % 1000) + 1);
    return NULL;
}

static void threads(void)
{
    TEST("hist: four threads adding");
    StatHist *h = calloc(1, sizeof *h);
    pthread_t th[4];
    job j[4];
    for (int k = 0; k < 4; k++) { j[k].h = h; pthread_create(&th[k], NULL, adder, &j[k]); }
    for (int k = 0; k < 4; k++) pthread_join(th[k], NULL);
    CHECK(stat_hist_count(h) == 80000, "every sample counted");
    CHECK(stat_hist_max(h) == 1000, "the max right");
    uint64_t counts[STAT_HIST_BUCKETS], sum = 0;
    stat_hist_read(h, counts, NULL, NULL);
    for (int b = 0; b < STAT_HIST_BUCKETS; b++) sum += counts[b];
    CHECK(sum == 80000, "every sample in a bucket");
    free(h);
}

static void rate(void)
{
    TEST("rate: the last whole seconds");
    StatRate r;
    memset(&r, 0, sizeof r);
    CHECK(stat_rate_per_s(&r, 1000000) == 0, "empty: 0");
    CHECK(stat_rate_total(&r) == 0, "empty: total 0");
    /* 9 events in each of seconds 100..108, read at second 109: 9/s over the last 9 whole seconds. */
    for (int64_t s = 100; s <= 108; s++) stat_rate_add(&r, s * 1000 + 500, 9);
    CHECK(stat_rate_total(&r) == 81, "total 81");
    CHECK(fabs(stat_rate_per_s(&r, 109000) - 9.0) < 1e-6, "9/s");
    /* The current, partial second is left out: 1000 events now change nothing. */
    stat_rate_add(&r, 109200, 1000);
    CHECK(fabs(stat_rate_per_s(&r, 109900) - 9.0) < 1e-6, "the partial second is left out");
    /* One second later they count and the oldest second (100) has left the window: (8*9+1000)/9. */
    CHECK(fabs(stat_rate_per_s(&r, 110000) - (72.0 + 1000.0) / 9.0) < 1e-4, "a second later they count");
    TEST("rate: stale slots are not counted");
    CHECK(stat_rate_per_s(&r, 125000) == 0, "ten seconds of silence: 0");
    /* A slot is taken over by the new second that maps to it (119 reuses 109's slot). */
    stat_rate_add(&r, 119000, 5);
    CHECK(fabs(stat_rate_per_s(&r, 120000) - 5.0 / 9.0) < 1e-6, "a reused slot was reset");
    stat_rate_add(&r, 119000, 5);
    CHECK(fabs(stat_rate_per_s(&r, 120000) - 10.0 / 9.0) < 1e-6, "and then counts");
    CHECK(stat_rate_total(&r) == 1091, "total 1091");
    TEST("rate: a clock near or before the epoch");
    /* The seconds before the epoch have a slot to read, not one before the array (the fuzzer's
     * find). Second -2 lives in slot 8 (a true modulo); a plain '%' would put it two words
     * before the array, which happens to be inside the struct and reads back consistently, so
     * the slot is checked by name. */
    memset(&r, 0, sizeof r);
    stat_rate_add(&r, -2500, 4);
    CHECK(atomic_load(&r.sec[8]) == -2, "second -2 is slot 8");
    CHECK(atomic_load(&r.count[8]) == 4, "and holds its count");
    stat_rate_add(&r, 500, 6);
    CHECK(fabs(stat_rate_per_s(&r, 1000) - 10.0 / 9.0) < 1e-6, "both seconds within the window of second 1");
    CHECK(fabs(stat_rate_per_s(&r, 3) - 4.0 / 9.0) < 1e-6, "second -2 is within the last nine of second 0");
    CHECK(fabs(stat_rate_per_s(&r, -1000) - 4.0 / 9.0) < 1e-6, "read at second -1");
    CHECK(stat_rate_per_s(&r, -3000) == 0, "read at second -3: nothing before it");
}

int main(void)
{
    buckets();
    percentiles();
    threads();
    rate();
    return report("hist");
}
