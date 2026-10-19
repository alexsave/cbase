#include "loop.h"

#include "poller.h"
#include "util/clock.h"
#include "util/err.h"
#include "util/log.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/eventfd.h>
#endif

typedef struct {
    int fd;           // -1: free slot
    int events;
    cb_io_fn fn;
    void *ud;
    uint64_t gen;     // bumped on every add, so a stale readiness result is ignored
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
    cb_poller *pl;     // the OS queue; its registrations always equal w[].events
    int *slot_of_fd;   // fd -> index in w, or -1
    int cap_fd;
    uint64_t gen;

    timer *heap;       // min-heap on (at, id)
    int nt, capt;
    cb_after_fn after;
    void *after_ud;
    int *pos;          // slot -> heap index (see the timers)
    int *freel, nfree, npos;   // free slots, and slots ever handed out
    uint64_t next_timer_id;

    pthread_mutex_t post_lock;
    post *post_head, *post_tail;
    int wake[2];       // self-pipe: posts and stop write one byte
    atomic_bool stopping;
    atomic_bool wake_pending;   // a wake byte is in the pipe (or about to be) that no drain has cleared
    bool stopped;      // after run returned: posts are refused

    int64_t now;
    pthread_t thread;
    atomic_bool running;
};

#ifndef __linux__
static int set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0) return -1;
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    return 0;
}
#endif

// The wake channel: an eventfd on Linux (one fd, a counter, no pipe buffer to
// allocate and copy through on every post), a self-pipe elsewhere.
static int wake_open(int w[2]) {
#ifdef __linux__
    int fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (fd < 0) return -1;
    w[0] = w[1] = fd;
    return 0;
#else
    if (pipe(w) != 0) return -1;
    if (set_nonblock(w[0]) || set_nonblock(w[1])) { close(w[0]); close(w[1]); return -1; }
    return 0;
#endif
}

static void wake_close(int w[2]) {
    close(w[0]);
    if (w[1] != w[0]) close(w[1]);
}

cb_loop *cb_loop_new(void) {
    cb_loop *l = calloc(1, sizeof *l);
    if (!l) return NULL;
    if (wake_open(l->wake) != 0) {
        free(l);
        return NULL;
    }
    l->pl = cb_poller_new();
    if (!l->pl || cb_poller_set(l->pl, l->wake[0], -1, CB_EV_READ, 0) != 0) {
        cb_poller_free(l->pl);
        wake_close(l->wake);
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
    wake_close(l->wake);
    pthread_mutex_destroy(&l->post_lock);
    cb_poller_free(l->pl);
    free(l->w);
    free(l->slot_of_fd);
    free(l->heap);
    free(l->pos);
    free(l->freel);
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
    if (cb_poller_set(l->pl, fd, -1, events, (uint32_t)(l->gen + 1)) != 0) return errno == ENOMEM ? CB_E_NOMEM : CB_E_IO;
    watch *x = &l->w[l->nw];
    x->fd = fd; x->events = events; x->fn = fn; x->ud = ud; x->gen = ++l->gen;
    l->slot_of_fd[fd] = l->nw++;
    return CB_OK;
}

int cb_loop_set(cb_loop *l, int fd, int events) {
    if (fd < 0 || fd >= l->cap_fd || l->slot_of_fd[fd] < 0) return CB_E_NOT_FOUND;
    watch *w = &l->w[l->slot_of_fd[fd]];
    if (w->events == events) return CB_OK;
    if (cb_poller_set(l->pl, fd, w->events, events, (uint32_t)w->gen) != 0) return CB_E_IO;
    w->events = events;
    return CB_OK;
}

void cb_loop_remove(cb_loop *l, int fd) {
    if (fd < 0 || fd >= l->cap_fd || l->slot_of_fd[fd] < 0) return;
    int i = l->slot_of_fd[fd];
    cb_poller_del(l->pl, fd, l->w[i].events);
    l->slot_of_fd[fd] = -1;
    // Keep the array dense: the last watch moves into the hole.
    l->nw--;
    if (i != l->nw) {
        l->w[i] = l->w[l->nw];
        l->slot_of_fd[l->w[i].fd] = i;
    }
}

// ---- timers: a binary min-heap on (at, id) ----
// An id is (sequence << TIMER_SLOT_BITS) | slot, so the order on ids is the
// order of creation, and the slot is where pos[] keeps the timer's heap
// index: a cancel is a lookup and a sift, O(log n), however many timers wait
// (one per bot game under serve). A stale id finds a slot that is free or
// reused, and its id there differs.

#define TIMER_SLOT_BITS 20
#define TIMER_SLOT_MASK ((1u << TIMER_SLOT_BITS) - 1)

static bool before(const timer *a, const timer *b) { return a->at < b->at || (a->at == b->at && a->id < b->id); }

static void place(cb_loop *l, int i) { l->pos[l->heap[i].id & TIMER_SLOT_MASK] = i; }

static void heap_up(cb_loop *l, int i) {
    timer *h = l->heap;
    while (i > 0) {
        int p = (i - 1) / 2;
        if (!before(&h[i], &h[p])) break;
        timer t = h[i]; h[i] = h[p]; h[p] = t;
        place(l, i);
        i = p;
    }
    place(l, i);
}

static void heap_down(cb_loop *l, int i) {
    timer *h = l->heap;
    int n = l->nt;
    for (;;) {
        int a = 2 * i + 1, b = a + 1, m = i;
        if (a < n && before(&h[a], &h[m])) m = a;
        if (b < n && before(&h[b], &h[m])) m = b;
        if (m == i) break;
        timer t = h[i]; h[i] = h[m]; h[m] = t;
        place(l, i);
        i = m;
    }
    if (i < n) place(l, i);
}

// Takes heap[i] out and returns its slot to the free list.
static void heap_remove(cb_loop *l, int i) {
    l->freel[l->nfree++] = (int)(l->heap[i].id & TIMER_SLOT_MASK);
    l->heap[i] = l->heap[--l->nt];
    if (i < l->nt) { heap_down(l, i); heap_up(l, i); }
}

void cb_loop_after(cb_loop *l, cb_after_fn fn, void *ud) { l->after = fn; l->after_ud = ud; }

uint64_t cb_loop_timer(cb_loop *l, int64_t delay_ms, cb_timer_fn fn, void *ud) {
    if (l->nt == l->capt) {
        int cap = l->capt ? l->capt * 2 : 32;
        if ((unsigned)cap > TIMER_SLOT_MASK + 1) return 0;
        timer *h = realloc(l->heap, (size_t)cap * sizeof *h);
        if (!h) return 0;
        l->heap = h;
        int *pos = realloc(l->pos, (size_t)cap * sizeof *pos);
        if (!pos) return 0;
        l->pos = pos;
        int *fl = realloc(l->freel, (size_t)cap * sizeof *fl);
        if (!fl) return 0;
        l->freel = fl;
        l->capt = cap;
    }
    int slot = l->nfree > 0 ? l->freel[--l->nfree] : l->npos++;
    timer t = { cb_mono_ms() + (delay_ms > 0 ? delay_ms : 0), (l->next_timer_id++ << TIMER_SLOT_BITS) | (uint64_t)slot, fn, ud };
    l->heap[l->nt] = t;
    heap_up(l, l->nt++);
    return t.id;
}

bool cb_loop_cancel(cb_loop *l, uint64_t id) {
    unsigned slot = (unsigned)(id & TIMER_SLOT_MASK);
    if (id == 0 || (int)slot >= l->npos) return false;
    int i = l->pos[slot];
    if (i >= l->nt || l->heap[i].id != id) return false;
    heap_remove(l, i);
    return true;
}

// ---- posts: the only way in from another thread ----

static void wake(cb_loop *l) {
    // A full pipe (or a saturated counter) already guarantees a wake-up; EAGAIN is fine.
    uint64_t b = 1;
    ssize_t r = write(l->wake[1], &b, l->wake[0] == l->wake[1] ? sizeof b : 1);
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
    // One wake per batch: the byte already in the pipe wakes the loop for this post too, because
    // drain_posts clears the flag after it reads the pipe and BEFORE it takes the list (a post that saw the flag set was queued
    // before the clear, so the take that follows includes it).
    if (!atomic_exchange(&l->wake_pending, true)) wake(l);
    return CB_OK;
}

void cb_loop_stop(cb_loop *l) {
    atomic_store(&l->stopping, true);
    wake(l);
}

static void drain_posts(cb_loop *l) {
    char buf[256];
    // An eventfd's read returns and resets the whole counter at once; a pipe is read dry.
    while (read(l->wake[0], buf, sizeof buf) > 0 && l->wake[0] != l->wake[1]) {}
    // After the bytes are read and BEFORE the list is taken: a post that saw the flag set was queued before
    // this clear, so the take below has it; one after the clear writes a byte of its own.
    atomic_store(&l->wake_pending, false);
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
        heap_remove(l, 0);
        t.fn(l, t.ud);
    }
}

int cb_loop_run(cb_loop *l) {
    l->thread = pthread_self();
    atomic_store(&l->running, true);
    cb_poll_ev ev[256];
    int rc = CB_OK;
    while (!atomic_load(&l->stopping)) {
        l->now = cb_mono_ms();
        int timeout = -1;
        if (l->nt > 0) {
            int64_t d = l->heap[0].at - cb_mono_ms();
            timeout = d <= 0 ? 0 : d > 60000 ? 60000 : (int)d;
        }
        int r = cb_poller_wait(l->pl, ev, (int)(sizeof ev / sizeof ev[0]), timeout);
        if (r < 0) {
            if (errno == EINTR) continue;
            CB_ERROR("loop", "poll failed: %s", strerror(errno));
            rc = CB_E_IO;
            break;
        }
        l->now = cb_mono_ms();
        for (int i = 0; i < r; i++) {
            int fd = ev[i].fd;
            if (fd == l->wake[0]) { drain_posts(l); continue; }
            // The watch may have been removed, or the fd reused, by an earlier callback.
            if (fd >= l->cap_fd || l->slot_of_fd[fd] < 0) continue;
            
            watch *w = &l->w[l->slot_of_fd[fd]];
            if ((uint32_t)w->gen != ev[i].tag) continue;
            
            w->fn(l, fd, ev[i].events, w->ud);
        }
        run_timers(l);
        if (l->after) l->after(l, l->after_ud);
    }
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
