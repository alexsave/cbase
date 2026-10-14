// unit_proc.c - the smoke tests' child process (lib_proc.h): its stderr is
// read all the time, not only while a test waits for a line.
//
// A server at debug level writes more than a pipe holds between two waits;
// read only during waits, its writes were refused or blocked mid-game and the
// smoke saw a server that stopped answering (found running the game smoke
// against DynamoDB Local with CBASE_LOG_LEVEL=debug).
#include "t.h"

#include "lib_proc.h"
#include "util/clock.h"

#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static void a_chatty_child_never_blocks(void) {
    // 4 MB of stderr (more than any pipe buffer, macOS's grown ones included),
    // then a last line, then exit.
    // "WROTE ALL" only when every byte went out (a write refused or cut
    // short makes tr fail).
    char *argv[] = { "/bin/sh", "-c",
                     "if head -c 4000000 /dev/zero | tr '\\0' x >&2; then echo >&2; echo 'WROTE ALL' >&2; "
                     "else echo >&2; echo 'WRITE FAILED' >&2; fi; echo LAST >&2", NULL };
    tp_child c;
    T_EQ_INT(tp_start(&c, argv, NULL), 0);
    // Nobody waits for a line: the child must still get to its end and exit,
    // within 3 s of wall time (it needs well under one).
    int status = 0;
    pid_t w = 0;
    const int64_t end = cb_mono_ms() + 3000;
    while (w == 0 && cb_mono_ms() < end) {
        w = waitpid(c.pid, &status, WNOHANG);
        if (w == 0) {
            struct timespec ts = { 0, 10 * 1000000 };
            nanosleep(&ts, NULL);
        }
    }
    T_EQ_INT(w, c.pid);
    if (w != c.pid) kill(c.pid, SIGKILL);
    // Every byte went out, and its last line was captured.
    T_EQ_INT(tp_wait_line(&c, "WROTE ALL", 5000, NULL, 0), 0);
    T_EQ_INT(tp_wait_line(&c, "LAST", 5000, NULL, 0), 0);
    c.pid = w == c.pid ? 0 : c.pid;   // reaped: tp_stop must not wait for it again
    tp_stop(&c, 0, 1000);
}

int main(void) {
    T_RUN(a_chatty_child_never_blocks);
    return T_DONE();
}
