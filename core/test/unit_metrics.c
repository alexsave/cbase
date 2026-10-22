// unit_metrics.c - metrics.h, the registry: an app's table of named metrics, the updates by
// index, the report's fixed layout, the platform's own rows in it, and GET /metrics. The
// machinery under it is shared/c's and tested there (stats/hist_test.c, proc_test.c).
#include "t.h"

#include "handler.h"
#include "metrics.h"
#include "net/http.h"
#include "net/router.h"
#include "util/clock.h"
#include "util/err.h"
#include "../../../shared/c/stats/hist.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// A check that says what it saw when it fails (t.h has the bare one).
#define T_CHECK_MSG(cond, ...) do { if (!(cond)) { fprintf(stderr, "  "); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } T_CHECK(cond); } while (0)

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
    T_CHECK(h.rss_mb >= 1 && h.cpu_us > 0);   // the process readers (shared/c proc.h) answered
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
    metrics_registry_tests();
    return T_DONE();
}
