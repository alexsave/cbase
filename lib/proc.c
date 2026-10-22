/* proc.c - proc.h. */
#include "proc.h"

#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>
#ifdef __APPLE__
#include <libproc.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#else
#include "num_parse.h"
#endif

#ifndef __APPLE__
/* The first cap-1 bytes of a small file (a /proc entry), NUL-terminated: how many were read. */
static size_t read_small(const char *path, char *buf, size_t cap)
{
    FILE *f = fopen(path, "r");
    if (!f) { buf[0] = 0; return 0; }
    size_t n = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[n] = 0;
    return n;
}

/* Field `want` (0-based) of a space-separated line, as a non-negative integer; -1 when it is
 * not there or not a number. */
static int64_t field_i64(const char *s, size_t n, int want)
{
    size_t i = 0;
    for (int f = 0;; f++) {
        while (i < n && s[i] == ' ') i++;
        size_t start = i;
        while (i < n && s[i] != ' ' && s[i] != '\n') i++;
        if (i == start) return -1;
        if (f == want) {
            int64_t v;
            return num_parse_i64(s + start, i - start, 0, INT64_MAX, &v) ? v : -1;
        }
    }
}

static const char *last_byte(const char *s, char c, size_t n)
{
    while (n--) if (s[n] == c) return s + n;
    return NULL;
}
#endif

uint32_t proc_rss_mb(int pid)
{
#ifdef __APPLE__
    if (pid == 0) {
        struct mach_task_basic_info info;
        mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
        if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info, &count) != KERN_SUCCESS) return 0;
        return (uint32_t)(info.resident_size >> 20);
    }
    struct proc_taskinfo ti;
    if (proc_pidinfo(pid, PROC_PIDTASKINFO, 0, &ti, sizeof ti) != (int)sizeof ti) return 0;
    return (uint32_t)(ti.pti_resident_size >> 20);
#else
    char path[64], buf[256];
    snprintf(path, sizeof path, "/proc/%d/statm", pid == 0 ? (int)getpid() : pid);
    size_t n = read_small(path, buf, sizeof buf);
    /* "size resident shared ...": the second field, in pages. */
    int64_t resident = field_i64(buf, n, 1);
    if (resident < 0) return 0;
    long page = sysconf(_SC_PAGESIZE);
    return (uint32_t)(((uint64_t)resident * (uint64_t)(page > 0 ? page : 4096)) >> 20);
#endif
}

static uint64_t self_cpu_us(void)
{
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) != 0) return 0;
    return (uint64_t)ru.ru_utime.tv_sec * 1000000 + (uint64_t)ru.ru_utime.tv_usec + (uint64_t)ru.ru_stime.tv_sec * 1000000 +
           (uint64_t)ru.ru_stime.tv_usec;
}

uint64_t proc_cpu_us(int pid)
{
    if (pid == 0) return self_cpu_us();
#ifdef __APPLE__
    struct proc_taskinfo ti;
    if (proc_pidinfo(pid, PROC_PIDTASKINFO, 0, &ti, sizeof ti) != (int)sizeof ti) return 0;
    /* Mach absolute time: nanoseconds on Apple silicon (timebase 1/1); the timebase is applied
     * for an Intel Mac, where it is not. */
    mach_timebase_info_data_t tb;
    if (mach_timebase_info(&tb) != KERN_SUCCESS || tb.denom == 0) return 0;
    uint64_t ns = (ti.pti_total_user + ti.pti_total_system) / tb.denom * tb.numer;
    return ns / 1000;
#else
    char path[64], buf[1024];
    snprintf(path, sizeof path, "/proc/%d/stat", pid);
    size_t n = read_small(path, buf, sizeof buf);
    /* "pid (comm) state ppid ...": the comm may hold spaces and parentheses, so the fields are
     * counted from the last ')'. After it: state ppid pgrp session tty tpgid flags minflt
     * cminflt majflt cmajflt utime stime: utime is the 12th field after the ')', stime the 13th. */
    const char *p = n ? last_byte(buf, ')', n) : NULL;
    if (!p) return 0;
    size_t off = (size_t)(p + 1 - buf);
    int64_t ut = field_i64(buf + off, n - off, 11), st = field_i64(buf + off, n - off, 12);
    if (ut < 0 || st < 0) return 0;
    long hz = sysconf(_SC_CLK_TCK);
    return (uint64_t)(ut + st) * 1000000 / (uint64_t)(hz > 0 ? hz : 100);
#endif
}
