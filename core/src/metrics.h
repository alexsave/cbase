// metrics.h - THE METRICS REGISTRY: an app declares, as one static table, what it counts (a
// name and a kind each), and the platform owns the storage, the updates, the once-a-second
// report and the route that serves it. The machinery itself (the histogram, the rate ring,
// the counter, the process readers) is util/metrics.h; nothing of it is copied per app.
//
//   the table      cb_metric_def defs[] = { { "act_cpu", CB_METRIC_HIST }, { "uploads", CB_METRIC_RATE }, ... }
//                  in cb_app.metrics / n_metrics (cbase/app.h); boot makes ctx->metrics from it.
//                  An app keeps an enum beside the table so an update names its row by index.
//   the updates    cb_metrics_add / set (a counter, a gauge), cb_metrics_rate (a rate),
//                  cb_metrics_observe (a histogram), from any thread, by index: a bounds check
//                  and one relaxed atomic each, no lookup, no lock.
//   the report   cb_metrics_report: a fixed little-endian layout (cb_metrics_hdr, then one
//                  cb_metric_row per metric), the platform's own rows first (requests,
//                  errors_5xx, http_conns, ws_conns, read from ctx->stats as the console reads
//                  them: the counting itself is untouched) and the app's after, so a page
//                  decodes every app's numbers with one reader structgen writes from this header.
//   the route      GET /metrics answers the report (application/octet-stream, no cache) when
//                  the app declares a table; an app with none serves nothing new.
#ifndef CB_METRICS_H
#define CB_METRICS_H

#include "ctx.h"

#include <stddef.h>
#include <stdint.h>

enum { CB_METRIC_COUNTER = 1, CB_METRIC_GAUGE = 2, CB_METRIC_RATE = 3, CB_METRIC_HIST = 4 };

#define CB_METRIC_NAME 32   // a name: 1..31 of [a-z0-9_.], unique in its table
#define CB_METRICS_MAX 256  // rows in one table

typedef struct cb_metric_def_s {
    const char *name;
    uint8_t     kind;   // CB_METRIC_*
} cb_metric_def;

typedef struct cb_metrics cb_metrics;

// The registry for defs[0..n): CB_OK, CB_E_INVALID (a name, a kind, a duplicate, n past
// CB_METRICS_MAX), CB_E_NOMEM. defs must outlive it (a static table). n may be 0.
int  cb_metrics_new(const cb_metric_def *defs, int n, cb_metrics **out);
void cb_metrics_free(cb_metrics *m);
int  cb_metrics_n(const cb_metrics *m);
const cb_metric_def *cb_metrics_def(const cb_metrics *m, int i);

// Updates, each ignored for an index out of range or of another kind (and for m NULL), so a
// writer needs no check of its own; m NULL is the registry of a test that made none.
void cb_metrics_add(cb_metrics *m, int i, uint64_t n);        // a counter or a gauge, up
void cb_metrics_sub(cb_metrics *m, int i, uint64_t n);        // a gauge, down
void cb_metrics_set(cb_metrics *m, int i, uint64_t v);        // a gauge
void cb_metrics_rate(cb_metrics *m, int i, int64_t now_ms, uint64_t n);
void cb_metrics_observe(cb_metrics *m, int i, uint64_t v);    // a histogram sample

// Reads, for an app that writes its own record from them: the value of a counter or gauge,
// the events ever of a rate, the samples ever of a histogram (0 for a bad index).
uint64_t cb_metrics_get(const cb_metrics *m, int i);
uint32_t cb_metrics_pct(const cb_metrics *m, int i, double pct);   // a histogram's percentile
uint64_t cb_metrics_max(const cb_metrics *m, int i);               // a histogram's max
float    cb_metrics_per_s(const cb_metrics *m, int i, int64_t now_ms);   // a rate's

// ---- the report's layout -------------------------------------------------------------------
// Fixed little-endian structs with no implicit padding (the static asserts pin each size): a
// header, then n_rows rows of row_size bytes, the first n_platform of them the platform's.
#define CB_METRICS_WIRE_VERSION 1

typedef struct {
    uint32_t version;      // CB_METRICS_WIRE_VERSION
    uint32_t n_rows;
    uint32_t n_platform;   // of n_rows, the platform's own, first
    uint32_t row_size;     // sizeof(cb_metric_row)
    int64_t  at_ms;        // when the report was taken
    uint32_t uptime_s;     // since boot (0 with no ctx)
    uint32_t rss_mb;       // the process now
    uint64_t cpu_us;       // the process so far, user + system
    uint32_t pid;
    uint32_t pad;
} cb_metrics_hdr;

typedef struct {
    uint8_t  kind;         // CB_METRIC_*
    uint8_t  name_len;
    uint16_t pad;
    uint32_t index;        // its index in its table (the platform's, or the app's)
    uint64_t total;        // counter, gauge: the value; rate: events ever; hist: samples ever
    float    per_s;        // rate: over the last whole seconds (util/metrics.h cb_rate_per_s)
    uint32_t p50, p90, p99;   // hist: bucket floors
    uint64_t max;          // hist
    char     name[CB_METRIC_NAME];   // NUL-terminated
} cb_metric_row;

_Static_assert(sizeof(cb_metrics_hdr) == 48, "cb_metrics_hdr");
_Static_assert(sizeof(cb_metric_row) == 72, "cb_metric_row");

#define CB_METRICS_N_PLATFORM 4   // requests, errors_5xx, http_conns, ws_conns

// The report's size for this registry, and the report itself into out: the bytes written,
// or 0 when cap is short. ctx may be NULL (no platform rows, no uptime). Reads only atomics
// and the process; safe on any thread.
size_t cb_metrics_report_size(const cb_metrics *m);
size_t cb_metrics_report(const cb_metrics *m, const cb_ctx *ctx, int64_t now_ms, void *out, size_t cap);

// GET /metrics (handler.c registers it when the app declares a table).
int cb_metrics_routes(cb_ctx *ctx);

#endif
