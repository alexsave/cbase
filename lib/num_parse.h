/* num_parse.h - the one strict decimal integer parser, kept once.
 *
 * Every integer read from text that someone else wrote (a setting, a port, a
 * query parameter, a cursor, a header, an import's cell) goes through
 * num_parse_i64. Header only and freestanding (stdint.h, stdbool.h,
 * stddef.h, no libc), so a wasm kernel can take it with nothing added to a
 * build list, the same way as rng.h.
 *
 * WHAT IT ACCEPTS is exactly
 *
 *     -?[0-9]+
 *
 * with the '-' allowed only when lo < 0, and the value in lo..hi (lo > hi is
 * an empty range: nothing is in it). No whitespace, no '+', no empty string,
 * nothing after the digits (a NUL inside the given length is something after
 * the digits). Overflow is seen digit by digit, never by clamping. Leading
 * zeros are taken ("007" is 7, "-0" is 0): the base is always 10, so a zero
 * changes no value and a zero-padded field reads the same as a bare one.
 *
 * WHY NOT THE C LIBRARY. strtol and its kin take a leading blank and a '+',
 * strtoull wraps "-1" to 2^64 - 1, and a caller that ignores the end pointer
 * takes anything after the digits; atoi and atol take all of that and cannot
 * report an error at all. c_base's test/unit_num_gate.c refuses them under
 * c_base/src.
 *
 * WHAT IS NOT HERE, ON PURPOSE: wasm/libc.c's atoi and atol. They are the
 * wasm fill-in for the C library and copy its lenient rules (a blank, a '+',
 * wrap on overflow), because a kernel compiled for wasm and natively must
 * behave the same. They stay lenient; code that reads untrusted text calls
 * this file instead.
 *
 * The test is c_base's test/unit_num.c, and fuzz/fz_num.c holds it against a
 * model that accumulates in __int128. */
#ifndef SHARED_NUM_PARSE_H
#define SHARED_NUM_PARSE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The n bytes at s (not NUL-terminated in general). On success *out is the
 * value and the answer is true; on failure *out is untouched. */
static inline bool num_parse_i64(const char *s, size_t n, int64_t lo, int64_t hi, int64_t *out)
{
    if (!s || n == 0) return false;   /* an empty range (lo > hi) refuses at the range check */
    size_t i = 0;
    bool neg = false;
    if (s[0] == '-') {
        if (lo >= 0) return false;
        neg = true;
        i = 1;
    }
    if (i == n) return false;   /* "" or a lone '-' */
    /* The magnitude, bounded by what the sign can hold: INT64_MAX, or one
     * more for a negative value (INT64_MIN's magnitude). */
    const uint64_t limit = neg ? (uint64_t)INT64_MAX + 1u : (uint64_t)INT64_MAX;
    uint64_t mag = 0;
    for (; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < '0' || c > '9') return false;
        uint64_t d = (uint64_t)(c - '0');
        if (mag > (limit - d) / 10u) return false;   /* mag * 10 + d would pass the limit */
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

/* The same over a C string (NULL is refused). */
static inline bool num_parse_i64_cstr(const char *s, int64_t lo, int64_t hi, int64_t *out)
{
    if (!s) return false;
    size_t n = 0;
    while (s[n]) n++;
    return num_parse_i64(s, n, lo, hi, out);
}

#endif
