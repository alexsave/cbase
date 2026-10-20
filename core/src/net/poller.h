// poller.h - the OS readiness queue behind the event loop (loop.c), and its one owner.
//
// epoll (or io_uring, when asked for) on Linux, kqueue on macOS and the BSDs. There is no poll(2) fallback:
// every platform c_base builds for has one of the two, and an unknown one is
// a compile error rather than a silent O(fds) wake. Level-triggered, like
// the poll(2) the loop used to call, so a callback that does not drain an fd
// is told again. The cost of a wake is the fds that are ready, not the fds
// that are registered.
//
// Loop-thread-only, like everything in loop.h.
#ifndef CB_POLLER_H
#define CB_POLLER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct cb_poller cb_poller;

// One ready fd. `tag` is what cb_poller_set was given when the fd was
// registered (the loop's generation), so a result for an fd that was removed
// and reused in the same batch is told apart from the new one.
typedef struct {
    int      fd;
    int      events;   // CB_EV_READ | CB_EV_WRITE | CB_EV_ERR (loop.h)
    uint32_t tag;
} cb_poll_ev;

// The kernel queue: kqueue on the BSDs and macOS; on Linux io_uring when `uring` is asked for and
// the kernel allows it (else epoll, with *why saying why not; why may be NULL).
cb_poller *cb_poller_new(bool uring, char *why, size_t cap);
void       cb_poller_free(cb_poller *p);
// "io_uring", "epoll" or "kqueue".
const char *cb_poller_backend(const cb_poller *p);

// Makes the kernel watch `fd` for `want` (CB_EV_READ|CB_EV_WRITE) given that it
// currently watches `have` (0: not registered). want 0 keeps the fd registered
// but silent for readiness (errors and hangups still report on epoll and
// io_uring, like poll(2); kqueue reports nothing). Returns 0, or -1 with errno.
// Under io_uring a watch is a one-shot poll armed again at every wait until it is
// changed: level-triggered, as epoll's.
int cb_poller_set(cb_poller *p, int fd, int have, int want, uint32_t tag);
// Forgets fd (still open); `have` is what it was registered for.
void cb_poller_del(cb_poller *p, int fd, int have);
// Waits up to timeout_ms (-1: forever) for at most `max` ready fds. Returns
// the count (0 on timeout), or -1 with errno (EINTR included).
int cb_poller_wait(cb_poller *p, cb_poll_ev *out, int max, int timeout_ms);
// THE ALARM: waits for ready fds until the CLOCK_MONOTONIC ms `until_ms` (-1: forever), as
// cb_poller_wait. epoll and kqueue keep the deadline in a kernel timer of their own (a timerfd, an
// EVFILT_TIMER) set only when it changes, so a wait arms no timer of its own: a wait until the
// same deadline as the last costs no timer work in the kernel. io_uring takes it as the enter's
// timeout. cb_poller_alarms: how many times the timer was set.
int cb_poller_wait_until(cb_poller *p, cb_poll_ev *out, int max, int64_t until_ms);
uint64_t cb_poller_alarms(const cb_poller *p);

// BATCHED SENDS (io_uring only: cb_poller_sends). A send of [p, p+n) on fd, handed to the kernel
// with the next wait and never waiting for room there (a full socket buffer answers -EAGAIN): all of
// a wait's sends have completed when it returns, and cb_poller_sent gives their results back, each
// with the cookie it was queued with, before the caller looks at any ready fd. The bytes must stay
// as they are until then. false: the queue is full (send it another way).
bool cb_poller_sends(const cb_poller *p);
bool cb_poller_send(cb_poller *p, int fd, const void *buf, size_t n, uint32_t cookie);
// What waits for the next wait to reach the kernel (io_uring: sends, polls to arm or remove - a
// closed socket is released only once its poll's removal has): nonzero means one more wait is due.
unsigned cb_poller_queued(const cb_poller *p);
typedef struct { uint32_t cookie; int32_t res; } cb_sent_ev;   // res: bytes sent, or -errno
int  cb_poller_sent(cb_poller *p, cb_sent_ev *out, int max);

#endif
