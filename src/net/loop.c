#include "loop.h"

#include "util/clock.h"
#include "util/err.h"
#include "util/log.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    int fd;           // -1: free slot
    int events;
    cb_io_fn fn;
    void *ud;
    uint64_t gen;     // bumped on every add, so a stale poll result is ignored
} watch;

typedef struct {
    int64_t at;
    uint64_t id;
    cb_timer_fn fn;
    void *ud;
} timer;

typedef struct post {
    cb_post_fn fn;
    void *ud;
    struct post *next;
} post;

struct cb_loop {
    watch *w;          // dense array of watches
    int nw, capw;
    int *slot_of_fd;   // fd -> index in w, or -1
    int cap_fd;
    uint64_t gen;

    timer *heap;       // min-heap on (at, id)
    int nt, capt;
    uint64_t next_timer_id;

    pthread_mutex_t post_lock;
    post *post_head, *post_tail;
    int wake[2];       // self-pipe: posts and stop write one byte
    atomic_bool stopping;
    bool stopped;      // after run returned: posts are refused

    int64_t now;
    pthread_t thread;
    atomic_bool running;
};

static int set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0) return -1;
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    return 0;
}

cb_loop *cb_loop_new(void) {
    cb_loop *l = calloc(1, sizeof *l);
    if (!l) return NULL;
    if (pipe(l->wake) != 0 || set_nonblock(l->wake[0]) || set_nonblock(l->wake[1])) {
        free(l);
        return NULL;
    }
    pthread_mutex_init(&l->post_lock, NULL);
    l->now = cb_mono_ms();
    l->next_timer_id = 1;
    return l;
}

void cb_loop_free(cb_loop *l) {
    if (!l) return;
    post *p = l->post_head;
    while (p) { post *n = p->next; free(p); p = n; }
    close(l->wake[0]);
    close(l->wake[1]);
    pthread_mutex_destroy(&l->post_lock);
    free(l->w);
    free(l->slot_of_fd);
    free(l->heap);
    free(l);
}

static int grow_fd_map(cb_loop *l, int fd) {
    if (fd < l->cap_fd) return 0;
    int cap = l->cap_fd ? l->cap_fd : 64;
    while (cap <= fd) cap *= 2;
    int *m = realloc(l->slot_of_fd, (size_t)cap * sizeof *m);
    if (!m) return -1;
    for (int i = l->cap_fd; i < cap; i++) m[i] = -1;
    l->slot_of_fd = m;
    l->cap_fd = cap;
    return 0;
}

int cb_loop_add(cb_loop *l, int fd, int events, cb_io_fn fn, void *ud) {
    if (fd < 0) return CB_E_INVALID;
    if (grow_fd_map(l, fd)) return CB_E_NOMEM;
    if (l->slot_of_fd[fd] >= 0) return CB_E_CONFLICT;
    if (l->nw == l->capw) {
        int cap = l->capw ? l->capw * 2 : 32;
        watch *w = realloc(l->w, (size_t)cap * sizeof *w);
        if (!w) return CB_E_NOMEM;
        l->w = w;
        l->capw = cap;
    }
    watch *x = &l->w[l->nw];
    x->fd = fd; x->events = events; x->fn = fn; x->ud = ud; x->gen = ++l->gen;
    l->slot_of_fd[fd] = l->nw++;
    return CB_OK;
}

int cb_loop_set(cb_loop *l, int fd, int events) {
    if (fd < 0 || fd >= l->cap_fd || l->slot_of_fd[fd] < 0) return CB_E_NOT_FOUND;
    l->w[l->slot_of_fd[fd]].events = events;
    return CB_OK;
}

void cb_loop_remove(cb_loop *l, int fd) {
    if (fd < 0 || fd >= l->cap_fd || l->slot_of_fd[fd] < 0) return;
    int i = l->slot_of_fd[fd];
    l->slot_of_fd[fd] = -1;
    // Keep the array dense: the last watch moves into the hole.
    l->nw--;
    if (i != l->nw) {
        l->w[i] = l->w[l->nw];
        l->slot_of_fd[l->w[i].fd] = i;
    }
}

// ---- timers: a binary min-heap on (at, id) ----

static bool before(const timer *a, const timer *b) { return a->at < b->at || (a->at == b->at && a->id < b->id); }

static void heap_up(timer *h, int i) {
    while (i > 0) {
        int p = (i - 1) / 2;
        if (!before(&h[i], &h[p])) break;
        timer t = h[i]; h[i] = h[p]; h[p] = t;
        i = p;
    }
}

static void heap_down(timer *h, int n, int i) {
    for (;;) {
        int a = 2 * i + 1, b = a + 1, m = i;
        if (a < n && before(&h[a], &h[m])) m = a;
        if (b < n && before(&h[b], &h[m])) m = b;
        if (m == i) return;
        timer t = h[i]; h[i] = h[m]; h[m] = t;
        i = m;
    }
}

uint64_t cb_loop_timer(cb_loop *l, int64_t delay_ms, cb_timer_fn fn, void *ud) {
    if (l->nt == l->capt) {
        int cap = l->capt ? l->capt * 2 : 32;
        timer *h = realloc(l->heap, (size_t)cap * sizeof *h);
        if (!h) return 0;
        l->heap = h;
        l->capt = cap;
    }
    timer t = { cb_mono_ms() + (delay_ms > 0 ? delay_ms : 0), l->next_timer_id++, fn, ud };
    l->heap[l->nt] = t;
    heap_up(l->heap, l->nt++);
    return t.id;
}

bool cb_loop_cancel(cb_loop *l, uint64_t id) {
    for (int i = 0; i < l->nt; i++) {
        if (l->heap[i].id != id) continue;
        l->heap[i] = l->heap[--l->nt];
        if (i < l->nt) { heap_down(l->heap, l->nt, i); heap_up(l->heap, i); }
        return true;
    }
    return false;
}

// ---- posts: the only way in from another thread ----

static void wake(cb_loop *l) {
    char b = 1;
    // A full pipe already guarantees a wake-up; EAGAIN is fine.
    ssize_t r = write(l->wake[1], &b, 1);
    (void)r;
}

int cb_loop_post(cb_loop *l, cb_post_fn fn, void *ud) {
    post *p = malloc(sizeof *p);
    if (!p) return CB_E_NOMEM;
    p->fn = fn; p->ud = ud; p->next = NULL;
    pthread_mutex_lock(&l->post_lock);
    if (l->stopped) { pthread_mutex_unlock(&l->post_lock); free(p); return CB_E_STATE; }
    if (l->post_tail) l->post_tail->next = p; else l->post_head = p;
    l->post_tail = p;
    pthread_mutex_unlock(&l->post_lock);
    wake(l);
    return CB_OK;
}

void cb_loop_stop(cb_loop *l) {
    atomic_store(&l->stopping, true);
    wake(l);
}

static void drain_posts(cb_loop *l) {
    char buf[256];
    while (read(l->wake[0], buf, sizeof buf) > 0) {}
    pthread_mutex_lock(&l->post_lock);
    post *p = l->post_head;
    l->post_head = l->post_tail = NULL;
    pthread_mutex_unlock(&l->post_lock);
    while (p) {
        post *n = p->next;
        p->fn(l, p->ud);
        free(p);
        p = n;
    }
}

static void run_timers(cb_loop *l) {
    // Only timers due as of now; one added by a callback waits for the next pass.
    int64_t now = cb_mono_ms();
    int budget = l->nt;
    while (budget-- > 0 && l->nt > 0 && l->heap[0].at <= now) {
        timer t = l->heap[0];
        l->heap[0] = l->heap[--l->nt];
        heap_down(l->heap, l->nt, 0);
        t.fn(l, t.ud);
    }
}

int cb_loop_run(cb_loop *l) {
    l->thread = pthread_self();
    atomic_store(&l->running, true);
    struct pollfd *pfd = NULL;
    uint64_t *gens = NULL;
    int cap = 0;
    int rc = CB_OK;
    while (!atomic_load(&l->stopping)) {
        l->now = cb_mono_ms();
        int n = l->nw + 1;
        if (n > cap) {
            int c = cap ? cap : 64;
            while (c < n) c *= 2;
            struct pollfd *p2 = realloc(pfd, (size_t)c * sizeof *p2);
            if (!p2) { rc = CB_E_NOMEM; break; }
            pfd = p2;
            uint64_t *g2 = realloc(gens, (size_t)c * sizeof *g2);
            if (!g2) { rc = CB_E_NOMEM; break; }
            gens = g2;
            cap = c;
        }
        pfd[0].fd = l->wake[0]; pfd[0].events = POLLIN; pfd[0].revents = 0;
        for (int i = 0; i < l->nw; i++) {
            pfd[i + 1].fd = l->w[i].fd;
            pfd[i + 1].events = (short)((l->w[i].events & CB_EV_READ ? POLLIN : 0) |
                                        (l->w[i].events & CB_EV_WRITE ? POLLOUT : 0));
            pfd[i + 1].revents = 0;
            gens[i + 1] = l->w[i].gen;
        }
        int timeout = -1;
        if (l->nt > 0) {
            int64_t d = l->heap[0].at - cb_mono_ms();
            timeout = d <= 0 ? 0 : d > 60000 ? 60000 : (int)d;
        }
        int r = poll(pfd, (nfds_t)n, timeout);
        if (r < 0) {
            if (errno == EINTR) continue;
            CB_ERROR("loop", "poll failed: %s", strerror(errno));
            rc = CB_E_IO;
            break;
        }
        l->now = cb_mono_ms();
        if (pfd[0].revents) drain_posts(l);
        for (int i = 1; i < n && r > 0; i++) {
            if (!pfd[i].revents) continue;
            int fd = pfd[i].fd;
            // The watch may have been removed, or the fd reused, by an earlier callback.
            if (fd >= l->cap_fd || l->slot_of_fd[fd] < 0) continue;
            watch *w = &l->w[l->slot_of_fd[fd]];
            if (w->gen != gens[i]) continue;
            int ev = (pfd[i].revents & POLLIN ? CB_EV_READ : 0) | (pfd[i].revents & POLLOUT ? CB_EV_WRITE : 0) |
                     (pfd[i].revents & (POLLERR | POLLHUP | POLLNVAL) ? CB_EV_ERR : 0);
            w->fn(l, fd, ev, w->ud);
        }
        run_timers(l);
    }
    free(pfd);
    free(gens);
    // Posts that raced the stop are run, so their owners can free what they carry.
    drain_posts(l);
    pthread_mutex_lock(&l->post_lock);
    l->stopped = true;
    pthread_mutex_unlock(&l->post_lock);
    atomic_store(&l->running, false);
    return rc;
}

int64_t cb_loop_now(const cb_loop *l) { return l->now; }

bool cb_loop_on_thread(const cb_loop *l) {
    return atomic_load(&((cb_loop *)l)->running) && pthread_equal(pthread_self(), l->thread);
}
