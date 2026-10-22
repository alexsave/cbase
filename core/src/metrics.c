// metrics.c - metrics.h.
#include "metrics.h"

#include "net/http.h"
#include "net/router.h"
#include "util/clock.h"
#include "util/err.h"
#include "../../../shared/c/proc.h"
#include "../../../shared/c/stats/hist.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// One metric's storage: the union of the three shapes (a counter and a gauge are one word).
typedef union {
    atomic_ullong c;
    StatRate      r;
    StatHist      h;
} slot;

static inline void     counter_add(atomic_ullong *c, uint64_t n) { atomic_fetch_add_explicit(c, n, memory_order_relaxed); }
static inline void     counter_sub(atomic_ullong *c, uint64_t n) { atomic_fetch_sub_explicit(c, n, memory_order_relaxed); }
static inline void     counter_set(atomic_ullong *c, uint64_t v) { atomic_store_explicit(c, v, memory_order_relaxed); }
static inline uint64_t counter_get(const atomic_ullong *c) { return atomic_load_explicit((atomic_ullong *)c, memory_order_relaxed); }

struct cb_metrics {
    const cb_metric_def *defs;
    int   n;
    slot *s;   // n of them, zeroed
};

static bool name_ok(const char *s) {
    if (!s || !*s) return false;
    size_t n = 0;
    for (const char *p = s; *p; p++, n++) {
        char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.')) return false;
    }
    return n < CB_METRIC_NAME;
}

int cb_metrics_new(const cb_metric_def *defs, int n, cb_metrics **out) {
    *out = NULL;
    if (n < 0 || n > CB_METRICS_MAX || (n > 0 && !defs)) return CB_E_INVALID;
    for (int i = 0; i < n; i++) {
        if (!name_ok(defs[i].name) || defs[i].kind < CB_METRIC_COUNTER || defs[i].kind > CB_METRIC_HIST) return CB_E_INVALID;
        for (int j = 0; j < i; j++)
            if (strcmp(defs[i].name, defs[j].name) == 0) return CB_E_INVALID;
    }
    cb_metrics *m = calloc(1, sizeof *m);
    if (!m) return CB_E_NOMEM;
    m->s = calloc((size_t)(n ? n : 1), sizeof *m->s);
    if (!m->s) { free(m); return CB_E_NOMEM; }
    m->defs = defs;
    m->n = n;
    *out = m;
    return CB_OK;
}

void cb_metrics_free(cb_metrics *m) {
    if (!m) return;
    free(m->s);
    free(m);
}

int cb_metrics_n(const cb_metrics *m) { return m ? m->n : 0; }
const cb_metric_def *cb_metrics_def(const cb_metrics *m, int i) { return m && i >= 0 && i < m->n ? &m->defs[i] : NULL; }

// The slot of index i when it is of kind k (or either of two kinds), else NULL.
static slot *of(cb_metrics *m, int i, int k1, int k2) {
    if (!m || i < 0 || i >= m->n) return NULL;
    int k = m->defs[i].kind;
    return k == k1 || k == k2 ? &m->s[i] : NULL;
}

void cb_metrics_add(cb_metrics *m, int i, uint64_t n) { slot *s = of(m, i, CB_METRIC_COUNTER, CB_METRIC_GAUGE); if (s) counter_add(&s->c, n); }
void cb_metrics_sub(cb_metrics *m, int i, uint64_t n) { slot *s = of(m, i, CB_METRIC_GAUGE, CB_METRIC_GAUGE); if (s) counter_sub(&s->c, n); }
void cb_metrics_set(cb_metrics *m, int i, uint64_t v) { slot *s = of(m, i, CB_METRIC_GAUGE, CB_METRIC_GAUGE); if (s) counter_set(&s->c, v); }
void cb_metrics_rate(cb_metrics *m, int i, int64_t now_ms, uint64_t n) { slot *s = of(m, i, CB_METRIC_RATE, CB_METRIC_RATE); if (s) stat_rate_add(&s->r, now_ms, n); }
void cb_metrics_observe(cb_metrics *m, int i, uint64_t v) { slot *s = of(m, i, CB_METRIC_HIST, CB_METRIC_HIST); if (s) stat_hist_add(&s->h, v); }

uint64_t cb_metrics_get(const cb_metrics *m, int i) {
    if (!m || i < 0 || i >= m->n) return 0;
    const slot *s = &m->s[i];
    switch (m->defs[i].kind) {
    case CB_METRIC_COUNTER: case CB_METRIC_GAUGE: return counter_get(&s->c);
    case CB_METRIC_RATE: return stat_rate_total(&s->r);
    default: return stat_hist_count(&s->h);
    }
}
uint32_t cb_metrics_pct(const cb_metrics *m, int i, double pct) { slot *s = of((cb_metrics *)m, i, CB_METRIC_HIST, CB_METRIC_HIST); return s ? stat_hist_pct(&s->h, pct) : 0; }
uint64_t cb_metrics_max(const cb_metrics *m, int i) { slot *s = of((cb_metrics *)m, i, CB_METRIC_HIST, CB_METRIC_HIST); return s ? stat_hist_max(&s->h) : 0; }
float cb_metrics_per_s(const cb_metrics *m, int i, int64_t now_ms) { slot *s = of((cb_metrics *)m, i, CB_METRIC_RATE, CB_METRIC_RATE); return s ? stat_rate_per_s(&s->r, now_ms) : 0; }

// ---- the report -----------------------------------------------------------------------------

static void row_name(cb_metric_row *r, const char *name) {
    size_t n = strlen(name);
    if (n > CB_METRIC_NAME - 1) n = CB_METRIC_NAME - 1;
    memcpy(r->name, name, n);
    r->name_len = (uint8_t)n;
}

static void row_counter(cb_metric_row *r, const char *name, uint8_t kind, uint32_t index, uint64_t v) {
    memset(r, 0, sizeof *r);
    r->kind = kind;
    r->index = index;
    r->total = v;
    row_name(r, name);
}

size_t cb_metrics_report_size(const cb_metrics *m) {
    return sizeof(cb_metrics_hdr) + (size_t)(CB_METRICS_N_PLATFORM + cb_metrics_n(m)) * sizeof(cb_metric_row);
}

size_t cb_metrics_report(const cb_metrics *m, const cb_ctx *ctx, int64_t now_ms, void *out, size_t cap) {
    int n_platform = ctx ? CB_METRICS_N_PLATFORM : 0, n_app = cb_metrics_n(m);
    size_t need = sizeof(cb_metrics_hdr) + (size_t)(n_platform + n_app) * sizeof(cb_metric_row);
    if (!out || cap < need) return 0;
    cb_metrics_hdr h;
    memset(&h, 0, sizeof h);
    h.version = CB_METRICS_WIRE_VERSION;
    h.n_rows = (uint32_t)(n_platform + n_app);
    h.n_platform = (uint32_t)n_platform;
    h.row_size = sizeof(cb_metric_row);
    h.at_ms = now_ms;
    h.uptime_s = ctx && now_ms > ctx->started_ms ? (uint32_t)((now_ms - ctx->started_ms) / 1000) : 0;
    h.rss_mb = proc_rss_mb(0);
    h.cpu_us = proc_cpu_us(0);
    h.pid = (uint32_t)getpid();
    uint8_t *p = out;
    memcpy(p, &h, sizeof h);
    p += sizeof h;
    cb_metric_row r;
    if (ctx) {
        // The platform's own, as the console's overview reads them (admin.c): relaxed loads of
        // the words net/server.c and core.c count into.
        const cb_stats *st = &ctx->stats;
        row_counter(&r, "requests", CB_METRIC_COUNTER, 0, atomic_load_explicit((atomic_ullong *)&st->requests, memory_order_relaxed));
        memcpy(p, &r, sizeof r); p += sizeof r;
        row_counter(&r, "errors_5xx", CB_METRIC_COUNTER, 1, atomic_load_explicit((atomic_ullong *)&st->responses_5xx, memory_order_relaxed));
        memcpy(p, &r, sizeof r); p += sizeof r;
        int hc = atomic_load_explicit((atomic_int *)&st->http_conns, memory_order_relaxed);
        int wc = atomic_load_explicit((atomic_int *)&st->ws_conns, memory_order_relaxed);
        row_counter(&r, "http_conns", CB_METRIC_GAUGE, 2, hc > 0 ? (uint64_t)hc : 0);
        memcpy(p, &r, sizeof r); p += sizeof r;
        row_counter(&r, "ws_conns", CB_METRIC_GAUGE, 3, wc > 0 ? (uint64_t)wc : 0);
        memcpy(p, &r, sizeof r); p += sizeof r;
    }
    for (int i = 0; i < n_app; i++) {
        const cb_metric_def *d = &m->defs[i];
        const slot *s = &m->s[i];
        memset(&r, 0, sizeof r);
        r.kind = d->kind;
        r.index = (uint32_t)i;
        row_name(&r, d->name);
        switch (d->kind) {
        case CB_METRIC_COUNTER: case CB_METRIC_GAUGE:
            r.total = counter_get(&s->c);
            break;
        case CB_METRIC_RATE:
            r.total = stat_rate_total(&s->r);
            r.per_s = stat_rate_per_s(&s->r, now_ms);
            break;
        default:
            r.total = stat_hist_count(&s->h);
            r.p50 = stat_hist_pct(&s->h, 50);
            r.p90 = stat_hist_pct(&s->h, 90);
            r.p99 = stat_hist_pct(&s->h, 99);
            r.max = stat_hist_max(&s->h);
            break;
        }
        memcpy(p, &r, sizeof r);
        p += sizeof r;
    }
    return need;
}

// ---- the route -----------------------------------------------------------------------------------

static void h_metrics(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    size_t need = cb_metrics_report_size(ctx->metrics);
    if (!cb_buf_reserve(&res->body, need)) { cb_router_error(res, 503, "out of memory"); return; }
    res->body.len = cb_metrics_report(ctx->metrics, ctx, cb_now_ms(), res->body.p, res->body.cap);
    res->status = 200;
    cb_res_header(res, "Content-Type", "application/octet-stream");
    cb_res_header(res, "Cache-Control", "no-store");
}

int cb_metrics_routes(cb_ctx *ctx) {
    if (!ctx->metrics) return CB_OK;
    return cb_router_add(ctx->router, "GET", "/metrics", CB_ROUTE_POOL, h_metrics);
}
