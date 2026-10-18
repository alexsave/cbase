// unit_num.c - num_parse_i64 (shared/c/num_parse.h), the one decimal integer parser.
#include "t.h"

#include "../../shared/c/num_parse.h"

#include <stdint.h>

// One parse over a C string: 1 and the value, or 0.
static int64_t g_v;
static int ok(const char *s, int64_t lo, int64_t hi) {
    g_v = 7777;
    return num_parse_i64_cstr(s, lo, hi, &g_v);
}

static void bounds(void) {
    T_CHECK(ok("1", 1, 100) && g_v == 1);
    T_CHECK(ok("100", 1, 100) && g_v == 100);
    T_CHECK(!ok("0", 1, 100));
    T_CHECK(!ok("101", 1, 100));
    T_CHECK(ok("-5", -5, 5) && g_v == -5);
    T_CHECK(!ok("-6", -5, 5));
    T_CHECK(!ok("6", -5, 5));
    // A refusal leaves *out alone.
    T_CHECK(!ok("101", 1, 100));
    T_EQ_INT(g_v, 7777);
    // An empty range is no range.
    T_CHECK(!ok("5", 6, 5));
}

static void int64_extremes(void) {
    T_CHECK(ok("9223372036854775807", INT64_MIN, INT64_MAX) && g_v == INT64_MAX);
    T_CHECK(ok("-9223372036854775808", INT64_MIN, INT64_MAX) && g_v == INT64_MIN);
    T_CHECK(ok("-9223372036854775807", INT64_MIN, INT64_MAX) && g_v == INT64_MIN + 1);
    // One past each end, by the last digit and by one more digit.
    T_CHECK(!ok("9223372036854775808", INT64_MIN, INT64_MAX));
    T_CHECK(!ok("-9223372036854775809", INT64_MIN, INT64_MAX));
    T_CHECK(!ok("92233720368547758070", INT64_MIN, INT64_MAX));
    T_CHECK(!ok("-92233720368547758080", INT64_MIN, INT64_MAX));
    T_CHECK(!ok("18446744073709551615", INT64_MIN, INT64_MAX));   // UINT64_MAX
    T_CHECK(!ok("18446744073709551616", INT64_MIN, INT64_MAX));   // wraps to 0 in 64 bits
    T_CHECK(!ok("99999999999999999999", 1, 100));                  // not clamped to hi
    T_EQ_INT(g_v, 7777);
}

static void shapes_refused(void) {
    T_CHECK(!ok("", INT64_MIN, INT64_MAX));
    T_CHECK(!ok("-", INT64_MIN, INT64_MAX));
    T_CHECK(!ok(" 20", 1, 100));
    T_CHECK(!ok("\t20", 1, 100));
    T_CHECK(!ok("20 ", 1, 100));
    T_CHECK(!ok("20\n", 1, 100));
    T_CHECK(!ok("+20", 1, 100));
    T_CHECK(!ok("+0", 0, 100));
    T_CHECK(!ok("--1", INT64_MIN, INT64_MAX));
    T_CHECK(!ok("- 1", INT64_MIN, INT64_MAX));
    T_CHECK(!ok("20x", 1, 100));
    T_CHECK(!ok("2.5", 1, 100));
    T_CHECK(!ok("1e3", 1, 10000));
    T_CHECK(!ok("0x10", 0, 100));
    T_CHECK(!ok("\xd9\xa3", 0, 100));   // ARABIC-INDIC DIGIT THREE is not a digit here
    T_CHECK(!num_parse_i64_cstr(NULL, 0, 1, &g_v));
    T_CHECK(!num_parse_i64(NULL, 0, 0, 1, &g_v));
    // A '-' only where the range has negatives: strtoull's wrap of "-1" is gone.
    T_CHECK(!ok("-1", 0, INT64_MAX));
    T_CHECK(!ok("-0", 0, 100));
    T_EQ_INT(g_v, 7777);
}

// Leading zeros are taken (num.h says why), and "-0" is 0 where '-' is allowed.
static void zeros(void) {
    T_CHECK(ok("0", 0, 0) && g_v == 0);
    T_CHECK(ok("007", 0, 100) && g_v == 7);
    T_CHECK(ok("-0", -1, 1) && g_v == 0);
    T_CHECK(ok("-007", -10, 0) && g_v == -7);
    T_CHECK(ok("00000000000000000000000000009223372036854775807", INT64_MIN, INT64_MAX) && g_v == INT64_MAX);
}

// With a length: the bytes given, not the C string.
static void with_a_length(void) {
    int64_t v = 0;
    T_CHECK(num_parse_i64("443/path", 3, 1, 65535, &v) && v == 443);
    T_CHECK(!num_parse_i64("443/path", 4, 1, 65535, &v));
    T_CHECK(!num_parse_i64("12\0" "3", 4, 0, 10000, &v));   // an embedded NUL is not a digit
    T_CHECK(!num_parse_i64("12\0", 3, 0, 10000, &v));
    T_CHECK(!num_parse_i64("12", 0, 0, 10000, &v));
    T_CHECK(num_parse_i64("-12", 3, -12, 0, &v) && v == -12);
    T_CHECK(!num_parse_i64("-12", 1, -12, 0, &v));
}

int main(void) {
    T_RUN(bounds);
    T_RUN(int64_extremes);
    T_RUN(shapes_refused);
    T_RUN(zeros);
    T_RUN(with_a_length);
    return T_DONE();
}
