// bench_stats.h - the bench's numbers: percentiles, the saved run format, the report.
//
// Pure functions over arrays and buffers, so test/unit_bench.c holds them to
// known answers. Nothing here reads a clock or a socket.
//
// THE RUN FILE (bench/results/*.txt), one line per fact, plain text so a diff
// reads and nothing parses JSON:
//
//   cbase-bench 1
//   kind roundtrip                     roundtrip | coldstart | cpu
//   date 2026-10-13T14:02:11Z
//   host Apple M1
//   target http://127.0.0.1:8080       "-" for cpu
//   note concurrency=1 store=sqlite    free text, may be empty
//   lat <name> n=.. p50_us=.. p95_us=.. p99_us=.. max_us=.. sent_b=.. recv_b=..
//   cpu <name> ns=.. iters=..
//
// A lat row is a latency distribution (microseconds; a push's latency may be
// negative when it arrives before the action's response), with the mean bytes
// sent and received per request (0 when not a request). A cpu row is one
// operation's median nanoseconds per call.
#ifndef CB_BENCH_STATS_H
#define CB_BENCH_STATS_H

#include "util/str.h"

#include <stdbool.h>
#include <stdint.h>

// The nearest-rank percentile of a SORTED array: the smallest value with at
// least p percent of the values at or below it (sorted[ceil(p/100 * n) - 1]).
// p in (0, 100]; 0 for an empty array.
int64_t bs_percentile(const int64_t *sorted, int n, double p);

typedef struct {
    int     n;
    int64_t p50, p95, p99, max;
} bs_summary;

// Sorts v in place and summarizes it.
void bs_summarize(int64_t *v, int n, bs_summary *out);

// The median of v (sorts in place): what a cpu row reports.
int64_t bs_median(int64_t *v, int n);

#define BS_ROWS_MAX 32
#define BS_NAME_MAX 48

typedef struct {
    bool       cpu;                 // a cpu row, else a lat row
    char       name[BS_NAME_MAX];
    bs_summary s;                   // lat
    int64_t    sent_b, recv_b;      // lat: mean bytes per request
    int64_t    ns, iters;           // cpu
} bs_row;

typedef struct {
    char   kind[16];
    char   date[32];
    char   host[64];
    char   target[256];
    char   note[256];
    int    n_rows;
    bs_row rows[BS_ROWS_MAX];
} bs_run;

// Every string is copied (cut to fit); main.c supplies the date and the host.
void bs_run_init(bs_run *r, const char *kind, const char *date, const char *host, const char *target,
                 const char *note);
// Adds a row; NULL when the run is full.
bs_row *bs_run_lat(bs_run *r, const char *name, int64_t *samples, int n, int64_t sent_b, int64_t recv_b);
bs_row *bs_run_cpu(bs_run *r, const char *name, int64_t ns, int64_t iters);

// The run as its file's text.
void bs_run_write(const bs_run *r, cb_buf *out);
// A run file's text back. false when it does not start "cbase-bench 1" or a
// row does not parse.
bool bs_run_parse(const char *text, size_t n, bs_run *out);

// The budget a cpu row's share is of: the 5 ms a warm request is said to take.
#define BS_BUDGET_NS 5000000

// The Markdown report of several runs: one latency table (roundtrip and
// coldstart runs) and one cpu table, each row naming its run's date and
// target, in the order given. A table with no rows is left out.
void bs_report(const bs_run *runs, int n, cb_buf *out);

#endif
