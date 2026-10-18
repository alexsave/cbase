// t.h - the c_base test harness: a handful of assertion macros and a runner.
//
// Each test/unit_*.c is its own program (the Makefile finds them by name, so a
// new file needs no Makefile edit) built against every c_base object and the
// kernel:
//
//     #include "t.h"
//     static void round_trip(void) { T_EQ_INT(cb_bytea_decode("0a", 2, b, 1), 1); }
//     int main(void) { T_RUN(round_trip); return T_DONE(); }
//
// A failed check prints file:line, the expression and both values, and the
// test keeps going so one run shows every failure. T_DONE() prints the tally
// and is the exit status: 0 only when every check passed AND at least one ran
// (a test binary that checks nothing is a failure, not a pass).
#ifndef CB_T_H
#define CB_T_H

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static int t_checks, t_failed, t_tests;
static const char *t_current = "?";

static inline int t_report(int ok, const char *file, int line, const char *expr) {
    t_checks++;
    if (!ok) { t_failed++; fprintf(stderr, "FAIL %s:%d [%s] %s\n", file, line, t_current, expr); }
    return ok;
}

#define T_CHECK(cond) ((void)t_report(!!(cond), __FILE__, __LINE__, #cond))

#define T_EQ_INT(a, b) do { \
    long long t_a_ = (long long)(a), t_b_ = (long long)(b); \
    if (!t_report(t_a_ == t_b_, __FILE__, __LINE__, #a " == " #b)) \
        fprintf(stderr, "     got %lld, want %lld\n", t_a_, t_b_); \
} while (0)

#define T_EQ_STR(a, b) do { \
    const char *t_a_ = (a), *t_b_ = (b); \
    int t_ok_ = t_a_ && t_b_ && strcmp(t_a_, t_b_) == 0; \
    if (!t_report(t_ok_, __FILE__, __LINE__, #a " == " #b)) \
        fprintf(stderr, "     got \"%s\", want \"%s\"\n", t_a_ ? t_a_ : "(null)", t_b_ ? t_b_ : "(null)"); \
} while (0)

#define T_EQ_MEM(a, an, b, bn) do { \
    size_t t_an_ = (size_t)(an), t_bn_ = (size_t)(bn); \
    int t_ok_ = t_an_ == t_bn_ && memcmp((a), (b), t_an_) == 0; \
    if (!t_report(t_ok_, __FILE__, __LINE__, #a " == " #b)) \
        fprintf(stderr, "     lengths %zu and %zu, or bytes differ\n", t_an_, t_bn_); \
} while (0)

#define T_RUN(fn) do { t_current = #fn; t_tests++; fn(); } while (0)

#define T_DONE() t_done(__FILE__)
static inline int t_done(const char *file) {
    const char *base = strrchr(file, '/');
    base = base ? base + 1 : file;
    if (t_checks == 0) { fprintf(stderr, "FAIL %s: no checks ran\n", base); return 1; }
    printf("%s %s: %d tests, %d checks, %d failed\n", t_failed ? "FAIL" : "ok  ", base, t_tests, t_checks, t_failed);
    return t_failed ? 1 : 0;
}

#endif
