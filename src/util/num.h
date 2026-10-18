// num.h - the one decimal integer parser in c_base.
//
// Every number c_base reads from text (a CBASE_* setting, a port, a query
// parameter, a cursor, a Lambda header, an import's cell) goes through
// cb_parse_i64. The C library's strtol family accepts a leading space, a
// leading '+', a '-' that strtoull wraps around, and (when the end pointer is
// ignored) anything after the digits; test/unit_num_gate.c fails the build if
// one of them, atoi/atol/atoll, or sscanf appears anywhere else under src/.
//
// The accepted text is exactly
//
//     -?[0-9]+
//
// with the '-' allowed only when lo < 0, and the value in lo..hi. No
// whitespace, no '+', no empty string, nothing after the digits (an embedded
// NUL inside the given length is something after the digits). Leading zeros
// are accepted ("007" is 7, "-0" is 0): the base is always 10, so a zero
// changes no value and a zero-padded field reads the same as a bare one.
// Overflow is detected digit by digit, never by clamping.
#ifndef CB_NUM_H
#define CB_NUM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// The n bytes at s (not NUL-terminated in general). On success *out is the
// value and the answer is true; on failure *out is untouched. lo > hi is an
// empty range: nothing is in it.
bool cb_parse_i64(const char *s, size_t n, int64_t lo, int64_t hi, int64_t *out);

// The same over a C string (NULL is refused).
static inline bool cb_parse_i64_cstr(const char *s, int64_t lo, int64_t hi, int64_t *out) {
    return s && cb_parse_i64(s, strlen(s), lo, hi, out);
}

#endif
