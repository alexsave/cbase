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
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static const char *g_self;   // this binary as it was started (argv[0])

// main's --chatty: 4 MB of stderr (more than any pipe buffer, macOS's grown
// ones included), then "WROTE ALL" only when every byte went out (a write
// refused or cut short says "WRITE FAILED"), then a last line, then exit.
static int chatty(void) {
    static char x[65536];
    memset(x, 'x', sizeof x);
    bool all = true;
    for (size_t left = 4000000; left > 0 && all;) {
        size_t n = left < sizeof x ? left : sizeof x;
        all = write(2, x, n) == (ssize_t)n;
        left -= n;
    }
    fprintf(stderr, "\n%s\nLAST\n", all ? "WROTE ALL" : "WRITE FAILED");
    return 0;
}

static void a_chatty_child_never_blocks(void) {
    // What the pump echoes to our stderr goes to a file meanwhile, to be measured.
    char echo_path[256];
    T_EQ_INT(tp_tempdir(echo_path, sizeof echo_path - 16), 0);
    strcat(echo_path, "/echo");
    fflush(stderr);
    int saved = dup(2), ef = open(echo_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    T_CHECK(saved >= 0 && ef >= 0);
    dup2(ef, 2);
    close(ef);
    // This binary as the child (tp_start runs only binaries of ours: lib_proc.h).
    char *argv[] = { (char *)g_self, "--chatty", NULL };
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
    fflush(stderr);
    dup2(saved, 2);
    close(saved);
    // The 4 MB line was echoed cut (lib_proc.c TP_ECHO_MAX), not whole: a
    // child cannot flood a test's log.
    struct stat st;
    T_EQ_INT(stat(echo_path, &st), 0);
    if (st.st_size >= 4096) fprintf(stderr, "     the pump echoed %lld bytes\n", (long long)st.st_size);
    T_CHECK(st.st_size < 4096);
    unlink(echo_path);
    *strrchr(echo_path, '/') = 0;
    rmdir(echo_path);
}

// ---- a child nothing outside the test can name ----------------------------------

// The pids `pgrep <args>` lists, as one " 12 34 " string (empty: none).
static void pgrep_pids(const char *args, char *out, size_t cap) {
    char cmd[1024], line[64];
    snprintf(cmd, sizeof cmd, "pgrep %s", args);
    snprintf(out, cap, " ");
    FILE *f = popen(cmd, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        size_t n = strlen(out);
        snprintf(out + n, cap - n, "%d ", atoi(line));
    }
    pclose(f);
}

static void a_child_is_out_of_reach_of_a_pattern_kill(void) {
    // This binary started again as a child that only sleeps (main's --sleep).
    char *argv[] = { (char *)g_self, "--sleep", NULL };
    tp_child c;
    T_EQ_INT(tp_start(&c, argv, NULL), 0);
    T_EQ_INT(tp_wait_line(&c, "sleeping", 5000, NULL, 0), 0);
    char me[32], found[4096], pattern[1024];
    snprintf(me, sizeof me, " %d ", (int)c.pid);
    // What another agent runs to stop "its" server: a pattern on the path the
    // binary was built at, and on the binary's name. Neither reaches the child.
    snprintf(pattern, sizeof pattern, "-f '%s --sleep'", g_self);
    pgrep_pids(pattern, found, sizeof found);
    if (strstr(found, me)) fprintf(stderr, "     pgrep %s listed the child %d:%s\n", pattern, (int)c.pid, found);
    T_CHECK(strstr(found, me) == NULL);
    const char *name = strrchr(g_self, '/') ? strrchr(g_self, '/') + 1 : g_self;
    snprintf(pattern, sizeof pattern, "'%s'", name);
    pgrep_pids(pattern, found, sizeof found);
    T_CHECK(strstr(found, me) == NULL);
    // The pgrep itself works: the child IS listed under the name it runs as.
    pgrep_pids("-x " TP_CHILD_NAME, found, sizeof found);
    T_CHECK(strstr(found, me) != NULL);
    tp_stop(&c, SIGTERM, 5000);
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--chatty") == 0) return chatty();
    if (argc == 2 && strcmp(argv[1], "--sleep") == 0) {
        fprintf(stderr, "sleeping\n");
        struct timespec ts = { 30, 0 };
        nanosleep(&ts, NULL);
        return 0;
    }
    g_self = argv[0];
    T_RUN(a_chatty_child_never_blocks);
    T_RUN(a_child_is_out_of_reach_of_a_pattern_kill);
    return T_DONE();
}
