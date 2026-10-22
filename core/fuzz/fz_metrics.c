// fz_metrics.c - the metrics registry (core/src/metrics.h) and its machinery (shared/c stats/hist.h)
// under an arbitrary table and an arbitrary run of updates, against the invariants a reader
// relies on: a table is accepted exactly when every name and kind is right and no name repeats;
// an update by a bad index or of the wrong kind changes nothing; a report fits exactly its
// stated size or is refused whole; every row's name is terminated and its percentiles are
// ordered under its max; a histogram's percentile is the floor of the bucket of a sample it
// holds; and a rate ring never counts a second twice.
//
// Input: [n_defs][per def: name_len, name bytes, kind][ops: op, index, 8 bytes value]... [cap].
#include "fz.h"

#include "metrics.h"
#include "util/err.h"
#include "../../../shared/c/stats/hist.h"

static bool name_model(const char *s) {
    size_t n = strlen(s);
    if (n == 0 || n >= CB_METRIC_NAME) return false;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.')) return false;
    }
    return true;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    fz_in in = fz_in_of(data, size);
    int n = fz_u8(&in) % 12;   // small tables: the shapes, not the count, are the point
    cb_metric_def defs[12] = { 0 };   // (GCC cannot see that only the n written are read)
    char *names[12];
    bool ok = true;
    for (int i = 0; i < n; i++) {
        size_t want = fz_u8(&in) % 40, got;
        const uint8_t *b = fz_bytes(&in, want, &got);
        names[i] = fz_cstr(b, got);
        defs[i].name = names[i];
        defs[i].kind = fz_u8(&in) % 6;   // 0 and 5 are not kinds
        if (!name_model(names[i]) || defs[i].kind < 1 || defs[i].kind > 4) ok = false;
        for (int j = 0; j < i; j++)
            if (strcmp(names[i], names[j]) == 0) ok = false;
    }
    cb_metrics *m = (cb_metrics *)1;
    int rc = cb_metrics_new(defs, n, &m);
    FZ_CHECK((rc == CB_OK) == ok);
    if (rc != CB_OK) {
        FZ_CHECK(m == NULL);
        for (int i = 0; i < n; i++) free(names[i]);
        return 0;
    }
    FZ_CHECK(cb_metrics_n(m) == n);
    // A model of the counters and gauges, and of each histogram's max and sample count.
    uint64_t value[12] = { 0 }, hmax[12] = { 0 }, hn[12] = { 0 }, rtotal[12] = { 0 };
    int64_t last_ms = 0;
    while (fz_left(&in) > 1) {
        uint8_t op = fz_u8(&in) % 8;
        int i = (int)(fz_u8(&in) % 16) - 2;   // -2..13: bad indices too
        uint64_t v = fz_u32(&in) | (uint64_t)fz_u32(&in) << 32;
        bool in_range = i >= 0 && i < n;
        int kind = in_range ? defs[i].kind : 0;
        switch (op) {
        case 0: cb_metrics_add(m, i, v); if (kind == CB_METRIC_COUNTER || kind == CB_METRIC_GAUGE) value[i] += v; break;
        case 1: cb_metrics_sub(m, i, v); if (kind == CB_METRIC_GAUGE) value[i] -= v; break;
        case 2: cb_metrics_set(m, i, v); if (kind == CB_METRIC_GAUGE) value[i] = v; break;
        case 3: case 4: {
            uint64_t s = v & 0xFFFFFF;   // a sample under 2^24: its bucket's floor is exact and under its max
            cb_metrics_observe(m, i, s);
            if (kind == CB_METRIC_HIST) { hn[i]++; if (s > hmax[i]) hmax[i] = s; }
            break;
        }
        default: {
            int64_t ms = (int64_t)(v & 0xFFFFFFF);   // monotone-ish: the ring is written at rising seconds mostly
            if (ms < last_ms) ms = last_ms;
            last_ms = ms;
            uint64_t k = (v >> 32) & 0xFFFF;
            cb_metrics_rate(m, i, ms, k);
            if (kind == CB_METRIC_RATE) rtotal[i] += k;
            break;
        }
        }
    }
    for (int i = 0; i < n; i++) {
        switch (defs[i].kind) {
        case CB_METRIC_COUNTER: case CB_METRIC_GAUGE: FZ_CHECK(cb_metrics_get(m, i) == value[i]); break;
        case CB_METRIC_RATE:
            FZ_CHECK(cb_metrics_get(m, i) == rtotal[i]);
            // The last whole seconds hold at most everything ever counted.
            FZ_CHECK(cb_metrics_per_s(m, i, last_ms + 1000) * (STAT_RATE_SLOTS - 1) <= (float)rtotal[i] + 1);
            break;
        default:
            FZ_CHECK(cb_metrics_get(m, i) == hn[i]);
            FZ_CHECK(cb_metrics_max(m, i) == hmax[i]);
            uint32_t p50 = cb_metrics_pct(m, i, 50), p90 = cb_metrics_pct(m, i, 90), p99 = cb_metrics_pct(m, i, 99);
            FZ_CHECK(p50 <= p90 && p90 <= p99 && p99 <= hmax[i]);
            if (hn[i]) FZ_CHECK(cb_metrics_pct(m, i, 100) == hmax[i]);   // p100 is the max
            break;
        }
    }
    FZ_CHECK(cb_metrics_get(m, n) == 0 && cb_metrics_get(m, -1) == 0);
    // The report: refused whole below its size, exact at it, and sane in every row.
    size_t need = cb_metrics_report_size(m);
    FZ_CHECK(need == sizeof(cb_metrics_hdr) + (size_t)(CB_METRICS_N_PLATFORM + n) * sizeof(cb_metric_row));
    size_t cap = fz_u8(&in) % 2 ? need : (size_t)(fz_u8(&in) % (need + 1));
    uint8_t *buf = fz_dup(NULL, 0);
    free(buf);
    buf = calloc(need + 8, 1);
    FZ_CHECK(buf);
    memset(buf + need, 0xAB, 8);
    cb_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.started_ms = 500;
    size_t got = cb_metrics_report(m, &ctx, 2500, buf, cap);
    FZ_CHECK(got == (cap >= need ? need : 0));
    FZ_CHECK(memcmp(buf + need, "\xAB\xAB\xAB\xAB\xAB\xAB\xAB\xAB", 8) == 0);   // nothing past the size
    if (got) {
        cb_metrics_hdr h;
        memcpy(&h, buf, sizeof h);
        FZ_CHECK(h.version == CB_METRICS_WIRE_VERSION && h.n_rows == (uint32_t)(CB_METRICS_N_PLATFORM + n) &&
                 h.n_platform == CB_METRICS_N_PLATFORM && h.row_size == sizeof(cb_metric_row) && h.uptime_s == 2 && h.at_ms == 2500);
        for (uint32_t r = 0; r < h.n_rows; r++) {
            cb_metric_row row;
            memcpy(&row, buf + sizeof h + r * sizeof row, sizeof row);
            FZ_CHECK(row.kind >= CB_METRIC_COUNTER && row.kind <= CB_METRIC_HIST);
            FZ_CHECK(row.name_len < CB_METRIC_NAME && row.name[row.name_len] == 0 && strlen(row.name) == row.name_len);
            FZ_CHECK(row.p50 <= row.p90 && row.p90 <= row.p99 && row.p99 <= row.max);
            if (r >= CB_METRICS_N_PLATFORM) {
                int i = (int)(r - CB_METRICS_N_PLATFORM);
                FZ_CHECK(row.index == (uint32_t)i && row.kind == defs[i].kind && strcmp(row.name, defs[i].name) == 0);
                FZ_CHECK(row.total == cb_metrics_get(m, i));
            } else {
                FZ_CHECK(row.index == r && row.total == 0);   // a zeroed ctx counts nothing
            }
        }
    }
    free(buf);
    cb_metrics_free(m);
    for (int i = 0; i < n; i++) free(names[i]);
    return 0;
}
