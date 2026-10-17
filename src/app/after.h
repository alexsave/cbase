// after.h - what an invocation does once its answer is out.
//
// A Lambda caller waits for the response the runtime API is handed, not for
// the invocation to end, and the runtime takes the next event only when the
// process asks for it. So an HTTP invocation answers first and then does what
// its caller does not wait for (main_lambda.c):
//
//   * the pushes of each commit it made (cb_fanout_commit): the stream's
//     connection Queries and @connections POSTs;
//   * the bot drive a move or a meta wakes (cb_after_kick): the async invoke;
//   * the edge cache's invalidation (app/edge.h).
//
// cb_after_begin opens the window for this thread; while it is open
// cb_fanout_commit and cb_after_kick queue their work, in call order, and
// cb_after_run does it in that order, closes the window, then flushes the
// edge cache. With the window closed (`cbase serve`, a bot drive, the scan,
// the tests unless they open one) both act at once, as before.
//
// ORDER. A game's pushes still go out in its commit order: the queue is FIFO,
// cb_after_run drains it on the thread that filled it before that thread
// takes another event, and under Lambda that thread is the only one that
// commits; each queued fan-out is still published under the game's order lock
// (cb_table_order_lock), the lock a commit and its pushes have always shared.
// A response can now arrive before its own pushes, which the clients already
// handle: they gate every push on its version.
//
// LOSS. Queuing copies the fan-out; a copy that cannot be made is published at
// once instead (logged), never dropped. A push that fails is logged by the
// transport as before, and because the invocation has already answered, a
// failure can no longer fail it: cb_after_run logs each fan-out whose pushes
// did not all arrive, with the game and version, at WARN.
#ifndef CB_AFTER_H
#define CB_AFTER_H

#include "ctx.h"
#include "fanout.h"

#include <stdbool.h>

void cb_after_begin(void);
// True while the window is open on this thread.
bool cb_after_open(void);

// Queues a copy of f when the window is open (true), else does nothing (false):
// cb_fanout_commit publishes it then.
bool cb_after_fanout(const cb_fanout *f);

// Wakes the game's bots (ctx->kick_bots): queued while the window is open.
void cb_after_kick(cb_ctx *ctx, const char *game_id);

// Does the queued work in order, closes the window, flushes the edge cache.
// Safe with nothing queued and with the window closed.
void cb_after_run(cb_ctx *ctx);

#endif
