// fz_num.c - num_parse_i64 (shared/c/num_parse.h) against a reference model: the same
// grammar checked by a plain scan and the value accumulated in __int128, so
// the model has no overflow to get wrong.
//
// Input: [mode][16 bytes of range, used by mode 3][text]. The mode picks the
// range: 0 the whole int64 range, 1 0..INT64_MAX, 2 1..65535 (a port), 3 two
// int64s from the input (either order; lo > hi must be refused).
#include "fz.h"

#include "../../../shared/c/num_parse.h"

static int64_t i64_at(const uint8_t *p) {
    uint64_t u = 0;
    for (int i = 0; i < 8; i++) u |= (uint64_t)p[i] << (8 * i);
    return (int64_t)u;
}

// The model: true and *out when s[0..n) is -?[0-9]+ ('-' only when lo < 0)
// with a value in lo..hi.
static bool model(const uint8_t *s, size_t n, int64_t lo, int64_t hi, int64_t *out) {
    if (lo > hi || n == 0) return false;
    size_t i = 0;
    bool neg = s[0] == '-';
    if (neg) {
        if (lo >= 0) return false;
        i = 1;
    }
    if (i == n) return false;
    __int128 v = 0;
    for (; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return false;
        if (v <= (__int128)1 << 70) v = v * 10 + (s[i] - '0');   // past 2^70 it only grows: out of range
    }
    if (neg) v = -v;
    if (v < lo || v > hi) return false;
    *out = (int64_t)v;
    return true;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 1) return 0;
    uint8_t mode = data[0] % 4;
    int64_t lo = INT64_MIN, hi = INT64_MAX;
    size_t at = 1;
    if (mode == 1) lo = 0;
    if (mode == 2) { lo = 1; hi = 65535; }
    if (mode == 3) {
        if (size < 17) return 0;
        lo = i64_at(data + 1);
        hi = i64_at(data + 9);
        at = 17;
    }
    const uint8_t *s = data + at;
    size_t n = size - at;

    int64_t want = 0, got = 12345;
    bool m = model(s, n, lo, hi, &want);
    bool p = num_parse_i64((const char *)s, n, lo, hi, &got);
    FZ_CHECK(m == p);
    if (p) FZ_CHECK(got == want && got >= lo && got <= hi);
    else FZ_CHECK(got == 12345);   // a refusal leaves *out alone

    // The C-string form reads up to the first NUL: the same answer as the
    // length form over that prefix.
    char *c = fz_cstr(s, n);
    int64_t got_c = 12345, want_c = 0;
    bool pc = num_parse_i64_cstr(c, lo, hi, &got_c);
    FZ_CHECK(pc == model(s, strlen(c), lo, hi, &want_c));
    if (pc) FZ_CHECK(got_c == want_c);
    free(c);
    return 0;
}
