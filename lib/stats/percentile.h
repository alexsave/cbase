/* percentile.h - the nearest-rank percentile of a SORTED array, header-only and
 * libm-free, and the comparator that sorts the doubles for it.
 *
 * NEAREST RANK: the smallest value with at least p percent of the values at or
 * below it, sorted[ceil(p/100 * n) - 1]. It is always a value that was
 * measured (no interpolation), p <= 0 is the minimum and p >= 100 the maximum,
 * and an empty array is 0. The product is rounded away from a 1e-9 error so
 * that 95% of 100 is rank 95, not 96.
 *
 * A bench that wants another convention (a floor index, an interpolated
 * median) names it; this is the one the report tables use. */
#ifndef SHARED_STATS_PERCENTILE_H
#define SHARED_STATS_PERCENTILE_H

#include <stdint.h>

static inline int stat_rank_nearest(int n, double p)
{
    if (n <= 0) return -1;
    if (p <= 0) return 0;
    const double r = p / 100.0 * n;
    long rank = (long)r;
    if (r - (double)rank > 1e-9) rank++;
    if (rank < 1) rank = 1;
    if (rank > n) rank = n;
    return (int)(rank - 1);
}

static inline int64_t stat_pct_i64(const int64_t *sorted, int n, double p)
{
    const int i = stat_rank_nearest(n, p);
    return i < 0 ? 0 : sorted[i];
}

static inline double stat_pct_f64(const double *sorted, int n, double p)
{
    const int i = stat_rank_nearest(n, p);
    return i < 0 ? 0.0 : sorted[i];
}

/* qsort comparator for doubles, ascending. */
static inline int stat_cmp_f64(const void *a, const void *b)
{
    const double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

#endif
