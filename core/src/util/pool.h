// pool.h - a fixed set of worker threads draining one FIFO of tasks.
//
// cbase runs up to three: the REQUEST pool, where every HTTP handler that may block
// (SQLite, bcrypt, a document's lock) runs so the network loop never waits; the SLOW pool,
// where a CB_ROUTE_SLOW handler (net/router.h: one that holds its thread for seconds or minutes
// on a remote call) runs so it never holds a request worker; and the JOB pool, where an app's
// jobs (cbase/app.h cb_job) run so a slow job never delays a request. A task is a function and
// a pointer; it owns the pointer.
//
// A pool is UNBOUNDED (cb_pool_new: every submit is queued) or BOUNDED (cb_pool_new_bounded:
// at most threads + queue tasks queued or running, and a submit past that is refused at once
// with CB_E_FULL, never waited for).
#ifndef CB_POOL_H
#define CB_POOL_H

#include <stdbool.h>
#include <stddef.h>

typedef struct cb_pool cb_pool;
typedef void (*cb_task_fn)(void *ud);

// `threads` workers (at least 1), named "<name>-<i>" where the OS allows it.
// NULL when a thread cannot be started.
cb_pool *cb_pool_new(int threads, const char *name);
// cb_pool_new, bounded: at most `queue` (>= 0) tasks wait for a worker once every worker runs one.
cb_pool *cb_pool_new_bounded(int threads, const char *name, int queue);
// Queues fn(ud). CB_OK; CB_E_STATE once the pool is stopping, or CB_E_FULL when a bounded pool
// holds threads + queue tasks (queued or running) already: either way fn is not run and ud is the
// caller's to free. A task counts until its fn has returned.
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
