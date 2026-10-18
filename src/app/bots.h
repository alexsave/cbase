// bots.h - the bot loop, rebuilt: a lease, a drive, a scan.
//
// The semantics preserved are the TypeScript server's (its bot_actions.ts and
// bot-heartbeat function, deleted once c_base served every client). There are no timers in a Lambda, so:
//   * after a commit that leaves bot work, the request kicks a drive
//     (the job CB_BOTS_JOB, jobs.h: an async self-invoke under Lambda, the job pool under serve);
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
//     finds games with needs_bots (a bot can act: table.h table_bots_due, so
//     never a game waiting on its human) whose updated_at is older than
//     CB_BOT_STALE_MS and whose last_commit_at is within CB_BOT_ABANDON_MS, and
//     kicks each (at most CB_BOT_SCAN_MAX);
//   * a client's ASK_BUMP kicks a drive when a bot can act;
//   * a drive that finds no bot can act on a row the scan holds takes the
//     game out of the scan (store bots_idle), fenced on the row's version.
#ifndef CB_BOTS_H
#define CB_BOTS_H

#include "ctx.h"
#include "table.h"   // the kernel's Table

#include <stdint.h>

// Production's timers. Each is used through cb_bots_ms, which divides it by
// CBASE_BOT_SPEEDUP (config.h; 1 in production, refused under Lambda), and
// unit_app's production_bot_timers pins every value here.
#define CB_BOT_LEASE_MS      25000      // BOT_LEASE_TTL_MS
#define CB_BOT_STALE_MS      10000      // STALE_MS
#define CB_BOT_ABANDON_MS    3600000    // ABANDON_MS: a game idle an hour gets no bots until a kick
#define CB_BOT_WALL_MS       120000     // the drive's wall ceiling
#define CB_BOT_SCAN_EVERY_MS 10000      // serve's heartbeat (main_serve.c); EventBridge's minute under Lambda
#define CB_BOT_SCAN_MAX      100        // MAX_GAMES

// A bot-loop timer as this process runs it: `production_ms` divided by the
// configured speedup (rounded up, so never 0). production_ms itself at speedup
// 1 and with no config.
int64_t cb_bots_ms(const cb_ctx *ctx, int64_t production_ms);
// THE BOARD'S TIME, the now every kernel call about one game is handed (the
// commit's, which stamps the board clock the bots pace by, and the bots' wait
// question): at the configured speedup k it runs k times faster than the wall
// clock from the game's creation (created_at, the row's, so a restarted
// process keeps the same time for the same game), and at k 1 it is now_ms.
// Every animation a commit queues and every pace then plays k times faster,
// a queue of human moves included. Only the kernel sees it: the row's
// updated_at and last_commit_at, the lease and the scan stay on the wall clock
// (their windows are the cb_bots_ms ones).
int64_t cb_bots_board_now(const cb_ctx *ctx, int64_t created_at, int64_t now_ms);
// The real ms a drive waits before it acts on the loaded board: the kernel's
// wait (table_bot_wait_ms) asked at the board's time, over k and rounded up,
// so the ask after the sleep, on the same board, answers 0. At k 1 the
// kernel's answer at now_ms, unchanged.
int64_t cb_bots_wait_ms(const cb_ctx *ctx, const Table *t, int64_t created_at, int64_t now_ms);

// Drives one game's bots until done or out of budget (see above). Blocks.
// CB_OK (including "someone else holds the lease"), CB_E_NOT_FOUND, CB_E_DB.
int cb_bots_drive(cb_ctx *ctx, const char *game_id);
// Test seam: called before each table_bot_drive with what the drive is handed:
// the session log's length (table_set_session_log) and the length of the moves
// a lost CAS already chose (the prefs). NULL in production.
extern void (*cb_bots_cycle_observer)(const char *game_id, int attempt, int logs_len, int prefs_len);
// The heartbeat's scan: kicks every due game. Returns how many it kicked.
int cb_bots_scan(cb_ctx *ctx, int64_t now_ms);

// A kick as every caller makes one (a commit's, a bump's, the scan's): through
// the job CB_BOTS_JOB (jobs.h), unless another drive holds the game's lease. That drive
// reloads before every cycle and looks once more after it lets go, so it
// takes the move; a second drive would only find the lease taken.
void cb_bots_kick(cb_ctx *ctx, const char *game_id);

// The drive as the app's job (cbase/app.h): cb_bots_drive under this name,
// so a kick is the payload "bot_drive:<id>" under Lambda (jobs.h).
#define CB_BOTS_JOB "bot_drive"
// The scan as the app's schedule, whose EventBridge input is this literal.
#define CB_BOTS_SCHEDULE "scan"

#endif
