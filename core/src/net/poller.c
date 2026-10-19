#include "poller.h"

#include "loop.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>

#if defined(__linux__)

#include <sys/epoll.h>

struct cb_poller {
    int ep;
};

cb_poller *cb_poller_new(void) {
    cb_poller *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->ep = epoll_create1(EPOLL_CLOEXEC);
    if (p->ep < 0) { free(p); return NULL; }
    return p;
}

void cb_poller_free(cb_poller *p) {
    if (!p) return;
    close(p->ep);
    free(p);
}

static uint32_t ep_mask(int want) {
    return (want & CB_EV_READ ? EPOLLIN : 0u) | (want & CB_EV_WRITE ? EPOLLOUT : 0u);
}

int cb_poller_set(cb_poller *p, int fd, int have, int want, uint32_t tag) {
    struct epoll_event e = { .events = ep_mask(want), .data.u64 = ((uint64_t)tag << 32) | (uint32_t)fd };
    // `have` -1 is "not registered"; 0 is registered and silent.
    return epoll_ctl(p->ep, have < 0 ? EPOLL_CTL_ADD : EPOLL_CTL_MOD, fd, &e);
}

void cb_poller_del(cb_poller *p, int fd, int have) {
    if (have < 0) return;
    struct epoll_event e = { 0 };
    epoll_ctl(p->ep, EPOLL_CTL_DEL, fd, &e);
}

int cb_poller_wait(cb_poller *p, cb_poll_ev *out, int max, int timeout_ms) {
    struct epoll_event ev[256];
    if (max > 256) max = 256;
    int n = epoll_wait(p->ep, ev, max, timeout_ms);
    for (int i = 0; i < n; i++) {
        out[i].fd = (int)(uint32_t)ev[i].data.u64;
        out[i].tag = (uint32_t)(ev[i].data.u64 >> 32);
        out[i].events = (ev[i].events & EPOLLIN ? CB_EV_READ : 0) | (ev[i].events & EPOLLOUT ? CB_EV_WRITE : 0) |
                        (ev[i].events & (EPOLLERR | EPOLLHUP) ? CB_EV_ERR : 0);
    }
    return n;
}

#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__DragonFly__)

#include <sys/event.h>
#include <sys/time.h>
#include <sys/types.h>

struct cb_poller {
    int kq;
};

cb_poller *cb_poller_new(void) {
    cb_poller *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->kq = kqueue();
    if (p->kq < 0) { free(p); return NULL; }
    fcntl(p->kq, F_SETFD, FD_CLOEXEC);
    return p;
}

void cb_poller_free(cb_poller *p) {
    if (!p) return;
    close(p->kq);
    free(p);
}

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

#else
#error "c_base's event loop needs epoll (Linux) or kqueue (macOS, BSD); there is no poll(2) fallback"
#endif
