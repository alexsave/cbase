/* proc.h - what a process looks like from outside: its resident set and its CPU time, by pid,
 * for a server's own numbers and for a harness watching the server it started.
 *
 * macOS: mach task_info for this process, proc_pidinfo for another. Linux: getrusage for this
 * process, /proc/<pid>/statm and /proc/<pid>/stat for another (the fields parsed with
 * num_parse.h, the one number reader). Both answer 0 when they cannot read (no such process,
 * no right to ask), never a guess. */
#ifndef SHARED_PROC_H
#define SHARED_PROC_H

#include <stdint.h>

/* A process's resident set now, in MB: pid 0 is this process; 0 when it cannot be read. */
uint32_t proc_rss_mb(int pid);
/* A process's CPU time so far (user + system), in microseconds; 0 when it cannot be read. */
uint64_t proc_cpu_us(int pid);

#endif
