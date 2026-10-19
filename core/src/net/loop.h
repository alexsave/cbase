// loop.h - the network event loop: one thread, epoll or kqueue (poller.h),
// timers, and a way in from other threads.
//
// The loop thread owns every socket. It never blocks on anything but the wait:
// handlers that touch SQLite, bcrypt or a game's lock run on the request pool
// (router.h CB_ROUTE_POOL) and hand their result back with cb_loop_post. A wake
// costs the fds that are ready, not the fds that are registered (poll(2) was
// 15% of a nano's CPU at 64 games and 62% at 512).
//
// THREADING. Everything here is loop-thread-only EXCEPT cb_loop_post and
// cb_loop_stop, which any thread may call, and cb_loop_stop is also safe inside
// a signal handler.
#ifndef CB_LOOP_H
#define CB_LOOP_H

#include <stdbool.h>
#include <stdint.h>

typedef struct cb_loop cb_loop;

#define CB_EV_READ  0x1
#define CB_EV_WRITE 0x2
#define CB_EV_ERR   0x4   // reported, never requested: error or hangup

// `events` is what fired (CB_EV_*). The callback may cb_loop_remove its own fd,
// or any other, and may close it after removing it.
typedef void (*cb_io_fn)(cb_loop *l, int fd, int events, void *ud);
typedef void (*cb_timer_fn)(cb_loop *l, void *ud);
typedef void (*cb_post_fn)(cb_loop *l, void *ud);

cb_loop *cb_loop_new(void);
// Frees the loop. Registered fds are not closed. Posts queued before
// cb_loop_run returned have run; a post after that is refused (CB_E_STATE),
// so its caller still owns what it carried.
void cb_loop_free(cb_loop *l);

// Watches fd (made non-blocking by the caller) for `events`. CB_E_CONFLICT if
// the fd is already registered, CB_E_NOMEM.
int  cb_loop_add(cb_loop *l, int fd, int events, cb_io_fn fn, void *ud);
// Changes what is watched (0 pauses the fd without forgetting it).
int  cb_loop_set(cb_loop *l, int fd, int events);
void cb_loop_remove(cb_loop *l, int fd);

// Runs fn(ud) once, `delay_ms` from now (0: on the next iteration). Returns a
// non-zero id, or 0 on allocation failure.
uint64_t cb_loop_timer(cb_loop *l, int64_t delay_ms, cb_timer_fn fn, void *ud);
// true when the timer was pending and will not run.
bool cb_loop_cancel(cb_loop *l, uint64_t id);

// ANY THREAD: runs fn(ud) on the loop thread soon, in post order.
// CB_OK, CB_E_NOMEM, or CB_E_STATE once the loop has stopped.
int  cb_loop_post(cb_loop *l, cb_post_fn fn, void *ud);
// ANY THREAD, or a signal handler: cb_loop_run returns after this iteration.
void cb_loop_stop(cb_loop *l);

// Runs until cb_loop_stop. Returns CB_OK, or CB_E_IO if the wait fails for good.
int  cb_loop_run(cb_loop *l);
// Monotonic ms (clock.h cb_mono_ms), as of the start of this iteration.
int64_t cb_loop_now(const cb_loop *l);
// true on the thread inside cb_loop_run.
bool cb_loop_on_thread(const cb_loop *l);

#endif
