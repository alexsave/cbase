/* clock_test.c - clock.h, the host clocks. Time is not exact, so every bound is a
 * loose one that still fails a wrong unit (ms read as us is a factor 1000) or a
 * wrong sign. The test sleeps about 60 ms in all. */
#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <time.h>
#include "clock.h"
#include "rng.h"
#include "test/check.h"

int main(void)
{
    TEST("clock: monotonic units agree and never go back");
    const int64_t n0 = host_mono_ns(), u0 = host_mono_us(), m0 = host_mono_ms();
    const double s0 = host_mono_s(), mf0 = host_mono_ms_f();
    CHECK(n0 > 0, "monotonic ns is positive");
    CHECK(u0 >= n0 / 1000 && u0 - n0 / 1000 < 50000, "us is ns / 1000 (%lld, %lld)", (long long)u0, (long long)n0);
    CHECK(m0 >= n0 / 1000000 && m0 - n0 / 1000000 < 50, "ms is ns / 1e6 (%lld, %lld)", (long long)m0, (long long)n0);
    CHECK(s0 * 1e9 >= (double)n0 - 1000 && s0 * 1e9 - (double)n0 < 5e7, "seconds is ns * 1e-9");
    CHECK(mf0 * 1e6 >= (double)n0 - 1000 && mf0 * 1e6 - (double)n0 < 5e7, "float ms is ns * 1e-6");

    TEST("clock: a sleep of 30 ms is 30 ms (not 30 us, not 30 s)");
    const int64_t a = host_mono_ns();
    host_sleep_ms(30);
    const int64_t d = host_mono_ns() - a;
    CHECK(d >= 29000000 && d < 2000000000, "slept %lld ns", (long long)d);
    const int64_t b = host_mono_ns();
    host_sleep_ms(0);
    host_sleep_ms(-5);
    CHECK(host_mono_ns() - b < 20000000, "a sleep of 0 or less returns at once");

    TEST("clock: wall time is the epoch in ms");
    const int64_t w = host_real_ms();
    CHECK(w > 1700000000000ll && w < 4000000000000ll, "real ms %lld is a date between 2023 and 2096", (long long)w);
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    const int64_t d2 = (int64_t)ts.tv_sec * 1000 - w;
    CHECK(d2 > -2000 && d2 < 2000, "and agrees with clock_gettime (%lld)", (long long)d2);

    TEST("clock: cpu time counts work, not sleeping");
    const int64_t c0 = host_process_cpu_ns(), t0 = host_thread_cpu_ms();
    volatile uint64_t x = 1;
    const int64_t until = host_mono_ns() + 20000000;
    while (host_mono_ns() < until) x = rng_splitmix64_at(x);
    const int64_t c1 = host_process_cpu_ns();
    CHECK(c1 - c0 >= 10000000 && c1 - c0 < 2000000000, "process cpu advanced %lld ns over 20 ms of spinning", (long long)(c1 - c0));
    CHECK(host_thread_cpu_ms() - t0 >= 10 && host_thread_cpu_ms() - t0 < 2000, "thread cpu ms advanced");
    CHECK(host_process_cpu_s() * 1e9 >= (double)c1 - 1000 && host_thread_cpu_ms_f() >= (double)t0 - 1, "the seconds and float forms follow");
    const int64_t p = host_process_cpu_ns();
    host_sleep_ms(30);
    CHECK(host_process_cpu_ns() - p < 25000000, "30 ms of sleeping is not 25 ms of cpu");

    TEST("clock: the monotonic clock moved forward");
    CHECK(host_mono_ns() > n0 && host_mono_s() > s0 && host_mono_ms() >= m0 + 30, "after the sleeps");

    TEST("clock: an absolute deadline");
    struct timespec t, now;
    clock_gettime(CLOCK_REALTIME, &now);
    host_deadline_after_ms(&t, 1500);
    int64_t dd = ((int64_t)t.tv_sec - now.tv_sec) * 1000 + (t.tv_nsec - now.tv_nsec) / 1000000;
    CHECK(dd >= 1490 && dd < 1700, "1500 ms ahead (%lld)", (long long)dd);
    CHECK(t.tv_nsec >= 0 && t.tv_nsec < 1000000000L, "nanoseconds in range");
    clock_gettime(CLOCK_REALTIME, &now);
    host_deadline_after_ms(&t, 999);   /* any carry of nanoseconds into seconds */
    dd = ((int64_t)t.tv_sec - now.tv_sec) * 1000 + (t.tv_nsec - now.tv_nsec) / 1000000;
    CHECK(dd >= 990 && dd < 1200 && t.tv_nsec >= 0 && t.tv_nsec < 1000000000L, "999 ms ahead (%lld)", (long long)dd);
    /* a fixed nanosecond field exercises the carry exactly */
    clock_gettime(CLOCK_REALTIME, &now);
    struct timespec u = t;
    u.tv_nsec = 0;
    host_deadline_after_ms(&u, 0);
    CHECK(u.tv_sec >= now.tv_sec && u.tv_nsec < 1000000000L && u.tv_nsec >= 0, "a deadline now");
    host_deadline_after_ms(&u, 2000);
    CHECK(u.tv_nsec >= 0 && u.tv_nsec < 1000000000L, "a whole number of seconds carries cleanly");
    return report("clock_test");
}
