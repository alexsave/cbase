// lib_proc.h - start a c_base binary as a child for the smoke tests.
#ifndef CB_TEST_PROC_H
#define CB_TEST_PROC_H

#include <sys/types.h>

// The child's stderr is read all the time by a thread of ours (echoed to our
// stderr as "  | <line>"), never only while a test waits for a line: a child
// that logs more than a pipe holds (64 KB) between two waits would otherwise
// block in write(2) and look hung.
typedef struct tp_pump tp_pump;
typedef struct {
    pid_t    pid;
    int      err_fd;   // the child's stderr (read by the pump)
    tp_pump *pump;
    char     dir[256]; // a fresh temporary directory for its files
} tp_child;

// What every child runs as, in its own directory (below).
#define TP_CHILD_NAME "tpchild"

// Starts `argv` with `env` ("NAME=value" strings, NULL-terminated) added to
// the current environment, stderr captured. 0, or -1.
//
// The child is a test's own: nothing outside the test may signal it. So it
// does not run as argv[0] but as $TMPDIR/tp-XXXXXX/tpchild, a hard link to it
// (a copy across file systems), with that path as its argv[0]. Its command
// line then never matches a pattern aimed at a hand-started binary: on a Mac
// where several agents work at once, `pkill -f "build/cbase serve"` (run to
// stop one server by hand) SIGTERMed the game smoke's server mid-game twice,
// and the smoke failed 14 checks against a server that had drained and gone.
// Nor does its process name ("tpchild") match `pkill cbase` or `killall cbase`.
// So argv[0] must be a binary of ours (a system one such as /bin/sh may not
// run from a copy): a test that needs a scripted child runs its own binary.
int tp_start(tp_child *c, char *const argv[], char *const env[]);
// Waits until a line containing `needle` appears in the child's stderr after
// the line the previous wait matched; copies that line into out. 0, or -1 on
// timeout or once the child has closed its stderr.
int tp_wait_line(tp_child *c, const char *needle, int timeout_ms, char *out, size_t cap);
// Sends `sig` (0: none, just wait for it to exit) and waits up to timeout_ms; returns the exit status, or -1
// (the child is then killed).
int tp_stop(tp_child *c, int sig, int timeout_ms);
// A fresh temporary directory (removed by tp_stop with its contents).
int tp_tempdir(char *out, size_t cap);

#endif
