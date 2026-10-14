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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BOT_PREDICT_FACTOR      1.5
#define BOT_DEADLINE_MARGIN_MS  5000   // Lambda: stop this long before the invocation's deadline
#define BOT_PREFS_CAP           4096

typedef struct {
    double  compute_ms;
    int     cycles;
    int64_t max_ms;
} cpu_acct;

// What one cycle came to.
enum { CYCLE_DONE = 0, CYCLE_AGAIN = 1, CYCLE_ERROR = -1 };

// One cycle, committed (runCycle). CYCLE_AGAIN with *delay_ms set to the
// kernel's wait (0 after a committed cycle), CYCLE_DONE when nothing is left to
// drive (no bot work, the game ended, or the game is gone), CYCLE_ERROR.
static int run_cycle(cb_ctx *ctx, cb_section *s, const char *game_id, int cycle, cpu_acct *cpu, int64_t *delay_ms) {
    // The moves an attempt that lost the CAS already chose, offered back: the
    // kernel replays one only while the reloaded board still makes it legal.
    uint8_t prefs[BOT_PREFS_CAP];
    int prefs_len = 0;
    int32_t ratings[CB_MAX_SEATS];
    bool have_ratings = false;
    *delay_ms = 0;

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
        if (!table_needs_bots(&s->t)) { cb_game_row_free(&row); return CYCLE_DONE; }
        int wait = table_bot_wait_ms(&s->t, cb_app_now());
        if (wait > 0) { cb_game_row_free(&row); *delay_ms = wait; return CYCLE_AGAIN; }
        kr = table_set_session_log(&s->t, row.logs, (int)row.logs_len);
        if (kr < 0) {
            CB_ERROR("bots", "game %s: session log refused (%d)", game_id, kr);
            cb_game_row_free(&row);
            return CYCLE_ERROR;
        }
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
        if (drv.n == 0) { cb_game_row_free(&row); return CYCLE_DONE; }

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

// ms left before this drive must stop, or INT64_MAX under serve (no deadline).
static int64_t budget_left(const cb_ctx *ctx) {
    if (ctx->deadline_ms <= 0) return INT64_MAX;
    return ctx->deadline_ms - BOT_DEADLINE_MARGIN_MS - cb_app_now();
}

int cb_bots_drive(cb_ctx *ctx, const char *game_id) {
    if (!ctx->store) return CB_E_STATE;
    cb_store_tally st;
    cb_store_tally_take(&st);   // this drive's count starts here
    cb_uuid lease;
    int rc = CB_STORE_CALL(ctx->store, lease_acquire, game_id, cb_app_now(), CB_BOT_LEASE_MS, lease);
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
    rc = CB_OK;
    for (int cycle = 0;; cycle++) {
        if (cycle > 0) {
            if (cb_app_now() - started > CB_BOT_WALL_MS) { rekick = true; break; }
            if (cpu.cycles > 0) {
                double avg = cpu.compute_ms / cpu.cycles;
                double predicted = avg * BOT_PREDICT_FACTOR;
                if (predicted < (double)cpu.max_ms) predicted = (double)cpu.max_ms;
                if (predicted > (double)budget_left(ctx)) { rekick = true; break; }
            }
            int r = CB_STORE_CALL(ctx->store, lease_renew, game_id, lease, cb_app_now(), CB_BOT_LEASE_MS);
            // Another drive took the lease over; a store failure keeps going (renewBotLease).
            if (r == CB_E_CONFLICT) break;
            if (r != CB_OK) CB_ERROR("bots", "game %s: lease renewal failed (%s), driving on", game_id, cb_strerror(r));
        }
        int64_t delay = 0;
        int c = run_cycle(ctx, s, game_id, cycle, &cpu, &delay);
        cb_store_tally_take(&st);   // the lease (acquire or renew) and the cycle
        if (st.reads || st.writes)
            CB_DEBUG("store", "bot cycle %s/%d: %d reads, %d writes (%s)", game_id, cycle, st.reads, st.writes, st.detail);
        if (c == CYCLE_ERROR) { rc = CB_E_DB; break; }
        if (c == CYCLE_DONE) break;
        if (delay > 0) {
            // A wait that would outlive the budget is the next drive's to sit out.
            if (delay > budget_left(ctx)) { rekick = true; break; }
            cb_app_sleep(delay);
        }
    }
    CB_STORE_CALL(ctx->store, lease_release, game_id, lease);
    cb_store_tally_take(&st);
    if (st.reads || st.writes)
        CB_DEBUG("store", "bot drive %s end: %d reads, %d writes (%s)", game_id, st.reads, st.writes, st.detail);
    cb_section_free(s);
    if (rekick && ctx->kick_bots) {
        CB_INFO("bots", "game %s: budget spent with bot work left, kicking a fresh drive", game_id);
        ctx->kick_bots(ctx, game_id);
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
    int rc = CB_STORE_CALL(ctx->store, games_needing_bots, now_ms - CB_BOT_STALE_MS, now_ms - CB_BOT_ABANDON_MS,
                           CB_BOT_SCAN_MAX, scan_collect, l);
    if (rc < 0) {
        CB_ERROR("bots", "heartbeat scan failed (%s)", cb_strerror(rc));
        free(l);
        return rc;
    }
    for (int i = 0; i < l->n; i++) if (ctx->kick_bots) ctx->kick_bots(ctx, l->ids[i]);
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

static void kick_run(void *ud) {
    kick_task *k = ud;
    cb_bots_drive(k->ctx, k->game_id);
    free(k);
}

void cb_bots_kick_local(cb_ctx *ctx, const char *game_id) {
    if (!ctx->bot_pool) { CB_WARN("bots", "kick %s: no bot pool", game_id); return; }
    kick_task *k = calloc(1, sizeof *k);
    if (!k) return;
    k->ctx = ctx;
    snprintf(k->game_id, sizeof k->game_id, "%s", game_id);
    if (cb_pool_submit(ctx->bot_pool, kick_run, k) != CB_OK) {
        CB_WARN("bots", "kick %s: the bot pool is stopping", game_id);
        free(k);
    }
}

void cb_bots_kick_lambda(cb_ctx *ctx, const char *game_id) {
    int rc = cb_lambda_invoke_bot_drive(game_id);
    // A lost kick is the scan's to catch, within a minute.
    if (rc != CB_OK) CB_WARN("bots", "kick %s: self-invoke failed (%s)", game_id, cb_strerror(rc));
}
