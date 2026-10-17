// table_ops.h - one game operation per request: load, run the kernel, commit
// with a compare-and-swap, push.
//
// The port of the TypeScript server's table_io.ts (runTableOp, commitProducts,
// ratingsFor) and server_table.ts, both deleted since, onto the C kernel called
// directly (foolish/c/src/table.h).
// There is no resident table: each request loads the row, runs ONE kernel
// section on per-thread scratch (a Game and TableSnaps, about 84 KB), and
// commits the products with store game_commit at expected_version = the
// version it loaded. A lost CAS reloads and re-runs, up to CB_TABLE_ATTEMPTS
// (5, as MAX_ATTEMPTS was). A warm process may cache the rows it just
// committed (game_cache.ts: PLAYING games only, 256 entries); a stale entry
// costs one CAS conflict, and a refusal computed from the cache is re-checked
// on a fresh read before it is answered (freshOnly).
//
// THE KERNEL SECTION, in order (server_table.ts names in brackets):
//   table_init, table_load(state, roster) [load], table_set_deal_seed(hex of
//   the 32-byte seed, lowercase, exactly as table_io.ts's bareHex wrote it)
//   [setDealSeed], the operation, table_commit_products [commit]; when the
//   products say wants_ratings: store ratings_for over the seats, table_rate,
//   table_commit_products again (the products are const: no re-run is needed
//   in C, unlike the wasm host); then copy out every envelope and cast the
//   fan-out needs before the scratch is reused.
// Every kernel call takes now_ms from cb_now_ms(); the kernel never reads a
// clock, a socket, a JWT or the store.
//
// AFTER A WINNING COMMIT (all before the response, as today):
//   * fan-out (fanout.h) when n_events > 0 or goods_changed;
//   * finalize when products.ended: table_replay_code + table_replay_extras
//     into store snapshot_put, then the next commit clears logs (finalize.ts);
//     a failure is logged and swallowed, as today;
//   * TABLE_EMPTY deletes the game (store game_delete at the new version);
//   * needs_bots: ctx->kick_bots(ctx, id).
#ifndef CB_TABLE_OPS_H
#define CB_TABLE_OPS_H

#include "auth/auth.h"
#include "ctx.h"
#include "table.h"   // the kernel: Table, TableAsk, TABLE_*, ASK_*

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CB_TABLE_ATTEMPTS 5
#define CB_TABLE_ID_DRAWS 5      // create: game ids tried before giving up (MAX_ID_DRAWS)
#define CB_ENVELOPE_MAX   16384  // one envelope or cast, with room to spare

// The operation: run on the loaded table, in the one kernel section. Returns
// the kernel's result (TABLE_OK / TABLE_APPLIED, TABLE_REJECTED, TABLE_MOOT,
// TABLE_STALE_ROUND, TABLE_EMPTY, or a refusal). `row` is the row the table
// was loaded from (a move reads its round_epoch); `deal_seed` is 32 fresh
// random bytes for an edit that can deal. Called once per attempt.
typedef int (*cb_table_fn)(Table *t, const cb_game_row *row, const uint8_t deal_seed[32], void *ud);

typedef struct {
    const char *game_id;
    const cb_user *actor;      // whose envelope the reply carries (NULL: none)
    cb_table_fn run;
    void *ud;
    bool fresh;                // skip the warm cache (delete-account's redactions)
} cb_table_op;

typedef struct {
    int      result;           // the operation's TABLE_* result
    int      reject;           // the ENGINE_REJECT_* behind TABLE_REJECTED
    int      detail;           // the ROSTER_E_* behind TABLE_E_ROSTER
    bool     committed;        // a commit was written (MOOT, REJECTED, STALE_ROUND write nothing)
    int64_t  version;          // the version the client should now hold
    bool     needs_bots;       // the committed table has bot work (the caller may kick)
    // The actor's envelope at `version` (their seat's, or the spectator's when
    // not seated): the body create/meta answer with. Owned; free() it.
    uint8_t *envelope;
    size_t   envelope_len;
} cb_table_result;

// A game's commit order lock, taken (striped by id; table_ops.c says why a
// commit and its pushes share it). The caller unlocks what it returns.
pthread_mutex_t *cb_table_order_lock(const char *gid);

// Runs one operation with the CAS retry loop. CB_OK (look at out->result for
// the kernel's answer), CB_E_NOT_FOUND (no such game), CB_E_CONFLICT (lost the
// CAS CB_TABLE_ATTEMPTS times: "write contention"), CB_E_DB.
int cb_table_run(cb_ctx *ctx, const cb_table_op *op, cb_table_result *out);

// /functions/v1/create: a new lobby with `u` seated, at a fresh 6-hex id
// (CB_TABLE_ID_DRAWS tries). out->envelope is the creator's.
int cb_table_create(cb_ctx *ctx, const cb_user *u, cb_table_result *out);

// One viewer's envelope of a stored game at its stored version, for PostgREST's
// player_views (viewer_id seated) and spectator_views (viewer_id NULL). This is
// the view row that used to be a table: computed on demand, never stored.
typedef struct {
    char     game_id[CB_GAME_ID_MAX + 1];
    cb_uuid  player_id;        // "" for a spectator row
    uint8_t *view;             // owned; free() it
    size_t   view_len;
    int64_t  version;
    char     status[16];       // "waiting" | "playing" | "game_over" (gameStatusLabel)
    int64_t  updated_at;
} cb_view_row;

void cb_view_row_free(cb_view_row *v);
// CB_OK, CB_E_NOT_FOUND (no game; or viewer_id given and not seated there).
int cb_table_view(cb_ctx *ctx, const char *game_id, const char *viewer_id, cb_view_row *out);
// Every game the user sits at, most recently updated first (the dashboard's
// player_views query), each with the user's envelope. fn returns false to stop.
typedef bool (*cb_view_fn)(const cb_view_row *v, void *ud);
int cb_table_views_of_user(cb_ctx *ctx, const char *user_id, int limit, cb_view_fn fn, void *ud);

// The status label the facade prints for a GAME_STATUS_* number.
const char *cb_table_status_label(int status);

#endif
