// unit_bots_kick.c - serve's kick (app/bots.h cb_bots_kick_local) queues at
// most one drive per game.
//
// Every bump and every commit that leaves bot work kicks a drive. While a long
// drive holds the one bot worker, a game's kicks used to pile up in the queue,
// one per bump (thousands over a game against DynamoDB Local), and each ran
// later as a drive that took the lease, read the row, found nothing to do and
// let go: three requests apiece, and a SIGTERM drain that outlived its 15 s.
// A drive already queued for the game sees whatever the later kick would
// have, so the later kick is dropped; one arriving while the drive RUNS is
// queued, since the running drive may already be past the work it brings.
#include "t.h"

#include "app/bots.h"
#include "store/store.h"
#include "util/err.h"
#include "util/pool.h"

#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <unistd.h>

static atomic_int g_drives_a, g_drives_b, g_hold_first;
static atomic_int g_in_first;

// The only store call a drive makes before it gives up on a missing game.
static int counting_lease_acquire(cb_store *s, const char *game, int64_t now, int64_t ttl, cb_uuid token) {
    if (strcmp(game, "aaaaaa") == 0) {
        if (atomic_fetch_add(&g_drives_a, 1) == 0) {
            atomic_store(&g_in_first, 1);
            while (atomic_load(&g_hold_first)) usleep(1000);   // the first drive is "running"
        }
    } else {
        atomic_fetch_add(&g_drives_b, 1);
    }
    return CB_E_NOT_FOUND;
}

static const cb_store_ops OPS = { .name = "counting", .lease_acquire = counting_lease_acquire };

static atomic_int g_block, g_blocking;
static void blocker(void *ud) {
    atomic_store(&g_blocking, 1);
    while (atomic_load(&g_block)) usleep(1000);
}

static void one_queued_drive_per_game(void) {
    cb_store store = { .ops = &OPS };
    cb_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.store = &store;
    ctx.bot_pool = cb_pool_new(1, "kick");
    T_CHECK(ctx.bot_pool != NULL);
    if (!ctx.bot_pool) return;

    // The worker is busy (a long drive of another game): kicks queue.
    atomic_store(&g_block, 1);
    atomic_store(&g_hold_first, 1);
    T_EQ_INT(cb_pool_submit(ctx.bot_pool, blocker, NULL), CB_OK);
    for (int i = 0; i < 2000 && !atomic_load(&g_blocking); i++) usleep(1000);
    for (int i = 0; i < 500; i++) cb_bots_kick_local(&ctx, "aaaaaa");
    cb_bots_kick_local(&ctx, "bbbbbb");
    cb_bots_kick_local(&ctx, "bbbbbb");
    T_EQ_INT(cb_pool_pending(ctx.bot_pool), 2);   // one per game
    atomic_store(&g_block, 0);

    // aaaaaa's drive is now running (held inside its lease call): a kick now
    // must queue a second drive, not be dropped.
    for (int i = 0; i < 2000 && !atomic_load(&g_in_first); i++) usleep(1000);
    T_CHECK(atomic_load(&g_in_first));
    cb_bots_kick_local(&ctx, "aaaaaa");
    cb_bots_kick_local(&ctx, "aaaaaa");
    atomic_store(&g_hold_first, 0);
    cb_pool_free(ctx.bot_pool);   // runs what is queued
    T_EQ_INT(atomic_load(&g_drives_a), 2);
    T_EQ_INT(atomic_load(&g_drives_b), 1);
}

int main(void) {
    T_RUN(one_queued_drive_per_game);
    return T_DONE();
}
