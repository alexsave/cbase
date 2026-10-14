#include "lib_proc.h"

#include "util/clock.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

struct tp_pump {
    pthread_t       th;
    pthread_mutex_t mu;
    pthread_cond_t  cv;
    char           *log;     // every whole line so far
    size_t          len, cap, seen;   // seen: the offset after the last matched line
    bool            eof;
    int             fd;
};

static void *pump_run(void *arg) {
    tp_pump *p = arg;
    char part[65536];
    size_t n = 0;
    for (;;) {
        struct pollfd pf = { p->fd, POLLIN, 0 };
        if (poll(&pf, 1, 1000) < 0 && errno != EINTR) break;
        ssize_t r = read(p->fd, part + n, sizeof part - n);
        if (r < 0 && (errno == EAGAIN || errno == EINTR)) continue;
        if (r <= 0) break;
        n += (size_t)r;
        char *nl;
        size_t used = 0;
        while ((nl = memchr(part + used, '\n', n - used)) || n == sizeof part) {
            size_t ll = nl ? (size_t)(nl - (part + used)) + 1 : n - used;
            fprintf(stderr, "  | %.*s%s", (int)ll, part + used, nl ? "" : "\n");
            pthread_mutex_lock(&p->mu);
            if (p->len + ll + 1 > p->cap) {
                size_t cap = (p->cap ? p->cap * 2 : 65536) + ll;
                char *q = realloc(p->log, cap);
                if (q) { p->log = q; p->cap = cap; }
            }
            if (p->len + ll + 1 <= p->cap) {
                memcpy(p->log + p->len, part + used, ll);
                p->len += ll;
                p->log[p->len] = 0;
            }
            pthread_cond_broadcast(&p->cv);
            pthread_mutex_unlock(&p->mu);
            used += ll;
            if (!nl) break;
        }
        memmove(part, part + used, n - used);
        n -= used;
    }
    pthread_mutex_lock(&p->mu);
    p->eof = true;
    pthread_cond_broadcast(&p->cv);
    pthread_mutex_unlock(&p->mu);
    return NULL;
}

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
    c->pump = calloc(1, sizeof *c->pump);
    if (!c->pump) return -1;
    pthread_mutex_init(&c->pump->mu, NULL);
    pthread_cond_init(&c->pump->cv, NULL);
    c->pump->fd = p[0];
    return pthread_create(&c->pump->th, NULL, pump_run, c->pump) == 0 ? 0 : -1;
}

int tp_wait_line(tp_child *c, const char *needle, int timeout_ms, char *out, size_t cap) {
    tp_pump *p = c->pump;
    if (!p) return -1;
    int64_t end = cb_mono_ms() + timeout_ms;
    int rc = -1;
    pthread_mutex_lock(&p->mu);
    for (;;) {
        // Whole lines after the last match.
        char *line = p->log ? p->log + p->seen : NULL, *nl;
        while (line && (nl = strchr(line, '\n'))) {
            *nl = 0;
            bool hit = strstr(line, needle) != NULL;
            if (hit && out) snprintf(out, cap, "%s", line);
            *nl = '\n';
            p->seen = (size_t)(nl + 1 - p->log);
            if (hit) { rc = 0; break; }
            line = nl + 1;
        }
        if (rc == 0 || p->eof) break;
        int64_t left = end - cb_mono_ms();
        if (left <= 0) break;
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        int64_t ns = ts.tv_nsec + (left > 100 ? 100 : left) * 1000000;
        ts.tv_sec += ns / 1000000000;
        ts.tv_nsec = ns % 1000000000;
        pthread_cond_timedwait(&p->cv, &p->mu, &ts);
    }
    pthread_mutex_unlock(&p->mu);
    return rc;
}

int tp_stop(tp_child *c, int sig, int timeout_ms) {
    int status = 0, rc = -1;
    if (c->pid > 0) {
        if (sig) kill(c->pid, sig);
        int64_t end = cb_mono_ms() + timeout_ms;
        for (;;) {
            pid_t w = waitpid(c->pid, &status, WNOHANG);
            if (w == c->pid) { rc = WIFEXITED(status) ? WEXITSTATUS(status) : -1; break; }
            if (cb_mono_ms() > end) { kill(c->pid, SIGKILL); waitpid(c->pid, &status, 0); break; }
            usleep(10000);
        }
        c->pid = 0;
    }
    // The pump reads what is left of its stderr, to the end (a child the
    // caller reaped itself included).
    if (c->pump) {
        pthread_join(c->pump->th, NULL);
        pthread_mutex_destroy(&c->pump->mu);
        pthread_cond_destroy(&c->pump->cv);
        free(c->pump->log);
        free(c->pump);
        c->pump = NULL;
        close(c->err_fd);
    }
    if (c->dir[0]) {
        char cmd[300];
        snprintf(cmd, sizeof cmd, "rm -rf '%s'", c->dir);
        if (system(cmd) != 0) fprintf(stderr, "could not remove %s\n", c->dir);
    }
    return rc;
}
