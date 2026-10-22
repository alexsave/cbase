// unit_metrics.c - util/metrics.h (the histogram, the rate ring, the counter, the process
// readers) and metrics.h (the registry: an app's table of named metrics, the report's fixed
// layout, and the platform's own rows in it).
#include "t.h"

#include "handler.h"
#include "metrics.h"
#include "net/http.h"
#include "net/router.h"
#include "util/clock.h"
#include "util/err.h"
#include "util/metrics.h"

#include <math.h>
#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// A check that says what it saw when it fails (t.h has the bare one).
#define T_CHECK_MSG(cond, ...) do { if (!(cond)) { fprintf(stderr, "  "); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } T_CHECK(cond); } while (0)

// ---- the histogram -------------------------------------------------------------------------

static void hist_buckets(void) {
    // Four per octave: 1 (and 0), then 2, 2.5, 3, 3.5, 4, 5, 6, 7, 8, ...
    T_EQ_INT(cb_hist_bucket(0), 0);
    T_EQ_INT(cb_hist_bucket(1), 0);
    T_EQ_INT(cb_hist_bucket(2), 4);
    T_EQ_INT(cb_hist_bucket(3), 6);
    T_EQ_INT(cb_hist_bucket(4), 8);
    T_EQ_INT(cb_hist_bucket(5), 9);
    T_EQ_INT(cb_hist_bucket(7), 11);
    T_EQ_INT(cb_hist_bucket(8), 12);
    T_EQ_INT(cb_hist_bucket(1000), 39);          // 2^9 * 1.75 = 896 <= 1000 < 1024
    T_EQ_INT(cb_hist_bucket(1ull << 31), 124);
    T_EQ_INT(cb_hist_bucket(UINT64_MAX), CB_HIST_BUCKETS - 1);
    // From 4 up every bucket's floor is an integer that lands in that bucket, and the floors
    // rise; below 4 the quarter steps round down (1.25 -> 1, 2.5 -> 2) and only never fall.
    for (int b = 0; b < CB_HIST_BUCKETS; b++) {
        uint32_t f = cb_hist_floor(b);
        if (b >= 8) {
            T_CHECK(cb_hist_bucket(f) == b);
            T_CHECK(f > cb_hist_floor(b - 1));
        } else if (b > 0) {
            T_CHECK(f >= cb_hist_floor(b - 1));
        }
    }
    T_EQ_INT(cb_hist_floor(0), 0);   // 0 and 1 share it: a histogram of zeros reads 0, not 1
    T_EQ_INT(cb_hist_floor(1), 1);
    T_EQ_INT(cb_hist_floor(4), 2);
    T_EQ_INT(cb_hist_floor(8), 4);
    T_EQ_INT(cb_hist_floor(9), 5);
    T_EQ_INT(cb_hist_floor(39), 896);
    T_EQ_INT(cb_hist_floor(CB_HIST_BUCKETS - 1), 3758096384u);   // 2^31 * 1.75
    T_EQ_INT(cb_hist_floor(-5), 0);
    T_EQ_INT(cb_hist_floor(999), 3758096384u);
}

static void hist_percentiles(void) {
    cb_hist h;
    memset(&h, 0, sizeof h);
    T_EQ_INT(cb_hist_pct(&h, 50), 0);
    T_EQ_INT(cb_hist_max(&h), 0);
    T_EQ_INT(cb_hist_count(&h), 0);
    // 1..100 once each: p50 is the 50th sample (50, in bucket [48, 56)), p99 the 99th (99 in
    // [96, 112)), p100 the 100th, p0 the first.
    for (uint64_t v = 1; v <= 100; v++) cb_hist_add(&h, v);
    T_EQ_INT(cb_hist_count(&h), 100);
    T_EQ_INT(cb_hist_max(&h), 100);
    T_EQ_INT(cb_hist_pct(&h, 50), 48);
    T_EQ_INT(cb_hist_pct(&h, 99), 96);
    T_EQ_INT(cb_hist_pct(&h, 100), 96);
    T_EQ_INT(cb_hist_pct(&h, 0), 0);    // the first sample, 1, is in bucket 0
    T_EQ_INT(cb_hist_pct(&h, 1), 0);
    T_EQ_INT(cb_hist_pct(&h, 2), 2);    // the 2nd sample
    T_EQ_INT(cb_hist_pct(&h, 150), 96);   // clamped
    T_EQ_INT(cb_hist_pct(&h, -1), 0);
    // Nothing but zeros: every percentile and the max read 0 (corpus/metrics/repro-hist-zero-floor).
    cb_hist z;
    memset(&z, 0, sizeof z);
    cb_hist_add(&z, 0);
    cb_hist_add(&z, 0);
    T_EQ_INT(cb_hist_pct(&z, 99), 0);
    T_EQ_INT(cb_hist_max(&z), 0);
    // One huge outlier moves the max and the p100, not the p99.
    cb_hist_add(&h, 5000000);
    T_EQ_INT(cb_hist_max(&h), 5000000);
    T_EQ_INT(cb_hist_pct(&h, 100), cb_hist_floor(cb_hist_bucket(5000000)));
    T_EQ_INT(cb_hist_pct(&h, 99), 96);
    // A smaller sample never lowers the max.
    cb_hist_add(&h, 3);
    T_EQ_INT(cb_hist_max(&h), 5000000);
    uint64_t counts[CB_HIST_BUCKETS], n = 0, max = 0;
    cb_hist_read(&h, counts, &n, &max);
    T_EQ_INT(n, 102);
    T_EQ_INT(max, 5000000);
    uint64_t sum = 0;
    for (int b = 0; b < CB_HIST_BUCKETS; b++) sum += counts[b];
    T_EQ_INT(sum, 102);
    T_EQ_INT(counts[6], 2);   // 3 twice
}

typedef struct { cb_hist *h; int k; } hist_job;
static void *hist_adder(void *ud) {
    hist_job *j = ud;
    for (int i = 0; i < 20000; i++) cb_hist_add(j->h, (uint64_t)(i % 1000) + 1);
    return NULL;
}

// Four threads adding: every sample counted, the max right.
static void hist_threads(void) {
    cb_hist *h = calloc(1, sizeof *h);
    pthread_t th[4];
    hist_job j[4];
    for (int k = 0; k < 4; k++) { j[k].h = h; j[k].k = k; pthread_create(&th[k], NULL, hist_adder, &j[k]); }
    for (int k = 0; k < 4; k++) pthread_join(th[k], NULL);
    T_EQ_INT(cb_hist_count(h), 80000);
    T_EQ_INT(cb_hist_max(h), 1000);
    uint64_t counts[CB_HIST_BUCKETS], sum = 0;
    cb_hist_read(h, counts, NULL, NULL);
    for (int b = 0; b < CB_HIST_BUCKETS; b++) sum += counts[b];
    T_EQ_INT(sum, 80000);
    free(h);
}

// ---- the rate ring -------------------------------------------------------------------------

static void rate_ring(void) {
    cb_rate r;
    memset(&r, 0, sizeof r);
    T_CHECK(cb_rate_per_s(&r, 1000000) == 0);
    T_EQ_INT(cb_rate_total(&r), 0);
    // 9 events in each of seconds 100..108, read at second 109: 9/s over the last 9 whole seconds.
    for (int64_t s = 100; s <= 108; s++) cb_rate_add(&r, s * 1000 + 500, 9);
    T_EQ_INT(cb_rate_total(&r), 81);
    T_CHECK(fabs(cb_rate_per_s(&r, 109000) - 9.0) < 1e-6);
    // The current, partial second is left out: 1000 events now change nothing.
    cb_rate_add(&r, 109200, 1000);
    T_CHECK(fabs(cb_rate_per_s(&r, 109900) - 9.0) < 1e-6);
    // One second later they count and the oldest second (100) has left the window: (8*9+1000)/9.
    T_CHECK(fabs(cb_rate_per_s(&r, 110000) - (72.0 + 1000.0) / 9.0) < 1e-4);
    // Ten seconds of silence: the slots still hold old seconds, which the reader must not count.
    T_CHECK(cb_rate_per_s(&r, 125000) == 0);
    // A slot is taken over by the new second that maps to it (119 reuses 109's slot).
    cb_rate_add(&r, 119000, 5);
    T_CHECK(fabs(cb_rate_per_s(&r, 120000) - 5.0 / 9.0) < 1e-6);
    cb_rate_add(&r, 119000, 5);
    T_CHECK(fabs(cb_rate_per_s(&r, 120000) - 10.0 / 9.0) < 1e-6);
    T_EQ_INT(cb_rate_total(&r), 1091);
    // A clock near or before the epoch (a test's, a wrong one): the seconds before it have no
    // slot to read out of bounds (the fuzzer's find: corpus/metrics/repro-rate-negative-second).
    memset(&r, 0, sizeof r);
    cb_rate_add(&r, -2500, 4);
    // Second -2 lives in slot 8 (a true modulo); a plain '%' would put it two words before the
    // array, which happens to be inside the struct and reads back consistently, so the slot is
    // checked by name.
    T_EQ_INT(atomic_load(&r.sec[8]), -2);
    T_EQ_INT(atomic_load(&r.count[8]), 4);
    cb_rate_add(&r, 500, 6);
    T_CHECK(fabs(cb_rate_per_s(&r, 1000) - 10.0 / 9.0) < 1e-6);
    T_CHECK(fabs(cb_rate_per_s(&r, 3) - 4.0 / 9.0) < 1e-6);   // second -2 is within the last nine of second 0
    T_CHECK(fabs(cb_rate_per_s(&r, -1000) - 4.0 / 9.0) < 1e-6);
    T_CHECK(cb_rate_per_s(&r, -3000) == 0);
}

// ---- the counter ---------------------------------------------------------------------------

static void counter(void) {
    cb_counter c = 0;
    cb_counter_add(&c, 5);
    cb_counter_add(&c, 7);
    T_EQ_INT(cb_counter_get(&c), 12);
    cb_counter_sub(&c, 2);
    T_EQ_INT(cb_counter_get(&c), 10);
    cb_counter_set(&c, 3);
    T_EQ_INT(cb_counter_get(&c), 3);
}

// ---- the process ---------------------------------------------------------------------------

static void process_readers(void) {
    uint32_t rss = cb_rss_mb(0);
    T_CHECK_MSG(rss >= 1 && rss < 4096, "rss %u MB", rss);
    // Our own pid by number reads the same process (within a few MB of churn).
    uint32_t by_pid = cb_rss_mb((int)getpid());
    T_CHECK_MSG(by_pid >= 1 && (by_pid > rss ? by_pid - rss : rss - by_pid) < 64, "rss %u by pid %u", rss, by_pid);
    uint64_t cpu0 = cb_cpu_us(0);
    // Burn some CPU: the reading must move.
    volatile double x = 1;
    for (int i = 0; i < 20000000; i++) x = x * 1.0000001 + 0.5;
    uint64_t cpu1 = cb_cpu_us(0);
    T_CHECK_MSG(cpu1 > cpu0 && cpu1 - cpu0 >= 5000, "cpu %llu -> %llu", (unsigned long long)cpu0, (unsigned long long)cpu1);
    uint64_t by_pid_cpu = cb_cpu_us((int)getpid());
    T_CHECK_MSG(by_pid_cpu + 200000 >= cpu1 && by_pid_cpu <= cpu1 + 200000, "cpu %llu by pid %llu", (unsigned long long)cpu1,
                (unsigned long long)by_pid_cpu);
    // A pid nobody has: 0, not a crash and not somebody else's number.
    T_EQ_INT(cb_rss_mb(2147483000), 0);
    T_EQ_INT(cb_cpu_us(2147483000), 0);
}

// ---- the registry ---------------------------------------------------------------------------

enum { M_ACT, M_UPLOADS, M_QUEUE, M_TILES, M_N };
static const cb_metric_def DEFS[M_N] = {
    { "act_cpu", CB_METRIC_HIST }, { "uploads", CB_METRIC_RATE }, { "queue", CB_METRIC_GAUGE }, { "tiles", CB_METRIC_COUNTER },
};

static void registry_refusals(void) {
    cb_metrics *m = (cb_metrics *)1;
    static const cb_metric_def bad_name[] = { { "Act", CB_METRIC_HIST } };
    T_EQ_INT(cb_metrics_new(bad_name, 1, &m), CB_E_INVALID);
    T_CHECK(m == NULL);
    static const cb_metric_def empty_name[] = { { "", CB_METRIC_HIST } };
    T_EQ_INT(cb_metrics_new(empty_name, 1, &m), CB_E_INVALID);
    static const cb_metric_def bad_kind[] = { { "a", 0 } };
    T_EQ_INT(cb_metrics_new(bad_kind, 1, &m), CB_E_INVALID);
    static const cb_metric_def bad_kind2[] = { { "a", 5 } };
    T_EQ_INT(cb_metrics_new(bad_kind2, 1, &m), CB_E_INVALID);
    static const cb_metric_def dup[] = { { "a", 1 }, { "b", 2 }, { "a", 3 } };
    T_EQ_INT(cb_metrics_new(dup, 3, &m), CB_E_INVALID);
    char longname[CB_METRIC_NAME + 1];
    memset(longname, 'x', CB_METRIC_NAME);
    longname[CB_METRIC_NAME] = 0;
    cb_metric_def too_long[] = { { longname, 1 } };
    T_EQ_INT(cb_metrics_new(too_long, 1, &m), CB_E_INVALID);
    longname[CB_METRIC_NAME - 1] = 0;   // 31 bytes: the most that fits
    T_EQ_INT(cb_metrics_new(too_long, 1, &m), CB_OK);
    cb_metrics_free(m);
    T_EQ_INT(cb_metrics_new(DEFS, -1, &m), CB_E_INVALID);
    T_EQ_INT(cb_metrics_new(NULL, 1, &m), CB_E_INVALID);
    T_EQ_INT(cb_metrics_new(DEFS, CB_METRICS_MAX + 1, &m), CB_E_INVALID);
    // None is a registry too.
    T_EQ_INT(cb_metrics_new(NULL, 0, &m), CB_OK);
    T_CHECK(m != NULL);
    T_EQ_INT(cb_metrics_n(m), 0);
    T_CHECK(cb_metrics_def(m, 0) == NULL);
    cb_metrics_free(m);
    cb_metrics_free(NULL);
    // Every update and read on a NULL registry is a no-op.
    cb_metrics_add(NULL, 0, 1);
    cb_metrics_observe(NULL, 0, 1);
    T_EQ_INT(cb_metrics_get(NULL, 0), 0);
    T_EQ_INT(cb_metrics_n(NULL), 0);
}

static void registry_updates(void) {
    cb_metrics *m = NULL;
    T_EQ_INT(cb_metrics_new(DEFS, M_N, &m), CB_OK);
    T_EQ_INT(cb_metrics_n(m), M_N);
    T_CHECK(cb_metrics_def(m, M_UPLOADS) == &DEFS[M_UPLOADS]);
    for (uint64_t v = 1; v <= 100; v++) cb_metrics_observe(m, M_ACT, v);
    T_EQ_INT(cb_metrics_get(m, M_ACT), 100);
    T_EQ_INT(cb_metrics_pct(m, M_ACT, 50), 48);
    T_EQ_INT(cb_metrics_max(m, M_ACT), 100);
    cb_metrics_rate(m, M_UPLOADS, 5000, 9);
    cb_metrics_rate(m, M_UPLOADS, 6000, 9);
    T_EQ_INT(cb_metrics_get(m, M_UPLOADS), 18);
    T_CHECK(fabs(cb_metrics_per_s(m, M_UPLOADS, 7000) - 2.0) < 1e-6);
    cb_metrics_add(m, M_QUEUE, 5);
    cb_metrics_sub(m, M_QUEUE, 2);
    T_EQ_INT(cb_metrics_get(m, M_QUEUE), 3);
    cb_metrics_set(m, M_QUEUE, 40);
    T_EQ_INT(cb_metrics_get(m, M_QUEUE), 40);
    cb_metrics_add(m, M_TILES, 7);
    T_EQ_INT(cb_metrics_get(m, M_TILES), 7);
    // The wrong kind of update, or an index outside the table, changes nothing.
    cb_metrics_observe(m, M_TILES, 50);
    cb_metrics_add(m, M_ACT, 50);
    cb_metrics_set(m, M_TILES, 99);   // a counter is not a gauge: set is refused
    cb_metrics_sub(m, M_TILES, 1);
    cb_metrics_rate(m, M_ACT, 1000, 1);
    cb_metrics_add(m, M_N, 1);
    cb_metrics_add(m, -1, 1);
    T_EQ_INT(cb_metrics_get(m, M_TILES), 7);
    T_EQ_INT(cb_metrics_get(m, M_ACT), 100);
    T_EQ_INT(cb_metrics_get(m, M_N), 0);
    T_EQ_INT(cb_metrics_pct(m, M_TILES, 50), 0);
    T_EQ_INT(cb_metrics_max(m, M_UPLOADS), 0);
    T_CHECK(cb_metrics_per_s(m, M_ACT, 7000) == 0);
    cb_metrics_free(m);
}

static const cb_metric_row *row_at(const uint8_t *buf, int i) {
    return (const cb_metric_row *)(buf + sizeof(cb_metrics_hdr) + (size_t)i * sizeof(cb_metric_row));
}

static void registry_report(void) {
    cb_metrics *m = NULL;
    T_EQ_INT(cb_metrics_new(DEFS, M_N, &m), CB_OK);
    for (uint64_t v = 1; v <= 100; v++) cb_metrics_observe(m, M_ACT, v);
    cb_metrics_rate(m, M_UPLOADS, 5000, 9);
    cb_metrics_rate(m, M_UPLOADS, 6000, 9);
    cb_metrics_set(m, M_QUEUE, 40);
    cb_metrics_add(m, M_TILES, 7);
    size_t need = cb_metrics_report_size(m);
    T_EQ_INT(need, sizeof(cb_metrics_hdr) + (CB_METRICS_N_PLATFORM + M_N) * sizeof(cb_metric_row));
    uint8_t buf[1024];
    size_t need_app = sizeof(cb_metrics_hdr) + M_N * sizeof(cb_metric_row);   // no ctx: no platform rows
    T_EQ_INT(cb_metrics_report(m, NULL, 7000, buf, need_app - 1), 0);   // too short: nothing
    T_EQ_INT(cb_metrics_report(m, NULL, 7000, NULL, need), 0);
    T_EQ_INT(cb_metrics_report(m, NULL, 7000, buf, need_app), need_app);
    // With no ctx: the app's rows alone, no platform rows and no uptime.
    size_t n = cb_metrics_report(m, NULL, 7000, buf, sizeof buf);
    T_EQ_INT(n, sizeof(cb_metrics_hdr) + M_N * sizeof(cb_metric_row));
    cb_metrics_hdr h;
    memcpy(&h, buf, sizeof h);
    T_EQ_INT(h.version, CB_METRICS_WIRE_VERSION);
    T_EQ_INT(h.n_rows, M_N);
    T_EQ_INT(h.n_platform, 0);
    T_EQ_INT(h.row_size, sizeof(cb_metric_row));
    T_EQ_INT(h.at_ms, 7000);
    T_EQ_INT(h.uptime_s, 0);
    T_CHECK(h.rss_mb >= 1);
    T_CHECK(h.cpu_us > 0);
    T_EQ_INT(h.pid, getpid());
    const cb_metric_row *r = row_at(buf, M_ACT);
    T_EQ_INT(r->kind, CB_METRIC_HIST);
    T_EQ_INT(r->index, M_ACT);
    T_EQ_STR(r->name, "act_cpu");
    T_EQ_INT(r->name_len, 7);
    T_EQ_INT(r->total, 100);
    T_EQ_INT(r->p50, 48);
    T_EQ_INT(r->p90, 80);
    T_EQ_INT(r->p99, 96);
    T_EQ_INT(r->max, 100);
    r = row_at(buf, M_UPLOADS);
    T_EQ_INT(r->kind, CB_METRIC_RATE);
    T_EQ_INT(r->total, 18);
    T_CHECK(fabs(r->per_s - 2.0) < 1e-6);
    T_EQ_INT(r->p99, 0);
    r = row_at(buf, M_QUEUE);
    T_EQ_INT(r->kind, CB_METRIC_GAUGE);
    T_EQ_INT(r->total, 40);
    r = row_at(buf, M_TILES);
    T_EQ_INT(r->kind, CB_METRIC_COUNTER);
    T_EQ_INT(r->total, 7);
    T_EQ_STR(r->name, "tiles");
    // With a ctx: the platform's four first, read from ctx->stats as the console reads them.
    cb_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.started_ms = 1000;
    atomic_store(&ctx.stats.requests, 1234);
    atomic_store(&ctx.stats.responses_5xx, 5);
    atomic_store(&ctx.stats.http_conns, 3);
    atomic_store(&ctx.stats.ws_conns, -1);   // a miscount below zero reads as 0, never as 2^64
    n = cb_metrics_report(m, &ctx, 61000, buf, sizeof buf);
    T_EQ_INT(n, need);
    memcpy(&h, buf, sizeof h);
    T_EQ_INT(h.n_rows, CB_METRICS_N_PLATFORM + M_N);
    T_EQ_INT(h.n_platform, CB_METRICS_N_PLATFORM);
    T_EQ_INT(h.uptime_s, 60);
    T_EQ_STR(row_at(buf, 0)->name, "requests");
    T_EQ_INT(row_at(buf, 0)->kind, CB_METRIC_COUNTER);
    T_EQ_INT(row_at(buf, 0)->total, 1234);
    T_EQ_STR(row_at(buf, 1)->name, "errors_5xx");
    T_EQ_INT(row_at(buf, 1)->total, 5);
    T_EQ_STR(row_at(buf, 2)->name, "http_conns");
    T_EQ_INT(row_at(buf, 2)->kind, CB_METRIC_GAUGE);
    T_EQ_INT(row_at(buf, 2)->total, 3);
    T_EQ_STR(row_at(buf, 3)->name, "ws_conns");
    T_EQ_INT(row_at(buf, 3)->total, 0);
    T_EQ_INT(row_at(buf, 3)->index, 3);
    r = row_at(buf, CB_METRICS_N_PLATFORM + M_TILES);
    T_EQ_STR(r->name, "tiles");
    T_EQ_INT(r->index, M_TILES);
    T_EQ_INT(r->total, 7);
    // A NULL registry with a ctx: the platform's rows alone.
    n = cb_metrics_report(NULL, &ctx, 61000, buf, sizeof buf);
    T_EQ_INT(n, sizeof(cb_metrics_hdr) + CB_METRICS_N_PLATFORM * sizeof(cb_metric_row));
    T_EQ_INT(cb_metrics_report_size(NULL), n);
    cb_metrics_free(m);
}

// GET /metrics through the handler: served when the app declares a table, 404 when it does not.
static void metrics_route(void) {
    static cb_config cfg;
    memset(&cfg, 0, sizeof cfg);
    strcpy(cfg.allowed_origins, "*");
    cb_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.cfg = &cfg;
    ctx.started_ms = cb_now_ms() - 5000;
    ctx.router = cb_router_new();
    T_EQ_INT(cb_metrics_routes(&ctx), CB_OK);   // no registry: no route
    cb_http_req q;
    memset(&q, 0, sizeof q);
    snprintf(q.method, sizeof q.method, "GET");
    q.path = "/metrics";
    q.query = "";
    q.minor = 1;
    cb_http_res r;
    cb_res_init(&r);
    cb_handle(&ctx, &q, &r);
    T_EQ_INT(r.status, 404);
    cb_res_free(&r);
    T_EQ_INT(cb_metrics_new(DEFS, M_N, &ctx.metrics), CB_OK);
    cb_metrics_add(ctx.metrics, M_TILES, 3);
    T_EQ_INT(cb_metrics_routes(&ctx), CB_OK);
    cb_res_init(&r);
    cb_handle(&ctx, &q, &r);
    T_EQ_INT(r.status, 200);
    T_EQ_INT(r.body.len, cb_metrics_report_size(ctx.metrics));
    cb_metrics_hdr h;
    memcpy(&h, r.body.p, sizeof h);
    T_EQ_INT(h.n_rows, CB_METRICS_N_PLATFORM + M_N);
    T_CHECK(h.uptime_s >= 5 && h.uptime_s <= 6);
    T_EQ_INT(row_at(r.body.p, CB_METRICS_N_PLATFORM + M_TILES)->total, 3);
    cb_res_free(&r);
    cb_metrics_free(ctx.metrics);
    cb_router_free(ctx.router);
}

static void metrics_registry_tests(void) {
    T_RUN(registry_refusals);
    T_RUN(registry_updates);
    T_RUN(registry_report);
    T_RUN(metrics_route);
}

int main(void) {
    T_RUN(hist_buckets);
    T_RUN(hist_percentiles);
    T_RUN(hist_threads);
    T_RUN(rate_ring);
    T_RUN(counter);
    T_RUN(process_readers);
    metrics_registry_tests();
    return T_DONE();
}
