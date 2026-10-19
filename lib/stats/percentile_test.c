/* percentile_test.c - nearest-rank percentiles (percentile.h). Every expected
 * rank is worked out by hand from rank = ceil(p/100 * n), clamped to 1..n. */
#include <stdlib.h>
#include "percentile.h"
#include "../test/check.h"

static int64_t seq_i[100];
static double  seq_d[100];

int main(void)
{
    for (int i = 0; i < 100; i++) { seq_i[i] = i + 1; seq_d[i] = (double)(i + 1); }

    TEST("percentile: nearest rank by hand");
    CHECK(stat_pct_i64(seq_i, 20, 95) == 19, "p95 of 20: ceil(19) = rank 19");
    CHECK(stat_pct_i64(seq_i, 21, 95) == 20, "p95 of 21: ceil(19.95) = rank 20");
    CHECK(stat_pct_i64(seq_i, 7, 50) == 4, "p50 of 7: ceil(3.5) = rank 4");
    CHECK(stat_pct_i64(seq_i, 100, 95) == 95, "p95 of 100 is rank 95, not 96");
    CHECK(stat_pct_i64(seq_i, 20, 35) == 7, "p35 of 20: ceil(7) = rank 7");
    CHECK(stat_pct_i64(seq_i, 20, 7) == 2, "p7 of 20: ceil(1.4) = rank 2");
    CHECK(stat_pct_i64(seq_i, 100, 7) == 7, "p7 of 100 is rank 7 (0.07 * 100 is 7.000000000000001: the 1e-9 slack keeps it off rank 8)");
    CHECK(stat_pct_i64(seq_i, 7, 100) == 7, "p100 of 7 is the maximum");
    CHECK(stat_pct_i64(seq_i, 20, 100) == 20, "p100 of 20 is the maximum");
    CHECK(stat_pct_i64(seq_i, 3, 1) == 1, "p1 of 3: ceil(0.03) = rank 1");
    CHECK(stat_pct_i64(seq_i, 3, 34) == 2, "p34 of 3: ceil(1.02) = rank 2");

    TEST("percentile: the ends and the empty case");
    CHECK(stat_pct_i64(seq_i, 20, 0) == 1, "p0 is the minimum");
    CHECK(stat_pct_i64(seq_i, 20, -5) == 1, "a negative p is the minimum");
    CHECK(stat_pct_i64(seq_i, 20, 250) == 20, "p over 100 is the maximum");
    CHECK(stat_pct_i64(seq_i, 0, 50) == 0, "an empty array is 0");
    CHECK(stat_pct_i64(seq_i, -1, 50) == 0, "a negative count is 0");
    CHECK(stat_pct_i64(seq_i, 1, 99) == 1, "one value is every percentile");
    CHECK(stat_rank_nearest(0, 50) == -1 && stat_rank_nearest(5, 0) == 0 && stat_rank_nearest(5, 100) == 4,
          "the rank index: -1 for none, 0-based otherwise");

    TEST("percentile: the double form is the same rank");
    CHECK(stat_pct_f64(seq_d, 20, 95) == 19.0, "f64 p95 of 20");
    CHECK(stat_pct_f64(seq_d, 21, 95) == 20.0, "f64 p95 of 21");
    CHECK(stat_pct_f64(seq_d, 7, 50) == 4.0, "f64 p50 of 7");
    CHECK(stat_pct_f64(seq_d, 100, 100) == 100.0, "f64 p100");
    CHECK(stat_pct_f64(seq_d, 0, 50) == 0.0, "f64 empty");

    TEST("percentile: the comparator sorts ascending, equal values together, negatives first");
    double v[] = { 3.5, -1, 2, 2, -7.25, 0, 1e9 };
    qsort(v, 7, sizeof v[0], stat_cmp_f64);
    const double want[] = { -7.25, -1, 0, 2, 2, 3.5, 1e9 };
    int sorted = 1;
    for (int i = 0; i < 7; i++) sorted &= v[i] == want[i];
    CHECK(sorted, "sorted");
    const double lo = 1, hi = 2;
    CHECK(stat_cmp_f64(&lo, &hi) < 0 && stat_cmp_f64(&hi, &lo) > 0 && stat_cmp_f64(&lo, &lo) == 0, "the three answers");
    return report("percentile_test");
}
