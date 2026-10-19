// poller.h - the OS readiness queue behind the event loop (loop.c), and its one owner.
//
// epoll on Linux, kqueue on macOS and the BSDs. There is no poll(2) fallback:
// every platform c_base builds for has one of the two, and an unknown one is
// a compile error rather than a silent O(fds) wake. Level-triggered, like
// the poll(2) the loop used to call, so a callback that does not drain an fd
// is told again. The cost of a wake is the fds that are ready, not the fds
// that are registered.
//
// Loop-thread-only, like everything in loop.h.
#ifndef CB_POLLER_H
#define CB_POLLER_H

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

cb_poller *cb_poller_new(void);
void       cb_poller_free(cb_poller *p);

// Makes the kernel watch `fd` for `want` (CB_EV_READ|CB_EV_WRITE) given that it
// currently watches `have` (0: not registered). want 0 keeps the fd registered
// but silent for readiness (errors and hangups still report on epoll, like
// poll(2); kqueue reports nothing). Returns 0, or -1 with errno.
int cb_poller_set(cb_poller *p, int fd, int have, int want, uint32_t tag);
// Forgets fd (still open); `have` is what it was registered for.
void cb_poller_del(cb_poller *p, int fd, int have);
// Waits up to timeout_ms (-1: forever) for at most `max` ready fds. Returns
// the count (0 on timeout), or -1 with errno (EINTR included).
int cb_poller_wait(cb_poller *p, cb_poll_ev *out, int max, int timeout_ms);

#endif
