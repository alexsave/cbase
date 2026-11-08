// spawn.c - child processes on the loop (spawn.h).
//
// A child's life, all on the loop thread but for cb_spawn's copy:
//   cb_spawn       copies the options into a `child`, lists it, posts its start
//   child_start    pipes, fork, the exec (or its failure, read back over a close-on-exec pipe),
//                  the parent's ends on the loop, the timeout armed
//   on_out ...     stdout, stderr and fd 3 read; stdin and fd 3 written
//   on_sigchld_fd  the self-pipe: each listed leader that is a zombie has its group killed, then
//                  is reaped
//   maybe_finish   reaped and every pipe at its end: done runs, the child is freed
#include "spawn.h"

#include "net/loop.h"
#include "util/clock.h"
#include "util/err.h"
#include "util/log.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#include <sys/syscall.h>
#endif

#define MOD "spawn"

extern char **environ;

// ---- the SIGCHLD self-pipes ---------------------------------------------------------------------
// One handler for the process; it writes a byte to every live spawner's pipe (async-signal-safe:
// atomic loads and write(2)), and calls the handler that was there before it.

#define WAKE_SLOTS 16
static atomic_int g_wake[WAKE_SLOTS];   // a pipe's write end + 1; 0: free
static struct sigaction g_old_chld;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

static void on_sigchld(int sig, siginfo_t *si, void *uc) {
    int saved = errno;
    for (int i = 0; i < WAKE_SLOTS; i++) {
        int f = atomic_load(&g_wake[i]);
        if (f > 0) {
            char b = 1;
            ssize_t r = write(f - 1, &b, 1);
            (void)r;
        }
    }
    if (g_old_chld.sa_flags & SA_SIGINFO) {
        if (g_old_chld.sa_sigaction) g_old_chld.sa_sigaction(sig, si, uc);
    } else if (g_old_chld.sa_handler != SIG_DFL && g_old_chld.sa_handler != SIG_IGN) {
        g_old_chld.sa_handler(sig);
    }
    errno = saved;
}

static void install_handlers(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_sigchld;
    sa.sa_flags = SA_SIGINFO | SA_RESTART | SA_NOCLDSTOP;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGCHLD, &sa, &g_old_chld);
#ifndef F_SETNOSIGPIPE
    // A write to a child's stdin after it closed it raises SIGPIPE, and a pipe cannot be told not to
    // here: ignored, unless the process already chose (a server ignores it anyway).
    struct sigaction cur;
    if (sigaction(SIGPIPE, NULL, &cur) == 0 && !(cur.sa_flags & SA_SIGINFO) && cur.sa_handler == SIG_DFL)
        signal(SIGPIPE, SIG_IGN);
#endif
}

// ---- a child ------------------------------------------------------------------------------------

typedef struct {
    char  *p;
    size_t n, cap, limit;
} obuf;

typedef struct child {
    struct child *next;
    cb_spawner   *sp;
    uint64_t      id;
    // what cb_spawn copied
    char        **argv;
    char        **envp;
    char         *cwd;
    unsigned char *in;
    size_t        in_n, in_off;
    int64_t       timeout_ms, grace_ms;
    bool          channel;
    size_t        line_max;
    cb_spawn_line_fn on_line;
    cb_spawn_done_fn done;
    void         *ud;
    // the run
    bool          started;
    pid_t         pid;
    int64_t       t0;
    int           fd_in, fd_out, fd_err, fd_chan;
    obuf          out, err;
    char         *line;        // line_max + 1 bytes
    size_t        line_n;
    bool          chan_dead;   // an overlong line: nothing more delivered or sent
    int           chan_rc;
    char         *wbuf;        // lines waiting to be written on fd 3
    size_t        wn, woff, wcap;
    int           chan_events;
    bool          reaped;
    int           exit_code, term_signal;
    cb_spawn_why  killed;
    bool          term_sent;
    uint64_t      t_timeout, t_kill, t_drain;
} child;

struct cb_spawner {
    cb_loop        *loop;
    pthread_mutex_t mu;        // the list: cb_spawn adds from any thread
    child          *head;
    int             n;
    atomic_ullong   next_id;
    atomic_bool     stopped;
    int             wake[2];
    int             slot;
};

const char *cb_spawn_why_name(cb_spawn_why w) {
    switch (w) {
    case CB_SPAWN_NOT_KILLED: return "not killed";
    case CB_SPAWN_TIMEOUT:    return "timeout";
    case CB_SPAWN_OUT_CAP:    return "stdout cap";
    case CB_SPAWN_ERR_CAP:    return "stderr cap";
    case CB_SPAWN_KILLED:     return "killed by the caller";
    case CB_SPAWN_SHUTDOWN:   return "shutdown";
    }
    return "?";
}

static void close_fd(int *fd) {
    if (*fd >= 0) close(*fd);
    *fd = -1;
}

static void strv_free(char **v) {
    for (char **p = v; p && *p; p++) free(*p);
    free(v);
}

static void child_free(child *c) {
    strv_free(c->argv);
    strv_free(c->envp);
    free(c->cwd);
    free(c->in);
    free(c->out.p);
    free(c->err.p);
    free(c->line);
    free(c->wbuf);
    free(c);
}

static void unlist(cb_spawner *sp, child *c) {
    pthread_mutex_lock(&sp->mu);
    for (child **pp = &sp->head; *pp; pp = &(*pp)->next)
        if (*pp == c) {
            *pp = c->next;
            sp->n--;
            break;
        }
    pthread_mutex_unlock(&sp->mu);
}

static child *find(cb_spawner *sp, uint64_t id) {
    pthread_mutex_lock(&sp->mu);
    child *c = sp->head;
    while (c && c->id != id) c = c->next;
    pthread_mutex_unlock(&sp->mu);
    return c;
}

static int rc_of_errno(int e) {
    switch (e) {
    case ENOENT: case ENOTDIR: return CB_E_NOT_FOUND;
    case EACCES: case EPERM:   return CB_E_FORBIDDEN;
    case ENOMEM:               return CB_E_NOMEM;
    default:                   return CB_E_IO;
    }
}

// Runs done and frees the child. Every fd is closed and off the loop, every timer cancelled.
static void finish(child *c, int rc, const char *why) {
    cb_spawner *sp = c->sp;
    int *fds[] = { &c->fd_in, &c->fd_out, &c->fd_err, &c->fd_chan };
    for (size_t i = 0; i < sizeof fds / sizeof fds[0]; i++)
        if (*fds[i] >= 0) {
            cb_loop_remove(sp->loop, *fds[i]);
            close_fd(fds[i]);
        }
    uint64_t *ts[] = { &c->t_timeout, &c->t_kill, &c->t_drain };
    for (size_t i = 0; i < sizeof ts / sizeof ts[0]; i++)
        if (*ts[i]) { cb_loop_cancel(sp->loop, *ts[i]); *ts[i] = 0; }
    cb_spawn_result r;
    memset(&r, 0, sizeof r);
    r.rc = rc;
    r.exit_code = rc == CB_OK ? c->exit_code : -1;
    r.term_signal = rc == CB_OK ? c->term_signal : 0;
    r.killed = c->killed;
    r.out = c->out.p ? c->out.p : "";
    r.out_n = c->out.n;
    r.err = c->err.p ? c->err.p : "";
    r.err_n = c->err.n;
    r.chan_rc = c->chan_rc;
    r.pid = c->started ? (int)c->pid : 0;
    r.ms = c->started ? cb_mono_ms() - c->t0 : 0;
    snprintf(r.why, sizeof r.why, "%s", why ? why : "");
    if (rc != CB_OK) CB_WARN(MOD, "%s: %s", c->argv[0], r.why);
    else if (c->killed) CB_INFO(MOD, "%s (pid %d): killed (%s)", c->argv[0], (int)c->pid, cb_spawn_why_name(c->killed));
    unlist(sp, c);
    if (c->done) c->done(&r, c->ud);
    child_free(c);
}

static void maybe_finish(child *c) {
    if (c->reaped && c->fd_out < 0 && c->fd_err < 0 && c->fd_chan < 0) finish(c, CB_OK, NULL);
}

// ---- killing ------------------------------------------------------------------------------------
// The group is signalled only while its leader is unreaped (alive or a zombie): its pid, which is
// the group's id, cannot have been handed to anyone else.

static void on_kill_timer(cb_loop *l, void *ud) {
    child *c = ud;
    c->t_kill = 0;
    if (!c->reaped) kill(-c->pid, SIGKILL);
}

static void kill_start(child *c, cb_spawn_why why) {
    if (!c->killed) c->killed = why;
    if (c->reaped || c->term_sent) return;
    c->term_sent = true;
    kill(-c->pid, SIGTERM);
    c->t_kill = cb_loop_timer(c->sp->loop, c->grace_ms, on_kill_timer, c);
}

static void on_timeout(cb_loop *l, void *ud) {
    child *c = ud;
    c->t_timeout = 0;
    kill_start(c, CB_SPAWN_TIMEOUT);
}

// ---- reading and writing ------------------------------------------------------------------------

#define READ_ROUNDS 16   // reads per wake per pipe: a flooding child shares the loop

// Keeps what fits under the cap; false when bytes past it arrived.
static bool obuf_put(obuf *b, const char *p, size_t n) {
    size_t room = b->limit - b->n;
    size_t take = n < room ? n : room;
    if (take) {
        if (b->n + take + 1 > b->cap) {
            size_t cap = b->cap ? b->cap : 4096;
            while (cap < b->n + take + 1) cap *= 2;
            if (cap > b->limit + 1) cap = b->limit + 1;
            char *q = realloc(b->p, cap);
            if (!q) return false;
            b->p = q;
            b->cap = cap;
        }
        memcpy(b->p + b->n, p, take);
        b->n += take;
        b->p[b->n] = 0;
    }
    return take == n;
}

static void close_watched(child *c, int *fd) {
    cb_loop_remove(c->sp->loop, *fd);
    close_fd(fd);
}

static void on_out(cb_loop *l, int fd, int ev, void *ud) {
    child *c = ud;
    bool is_out = fd == c->fd_out;
    obuf *b = is_out ? &c->out : &c->err;
    char tmp[16384];
    for (int round = 0; round < READ_ROUNDS; round++) {
        ssize_t r = read(fd, tmp, sizeof tmp);
        if (r > 0) {
            if (!obuf_put(b, tmp, (size_t)r)) kill_start(c, is_out ? CB_SPAWN_OUT_CAP : CB_SPAWN_ERR_CAP);
            continue;
        }
        if (r < 0 && errno == EINTR) continue;
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        close_watched(c, is_out ? &c->fd_out : &c->fd_err);   // the end, or an error that is one
        maybe_finish(c);
        return;
    }
}

static void on_in(cb_loop *l, int fd, int ev, void *ud) {
    child *c = ud;
    while (c->in_off < c->in_n) {
        ssize_t w = write(fd, c->in + c->in_off, c->in_n - c->in_off);
        if (w > 0) { c->in_off += (size_t)w; continue; }
        if (w < 0 && errno == EINTR) continue;
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        break;   // EPIPE: the child closed its stdin, and what it did not read is dropped
    }
    close_watched(c, &c->fd_in);
}

static void chan_watch(child *c, int events) {
    if (c->fd_chan < 0 || c->chan_events == events) return;
    if (cb_loop_set(c->sp->loop, c->fd_chan, events) == CB_OK) c->chan_events = events;
}

static void chan_flush(child *c) {
    while (c->fd_chan >= 0 && c->woff < c->wn) {
#ifdef MSG_NOSIGNAL
        ssize_t w = send(c->fd_chan, c->wbuf + c->woff, c->wn - c->woff, MSG_NOSIGNAL);
#else
        ssize_t w = send(c->fd_chan, c->wbuf + c->woff, c->wn - c->woff, 0);   // SO_NOSIGPIPE is set
#endif
        if (w > 0) { c->woff += (size_t)w; continue; }
        if (w < 0 && errno == EINTR) continue;
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) { chan_watch(c, CB_EV_READ | CB_EV_WRITE); return; }
        c->woff = c->wn;   // the child no longer reads fd 3: what waits is dropped
    }
    c->woff = c->wn = 0;
    chan_watch(c, CB_EV_READ);
}

static void chan_overlong(child *c) {
    CB_WARN(MOD, "%s (pid %d): a line on fd 3 is longer than %zu bytes: the channel is closed", c->argv[0], (int)c->pid,
            c->line_max);
    c->chan_dead = true;
    c->chan_rc = CB_E_FULL;
    c->line_n = 0;
    c->woff = c->wn = 0;
    chan_watch(c, CB_EV_READ);
}

static void deliver(child *c) {
    c->line[c->line_n] = 0;
    size_t n = c->line_n;
    c->line_n = 0;
    if (c->on_line) c->on_line(c->sp, c->id, c->line, n, c->ud);
}

// The lines in p[0..n): each complete one delivered, the rest kept for the next read.
static void chan_take(child *c, const char *p, size_t n) {
    while (n > 0 && !c->chan_dead) {
        const char *nl = memchr(p, '\n', n);
        size_t seg = nl ? (size_t)(nl - p) : n;
        if (c->line_n + seg > c->line_max) { chan_overlong(c); return; }
        memcpy(c->line + c->line_n, p, seg);
        c->line_n += seg;
        if (!nl) return;
        deliver(c);
        p += seg + 1;
        n -= seg + 1;
    }
}

static void on_chan(cb_loop *l, int fd, int ev, void *ud) {
    child *c = ud;
    if ((ev & CB_EV_WRITE) && c->wn > c->woff) chan_flush(c);
    if (!(ev & (CB_EV_READ | CB_EV_ERR))) return;
    char tmp[16384];
    for (int round = 0; round < READ_ROUNDS; round++) {
        ssize_t r = recv(fd, tmp, sizeof tmp, 0);
        if (r > 0) { chan_take(c, tmp, (size_t)r); continue; }
        if (r < 0 && errno == EINTR) continue;
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        if (!c->chan_dead && c->line_n > 0) deliver(c);   // the last line, without its '\n'
        close_watched(c, &c->fd_chan);
        maybe_finish(c);
        return;
    }
}

// ---- the end of a leader ------------------------------------------------------------------------

static void on_drain_timer(cb_loop *l, void *ud) {
    child *c = ud;
    c->t_drain = 0;
    // A descendant that left the group holds a pipe open: what it has written so far is the output.
    if (c->fd_out >= 0) close_watched(c, &c->fd_out);
    if (c->fd_err >= 0) close_watched(c, &c->fd_err);
    if (c->fd_chan >= 0) close_watched(c, &c->fd_chan);
    maybe_finish(c);
}

// Is the leader done (a zombie, or reaped by someone else)? Its group is killed BEFORE it is reaped.
static bool leader_exited(pid_t pid, bool *lost) {
    siginfo_t si;
    memset(&si, 0, sizeof si);
    *lost = false;
    if (waitid(P_PID, (id_t)pid, &si, WEXITED | WNOHANG | WNOWAIT) != 0) {
        if (errno == ECHILD) { *lost = true; return true; }
        return false;
    }
    return si.si_pid == pid;
}

static void reap(child *c, bool lost) {
    if (!lost) {
        kill(-c->pid, SIGKILL);   // what is left of its group
        int st = 0;
        pid_t w;
        do w = waitpid(c->pid, &st, 0); while (w < 0 && errno == EINTR);
        if (w == c->pid && WIFEXITED(st)) c->exit_code = WEXITSTATUS(st), c->term_signal = 0;
        else if (w == c->pid && WIFSIGNALED(st)) c->exit_code = -1, c->term_signal = WTERMSIG(st);
        else lost = true;
    }
    if (lost) {
        // Another part of the process waited for it (waitpid(-1)): its status is gone with that.
        CB_WARN(MOD, "%s (pid %d): reaped elsewhere, its exit status is not known", c->argv[0], (int)c->pid);
        c->exit_code = -1;
        c->term_signal = 0;
    }
    c->reaped = true;
    if (c->t_timeout) { cb_loop_cancel(c->sp->loop, c->t_timeout); c->t_timeout = 0; }
    if (c->t_kill) { cb_loop_cancel(c->sp->loop, c->t_kill); c->t_kill = 0; }
    if (c->fd_in >= 0) close_watched(c, &c->fd_in);
}

// Every started, unreaped child of the spawner: up to max of them, into out.
static int list_children(cb_spawner *sp, child **out, int max, bool started_only) {
    int n = 0;
    pthread_mutex_lock(&sp->mu);
    for (child *c = sp->head; c && n < max; c = c->next)
        if (!started_only || c->started) out[n++] = c;
    pthread_mutex_unlock(&sp->mu);
    return n;
}

static void on_sigchld_fd(cb_loop *l, int fd, int ev, void *ud) {
    cb_spawner *sp = ud;
    char b[64];
    while (read(fd, b, sizeof b) > 0) {}
    child *cs[CB_SPAWN_MAX];
    int n = list_children(sp, cs, CB_SPAWN_MAX, true);
    for (int i = 0; i < n; i++) {
        child *c = cs[i];
        bool lost;
        if (c->reaped || !leader_exited(c->pid, &lost)) continue;
        reap(c, lost);
        if (c->fd_out >= 0 || c->fd_err >= 0 || c->fd_chan >= 0)
            c->t_drain = cb_loop_timer(sp->loop, c->grace_ms, on_drain_timer, c);
        maybe_finish(c);
    }
}

// ---- the start ----------------------------------------------------------------------------------

// The value of NAME in a "NAME=value" list, or NULL.
static const char *env_get(char *const *envp, const char *name) {
    size_t k = strlen(name);
    for (char *const *p = envp; p && *p; p++)
        if (strncmp(*p, name, k) == 0 && (*p)[k] == '=') return *p + k + 1;
    return NULL;
}

// argv[0] as a path to exec: itself when it holds a '/', else the first executable in PATH.
static bool resolve(const char *name, const char *path_var, char *out, size_t cap) {
    if (strchr(name, '/')) { snprintf(out, cap, "%s", name); return true; }
    const char *p = path_var ? path_var : "/usr/bin:/bin";
    while (*p) {
        const char *e = strchr(p, ':');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        int w = len ? snprintf(out, cap, "%.*s/%s", (int)len, p, name) : snprintf(out, cap, "./%s", name);
        if (w > 0 && (size_t)w < cap && access(out, X_OK) == 0) return true;
        if (!e) break;
        p = e + 1;
    }
    return false;
}

// fd as a close-on-exec descriptor of 4 or more (the child's 0..3 are dup2 targets).
static int high_cloexec(int fd) {
    if (fd < 0) return -1;
    if (fd >= 4) { fcntl(fd, F_SETFD, FD_CLOEXEC); return fd; }
    int h = fcntl(fd, F_DUPFD_CLOEXEC, 4);
    close(fd);
    return h;
}

static int mk_pipe(int p[2]) {
#ifdef __linux__
    if (pipe2(p, O_CLOEXEC) != 0) { p[0] = p[1] = -1; return -1; }
#else
    if (pipe(p) != 0) { p[0] = p[1] = -1; return -1; }
#endif
    p[0] = high_cloexec(p[0]);
    p[1] = high_cloexec(p[1]);
    if (p[0] < 0 || p[1] < 0) { close_fd(&p[0]); close_fd(&p[1]); return -1; }
    return 0;
}

static void nonblock(int fd) { fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK); }

typedef struct {
    const char *path;
    char *const *argv;
    char *const *envp;
    const char *cwd;
    int in, out, err, chan, report;   // the child's ends, all >= 4
    int maxfd;
    pid_t parent;
} plan;

static void child_report(int fd, int stage, int e) {
    int msg[2] = { stage, e };
    ssize_t w = write(fd, msg, sizeof msg);
    (void)w;
    _exit(127);
}

static void close_from(int first, int keep, int maxfd) {
#if defined(__linux__) && defined(SYS_close_range)
    if (keep > first && syscall(SYS_close_range, (unsigned)first, (unsigned)keep - 1, 0) == 0 &&
        syscall(SYS_close_range, (unsigned)keep + 1, ~0u, 0) == 0)
        return;
#endif
    for (int fd = first; fd < maxfd; fd++)
        if (fd != keep) close(fd);
}

// After the fork, in the child: async-signal-safe calls only, then the exec.
static void child_run(const plan *p) {
    setpgid(0, 0);
#ifdef __linux__
    prctl(PR_SET_PDEATHSIG, SIGKILL);
    if (getppid() != p->parent) _exit(127);   // the server died before the death signal was set
#endif
    struct sigaction dfl;
    memset(&dfl, 0, sizeof dfl);
    dfl.sa_handler = SIG_DFL;
    sigemptyset(&dfl.sa_mask);
    for (int s = 1; s < NSIG; s++)
        if (s != SIGKILL && s != SIGSTOP) sigaction(s, &dfl, NULL);
    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, NULL);
    if (dup2(p->in, 0) < 0 || dup2(p->out, 1) < 0 || dup2(p->err, 2) < 0 || (p->chan >= 0 && dup2(p->chan, 3) < 0))
        child_report(p->report, 0, errno);
    close_from(p->chan >= 0 ? 4 : 3, p->report, p->maxfd);
    if (p->cwd && chdir(p->cwd) != 0) child_report(p->report, 1, errno);
    execve(p->path, p->argv, p->envp);
    child_report(p->report, 2, errno);
}

static int max_fd(void) {
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) != 0 || rl.rlim_cur == RLIM_INFINITY || rl.rlim_cur > 65536) return 65536;
    return (int)rl.rlim_cur;
}

static void child_start(child *c) {
    cb_spawner *sp = c->sp;
    char why[160];
    if (atomic_load(&sp->stopped)) { finish(c, CB_E_STATE, "the spawner has stopped"); return; }
    const char *pv = c->envp ? env_get(c->envp, "PATH") : NULL;
    if (!pv) pv = getenv("PATH");
    char path[1024];
    if (!resolve(c->argv[0], pv, path, sizeof path)) {
        snprintf(why, sizeof why, "no such program in PATH: %s", c->argv[0]);
        finish(c, CB_E_NOT_FOUND, why);
        return;
    }
    int pin[2] = { -1, -1 }, pout[2] = { -1, -1 }, perr[2] = { -1, -1 }, prep[2] = { -1, -1 }, pch[2] = { -1, -1 };
    bool ok = mk_pipe(pin) == 0 && mk_pipe(pout) == 0 && mk_pipe(perr) == 0 && mk_pipe(prep) == 0;
    if (ok && c->channel) {
        ok = socketpair(AF_UNIX, SOCK_STREAM, 0, pch) == 0;
        if (ok) {
            pch[0] = high_cloexec(pch[0]);
            pch[1] = high_cloexec(pch[1]);
            ok = pch[0] >= 0 && pch[1] >= 0;
        }
    }
    char *const empty[] = { NULL };
    plan p = { path, c->argv, c->envp ? c->envp : empty, c->cwd, pin[0], pout[1], perr[1], c->channel ? pch[1] : -1,
               prep[1], max_fd(), getpid() };
    pid_t pid = ok ? fork() : -1;
    int fork_errno = errno;
    if (pid == 0) child_run(&p);   // never returns
    int *child_ends[] = { &pin[0], &pout[1], &perr[1], &pch[1], &prep[1] };
    for (size_t i = 0; i < sizeof child_ends / sizeof child_ends[0]; i++) close_fd(child_ends[i]);
    if (pid < 0) {
        int *ours[] = { &pin[1], &pout[0], &perr[0], &pch[0], &prep[0] };
        for (size_t i = 0; i < sizeof ours / sizeof ours[0]; i++) close_fd(ours[i]);
        snprintf(why, sizeof why, "%s: %s", ok ? "fork" : "pipes", strerror(fork_errno));
        finish(c, rc_of_errno(fork_errno), why);
        return;
    }
    c->started = true;
    c->pid = pid;
    c->t0 = cb_mono_ms();
    // The exec's answer: the report pipe closes at the exec, or carries the step that failed.
    int msg[2];
    ssize_t r;
    do r = read(prep[0], msg, sizeof msg); while (r < 0 && errno == EINTR);
    close_fd(&prep[0]);
    c->fd_in = pin[1];
    c->fd_out = pout[0];
    c->fd_err = perr[0];
    c->fd_chan = pch[0];
    if (r == (ssize_t)sizeof msg) {
        int st;
        while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
        c->reaped = true;
        static const char *const step[] = { "dup2", "chdir", "exec" };
        snprintf(why, sizeof why, "%s %s: %s", step[msg[0] >= 0 && msg[0] < 3 ? msg[0] : 2],
                 msg[0] == 1 ? c->cwd : path, strerror(msg[1]));
        c->started = false;
        finish(c, rc_of_errno(msg[1]), why);
        return;
    }
    nonblock(c->fd_in);
    nonblock(c->fd_out);
    nonblock(c->fd_err);
#ifdef F_SETNOSIGPIPE
    fcntl(c->fd_in, F_SETNOSIGPIPE, 1);
#endif
    if (c->fd_chan >= 0) {
        nonblock(c->fd_chan);
#ifdef SO_NOSIGPIPE
        int one = 1;
        setsockopt(c->fd_chan, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
        c->line = malloc(c->line_max + 1);
    }
    bool added = cb_loop_add(sp->loop, c->fd_out, CB_EV_READ, on_out, c) == CB_OK &&
                 cb_loop_add(sp->loop, c->fd_err, CB_EV_READ, on_out, c) == CB_OK;
    if (added && c->fd_chan >= 0) {
        added = c->line && cb_loop_add(sp->loop, c->fd_chan, CB_EV_READ, on_chan, c) == CB_OK;
        if (added) c->chan_events = CB_EV_READ;
    }
    if (added && c->in_n > 0) added = cb_loop_add(sp->loop, c->fd_in, CB_EV_WRITE, on_in, c) == CB_OK;
    else if (added) close_fd(&c->fd_in);   // nothing to write: the child reads its end at once
    if (!added) {
        // The child runs but cannot be watched: it is killed, and ends as any killed child does.
        CB_ERROR(MOD, "%s (pid %d): its pipes could not be watched: killed", c->argv[0], (int)pid);
        kill(-pid, SIGKILL);
        int st;
        while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
        c->reaped = true;
        c->started = false;
        finish(c, CB_E_NOMEM, "its pipes could not be watched");
        return;
    }
    if (c->timeout_ms > 0) c->t_timeout = cb_loop_timer(sp->loop, c->timeout_ms, on_timeout, c);
    // The leader may have exited already, its SIGCHLD read before it was listed as started.
    on_sigchld_fd(sp->loop, sp->wake[0], CB_EV_READ, sp);
}

// ---- the posts ----------------------------------------------------------------------------------

typedef struct {
    cb_spawner *sp;
    uint64_t    id;
    int         kind;   // 0 start, 1 kill, 2 send
    size_t      n;
    char        line[];
} msg;

static int do_kill(cb_spawner *sp, uint64_t id) {
    child *c = find(sp, id);
    if (!c || !c->started || c->reaped) return CB_E_NOT_FOUND;
    kill_start(c, CB_SPAWN_KILLED);
    return CB_OK;
}

static int do_send(cb_spawner *sp, uint64_t id, const char *line, size_t n) {
    child *c = find(sp, id);
    if (!c || !c->started) return CB_E_NOT_FOUND;
    if (c->fd_chan < 0 || c->chan_dead) return CB_E_STATE;
    if (n > c->line_max) return CB_E_INVALID;
    if (c->wn - c->woff + n + 1 > 64 * (c->line_max + 1)) return CB_E_FULL;
    if (c->wn + n + 1 > c->wcap) {
        size_t cap = c->wcap ? c->wcap : 256;
        while (cap < c->wn + n + 1) cap *= 2;
        char *q = realloc(c->wbuf, cap);
        if (!q) return CB_E_NOMEM;
        c->wbuf = q;
        c->wcap = cap;
    }
    memcpy(c->wbuf + c->wn, line, n);
    c->wbuf[c->wn + n] = '\n';
    c->wn += n + 1;
    chan_flush(c);
    return CB_OK;
}

static void on_msg(cb_loop *l, void *ud) {
    msg *m = ud;
    if (m->kind == 0) {
        child *c = find(m->sp, m->id);
        if (c) child_start(c);
    } else if (m->kind == 1) {
        (void)do_kill(m->sp, m->id);
    } else {
        int rc = do_send(m->sp, m->id, m->line, m->n);
        if (rc != CB_OK && rc != CB_E_NOT_FOUND) CB_WARN(MOD, "a line for child %llu was not sent (%s)", (unsigned long long)m->id, cb_strerror(rc));
    }
    free(m);
}

static int post(cb_spawner *sp, uint64_t id, int kind, const char *line, size_t n) {
    msg *m = malloc(sizeof *m + n + 1);
    if (!m) return CB_E_NOMEM;
    m->sp = sp;
    m->id = id;
    m->kind = kind;
    m->n = n;
    if (n) memcpy(m->line, line, n);
    m->line[n] = 0;
    int rc = cb_loop_post(sp->loop, on_msg, m);
    if (rc != CB_OK) free(m);
    return rc;
}

// ---- the API ------------------------------------------------------------------------------------

static char **strv_copy(const char *const *v) {
    size_t n = 0;
    while (v[n]) n++;
    char **o = calloc(n + 1, sizeof *o);
    if (!o) return NULL;
    for (size_t i = 0; i < n; i++)
        if (!(o[i] = strdup(v[i]))) { strv_free(o); return NULL; }
    return o;
}

static bool env_has(const char *const *env, const char *entry) {
    const char *eq = strchr(entry, '=');
    size_t k = eq ? (size_t)(eq - entry) : strlen(entry);
    for (const char *const *p = env; p && *p; p++)
        if (strncmp(*p, entry, k) == 0 && (*p)[k] == '=') return true;
    return false;
}

static char **env_build(const cb_spawn_opts *o) {
    size_t n = 0, m = 0;
    for (const char *const *p = o->env; p && *p; p++) n++;
    if (o->inherit_env)
        for (char **p = environ; p && *p; p++) m++;
    char **v = calloc(n + m + 1, sizeof *v);
    if (!v) return NULL;
    size_t k = 0;
    if (o->inherit_env)
        for (char **p = environ; p && *p; p++)
            if (!env_has(o->env, *p) && !(v[k++] = strdup(*p))) { strv_free(v); return NULL; }
    for (const char *const *p = o->env; p && *p; p++)
        if (!(v[k++] = strdup(*p))) { strv_free(v); return NULL; }
    return v;
}

cb_spawner *cb_spawner_new(cb_loop *l) {
    if (!l) return NULL;
    pthread_once(&g_once, install_handlers);
    cb_spawner *sp = calloc(1, sizeof *sp);
    if (!sp) return NULL;
    sp->loop = l;
    sp->slot = -1;
    pthread_mutex_init(&sp->mu, NULL);
    atomic_store(&sp->next_id, 1);
    if (mk_pipe(sp->wake) != 0) { pthread_mutex_destroy(&sp->mu); free(sp); return NULL; }
    nonblock(sp->wake[0]);
    nonblock(sp->wake[1]);
    for (int i = 0; i < WAKE_SLOTS && sp->slot < 0; i++) {
        int expect = 0;
        if (atomic_compare_exchange_strong(&g_wake[i], &expect, sp->wake[1] + 1)) sp->slot = i;
    }
    if (sp->slot < 0 || cb_loop_add(l, sp->wake[0], CB_EV_READ, on_sigchld_fd, sp) != CB_OK) {
        CB_ERROR(MOD, "no room for another spawner (%d at most)", WAKE_SLOTS);
        if (sp->slot >= 0) atomic_store(&g_wake[sp->slot], 0);
        close_fd(&sp->wake[0]);
        close_fd(&sp->wake[1]);
        pthread_mutex_destroy(&sp->mu);
        free(sp);
        return NULL;
    }
    return sp;
}

static void sleep_ms(int ms) {
    struct timespec ts = { 0, (long)ms * 1000000L };
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
}

// Reads what the child's pipe *fd holds now, without waiting (the loop no longer runs it): false
// when the read ended the child (it is freed).
static bool drain_one(child *c, const int *fd) {
    cb_spawner *sp = c->sp;
    uint64_t id = c->id;
    if (*fd < 0) return true;
    if (fd == &c->fd_chan) on_chan(sp->loop, *fd, CB_EV_READ, c);
    else on_out(sp->loop, *fd, CB_EV_READ, c);
    return find(sp, id) == c;
}

void cb_spawner_stop(cb_spawner *sp) {
    if (!sp || atomic_exchange(&sp->stopped, true)) return;
    child *cs[CB_SPAWN_MAX];
    int n = list_children(sp, cs, CB_SPAWN_MAX, true);
    int64_t grace = 0;
    for (int i = 0; i < n; i++) {
        child *c = cs[i];
        if (!c->killed) c->killed = CB_SPAWN_SHUTDOWN;
        if (c->t_kill) { cb_loop_cancel(sp->loop, c->t_kill); c->t_kill = 0; }
        if (!c->reaped) kill(-c->pid, SIGTERM);
        if (c->grace_ms > grace) grace = c->grace_ms;
    }
    if (n) CB_INFO(MOD, "stopping: %d child(ren) sent SIGTERM, up to %lld ms before SIGKILL", n, (long long)grace);
    int64_t until = cb_mono_ms() + grace;
    for (;;) {
        int left = 0;
        for (int i = 0; i < n; i++) {
            child *c = cs[i];
            if (c->reaped) continue;
            if (c->fd_out >= 0) on_out(sp->loop, c->fd_out, CB_EV_READ, c);   // a child blocked on a full pipe
            if (c->fd_err >= 0) on_out(sp->loop, c->fd_err, CB_EV_READ, c);   // can then still exit
            bool lost;
            if (leader_exited(c->pid, &lost)) reap(c, lost);
            else left++;
        }
        if (!left) break;
        if (cb_mono_ms() >= until) {
            for (int i = 0; i < n; i++)
                if (!cs[i]->reaped) kill(-cs[i]->pid, SIGKILL);
            for (int i = 0; i < n; i++)
                if (!cs[i]->reaped) reap(cs[i], false);
            break;
        }
        sleep_ms(5);
    }
    // Every leader is reaped and its group killed: what the pipes hold is all there will be. Each
    // read below may end the child (its last pipe at its end), so the list is taken again each time.
    for (;;) {
        child *one[1];
        if (list_children(sp, one, 1, true) == 0) break;
        child *c = one[0];
        // One pipe at a time, each read once: a pipe a straggler outside the group still holds is
        // not waited for.
        if (!drain_one(c, &c->fd_out) || !drain_one(c, &c->fd_err) || !drain_one(c, &c->fd_chan)) continue;
        finish(c, CB_OK, NULL);
    }
}

void cb_spawner_free(cb_spawner *sp) {
    if (!sp) return;
    cb_spawner_stop(sp);
    if (sp->slot >= 0) atomic_store(&g_wake[sp->slot], 0);
    cb_loop_remove(sp->loop, sp->wake[0]);
    close_fd(&sp->wake[0]);
    close_fd(&sp->wake[1]);
    pthread_mutex_destroy(&sp->mu);
    free(sp);
}

int cb_spawn(cb_spawner *sp, const cb_spawn_opts *o, uint64_t *id) {
    if (id) *id = 0;
    if (!sp || !o || !o->argv || !o->argv[0] || !o->argv[0][0]) return CB_E_INVALID;
    if (atomic_load(&sp->stopped)) return CB_E_STATE;
    child *c = calloc(1, sizeof *c);
    if (!c) return CB_E_NOMEM;
    c->sp = sp;
    c->fd_in = c->fd_out = c->fd_err = c->fd_chan = -1;
    c->argv = strv_copy(o->argv);
    c->envp = env_build(o);
    c->cwd = o->cwd ? strdup(o->cwd) : NULL;
    if (o->stdin_n) {
        c->in = malloc(o->stdin_n);
        if (c->in) memcpy(c->in, o->stdin_data, o->stdin_n);
        c->in_n = o->stdin_n;
    }
    if (!c->argv || !c->envp || (o->cwd && !c->cwd) || (o->stdin_n && !c->in)) { child_free(c); return CB_E_NOMEM; }
    c->out.limit = o->out_cap ? o->out_cap : CB_SPAWN_CAP_DEFAULT;
    c->err.limit = o->err_cap ? o->err_cap : CB_SPAWN_CAP_DEFAULT;
    c->timeout_ms = o->timeout_ms > 0 ? o->timeout_ms : 0;
    c->grace_ms = o->grace_ms > 0 ? o->grace_ms : CB_SPAWN_GRACE_DEFAULT;
    c->channel = o->channel;
    c->line_max = o->line_max ? o->line_max : CB_SPAWN_LINE_DEFAULT;
    c->on_line = o->on_line;
    c->done = o->done;
    c->ud = o->ud;
    c->id = atomic_fetch_add(&sp->next_id, 1);
    pthread_mutex_lock(&sp->mu);
    bool full = sp->n >= CB_SPAWN_MAX;
    if (!full) { c->next = sp->head; sp->head = c; sp->n++; }
    pthread_mutex_unlock(&sp->mu);
    if (full) { child_free(c); return CB_E_FULL; }
    uint64_t cid = c->id;
    int rc = post(sp, cid, 0, NULL, 0);
    if (rc != CB_OK) {   // the loop has stopped: the start never runs, so the child is still ours
        unlist(sp, c);
        child_free(c);
        return rc;
    }
    if (id) *id = cid;
    return CB_OK;
}

int cb_spawn_kill(cb_spawner *sp, uint64_t id) {
    if (!sp) return CB_E_INVALID;
    if (cb_loop_on_thread(sp->loop)) return do_kill(sp, id);
    return post(sp, id, 1, NULL, 0);
}

int cb_spawn_send(cb_spawner *sp, uint64_t id, const char *line, size_t n) {
    if (!sp || (!line && n)) return CB_E_INVALID;
    if (n && memchr(line, '\n', n)) return CB_E_INVALID;
    if (cb_loop_on_thread(sp->loop)) return do_send(sp, id, line, n);
    return post(sp, id, 2, line, n);
}

// ---- waited for ---------------------------------------------------------------------------------

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t  cv;
    bool            over;
    cb_spawn_result r;
} waiter;

static char *dup_bytes(const char *p, size_t n) {
    char *q = malloc(n + 1);
    if (!q) return NULL;
    memcpy(q, p, n);
    q[n] = 0;
    return q;
}

static void wait_done(const cb_spawn_result *r, void *ud) {
    waiter *w = ud;
    pthread_mutex_lock(&w->mu);
    w->r = *r;
    w->r.out = dup_bytes(r->out, r->out_n);
    w->r.err = dup_bytes(r->err, r->err_n);
    if (!w->r.out || !w->r.err) {   // the result stands; the bytes are lost and said to be
        free((char *)w->r.out);
        free((char *)w->r.err);
        w->r.out = w->r.err = NULL;
        w->r.out_n = w->r.err_n = 0;
        if (w->r.rc == CB_OK) w->r.rc = CB_E_NOMEM;
        snprintf(w->r.why, sizeof w->r.why, "out of memory copying its output");
    }
    w->over = true;
    pthread_cond_signal(&w->cv);
    pthread_mutex_unlock(&w->mu);
}

int cb_spawn_wait(cb_spawner *sp, const cb_spawn_opts *o, cb_spawn_result *r) {
    memset(r, 0, sizeof *r);
    if (!sp || !o) return CB_E_INVALID;
    if (cb_loop_on_thread(sp->loop)) return CB_E_STATE;
    waiter w;
    memset(&w, 0, sizeof w);
    pthread_mutex_init(&w.mu, NULL);
    pthread_cond_init(&w.cv, NULL);
    cb_spawn_opts mine = *o;
    mine.done = wait_done;
    mine.ud = &w;
    int rc = cb_spawn(sp, &mine, NULL);
    if (rc == CB_OK) {
        pthread_mutex_lock(&w.mu);
        while (!w.over) pthread_cond_wait(&w.cv, &w.mu);
        pthread_mutex_unlock(&w.mu);
        *r = w.r;
    }
    pthread_cond_destroy(&w.cv);
    pthread_mutex_destroy(&w.mu);
    return rc;
}

void cb_spawn_result_free(cb_spawn_result *r) {
    if (!r) return;
    free((char *)r->out);
    free((char *)r->err);
    r->out = r->err = NULL;
    r->out_n = r->err_n = 0;
}
