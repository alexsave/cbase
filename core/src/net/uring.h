// uring.h - a minimal io_uring over the raw system calls (the static musl build has no liburing).
//
// Only what the loop's io_uring poller (poller.c) needs: one ring, submission queue entries filled
// in place and handed to the kernel with the next wait, a wait with a timeout, and the completions
// read back in order. Linux only (poller.c uses kqueue elsewhere); where the kernel, its
// configuration or a seccomp filter refuses io_uring, cb_uring_new answers NULL with the reason, and
// the loop uses epoll.
//
// Loop-thread-only, like the poller that owns it.
#ifndef CB_URING_H
#define CB_URING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct cb_uring cb_uring;

// A ring of `entries` submissions (a power of two) and 4x as many completions, or NULL with why.
cb_uring *cb_uring_new(unsigned entries, char *why, size_t cap);
void cb_uring_free(cb_uring *u);

// The submission kinds the poller uses. Each fills one entry; false when the queue is full (the
// caller submits with cb_uring_enter and tries again).
bool cb_uring_poll_add(cb_uring *u, int fd, uint32_t poll_mask, uint64_t ud);   // one shot
bool cb_uring_poll_remove(cb_uring *u, uint64_t target_ud, uint64_t ud);
bool cb_uring_send(cb_uring *u, int fd, const void *p, size_t n, uint64_t ud);   // never waits: MSG_DONTWAIT
// Entries filled and not yet handed to the kernel.
unsigned cb_uring_pending(const cb_uring *u);

// Hands every filled entry to the kernel and waits until `min_complete` completions are ready or
// timeout_ms passes (-1: no limit; 0 with min_complete 0: never waits). 0, or -1 with errno
// (EINTR; a timeout is not an error).
int cb_uring_enter(cb_uring *u, unsigned min_complete, int timeout_ms);

// The next completion, consumed: false when there is none ready.
bool cb_uring_next(cb_uring *u, uint64_t *ud, int32_t *res);

#endif
