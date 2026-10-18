// num.c - cb_parse_i64 (num.h says what it accepts and why).
#include "util/num.h"

bool cb_parse_i64(const char *s, size_t n, int64_t lo, int64_t hi, int64_t *out) {
    if (!s || n == 0) return false;   // an empty range (lo > hi) refuses every value below
    size_t i = 0;
    bool neg = false;
    if (s[0] == '-') {
        if (lo >= 0) return false;
        neg = true;
        i = 1;
    }
    if (i == n) return false;   // "" or a lone '-'
    // The magnitude, bounded by what the sign can hold: INT64_MAX, or one more
    // for a negative value (INT64_MIN's magnitude).
    const uint64_t limit = neg ? (uint64_t)INT64_MAX + 1u : (uint64_t)INT64_MAX;
    uint64_t mag = 0;
    for (; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < '0' || c > '9') return false;
        uint64_t d = (uint64_t)(c - '0');
        if (mag > (limit - d) / 10u) return false;   // mag * 10 + d would pass the limit
        mag = mag * 10u + d;
    }
    int64_t v;
    if (!neg) v = (int64_t)mag;
    else if (mag == (uint64_t)INT64_MAX + 1u) v = INT64_MIN;
    else v = -(int64_t)mag;
    if (v < lo || v > hi) return false;
    *out = v;
    return true;
}
