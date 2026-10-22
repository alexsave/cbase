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
    TEST("hist: eight buckets per octave");
    /* 1 (and 0), then 2, 2.25, ..., 4, 4.5, ..., 8, 9, 10, ... */
    CHECK(stat_hist_bucket(0) == 0, "0");
    CHECK(stat_hist_bucket(1) == 0, "1");
    CHECK(stat_hist_bucket(2) == 8, "2");
    CHECK(stat_hist_bucket(3) == 12, "3");
    CHECK(stat_hist_bucket(4) == 16, "4");
    CHECK(stat_hist_bucket(5) == 18, "5");
    CHECK(stat_hist_bucket(7) == 22, "7");
    CHECK(stat_hist_bucket(8) == 24, "8");
    CHECK(stat_hist_bucket(9) == 25, "9");
    CHECK(stat_hist_bucket(1000) == 79, "1000 (2^9 * 1.875 = 960 <= 1000 < 1024)");
    CHECK(stat_hist_bucket(1ull << 31) == 248, "2^31");
    CHECK(stat_hist_bucket(UINT64_MAX) == STAT_HIST_BUCKETS - 1, "the largest value lands in the last bucket");
    TEST("hist: floors");
    CHECK(stat_hist_floor(0) == 0, "bucket 0 holds 0 and 1: its floor is 0");
    CHECK(stat_hist_floor(1) == 1, "bucket 1");
    CHECK(stat_hist_floor(8) == 2, "bucket 8");
    CHECK(stat_hist_floor(16) == 4, "bucket 16");
    CHECK(stat_hist_floor(18) == 5, "bucket 18");
    CHECK(stat_hist_floor(79) == 960, "bucket 79");
    CHECK(stat_hist_floor(STAT_HIST_BUCKETS - 1) == 4026531840u, "the last bucket: 2^31 * 1.875");
    CHECK(stat_hist_floor(-5) == 0, "a bucket below 0 reads as 0");
    CHECK(stat_hist_floor(999) == UINT32_MAX, "a bucket past the end reads as the top");
    /* From 8 up every bucket's floor is an integer that lands in that bucket, and the floors
     * rise; below 8 the eighth steps round down and only never fall. */
    for (int b = 0; b < STAT_HIST_BUCKETS; b++) {
        uint32_t f = stat_hist_floor(b);
        if (b >= 24) {
            CHECK(stat_hist_bucket(f) == b, "a floor lands in its bucket");
            CHECK(f > stat_hist_floor(b - 1), "floors rise");
        } else if (b > 0) {
            CHECK(f >= stat_hist_floor(b - 1), "floors never fall");
        }
    }
}

/* The nearest-rank percentile of sorted v[0..n): the truth the estimate is held to. */
static uint64_t true_pct(const uint64_t *v, uint64_t n, double pct)
{
    uint64_t want = (uint64_t)((n * pct + 99.999999) / 100.0);
    if (want < 1) want = 1;
    if (want > n) want = n;
    return v[want - 1];
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

static void percentiles(void)
{
    TEST("hist: percentiles and the max");
    StatHist h;
    memset(&h, 0, sizeof h);
    CHECK(stat_hist_pct(&h, 50) == 0, "empty: p50 0");
    CHECK(stat_hist_max(&h) == 0, "empty: max 0");
    CHECK(stat_hist_count(&h) == 0, "empty: count 0");
    /* 1..100 once each: the pct-th sample is pct itself (nearest rank), and the estimate places
     * it inside its bucket by rank: p50 = 50 in [48, 54): the 3rd of the 6 there, 48 + 6 * 2.5/6 = 50.5. */
    for (uint64_t v = 1; v <= 100; v++) stat_hist_add(&h, v);
    CHECK(stat_hist_count(&h) == 100, "100 samples");
    CHECK(stat_hist_max(&h) == 100, "max 100");
    CHECK(stat_hist_pct(&h, 50) == 51, "p50 of 1..100 reads 51 (50.5 rounded)");
    CHECK(stat_hist_pct(&h, 99) == 100, "p99 of 1..100 reads 100 (99 is the 4th of the 5 in [96, 104): 101.6, clamped to the max)");
    CHECK(stat_hist_pct(&h, 100) == 100, "p100 is the max exactly");
    CHECK(stat_hist_pct(&h, 0) == 1, "p0: the first sample, 1, alone in bucket 0 (floor 0, width 1): 0.5, rounded");
    CHECK(stat_hist_pct(&h, 2) == 2, "p2: the 2nd sample, 2, alone in its bucket");
    CHECK(stat_hist_pct(&h, 150) == 100, "pct over 100 is clamped to the max");
    CHECK(stat_hist_pct(&h, -1) == 1, "pct under 0 is clamped");
    /* One huge outlier moves the max and the p100, not the p99. */
    stat_hist_add(&h, 5000000);
    CHECK(stat_hist_max(&h) == 5000000, "the outlier is the max");
    CHECK(stat_hist_pct(&h, 100) == 5000000, "p100 is the outlier");
    CHECK(stat_hist_pct(&h, 99) == 103, "p99 moved a little: the 100th of 101 samples, 100, is the 5th of the 5 in [96, 104): 96 + 8 * 4.5/5 = 103.2, no max to clamp it now");
    /* A smaller sample never lowers the max. */
    stat_hist_add(&h, 3);
    CHECK(stat_hist_max(&h) == 5000000, "max stays");
    uint64_t counts[STAT_HIST_BUCKETS], n = 0, max = 0, sum = 0;
    stat_hist_read(&h, counts, &n, &max);
    CHECK(n == 102 && max == 5000000, "read: n and max");
    for (int b = 0; b < STAT_HIST_BUCKETS; b++) sum += counts[b];
    CHECK(sum == 102, "read: the counts sum to n");
    CHECK(counts[12] == 2, "3 twice, in bucket 12");
    stat_hist_read(&h, counts, NULL, NULL);
    TEST("hist: nothing but zeros reads 0");
    StatHist z;
    memset(&z, 0, sizeof z);
    stat_hist_add(&z, 0);
    stat_hist_add(&z, 0);
    CHECK(stat_hist_pct(&z, 99) == 0, "p99 of zeros is 0, not bucket 0's old floor of 1");
    CHECK(stat_hist_max(&z) == 0, "max 0");
    TEST("hist: the estimate is within the bucket of the truth, and the max bounds it");
    /* THE ERROR BOUND. Whatever the distribution, the estimate lies in the true sample's bucket,
     * [floor, floor * 2^(1/8)), and under the max: within 9.05% of the truth. A nasty shape: a
     * spike at one value with a long tail, and values crowding a bucket's top. */
    static uint64_t v[20000];
    uint64_t rng = 88172645463325252ull;
    uint64_t m = 0;
    StatHist w;
    memset(&w, 0, sizeof w);
    for (int i = 0; i < 20000; i++) {
        rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
        uint64_t x;
        if (i % 3 == 0) x = 777;                              /* the spike */
        else if (i % 3 == 1) x = 1000 + (rng % 4000000);       /* the tail, a value with a wide range */
        else x = 2047 - (rng % 4);                             /* the top of [1920, 2048) */
        v[m++] = x;
        stat_hist_add(&w, x);
    }
    qsort(v, m, sizeof *v, cmp_u64);
    double worst = 0;
    for (int p = 0; p <= 100; p++) {
        uint64_t truth = true_pct(v, m, p), est = stat_hist_pct(&w, p);
        uint64_t diff = est > truth ? est - truth : truth - est;
        double err = truth ? (double)diff / (double)truth : 0;
        if (err > worst) worst = err;
        CHECK(est <= stat_hist_max(&w), "never above the max");
        CHECK((double)diff <= 0.0906 * (double)truth + 1, "within 9.05%% of the nearest-rank truth (plus a half unit of rounding)");
    }
    CHECK(worst > 0.001, "the bound is exercised (some percentile is off by more than 0.1%%)");
    CHECK(stat_hist_pct(&w, 100) == v[m - 1], "p100 is the max");
    TEST("hist: a uniform spread is read to 1%");
    /* Values spread evenly across each bucket: the rank interpolation lands on the truth. */
    StatHist u;
    memset(&u, 0, sizeof u);
    m = 0;
    for (uint64_t x = 1000; x < 200000; x += 7) { v[m++] = x; stat_hist_add(&u, x); }
    for (int p = 1; p <= 100; p++) {
        uint64_t truth = true_pct(v, m, p), est = stat_hist_pct(&u, p);
        double err = (est > truth ? (double)(est - truth) : (double)(truth - est)) / (double)truth;
        CHECK(err <= 0.01, "within 1%% of the nearest-rank truth on a uniform spread");
    }
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
