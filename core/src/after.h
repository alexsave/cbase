// after.h - what an invocation does once its answer is out.
//
// A Lambda caller waits for the response the runtime API is handed, not for
// the invocation to end, and the runtime takes the next event only when the
// process asks for it. So an HTTP invocation answers first and then does what
// its caller does not wait for (main_lambda.c): whatever the app deferred
// (foolish: each commit's pushes, app/fanout.h, and the bot drives a move or
// a meta wakes, app/bots.h), then the edge cache's invalidation (app/edge.h).
//
// cb_after_begin opens the window for this thread; while it is open
// cb_after_defer queues a step, in call order, and cb_after_run runs them in
// that order, closes the window, then flushes the edge cache. With the window
// closed (`cbase serve`, a job, a schedule, the tests unless they open one)
// cb_after_defer refuses and the caller acts at once, as before.
//
// ORDER. The queue is FIFO and cb_after_run drains it on the thread that
// filled it before that thread takes another event; under Lambda that thread
// is the only one that commits. A step that must keep an order of its own
// (foolish's pushes, in a game's commit order) takes its own lock.
//
// LOSS. A step the queue cannot hold (no memory) is refused, and its caller
// does it at once: late is better than lost.
#ifndef CB_AFTER_H
#define CB_AFTER_H

#include "ctx.h"

#include <stdbool.h>

void cb_after_begin(void);
// True while the window is open on this thread.
bool cb_after_open(void);

// A deferred step: run(ctx, arg) once the answer is out, then drop(arg) (may be NULL).
typedef void (*cb_after_fn)(cb_ctx *ctx, void *arg);
// Queues the step when the window is open (true; it now owns arg), else
// refuses (false: the caller does it now, and keeps arg).
bool cb_after_defer(cb_after_fn run, void (*drop)(void *arg), void *arg);

// Runs the queued steps in order, closes the window, flushes the edge cache.
// Safe with nothing queued and with the window closed.
void cb_after_run(cb_ctx *ctx);

#endif
