/* proc_test.c - proc.h: this process by 0 and by its pid, a reading that moves when CPU is
 * burned, and a pid nobody has.
 *
 * Exits 1 on any failure. */
#include <stdint.h>
#include <unistd.h>
#include "test/check.h"
#include "proc.h"

int main(void)
{
    TEST("proc: the resident set");
    uint32_t rss = proc_rss_mb(0);
    CHECK(rss >= 1 && rss < 4096, "this process: at least a megabyte, under four gigabytes");
    /* Our own pid by number reads the same process (within a few MB of churn). */
    uint32_t by_pid = proc_rss_mb((int)getpid());
    CHECK(by_pid >= 1 && (by_pid > rss ? by_pid - rss : rss - by_pid) < 64, "by pid: the same process");
    TEST("proc: CPU time");
    uint64_t cpu0 = proc_cpu_us(0);
    volatile double x = 1;
    for (int i = 0; i < 20000000; i++) x = x * 1.0000001 + 0.5;
    uint64_t cpu1 = proc_cpu_us(0);
    CHECK(cpu1 > cpu0 && cpu1 - cpu0 >= 5000, "the reading moved by at least 5 ms of work");
    uint64_t by_pid_cpu = proc_cpu_us((int)getpid());
    CHECK(by_pid_cpu + 200000 >= cpu1 && by_pid_cpu <= cpu1 + 200000, "by pid: within 200 ms of the self reading");
    TEST("proc: a pid nobody has");
    CHECK(proc_rss_mb(2147483000) == 0, "rss 0, not a crash and not somebody else's number");
    CHECK(proc_cpu_us(2147483000) == 0, "cpu 0");
    return report("proc");
}
