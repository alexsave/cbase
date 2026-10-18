// bots.c - the bot loop, rebuilt (bots.h): a lease, a drive, a scan.
//
// The port of bot_actions.ts (lockedBotLoop, runCycle) and
// functions/bot-heartbeat/index.ts. What moved: the CPU soft budget was the
// edge runtime's ~2 s per request; a Lambda has none, so the budget here is
// the invocation's deadline (ctx->deadline_ms) minus a margin, predicted the
// same way (the next cycle costs max(average * 1.5, the slowest so far)), plus
// the same 120 s wall ceiling. Continuation was the heartbeat's alone; here a
// drive that stops on its budget with work left kicks a fresh one itself.
#include "bots.h"

#include "app_internal.h"
#include "store/store.h"
#include "util/clock.h"
#include "util/err.h"
#include "util/log.h"
#include "util/pool.h"
#include "aws/lambda_invoke.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BOT_PREDICT_FACTOR      1.5
#define BOT_DEADLINE_MARGIN_MS  5000   // Lambda: stop this long before the invocation's deadline
#define BOT_PREFS_CAP           4096

void (*cb_bots_cycle_observer)(const char *game_id, int attempt, int logs_len, int prefs_len);

typedef struct {
    double  compute_ms;
    int     cycles;
    int64_t max_ms;
} cpu_acct;

// What one cycle came to.
enum { CYCLE_DONE = 0, CYCLE_AGAIN = 1, CYCLE_ERROR = -1 };

// One cycle, committed (runCycle). CYCLE_AGAIN with *delay_ms set to the
// kernel's wait (0 after a committed cycle), CYCLE_DONE when nothing is left to
// drive (no bot can act, the game ended, or the game is gone), CYCLE_ERROR.
// *idle_version is the version of a row the scan holds although the kernel
// says no bot can act on it (store.h bots_idle), else -1.
static int run_cycle(cb_ctx *ctx, cb_section *s, const char *game_id, int cycle, cpu_acct *cpu, int64_t *delay_ms,
                     int64_t *idle_version) {
    // The moves an attempt that lost the CAS already chose, offered back: the
    // kernel replays one only while the reloaded board still makes it legal.
    uint8_t prefs[BOT_PREFS_CAP];
    int prefs_len = 0;
    int32_t ratings[CB_MAX_SEATS];
    bool have_ratings = false;
    *delay_ms = 0;
    *idle_version = -1;

    for (int attempt = 1; attempt <= CB_TABLE_ATTEMPTS; attempt++) {
        // Always fresh, and always with the log: its length seeds every bot
        // decision (bot_drive.h), whatever the seated brains read.
        cb_game_row row;
        bool cached = false;
        int rc = cb_row_load(ctx, game_id, false, &row, &cached);
        if (rc == CB_E_NOT_FOUND) return CYCLE_DONE;
        if (rc != CB_OK) return CYCLE_ERROR;

        // ---- the kernel section ----
        int kr = cb_section_load(s, &row);
        if (kr < 0) {
            CB_ERROR("bots", "game %s: load refused (%d)", game_id, kr);
            cb_game_row_free(&row);
            return CYCLE_ERROR;
        }
        // The kernel's own answer, the one the row's needs_bots was written
        // from (table_bots_due): a row that says otherwise leaves the scan.
        if (!table_bots_due(&s->t)) {
            if (row.needs_bots) *idle_version = row.version;
            cb_game_row_free(&row);
            return CYCLE_DONE;
        }
        int64_t wait = cb_bots_wait_ms(ctx, &s->t, row.created_at, cb_app_now());
        if (wait > 0) { cb_game_row_free(&row); *delay_ms = wait; return CYCLE_AGAIN; }
        const int log_len = (int)row.logs_len;
        kr = table_set_session_log(&s->t, row.logs, log_len);
        if (kr < 0) {
            CB_ERROR("bots", "game %s: session log refused (%d)", game_id, kr);
            cb_game_row_free(&row);
            return CYCLE_ERROR;
        }
        if (cb_bots_cycle_observer) cb_bots_cycle_observer(game_id, attempt, log_len, prefs_len);
        BotDriveOut drv;
        const int64_t t0 = cb_mono_ms();
        kr = table_bot_drive(&s->t, prefs, prefs_len, 0, &drv);
        const int64_t drive_ms = cb_mono_ms() - t0;
        if (kr < 0) {
            CB_ERROR("bots", "game %s: bot drive refused (%d)", game_id, kr);
            cb_game_row_free(&row);
            return CYCLE_ERROR;
        }
        cpu->compute_ms += (double)drive_ms;
        cpu->cycles += 1;
        if (drive_ms > cpu->max_ms) cpu->max_ms = drive_ms;
        CB_INFO("bots", "game %s cycle %d: drove %d action(s) in %lldms (stop %d)", game_id, cycle, drv.n,
                (long long)drive_ms, drv.stop);
        if (drv.n == 0) {
            // table_bots_due said a bot could act and the drive found none:
            // the kernel disagrees with itself. Out of the scan all the same,
            // or the heartbeat would ask it the same question every minute.
            CB_WARN("bots", "game %s: a bot was due at version %lld and none acted", game_id, (long long)row.version);
            if (row.needs_bots) *idle_version = row.version;
            cb_game_row_free(&row);
            return CYCLE_DONE;
        }

        int pl = table_drive_prefs(&s->t, &drv, prefs, sizeof prefs);
        prefs_len = pl > 0 ? pl : 0;
        cb_commit_out co;
        rc = cb_section_commit(ctx, s, &row, NULL, NULL, ratings, &have_ratings, &co);
        cb_game_row_free(&row);
        if (rc != CB_OK) return CYCLE_ERROR;
        if (co.lost) continue;   // somebody else committed: reload and drive again
        return co.ended ? CYCLE_DONE : CYCLE_AGAIN;
    }
    CB_ERROR("bots", "could not commit game %s after %d attempts: write contention", game_id, CB_TABLE_ATTEMPTS);
    return CYCLE_ERROR;
}

// ---- the speedup (config.h CBASE_BOT_SPEEDUP) ----------------------------------------

static int64_t speedup(const cb_ctx *ctx) {
    return ctx && ctx->cfg && ctx->cfg->bot_speedup > 1 ? ctx->cfg->bot_speedup : 1;
}

int64_t cb_bots_ms(const cb_ctx *ctx, int64_t production_ms) {
    const int64_t k = speedup(ctx);
    return (production_ms + k - 1) / k;
}

int64_t cb_bots_board_now(const cb_ctx *ctx, int64_t created_at, int64_t now_ms) {
    const int64_t k = speedup(ctx);
    if (k == 1 || created_at <= 0 || now_ms <= created_at) return now_ms;
    return created_at + (now_ms - created_at) * k;
}

int64_t cb_bots_wait_ms(const cb_ctx *ctx, const Table *t, int64_t created_at, int64_t now_ms) {
    const int64_t k = speedup(ctx);
    const int64_t wait = table_bot_wait_ms(t, cb_bots_board_now(ctx, created_at, now_ms));
    return wait > 0 ? (wait + k - 1) / k : 0;
}

// ms left before this drive must stop, or INT64_MAX under serve (no deadline).
static int64_t budget_left(const cb_ctx *ctx) {
    if (ctx->deadline_ms <= 0) return INT64_MAX;
    return ctx->deadline_ms - BOT_DEADLINE_MARGIN_MS - cb_app_now();
}

// One leased round of a drive: acquire, cycle until done or out of budget,
// release. *settled is true when it let go because no bot could act (or the
// game ended) with the lease still its own: the case a kick skipped while the
// lease was held relies on (kick_wanted), so the caller looks once more.
static int drive_round(cb_ctx *ctx, const char *game_id, cb_store_tally *stp, bool *settled) {
    cb_store_tally st = *stp;
    *settled = false;
    cb_uuid lease;
    int rc = CB_STORE_CALL(ctx->store, lease_acquire, game_id, cb_app_now(), cb_bots_ms(ctx, CB_BOT_LEASE_MS), lease);
    if (rc == CB_E_CONFLICT) {
        CB_INFO("bots", "game %s: bot lease held by another drive, skipping", game_id);
        return CB_OK;
    }
    if (rc != CB_OK) {
        if (rc != CB_E_NOT_FOUND) CB_ERROR("bots", "game %s: lease not acquired (%s)", game_id, cb_strerror(rc));
        return rc;
    }
    cb_section *s = cb_section_new();
    if (!s) {
        CB_STORE_CALL(ctx->store, lease_release, game_id, lease);
        return CB_E_NOMEM;
    }
    cpu_acct cpu = { 0, 0, 0 };
    const int64_t started = cb_app_now();
    bool rekick = false;
    int64_t idle_version = -1;
    rc = CB_OK;
    for (int cycle = 0;; cycle++) {
        if (cycle > 0) {
            if (cb_app_now() - started > cb_bots_ms(ctx, CB_BOT_WALL_MS)) { rekick = true; break; }
            if (cpu.cycles > 0) {
                double avg = cpu.compute_ms / cpu.cycles;
                double predicted = avg * BOT_PREDICT_FACTOR;
                if (predicted < (double)cpu.max_ms) predicted = (double)cpu.max_ms;
                if (predicted > (double)budget_left(ctx)) { rekick = true; break; }
            }
            int r = CB_STORE_CALL(ctx->store, lease_renew, game_id, lease, cb_app_now(), cb_bots_ms(ctx, CB_BOT_LEASE_MS));
            // Another drive took the lease over; a store failure keeps going (renewBotLease).
            if (r == CB_E_CONFLICT) break;
            if (r != CB_OK) CB_ERROR("bots", "game %s: lease renewal failed (%s), driving on", game_id, cb_strerror(r));
        }
        int64_t delay = 0;
        int c = run_cycle(ctx, s, game_id, cycle, &cpu, &delay, &idle_version);
        cb_store_tally_take(&st);   // the lease (acquire or renew) and the cycle
        if (st.reads || st.writes)
            CB_DEBUG("store", "bot cycle %s/%d: %d reads, %d writes, %g RCU, %g WCU (%s)", game_id, cycle, st.reads,
                     st.writes, st.rcu, st.wcu, st.detail);
        if (c == CYCLE_ERROR) { rc = CB_E_DB; break; }
        if (c == CYCLE_DONE) { *settled = true; break; }
        if (delay > 0) {
            // A wait that would outlive the budget is the next drive's to sit out.
            if (delay > budget_left(ctx)) { rekick = true; break; }
            cb_app_sleep(delay);
        }
    }
    if (idle_version >= 0) {
        // Before the release: the delete takes the lease with the marker.
        int r = CB_STORE_CALL(ctx->store, bots_idle, game_id, idle_version);
        if (r == CB_OK)
            CB_INFO("bots", "game %s: no bot can act at version %lld, out of the scan", game_id, (long long)idle_version);
        else if (r != CB_E_CONFLICT)
            CB_ERROR("bots", "game %s: could not take it out of the scan (%s)", game_id, cb_strerror(r));
    }
    CB_STORE_CALL(ctx->store, lease_release, game_id, lease);
    cb_store_tally_take(&st);
    if (st.reads || st.writes)
        CB_DEBUG("store", "bot drive %s end: %d reads, %d writes, %g RCU, %g WCU (%s)", game_id, st.reads, st.writes,
                 st.rcu, st.wcu, st.detail);
    cb_section_free(s);
    if (rekick && ctx->kick_bots) {
        CB_INFO("bots", "game %s: budget spent with bot work left, kicking a fresh drive", game_id);
        ctx->kick_bots(ctx, game_id);
    }
    *stp = st;
    return rc;
}

// A bot can act on the stored row now (a fresh read).
static bool due_now(cb_ctx *ctx, const char *game_id) {
    cb_game_row row;
    bool cached = false;
    if (cb_row_load(ctx, game_id, false, &row, &cached) != CB_OK) return false;
    cb_section *s = cb_section_new();
    bool due = s && cb_section_load(s, &row) >= 0 && table_bots_due(&s->t);
    cb_section_free(s);
    cb_game_row_free(&row);
    return due;
}

#define BOT_DRIVE_ROUNDS 8

int cb_bots_drive(cb_ctx *ctx, const char *game_id) {
    if (!ctx->store) return CB_E_STATE;
    cb_store_tally st;
    cb_store_tally_take(&st);   // this drive's count starts here
    int rc = CB_OK;
    for (int round = 0; round < BOT_DRIVE_ROUNDS; round++) {
        bool settled = false;
        rc = drive_round(ctx, game_id, &st, &settled);
        // A commit that landed while this drive held the lease kicked no drive
        // (kick_wanted): its kick's lease check came after its commit, and this
        // read comes after the release, so one of the two saw the other.
        if (rc != CB_OK || !settled || !due_now(ctx, game_id)) break;
        CB_INFO("bots", "game %s: a move landed while the lease was held, driving again", game_id);
    }
    return rc;
}

// ---- the heartbeat's scan -----------------------------------------------------------

typedef struct {
    int  n;
    char ids[CB_BOT_SCAN_MAX][CB_GAME_ID_MAX + 1];
} scan_list;

static bool scan_collect(const char *id, void *ud) {
    scan_list *l = ud;
    if (l->n >= CB_BOT_SCAN_MAX) return false;
    snprintf(l->ids[l->n++], sizeof l->ids[0], "%s", id);
    return true;
}

int cb_bots_scan(cb_ctx *ctx, int64_t now_ms) {
    if (!ctx->store) return CB_E_STATE;
    scan_list *l = calloc(1, sizeof *l);
    if (!l) return CB_E_NOMEM;
    // Stale on updated_at (a drive in flight has just touched the row), active
    // on last_commit_at (only a commit moves it, so the scan's own leases never
    // refresh the abandon guard).
    int rc = CB_STORE_CALL(ctx->store, games_needing_bots, now_ms - cb_bots_ms(ctx, CB_BOT_STALE_MS),
                           now_ms - cb_bots_ms(ctx, CB_BOT_ABANDON_MS),
                           CB_BOT_SCAN_MAX, scan_collect, l);
    if (rc < 0) {
        CB_ERROR("bots", "heartbeat scan failed (%s)", cb_strerror(rc));
        free(l);
        return rc;
    }
    for (int i = 0; i < l->n; i++) cb_bots_kick(ctx, l->ids[i]);
    // An idle minute (or, under serve, an idle 10 s) is not news.
    if (l->n > 0) CB_INFO("bots", "heartbeat dispatched=%d", l->n);
    else CB_DEBUG("bots", "heartbeat dispatched=0");
    rc = l->n;
    free(l);
    return rc;
}

// ---- the two kicks ------------------------------------------------------------------

typedef struct {
    cb_ctx *ctx;
    char    game_id[CB_GAME_ID_MAX + 1];
} kick_task;

// The games with a drive queued and not yet started. A kick for one of them
// is dropped: the queued drive reads the row when it starts, so it sees
// whatever the later kick would have. Without this every bump during a long
// drive queued one more drive (thousands in a game), each of which later took
// the lease, read the row, found nothing and let go. The game leaves the set
// when its drive STARTS, so a kick during a running drive still queues one.
// Full (more games waiting than slots): kicks queue as they always did.
#define KICK_PENDING_MAX 1024
static pthread_mutex_t g_kick_mu = PTHREAD_MUTEX_INITIALIZER;
static char g_kick_pending[KICK_PENDING_MAX][CB_GAME_ID_MAX + 1];
static int g_kick_n;

// true when `id` was not pending and now is (or the set is full).
static bool kick_mark(const char *id) {
    pthread_mutex_lock(&g_kick_mu);
    bool add = true;
    for (int i = 0; i < g_kick_n && add; i++) add = strcmp(g_kick_pending[i], id) != 0;
    if (add && g_kick_n < KICK_PENDING_MAX) snprintf(g_kick_pending[g_kick_n++], sizeof g_kick_pending[0], "%s", id);
    pthread_mutex_unlock(&g_kick_mu);
    return add;
}

static void kick_unmark(const char *id) {
    pthread_mutex_lock(&g_kick_mu);
    for (int i = 0; i < g_kick_n; i++)
        if (strcmp(g_kick_pending[i], id) == 0) {
            memcpy(g_kick_pending[i], g_kick_pending[--g_kick_n], sizeof g_kick_pending[0]);
            break;
        }
    pthread_mutex_unlock(&g_kick_mu);
}

// Whether a kick should start a drive: not while another drive holds the
// game's lease. That drive reloads the row before every cycle, and looks once
// more after it lets go (cb_bots_drive), so the move that asked for the kick
// is driven either way; a second drive would only find the lease taken
// (production, 2026-10-17: about one such drive per human move in a bot
// game). A store that cannot say kicks: a wasted drive beats a stalled game.
static bool kick_wanted(cb_ctx *ctx, const char *game_id) {
    if (!ctx->store) return true;
    int held = CB_STORE_CALL(ctx->store, lease_held, game_id, cb_app_now());
    if (held == 1) {
        CB_DEBUG("bots", "kick %s: a drive holds the lease, it takes this move", game_id);
        return false;
    }
    return true;
}

void cb_bots_kick(cb_ctx *ctx, const char *game_id) {
    if (ctx->kick_bots && kick_wanted(ctx, game_id)) ctx->kick_bots(ctx, game_id);
}

static void kick_run(void *ud) {
    kick_task *k = ud;
    kick_unmark(k->game_id);
    cb_bots_drive(k->ctx, k->game_id);
    free(k);
}

void cb_bots_kick_local(cb_ctx *ctx, const char *game_id) {
    if (!ctx->bot_pool) { CB_WARN("bots", "kick %s: no bot pool", game_id); return; }
    if (!kick_mark(game_id)) return;   // a drive for it is already waiting
    kick_task *k = calloc(1, sizeof *k);
    if (!k) { kick_unmark(game_id); return; }
    k->ctx = ctx;
    snprintf(k->game_id, sizeof k->game_id, "%s", game_id);
    if (cb_pool_submit(ctx->bot_pool, kick_run, k) != CB_OK) {
        CB_WARN("bots", "kick %s: the bot pool is stopping", game_id);
        kick_unmark(game_id);
        free(k);
    }
}

void cb_bots_kick_lambda(cb_ctx *ctx, const char *game_id) {
    int rc = cb_lambda_invoke_bot_drive(game_id);
    // A lost kick is the scan's to catch, within a minute.
    if (rc != CB_OK) CB_WARN("bots", "kick %s: self-invoke failed (%s)", game_id, cb_strerror(rc));
}
