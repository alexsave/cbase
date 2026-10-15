// bots.h - the bot loop, rebuilt: a lease, a drive, a scan.
//
// The semantics being preserved are today's, in
// foolish/server/impls/supabase/functions/_shared/adapter/bot_actions.ts and
// functions/bot-heartbeat/index.ts. There are no timers in a Lambda, so:
//   * after a commit that leaves bot work, the request kicks a drive
//     (ctx->kick_bots: an async self-invoke under Lambda, the bot pool under serve);
//   * a drive takes the game's lease (store lease_acquire, CB_BOT_LEASE_MS) or
//     returns at once when someone else holds it; then loops: load the row,
//     table_set_deal_seed, table_set_session_log, table_bot_wait_ms (sleep that
//     long, then reload and ask again: a move meanwhile restarts the wait),
//     table_bot_drive, rate if wanted, table_commit_products, CAS commit,
//     fan-out, renew the lease; until no bot can move, the lease is lost, or
//     the budget runs out (the invocation deadline minus a margin, and a wall
//     ceiling of CB_BOT_WALL_MS); a lost CAS retries with table_drive_prefs.
//     The lease is released on every exit path. If work remains when the
//     budget ends, it kicks a fresh drive and returns;
//   * a scan (EventBridge every minute under Lambda, a 10 s timer under serve)
//     finds games with needs_bots whose updated_at is older than
//     CB_BOT_STALE_MS and whose last_commit_at is within CB_BOT_ABANDON_MS, and
//     kicks each (at most CB_BOT_SCAN_MAX);
//   * a client's ASK_BUMP kicks a drive when the table has bot work.
#ifndef CB_BOTS_H
#define CB_BOTS_H

#include "ctx.h"

#include <stdint.h>

#define CB_BOT_LEASE_MS    25000      // BOT_LEASE_TTL_MS
#define CB_BOT_STALE_MS    10000      // STALE_MS
#define CB_BOT_ABANDON_MS  3600000    // ABANDON_MS: a game idle an hour gets no bots until a kick
#define CB_BOT_WALL_MS     120000     // the drive's wall ceiling
#define CB_BOT_SCAN_MAX    100        // MAX_GAMES

// Drives one game's bots until done or out of budget (see above). Blocks.
// CB_OK (including "someone else holds the lease"), CB_E_NOT_FOUND, CB_E_DB.
int cb_bots_drive(cb_ctx *ctx, const char *game_id);
// The heartbeat's scan: kicks every due game. Returns how many it kicked.
int cb_bots_scan(cb_ctx *ctx, int64_t now_ms);

// serve's cb_kick_fn: submits cb_bots_drive to ctx->bot_pool.
void cb_bots_kick_local(cb_ctx *ctx, const char *game_id);
// Lambda's cb_kick_fn: an async self-invoke with the literal "bot_drive:<id>"
// (aws/lambda_invoke.h).
void cb_bots_kick_lambda(cb_ctx *ctx, const char *game_id);

#endif
