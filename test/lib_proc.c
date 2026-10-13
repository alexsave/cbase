#include "lib_proc.h"

#include "util/clock.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

int tp_tempdir(char *out, size_t cap) {
    const char *base = getenv("TMPDIR");
    snprintf(out, cap, "%s/cbase-test-XXXXXX", base && *base ? base : "/tmp");
    return mkdtemp(out) ? 0 : -1;
}

int tp_start(tp_child *c, char *const argv[], char *const env[]) {
    memset(c, 0, sizeof *c);
    if (tp_tempdir(c->dir, sizeof c->dir)) return -1;
    int p[2];
    if (pipe(p)) return -1;
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        dup2(p[1], 2);
        close(p[0]);
        close(p[1]);
        for (int i = 0; env && env[i]; i++) putenv(env[i]);
        execv(argv[0], argv);
        fprintf(stderr, "exec %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    close(p[1]);
    fcntl(p[0], F_SETFL, O_NONBLOCK);
    c->pid = pid;
    c->err_fd = p[0];
    return 0;
}

int tp_wait_line(tp_child *c, const char *needle, int timeout_ms, char *out, size_t cap) {
    static char acc[65536];
    static size_t n;
    int64_t end = cb_mono_ms() + timeout_ms;
    for (;;) {
        char *nl;
        while ((nl = memchr(acc, '\n', n))) {
            size_t len = (size_t)(nl - acc);
            fprintf(stderr, "  | %.*s\n", (int)len, acc);
            int hit = 0;
            *nl = 0;
            if (strstr(acc, needle)) {
                hit = 1;
                if (out) snprintf(out, cap, "%s", acc);
            }
            memmove(acc, nl + 1, n - len - 1);
            n -= len + 1;
            if (hit) return 0;
        }
        int64_t left = end - cb_mono_ms();
        if (left <= 0) return -1;
        struct pollfd p = { c->err_fd, POLLIN, 0 };
        if (poll(&p, 1, (int)left) <= 0) return -1;
        ssize_t r = read(c->err_fd, acc + n, sizeof acc - n - 1);
        if (r <= 0) return -1;
        n += (size_t)r;
    }
}

int tp_stop(tp_child *c, int sig, int timeout_ms) {
    int status = 0, rc = -1;
    if (c->pid > 0) {
        kill(c->pid, sig);
        int64_t end = cb_mono_ms() + timeout_ms;
        for (;;) {
            pid_t w = waitpid(c->pid, &status, WNOHANG);
            if (w == c->pid) { rc = WIFEXITED(status) ? WEXITSTATUS(status) : -1; break; }
            if (cb_mono_ms() > end) { kill(c->pid, SIGKILL); waitpid(c->pid, &status, 0); break; }
            usleep(10000);
        }
        // Drain what is left of its stderr, for the log.
        char b[4096];
        ssize_t r;
        while ((r = read(c->err_fd, b, sizeof b)) > 0) fprintf(stderr, "  | %.*s", (int)r, b);
        close(c->err_fd);
        c->pid = 0;
    }
    if (c->dir[0]) {
        char cmd[300];
        snprintf(cmd, sizeof cmd, "rm -rf '%s'", c->dir);
        if (system(cmd) != 0) fprintf(stderr, "could not remove %s\n", c->dir);
    }
    return rc;
}
