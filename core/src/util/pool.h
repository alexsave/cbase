// pool.h - a fixed set of worker threads draining one FIFO of tasks.
//
// c_base runs two: the REQUEST pool, where every HTTP handler that may block
// (SQLite, bcrypt, a game's lock) runs so the network loop never waits, and the
// BOT pool (app/bots_sched.h), where the kernel's solvers run so a slow bot
// never delays a human's request. A task is a function and a pointer; it owns
// the pointer.
#ifndef CB_POOL_H
#define CB_POOL_H

#include <stdbool.h>
#include <stddef.h>

typedef struct cb_pool cb_pool;
typedef void (*cb_task_fn)(void *ud);

// `threads` workers (at least 1), named "<name>-<i>" where the OS allows it.
// NULL when a thread cannot be started.
cb_pool *cb_pool_new(int threads, const char *name);
// Queues fn(ud). CB_OK, or CB_E_STATE once the pool is stopping (fn is not run
// and ud is the caller's to free).
int cb_pool_submit(cb_pool *p, cb_task_fn fn, void *ud);
// true when no task is queued or running: one atomic load, no lock. A
// caller that acts on it must tolerate the answer going stale at once.
bool cb_pool_idle(cb_pool *p);
// Tasks queued and not yet started.
size_t cb_pool_pending(cb_pool *p);
// Stops taking tasks, runs every task already queued, joins the workers and
// frees the pool. Must not be called from one of the pool's own tasks.
void cb_pool_free(cb_pool *p);

#endif
