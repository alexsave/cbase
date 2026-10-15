// unit_bench.c - the bench's statistics, run file and report (bench_stats.h).
//
// Built and run by `make test` with the other unit programs (the Makefile
// names it: it lives beside the code it checks, not in test/).
#include "t.h"

#include "bench_stats.h"

#include <stdlib.h>

// Nearest rank over 1..100: the p-th percentile is p itself.
static void percentiles_of_one_to_hundred(void) {
    int64_t v[100];
    for (int i = 0; i < 100; i++) v[i] = 100 - i;   // reversed: bs_summarize sorts
    bs_summary s;
    bs_summarize(v, 100, &s);
    T_EQ_INT(s.n, 100);
    T_EQ_INT(s.p50, 50);
    T_EQ_INT(s.p95, 95);
    T_EQ_INT(s.p99, 99);
    T_EQ_INT(s.max, 100);
    T_EQ_INT(v[0], 1);
    // 7 / 100.0 * 100 is 7.000000000000001 in doubles: still the 7th, not the 8th.
    T_EQ_INT(bs_percentile(v, 100, 7), 7);
}

// A short array: ranks round up (ceil), never interpolate.
static void percentiles_round_up(void) {
    int64_t v[] = { 40, 10, 30, 20, 50 };
    bs_summary s;
    bs_summarize(v, 5, &s);
    T_EQ_INT(s.p50, 30);   // ceil(2.5) = 3rd
    T_EQ_INT(s.p95, 50);   // ceil(4.75) = 5th
    T_EQ_INT(s.p99, 50);
    T_EQ_INT(bs_percentile(v, 5, 20), 10);   // exactly the 1st
    T_EQ_INT(bs_percentile(v, 5, 21), 20);
    T_EQ_INT(bs_percentile(v, 0, 50), 0);
    int64_t one[] = { -7 };
    T_EQ_INT(bs_percentile(one, 1, 99), -7);
    int64_t m[] = { 9, 1, 5, 3 };
    T_EQ_INT(bs_median(m, 4), 3);   // nearest rank: the 2nd of 4
}

static bs_run sample_runs(bs_run *cpu) {
    bs_run r;
    bs_run_init(&r, "roundtrip", "2026-10-13T12:00:00Z", "Apple M1", "http://127.0.0.1:8080", "concurrency=1");
    int64_t a[] = { 1500, 1000, 2000, 900, 12345 };
    bs_run_lat(&r, "action", a, 5, 410, 250);
    int64_t p[] = { -40, -5, 3 };
    bs_run_lat(&r, "push pv", p, 3, 0, 0);   // a space in a name becomes '_'
    bs_run_init(cpu, "cpu", "2026-10-13T12:01:00Z", "Apple M1", NULL, "");
    bs_run_cpu(cpu, "event_decode", 12500, 4096);
    bs_run_cpu(cpu, "kernel_action", 250000, 64);
    return r;
}

static void run_file_round_trip(void) {
    bs_run cpu, r = sample_runs(&cpu);
    cb_buf t;
    cb_buf_init(&t);
    bs_run_write(&r, &t);
    T_EQ_STR(cb_buf_cstr(&t),
             "cbase-bench 1\nkind roundtrip\ndate 2026-10-13T12:00:00Z\nhost Apple M1\ntarget http://127.0.0.1:8080\n"
             "note concurrency=1\n"
             "lat action n=5 p50_us=1500 p95_us=12345 p99_us=12345 max_us=12345 sent_b=410 recv_b=250\n"
             "lat push_pv n=3 p50_us=-5 p95_us=3 p99_us=3 max_us=3 sent_b=0 recv_b=0\n");
    bs_run back;
    T_CHECK(bs_run_parse((const char *)t.p, t.len, &back));
    T_EQ_STR(back.kind, "roundtrip");
    T_EQ_STR(back.host, "Apple M1");
    T_EQ_STR(back.note, "concurrency=1");
    T_EQ_INT(back.n_rows, 2);
    T_EQ_INT(back.rows[0].s.p95, 12345);
    T_EQ_INT(back.rows[0].recv_b, 250);
    T_EQ_INT(back.rows[1].s.p50, -5);
    T_CHECK(!back.rows[1].cpu);

    cb_buf_reset(&t);
    bs_run_write(&cpu, &t);
    T_CHECK(bs_run_parse((const char *)t.p, t.len, &back));
    T_EQ_STR(back.target, "-");
    T_EQ_STR(back.note, "");
    T_EQ_INT(back.n_rows, 2);
    T_CHECK(back.rows[1].cpu);
    T_EQ_INT(back.rows[1].ns, 250000);
    T_EQ_INT(back.rows[1].iters, 64);

    // Not a run file, and a row with trailing junk.
    T_CHECK(!bs_run_parse("hello\n", 6, &back));
    static const char JUNK[] = "cbase-bench 1\nkind cpu\ncpu x ns=1 iters=2 extra\n";
    T_CHECK(!bs_run_parse(JUNK, sizeof JUNK - 1, &back));
    cb_buf_free(&t);
}

static void report_tables(void) {
    bs_run runs[2];
    runs[0] = sample_runs(&runs[1]);
    cb_buf md;
    cb_buf_init(&md);
    bs_report(runs, 2, &md);
    T_EQ_STR(cb_buf_cstr(&md),
             "| date | target | note | metric | n | p50 ms | p95 ms | p99 ms | max ms | sent B | recv B |\n"
             "|---|---|---|---|--:|--:|--:|--:|--:|--:|--:|\n"
             "| 2026-10-13T12:00:00Z | http://127.0.0.1:8080 | concurrency=1 | roundtrip action | 5 | 1.50 | 12.35 | 12.35 | 12.35 | 410 | 250 |\n"
             "| 2026-10-13T12:00:00Z | http://127.0.0.1:8080 | concurrency=1 | roundtrip push_pv | 3 | -0.01 | 0.00 | 0.00 | 0.00 | 0 | 0 |\n"
             "\n"
             "| date | host | operation | ns per op | share of 5 ms |\n"
             "|---|---|---|--:|--:|\n"
             "| 2026-10-13T12:01:00Z | Apple M1 | event_decode | 12500 | 0.25% |\n"
             "| 2026-10-13T12:01:00Z | Apple M1 | kernel_action | 250000 | 5.00% |\n");
    // A push a few microseconds early rounds to 0.00, never "-0.00".
    bs_run tiny;
    bs_run_init(&tiny, "roundtrip", "d", "h", "t", "");
    int64_t z[] = { -3 };
    bs_run_lat(&tiny, "push_pv", z, 1, 0, 0);
    cb_buf_reset(&md);
    bs_report(&tiny, 1, &md);
    T_CHECK(strstr(cb_buf_cstr(&md), "| roundtrip push_pv | 1 | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 0 |\n") != NULL);
    // Only cpu rows: no latency table.
    cb_buf_reset(&md);
    bs_report(&runs[1], 1, &md);
    T_CHECK(strncmp(cb_buf_cstr(&md), "| date | host | operation |", 27) == 0);
    cb_buf_free(&md);
}

int main(void) {
    T_RUN(percentiles_of_one_to_hundred);
    T_RUN(percentiles_round_up);
    T_RUN(run_file_round_trip);
    T_RUN(report_tables);
    return T_DONE();
}
