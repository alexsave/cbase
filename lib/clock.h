/* clock.h - the host clocks, header-only: wall time, monotonic time, CPU time
 * and a sleep, each in the unit the caller wants (ns, us, ms, or seconds as a
 * double), and the absolute CLOCK_REALTIME deadline a pthread_cond_timedwait
 * takes.
 *
 * HOST ONLY. It needs POSIX clock_gettime, so a kernel never includes it: a
 * kernel takes now_ms from its host (that is a rule, not an accident). A
 * wasm build has no use for it, and the iOS and Android hosts have their own
 * clocks. Tools, benches, tests and the servers use this one.
 *
 * Under -std=c11 on glibc, clock_gettime is hidden: define _POSIX_C_SOURCE
 * 200809L (or _GNU_SOURCE) before the first system include, as the callers do. */
#ifndef SHARED_CLOCK_H
#define SHARED_CLOCK_H

#include <errno.h>
#include <stdint.h>
#include <time.h>

static inline int64_t host_clock_ns(clockid_t id)
{
    struct timespec ts;
    clock_gettime(id, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

/* Monotonic: never jumps; for durations and timers. */
static inline int64_t host_mono_ns(void) { return host_clock_ns(CLOCK_MONOTONIC); }
static inline int64_t host_mono_us(void) { return host_mono_ns() / 1000; }
static inline int64_t host_mono_ms(void) { return host_mono_ns() / 1000000; }
static inline double  host_mono_s(void)  { return (double)host_mono_ns() * 1e-9; }
static inline double  host_mono_ms_f(void) { return (double)host_mono_ns() * 1e-6; }

/* Wall clock since the Unix epoch. */
static inline int64_t host_real_ms(void) { return host_clock_ns(CLOCK_REALTIME) / 1000000; }

/* CPU time of the calling thread, and of the whole process. */
static inline int64_t host_thread_cpu_ms(void) { return host_clock_ns(CLOCK_THREAD_CPUTIME_ID) / 1000000; }
static inline double  host_thread_cpu_ms_f(void) { return (double)host_clock_ns(CLOCK_THREAD_CPUTIME_ID) * 1e-6; }
static inline int64_t host_process_cpu_ns(void) { return host_clock_ns(CLOCK_PROCESS_CPUTIME_ID); }
static inline double  host_process_cpu_s(void) { return (double)host_process_cpu_ns() * 1e-9; }

/* Sleep ms milliseconds (nothing for ms <= 0), resuming after a signal. */
static inline void host_sleep_ms(int64_t ms)
{
    if (ms <= 0) return;
    struct timespec ts = { (time_t)(ms / 1000), (long)(ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
}

/* *t = now (CLOCK_REALTIME) + ms: the absolute deadline pthread_cond_timedwait
 * and sem_timedwait take. */
static inline void host_deadline_after_ms(struct timespec *t, int64_t ms)
{
    clock_gettime(CLOCK_REALTIME, t);
    t->tv_sec += (time_t)(ms / 1000);
    t->tv_nsec += (long)(ms % 1000) * 1000000L;
    if (t->tv_nsec >= 1000000000L) { t->tv_sec++; t->tv_nsec -= 1000000000L; }
}

#endif
