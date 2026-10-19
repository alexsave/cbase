// bench_stats.c - percentiles, the run file and the Markdown report (bench_stats.h).
#include "bench_stats.h"
#include "../../shared/c/stats/percentile.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int64_t bs_percentile(const int64_t *sorted, int n, double p) { return stat_pct_i64(sorted, n, p); }

static int cmp_i64(const void *a, const void *b) {
    int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return (x > y) - (x < y);
}

void bs_summarize(int64_t *v, int n, bs_summary *out) {
    memset(out, 0, sizeof *out);
    if (n <= 0) return;
    qsort(v, (size_t)n, sizeof *v, cmp_i64);
    out->n = n;
    out->p50 = bs_percentile(v, n, 50);
    out->p95 = bs_percentile(v, n, 95);
    out->p99 = bs_percentile(v, n, 99);
    out->max = v[n - 1];
}

int64_t bs_median(int64_t *v, int n) {
    if (n <= 0) return 0;
    qsort(v, (size_t)n, sizeof *v, cmp_i64);
    return bs_percentile(v, n, 50);
}

// ---- the run -----------------------------------------------------------------------

void bs_run_init(bs_run *r, const char *kind, const char *date, const char *host, const char *target,
                 const char *note) {
    memset(r, 0, sizeof *r);
    cb_strlcpy(r->kind, kind ? kind : "", sizeof r->kind);
    cb_strlcpy(r->date, date ? date : "", sizeof r->date);
    cb_strlcpy(r->host, host ? host : "", sizeof r->host);
    cb_strlcpy(r->target, target && *target ? target : "-", sizeof r->target);
    cb_strlcpy(r->note, note ? note : "", sizeof r->note);
}

static bs_row *add_row(bs_run *r, const char *name) {
    if (r->n_rows >= BS_ROWS_MAX) return NULL;
    bs_row *w = &r->rows[r->n_rows++];
    memset(w, 0, sizeof *w);
    cb_strlcpy(w->name, name, sizeof w->name);
    // A name is one word in the file.
    for (char *c = w->name; *c; c++)
        if (*c == ' ' || *c == '\t' || *c == '\n') *c = '_';
    return w;
}

bs_row *bs_run_lat(bs_run *r, const char *name, int64_t *samples, int n, int64_t sent_b, int64_t recv_b) {
    bs_row *w = add_row(r, name);
    if (!w) return NULL;
    bs_summarize(samples, n, &w->s);
    w->sent_b = sent_b;
    w->recv_b = recv_b;
    return w;
}

bs_row *bs_run_cpu(bs_run *r, const char *name, int64_t ns, int64_t iters) {
    bs_row *w = add_row(r, name);
    if (!w) return NULL;
    w->cpu = true;
    w->ns = ns;
    w->iters = iters;
    return w;
}

void bs_run_write(const bs_run *r, cb_buf *out) {
    cb_buf_printf(out, "cbase-bench 1\nkind %s\ndate %s\nhost %s\ntarget %s\nnote %s\n", r->kind, r->date, r->host,
                  r->target, r->note);
    for (int i = 0; i < r->n_rows; i++) {
        const bs_row *w = &r->rows[i];
        if (w->cpu)
            cb_buf_printf(out, "cpu %s ns=%" PRId64 " iters=%" PRId64 "\n", w->name, w->ns, w->iters);
        else
            cb_buf_printf(out,
                          "lat %s n=%d p50_us=%" PRId64 " p95_us=%" PRId64 " p99_us=%" PRId64 " max_us=%" PRId64
                          " sent_b=%" PRId64 " recv_b=%" PRId64 "\n",
                          w->name, w->s.n, w->s.p50, w->s.p95, w->s.p99, w->s.max, w->sent_b, w->recv_b);
    }
}

// Copies the rest of the line after `key ` into out.
static bool header_line(const char *line, size_t len, const char *key, char *out, size_t cap) {
    size_t kl = strlen(key);
    if (len < kl || memcmp(line, key, kl) != 0 || (len > kl && line[kl] != ' ')) return false;
    size_t off = len > kl ? kl + 1 : kl;
    size_t n = len - off;
    if (n >= cap) n = cap - 1;
    memcpy(out, line + off, n);
    out[n] = 0;
    return true;
}

bool bs_run_parse(const char *text, size_t n, bs_run *out) {
    memset(out, 0, sizeof *out);
    const char *p = text, *end = text + n;
    bool magic = false;
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t len = nl ? (size_t)(nl - p) : (size_t)(end - p);
        char line[512];
        if (len >= sizeof line) return false;
        memcpy(line, p, len);
        line[len] = 0;
        p = nl ? nl + 1 : end;
        if (len == 0) continue;
        if (!magic) {
            if (strcmp(line, "cbase-bench 1") != 0) return false;
            magic = true;
            continue;
        }
        if (header_line(line, len, "kind", out->kind, sizeof out->kind)) continue;
        if (header_line(line, len, "date", out->date, sizeof out->date)) continue;
        if (header_line(line, len, "host", out->host, sizeof out->host)) continue;
        if (header_line(line, len, "target", out->target, sizeof out->target)) continue;
        if (header_line(line, len, "note", out->note, sizeof out->note)) continue;
        if (out->n_rows >= BS_ROWS_MAX) return false;
        bs_row *w = &out->rows[out->n_rows];
        memset(w, 0, sizeof *w);
        long long a, b, c, d, e, f;
        int nn, used = 0;
        char name[BS_NAME_MAX];
        if (sscanf(line, "cpu %47s ns=%lld iters=%lld%n", name, &a, &b, &used) == 3 && (size_t)used == len) {
            w->cpu = true;
            w->ns = a;
            w->iters = b;
        } else if (sscanf(line, "lat %47s n=%d p50_us=%lld p95_us=%lld p99_us=%lld max_us=%lld sent_b=%lld recv_b=%lld%n",
                          name, &nn, &a, &b, &c, &d, &e, &f, &used) == 8 && (size_t)used == len) {
            w->s.n = nn;
            w->s.p50 = a;
            w->s.p95 = b;
            w->s.p99 = c;
            w->s.max = d;
            w->sent_b = e;
            w->recv_b = f;
        } else {
            return false;
        }
        cb_strlcpy(w->name, name, sizeof w->name);
        out->n_rows++;
    }
    return magic;
}

// ---- the report ----------------------------------------------------------------------

// Microseconds as milliseconds with two decimals, rounded half away from zero,
// with no "-0.00".
static void ms(cb_buf *out, int64_t us) {
    int64_t a = us < 0 ? -us : us;
    int64_t h = (a + 5) / 10;   // hundredths of a millisecond
    cb_buf_printf(out, "%s%" PRId64 ".%02" PRId64, us < 0 && h > 0 ? "-" : "", h / 100, h % 100);
}

void bs_report(const bs_run *runs, int n, cb_buf *out) {
    bool any_lat = false, any_cpu = false;
    for (int i = 0; i < n; i++)
        for (int j = 0; j < runs[i].n_rows; j++) {
            if (runs[i].rows[j].cpu) any_cpu = true;
            else any_lat = true;
        }
    if (any_lat) {
        cb_buf_puts(out, "| date | target | note | metric | n | p50 ms | p95 ms | p99 ms | max ms | sent B | recv B |\n"
                         "|---|---|---|---|--:|--:|--:|--:|--:|--:|--:|\n");
        for (int i = 0; i < n; i++)
            for (int j = 0; j < runs[i].n_rows; j++) {
                const bs_row *w = &runs[i].rows[j];
                if (w->cpu) continue;
                cb_buf_printf(out, "| %s | %s | %s | %s %s | %d | ", runs[i].date, runs[i].target, runs[i].note,
                              runs[i].kind, w->name, w->s.n);
                ms(out, w->s.p50);
                cb_buf_puts(out, " | ");
                ms(out, w->s.p95);
                cb_buf_puts(out, " | ");
                ms(out, w->s.p99);
                cb_buf_puts(out, " | ");
                ms(out, w->s.max);
                cb_buf_printf(out, " | %" PRId64 " | %" PRId64 " |\n", w->sent_b, w->recv_b);
            }
    }
    if (any_cpu) {
        if (any_lat) cb_buf_puts(out, "\n");
        cb_buf_puts(out, "| date | host | operation | ns per op | share of 5 ms |\n|---|---|---|--:|--:|\n");
        for (int i = 0; i < n; i++)
            for (int j = 0; j < runs[i].n_rows; j++) {
                const bs_row *w = &runs[i].rows[j];
                if (!w->cpu) continue;
                // The share in hundredths of a percent, rounded, in integers.
                int64_t bp = (w->ns * 10000 + BS_BUDGET_NS / 2) / BS_BUDGET_NS;
                cb_buf_printf(out, "| %s | %s | %s | %" PRId64 " | %" PRId64 ".%02" PRId64 "%% |\n", runs[i].date,
                              runs[i].host, w->name, w->ns, bp / 100, bp % 100);
            }
    }
}
