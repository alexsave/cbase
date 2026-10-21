#include "poller.h"

#include "loop.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static int64_t mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// A deadline as the wait's timeout: -1 forever, else the ms left (0 when past).
static int timeout_of(int64_t until_ms) {
    if (until_ms < 0) return -1;
    const int64_t d = until_ms - mono_ms();
    return d <= 0 ? 0 : d > 0x7fffffff ? 0x7fffffff : (int)d;
}

#if defined(__linux__)

#include "uring.h"

#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>

#define ALARM_TAG 0xffffffffu   // the alarm timerfd's epoll tag: never a loop generation the caller sees

// ---- io_uring's state (unused under epoll) ----------------------------------------------------

// A completion's user data: a sequence number (the arm of a poll, the cookie of a send), the kind,
// and the fd.
enum { UD_POLL = 1, UD_SEND = 2, UD_REMOVE = 3 };
static uint64_t ud_make(uint32_t seq, unsigned kind, int fd) {
    return (uint64_t)seq << 32 | (uint64_t)kind << 28 | ((uint32_t)fd & 0x0fffffffu);
}

typedef struct {
    uint32_t tag;      // the loop's, given by cb_poller_set
    uint32_t arm;      // the poll in the kernel for it (its user data's sequence), when armed
    uint8_t  want;     // CB_EV_* watched
    bool     on;       // registered
    bool     armed;    // a one-shot poll for it is in the kernel
    bool     queued;   // on the to-arm list
} ufd;

struct cb_poller {
    int ep;                       // epoll; -1 under io_uring
    cb_uring *ur;
    ufd *fds;                     // by fd
    int cap_fds;
    int *to_arm;                  // fds to arm at the next wait
    int n_to_arm, cap_to_arm;
    uint32_t arm_seq;
    unsigned sends_out;           // sends given to the kernel whose result is not read yet
    unsigned sends_queued;        // sends filled since the last wait
    cb_poll_ev *ready;            // read from the kernel, not yet returned (beyond a wait's max)
    int n_ready, cap_ready, at_ready;
    cb_sent_ev *sent;             // sends' results, not yet returned
    int n_sent, cap_sent, at_sent;
    int alarm_fd;                 // epoll: the alarm (a timerfd), -1 until the first deadline
    int64_t alarm_at;             // the deadline it is set to; -1: none
    uint64_t alarms;              // times it was set (cb_poller_alarms)
};

#define URING_ENTRIES 4096

cb_poller *cb_poller_new(bool uring, char *why, size_t cap) {
    cb_poller *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->ep = -1;
    p->alarm_fd = -1;
    p->alarm_at = -1;
    if (uring) {
        char w[160] = "";
        p->ur = cb_uring_new(URING_ENTRIES, w, sizeof w);
        if (p->ur) return p;
        if (why) snprintf(why, cap, "%s", w);
    }
    p->ep = epoll_create1(EPOLL_CLOEXEC);
    if (p->ep < 0) { free(p); return NULL; }
    return p;
}

void cb_poller_free(cb_poller *p) {
    if (!p) return;
    if (p->alarm_fd >= 0) close(p->alarm_fd);
    if (p->ep >= 0) close(p->ep);
    cb_uring_free(p->ur);
    free(p->fds);
    free(p->to_arm);
    free(p->ready);
    free(p->sent);
    free(p);
}

const char *cb_poller_backend(const cb_poller *p) { return p->ur ? "io_uring" : "epoll"; }

bool cb_poller_sends(const cb_poller *p) { return p->ur != NULL; }

// Grows *arr (of `size`-byte items) to hold at least `need`.
static bool grow(void **arr, int *cap, int need, size_t size) {
    if (need <= *cap) return true;
    int c = *cap ? *cap : 64;
    while (c < need) c *= 2;
    void *n = realloc(*arr, (size_t)c * size);
    if (!n) return false;
    *arr = n;
    *cap = c;
    return true;
}

static uint32_t poll_mask(int want) { return (want & CB_EV_READ ? POLLIN : 0u) | (want & CB_EV_WRITE ? POLLOUT : 0u); }

// Fills nothing more: the queue is full, so what is filled goes to the kernel now (never waits).
static void ur_flush_sq(cb_poller *p) { (void)cb_uring_enter(p->ur, 0, 0); }

static void ur_queue_arm(cb_poller *p, int fd) {
    ufd *e = &p->fds[fd];
    if (e->queued) return;
    if (!grow((void **)&p->to_arm, &p->cap_to_arm, p->n_to_arm + 1, sizeof *p->to_arm)) return;   // armed at a later set
    p->to_arm[p->n_to_arm++] = fd;
    e->queued = true;
}

static void ur_disarm(cb_poller *p, int fd) {
    ufd *e = &p->fds[fd];
    if (!e->armed) return;
    const uint64_t target = ud_make(e->arm, UD_POLL, fd);
    while (!cb_uring_poll_remove(p->ur, target, ud_make(0, UD_REMOVE, fd))) ur_flush_sq(p);
    e->armed = false;   // its completion (cancelled, or an event that raced) is told apart by arm
}

static uint32_t ep_mask(int want) {
    return (want & CB_EV_READ ? EPOLLIN : 0u) | (want & CB_EV_WRITE ? EPOLLOUT : 0u);
}

int cb_poller_set(cb_poller *p, int fd, int have, int want, uint32_t tag) {
    if (!p->ur) {
        struct epoll_event e = { .events = ep_mask(want), .data.u64 = ((uint64_t)tag << 32) | (uint32_t)fd };
        // `have` -1 is "not registered"; 0 is registered and silent.
        return epoll_ctl(p->ep, have < 0 ? EPOLL_CTL_ADD : EPOLL_CTL_MOD, fd, &e);
    }
    const int had = p->cap_fds;
    if (fd < 0 || !grow((void **)&p->fds, &p->cap_fds, fd + 1, sizeof *p->fds)) { errno = ENOMEM; return -1; }
    if (p->cap_fds > had) memset(p->fds + had, 0, (size_t)(p->cap_fds - had) * sizeof *p->fds);   // fds never seen
    ufd *e = &p->fds[fd];
    if (have < 0) {
        const bool q = e->queued;
        memset(e, 0, sizeof *e);
        e->queued = q;   // still on the list: armed from there
    }
    if (e->armed && (e->want != (uint8_t)want || e->tag != tag)) ur_disarm(p, fd);
    e->on = true;
    e->want = (uint8_t)want;
    e->tag = tag;
    if (!e->armed) ur_queue_arm(p, fd);
    return 0;
}

void cb_poller_del(cb_poller *p, int fd, int have) {
    if (have < 0) return;
    if (!p->ur) {
        struct epoll_event e = { 0 };
        epoll_ctl(p->ep, EPOLL_CTL_DEL, fd, &e);
        return;
    }
    if (fd < 0 || fd >= p->cap_fds) return;
    ur_disarm(p, fd);
    p->fds[fd].on = false;
}

bool cb_poller_send(cb_poller *p, int fd, const void *buf, size_t n, uint32_t cookie) {
    if (!p->ur || !cb_uring_send(p->ur, fd, buf, n, ud_make(cookie, UD_SEND, fd))) return false;
    p->sends_queued++;
    return true;
}

unsigned cb_poller_queued(const cb_poller *p) { return p->ur ? cb_uring_pending(p->ur) + (unsigned)p->n_to_arm : 0; }

int cb_poller_sent(cb_poller *p, cb_sent_ev *out, int max) {
    int k = 0;
    while (k < max && p->at_sent < p->n_sent) out[k++] = p->sent[p->at_sent++];
    if (p->at_sent == p->n_sent) p->at_sent = p->n_sent = 0;
    return k;
}

// Every completion the kernel has, into the poller's lists.
static void ur_reap(cb_poller *p) {
    uint64_t ud;
    int32_t res;
    while (cb_uring_next(p->ur, &ud, &res)) {
        const unsigned kind = (unsigned)(ud >> 28) & 0xfu;
        const int fd = (int)(ud & 0x0fffffffu);
        const uint32_t seq = (uint32_t)(ud >> 32);
        if (kind == UD_SEND) {
            p->sends_out--;
            if (grow((void **)&p->sent, &p->cap_sent, p->n_sent + 1, sizeof *p->sent))
                p->sent[p->n_sent++] = (cb_sent_ev){ seq, res };
            continue;
        }
        if (kind != UD_POLL || fd >= p->cap_fds) continue;
        ufd *e = &p->fds[fd];
        if (!e->on || !e->armed || e->arm != seq) continue;   // removed, changed, or cancelled
        e->armed = false;   // one shot: armed again at the next wait while still wanted
        ur_queue_arm(p, fd);
        int ev = res < 0 ? CB_EV_ERR
                         : (res & (POLLIN | POLLPRI) ? CB_EV_READ : 0) | (res & POLLOUT ? CB_EV_WRITE : 0) |
                               (res & (POLLERR | POLLHUP | POLLNVAL) ? CB_EV_ERR : 0);
        if (!ev) continue;
        if (grow((void **)&p->ready, &p->cap_ready, p->n_ready + 1, sizeof *p->ready))
            p->ready[p->n_ready++] = (cb_poll_ev){ fd, ev, e->tag };
    }
}

static int ur_wait(cb_poller *p, cb_poll_ev *out, int max, int timeout_ms) {
    // Every registered fd with no poll in the kernel is armed, with what it wants now.
    for (int i = 0; i < p->n_to_arm; i++) {
        const int fd = p->to_arm[i];
        ufd *e = &p->fds[fd];
        e->queued = false;
        if (!e->on || e->armed) continue;
        e->arm = ++p->arm_seq;
        while (!cb_uring_poll_add(p->ur, fd, poll_mask(e->want), ud_make(e->arm, UD_POLL, fd))) ur_flush_sq(p);
        e->armed = true;
    }
    p->n_to_arm = 0;
    // One enter: this pass's sends and arms handed over. A pass that queued sends waits for their
    // completions and no more: what a send's result asks for next (a close after a last answer, the
    // rest of a partial send, a WebSocket upgrade) is the loop's to do now, and waiting for one more
    // event besides would hold it until the next event or timer came, a second away when the peer
    // has nothing more to say. A pass with no sends waits for one event. Nothing waits while events
    // read before are still to be returned.
    const unsigned sends = p->sends_queued;
    p->sends_out += sends;
    p->sends_queued = 0;
    const bool have = p->at_ready < p->n_ready;
    if (cb_uring_enter(p->ur, have ? 0 : sends ? sends : 1, have ? 0 : timeout_ms) < 0) return -1;
    ur_reap(p);
    // A send the kernel did not finish within the enter (none should: they never wait) is waited
    // for here, so no sent buffer is in the kernel's hands when the caller moves on.
    while (p->sends_out > 0) {
        if (cb_uring_enter(p->ur, 1, -1) < 0 && errno != EINTR) return -1;
        ur_reap(p);
    }
    int k = 0;
    while (k < max && p->at_ready < p->n_ready) out[k++] = p->ready[p->at_ready++];
    if (p->at_ready == p->n_ready) p->at_ready = p->n_ready = 0;
    return k;
}

int cb_poller_wait(cb_poller *p, cb_poll_ev *out, int max, int timeout_ms) {
    if (p->ur) return ur_wait(p, out, max, timeout_ms);
    struct epoll_event ev[256];
    if (max > 256) max = 256;
    int n = epoll_wait(p->ep, ev, max, timeout_ms);
    int m = 0;
    for (int i = 0; i < n; i++) {
        if ((uint32_t)(ev[i].data.u64 >> 32) == ALARM_TAG) {   // the alarm rang: read, and none is set now
            uint64_t x;
            if (read(p->alarm_fd, &x, sizeof x) < 0) {}
            p->alarm_at = -1;
            continue;
        }
        out[m].fd = (int)(uint32_t)ev[i].data.u64;
        out[m].tag = (uint32_t)(ev[i].data.u64 >> 32);
        out[m].events = (ev[i].events & EPOLLIN ? CB_EV_READ : 0) | (ev[i].events & EPOLLOUT ? CB_EV_WRITE : 0) |
                        (ev[i].events & (EPOLLERR | EPOLLHUP) ? CB_EV_ERR : 0);
        m++;
    }
    return n < 0 ? -1 : m;
}

int cb_poller_wait_until(cb_poller *p, cb_poll_ev *out, int max, int64_t until_ms) {
    if (p->ur) return ur_wait(p, out, max, timeout_of(until_ms));
    if (until_ms != p->alarm_at) {
        if (p->alarm_fd < 0) {
            p->alarm_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
            struct epoll_event e = { .events = EPOLLIN, .data.u64 = (uint64_t)ALARM_TAG << 32 | (uint32_t)p->alarm_fd };
            if (p->alarm_fd < 0 || epoll_ctl(p->ep, EPOLL_CTL_ADD, p->alarm_fd, &e) < 0) {
                if (p->alarm_fd >= 0) close(p->alarm_fd);
                p->alarm_fd = -1;
                return cb_poller_wait(p, out, max, timeout_of(until_ms));   // no timerfd: the wait's own timeout
            }
        }
        struct itimerspec its;
        memset(&its, 0, sizeof its);   // zero: disarmed
        if (until_ms >= 0) {
            its.it_value.tv_sec = until_ms / 1000;
            its.it_value.tv_nsec = (long)(until_ms % 1000) * 1000000L;
            if (!its.it_value.tv_sec && !its.it_value.tv_nsec) its.it_value.tv_nsec = 1;
        }
        if (timerfd_settime(p->alarm_fd, TFD_TIMER_ABSTIME, &its, NULL) < 0) return cb_poller_wait(p, out, max, timeout_of(until_ms));
        p->alarm_at = until_ms;
        p->alarms++;
    }
    return cb_poller_wait(p, out, max, -1);
}

uint64_t cb_poller_alarms(const cb_poller *p) { return p->alarms; }

#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__DragonFly__)

#include <stdio.h>
#include <sys/event.h>
#include <sys/time.h>
#include <sys/types.h>

struct cb_poller {
    int kq;
    int64_t alarm_at;   // the deadline the EVFILT_TIMER is set to; -1: none
    uint64_t alarms;    // times it was set (cb_poller_alarms)
};

#define ALARM_IDENT ((uintptr_t)-2)   // the alarm's timer: an ident no fd has

cb_poller *cb_poller_new(bool uring, char *why, size_t cap) {
    if (uring && why) snprintf(why, cap, "io_uring is Linux's");
    cb_poller *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->kq = kqueue();
    if (p->kq < 0) { free(p); return NULL; }
    p->alarm_at = -1;
    fcntl(p->kq, F_SETFD, FD_CLOEXEC);
    return p;
}

void cb_poller_free(cb_poller *p) {
    if (!p) return;
    close(p->kq);
    free(p);
}

const char *cb_poller_backend(const cb_poller *p) { (void)p; return "kqueue"; }
bool cb_poller_sends(const cb_poller *p) { (void)p; return false; }
bool cb_poller_send(cb_poller *p, int fd, const void *buf, size_t n, uint32_t cookie) {
    (void)p; (void)fd; (void)buf; (void)n; (void)cookie;
    return false;
}
int cb_poller_sent(cb_poller *p, cb_sent_ev *out, int max) { (void)p; (void)out; (void)max; return 0; }
unsigned cb_poller_queued(const cb_poller *p) { (void)p; return 0; }

// One filter at a time, applied now: a changelist that fails for one filter
// (a delete of a filter never added) must not hide the other's result.
static int kq_filter(cb_poller *p, int fd, int filter, bool on, uint32_t tag) {
    struct kevent k;
    EV_SET(&k, (uintptr_t)fd, filter, on ? EV_ADD | EV_ENABLE : EV_DELETE, 0, 0, (void *)(uintptr_t)tag);
    if (kevent(p->kq, &k, 1, NULL, 0, NULL) < 0) return on || errno != ENOENT ? -1 : 0;
    return 0;
}

int cb_poller_set(cb_poller *p, int fd, int have, int want, uint32_t tag) {
    if (have < 0) have = 0;
    // A re-add also refreshes the tag, so it is sent again for an unchanged filter.
    if ((want & CB_EV_READ) && kq_filter(p, fd, EVFILT_READ, true, tag)) return -1;
    if (!(want & CB_EV_READ) && (have & CB_EV_READ) && kq_filter(p, fd, EVFILT_READ, false, tag)) return -1;
    if ((want & CB_EV_WRITE) && kq_filter(p, fd, EVFILT_WRITE, true, tag)) return -1;
    if (!(want & CB_EV_WRITE) && (have & CB_EV_WRITE) && kq_filter(p, fd, EVFILT_WRITE, false, tag)) return -1;
    return 0;
}

void cb_poller_del(cb_poller *p, int fd, int have) {
    if (have < 0) return;
    if (have & CB_EV_READ) kq_filter(p, fd, EVFILT_READ, false, 0);
    if (have & CB_EV_WRITE) kq_filter(p, fd, EVFILT_WRITE, false, 0);
}

int cb_poller_wait(cb_poller *p, cb_poll_ev *out, int max, int timeout_ms) {
    struct kevent ev[256];
    if (max > 256) max = 256;
    struct timespec ts, *tp = NULL;
    if (timeout_ms >= 0) {
        ts.tv_sec = timeout_ms / 1000;
        ts.tv_nsec = (long)(timeout_ms % 1000) * 1000000L;
        tp = &ts;
    }
    int n = kevent(p->kq, NULL, 0, ev, max, tp);
    // A fd can have both filters ready: kqueue reports two events, the loop wants one.
    int m = 0;
    for (int i = 0; i < n; i++) {
        if (ev[i].filter == EVFILT_TIMER) { p->alarm_at = -1; continue; }   // the alarm rang (one-shot)
        int fd = (int)ev[i].ident;
        int e = ev[i].filter == EVFILT_READ ? CB_EV_READ : ev[i].filter == EVFILT_WRITE ? CB_EV_WRITE : 0;
        if (ev[i].flags & EV_ERROR) e = CB_EV_ERR;
        // EOF on the read filter stays a read (the read returns 0); on the write filter it is a hangup.
        if ((ev[i].flags & EV_EOF) && ev[i].filter == EVFILT_WRITE) e |= CB_EV_ERR;
        int j = m - 1;
        if (j >= 0 && out[j].fd == fd) { out[j].events |= e; continue; }
        out[m].fd = fd;
        out[m].events = e;
        out[m].tag = (uint32_t)(uintptr_t)ev[i].udata;
        m++;
    }
    return n < 0 ? -1 : m;
}

int cb_poller_wait_until(cb_poller *p, cb_poll_ev *out, int max, int64_t until_ms) {
    if (until_ms != p->alarm_at) {
        struct kevent k;
        if (until_ms < 0) {
            EV_SET(&k, ALARM_IDENT, EVFILT_TIMER, EV_DELETE, 0, 0, NULL);
            (void)kevent(p->kq, &k, 1, NULL, 0, NULL);   // (ENOENT: it had rung)
        } else {
            const int64_t in = until_ms - mono_ms();
            EV_SET(&k, ALARM_IDENT, EVFILT_TIMER, EV_ADD | EV_ENABLE | EV_ONESHOT, 0, in > 0 ? in : 0, NULL);
            if (kevent(p->kq, &k, 1, NULL, 0, NULL) < 0) return cb_poller_wait(p, out, max, timeout_of(until_ms));
        }
        p->alarm_at = until_ms;
        p->alarms++;
    }
    return cb_poller_wait(p, out, max, -1);
}

uint64_t cb_poller_alarms(const cb_poller *p) { return p->alarms; }

#else
#error "c_base's event loop needs epoll (Linux) or kqueue (macOS, BSD); there is no poll(2) fallback"
#endif
