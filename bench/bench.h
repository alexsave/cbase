// bench.h - cbase-bench's sub-commands and what they share (main.c).
//
//   cbase-bench roundtrip --url <base> [--n 500] [--concurrency 1|4] [--out DIR] [--note TEXT]
//   cbase-bench coldstart --url <base> [--idle S] [--repeat K] [--path P] [--force-cmd CMD] [--out DIR]
//   cbase-bench cpu [--out DIR]
//   cbase-bench direct --url <base> [--n 200] [--table cbase] [--region us-east-1] [--edge URL] [--burst K]
//   cbase-bench subscribe --url <base> [--n 30] [--as 1]
//   cbase-bench page --url <base> [--n 5] [--batch 0|1] [--pushes 20] [--gap MS]
//   cbase-bench spaced --url <base> [--path P] [--n 30] [--gap MS] [--games K]
//   cbase-bench report [FILE...]        (default: every bench/results/*.txt)
//
// Each measuring command prints its run and saves it as
// <out>/<kind>-<UTC stamp>.txt (bench_stats.h has the format). --out defaults to
// bench/results when that directory exists from the current one, else
// c_base/bench/results, else the current directory.
#ifndef CB_BENCH_H
#define CB_BENCH_H

#include "bench_stats.h"

#include <stdbool.h>

typedef struct {
    const char *url;
    const char *out_dir;
    const char *note;
    const char *path;        // coldstart
    const char *force_cmd;   // coldstart
    int         n;
    int         concurrency;
    int         idle_s;      // coldstart
    int         repeat;      // coldstart
    const char *table;       // direct: the DynamoDB table
    const char *region;      // direct: when the environment names none
    const char *edge;        // direct: a cached file on the CDN, https://host/path
    int         burst;       // direct: fresh connections at once, for a cold start
    bool        as;          // subscribe: also time NW_WS_SUB_AS
    int         gap_ms;      // spaced, roundtrip: quiet before each timed request
    int         games;       // spaced: a throwaway account with this many games
    bool        batch;       // page: subscribe with one NW_WS_BATCH
    int         pushes;      // page: chat pushes timed after the page loads
} bench_opts;

int bench_roundtrip(const bench_opts *o);
int bench_coldstart(const bench_opts *o);
int bench_cpu(const bench_opts *o);
int bench_direct(const bench_opts *o);
int bench_subscribe(const bench_opts *o);
int bench_spaced(const bench_opts *o);
int bench_page(const bench_opts *o);

// "2026-10-13T14:02:11Z" now.
void bench_date(char *out, size_t cap);
// The machine's CPU name ("Apple M1"), or "unknown".
void bench_host(char *out, size_t cap);
// Appends " load1=<n>" (the 1-minute load average) to a run's note: a number
// taken on a busy machine says so in its own file.
void bench_note_load(char *note, size_t cap);
// Prints the run and saves it. 0, or -1 when the file cannot be written.
int bench_save(const bs_run *r, const bench_opts *o);

#endif
