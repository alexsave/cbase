// fanout.c - after a winning commit, who hears about it (fanout.h).
//
// Two things after every winning commit: each human seat's view on its own
// views stream (realtime.h CB_RT_VIEWS, on EVERY commit; a seat that left hears
// the view with no envelope), and the casts, one per human seat (CB_RT_SEAT)
// and one for the spectators (CB_RT_WATCH), on every commit that wrote them
// (all of them but an empty table's deletion, which has no board to cast).
#include "fanout.h"

#include "after.h"
#include "pushes.h"
#include "util/log.h"
#include "util/str.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void (*cb_fanout_observer)(const cb_fanout *f);

// ---- once the answer is out (after.h) ----------------------------------------------

// A queued fan-out, with its own copies of every byte.
typedef struct {
    char      game_id[CB_GAME_ID_MAX + 1];
    cb_fanout f;   // game_id points at the field above, the bytes are owned
} held_fanout;

static void held_free(void *arg) {
    held_fanout *h = arg;
    if (!h) return;
    for (int i = 0; i < CB_MAX_SEATS; i++) {
        free((void *)h->f.seats[i].envelope);
        free((void *)h->f.seats[i].cast);
    }
    free((void *)h->f.spectator_cast);
    free(h);
}

static bool dup_into(const uint8_t *p, size_t n, const uint8_t **out) {
    *out = NULL;
    if (!p) return true;
    uint8_t *c = malloc(n ? n : 1);
    if (!c) return false;
    if (n) memcpy(c, p, n);
    *out = c;
    return true;
}

// Each queued fan-out is published under the game's order lock
// (cb_table_order_lock), the lock a commit and its pushes have always shared,
// so a game's pushes still go out in its commit order. A response can now
// arrive before its own pushes, which the clients already handle: they gate
// every push on its version. Because the invocation has already answered, a
// failed push can no longer fail it: it is logged with the game and version.
static void held_run(cb_ctx *ctx, void *arg) {
    held_fanout *h = arg;
    pthread_mutex_t *order = cb_table_order_lock(h->game_id);
    cb_rt_failures_take();
    cb_fanout_publish(ctx, &h->f);
    int failed = cb_rt_failures_take();
    pthread_mutex_unlock(order);
    if (failed)
        CB_WARN("after", "game %s v%lld: %d push(es) failed after the answer was sent", h->game_id,
                (long long)h->f.version, failed);
}

bool cb_after_fanout(const cb_fanout *f) {
    if (!cb_after_open()) return false;
    held_fanout *h = calloc(1, sizeof *h);
    bool ok = h != NULL;
    if (ok) {
        h->f = *f;
        cb_strlcpy(h->game_id, f->game_id, sizeof h->game_id);
        h->f.game_id = h->game_id;
        // status is a static label (cb_table_status_label): kept as a pointer.
        for (int i = 0; i < CB_MAX_SEATS; i++) h->f.seats[i].envelope = h->f.seats[i].cast = NULL;
        h->f.spectator_cast = NULL;
        for (int i = 0; ok && i < f->n_seats; i++)
            ok = dup_into(f->seats[i].envelope, f->seats[i].envelope_len, &h->f.seats[i].envelope) &&
                 dup_into(f->seats[i].cast, f->seats[i].cast_len, &h->f.seats[i].cast);
        ok = ok && dup_into(f->spectator_cast, f->spectator_cast_len, &h->f.spectator_cast);
    }
    if (ok && cb_after_defer(held_run, held_free, h)) return true;
    // Published at once by the caller: late is better than lost.
    CB_WARN("after", "game %s v%lld: no memory to queue its pushes, sending them before the answer", f->game_id,
            (long long)f->version);
    held_free(h);
    return false;
}

void cb_fanout_commit(cb_ctx *ctx, const cb_fanout *f) {
    if (!cb_after_fanout(f)) cb_fanout_publish(ctx, f);
}

void cb_fanout_publish(cb_ctx *ctx, const cb_fanout *f) {
    if (cb_fanout_observer) cb_fanout_observer(f);
    cb_rt *rt = ctx->rt;
    if (!rt) {
        CB_DEBUG("fanout", "game %s v%lld: no realtime, nothing published", f->game_id, (long long)f->version);
        return;
    }
    // One batch (realtime.h): every push of the commit goes out at once, each
    // user's view before its cast.
    cb_rt_batch_begin(rt);
    for (int i = 0; i < f->n_seats; i++) {
        if (!f->seats[i].user_id[0]) continue;   // a bot has no client
        cb_view_row row;
        memset(&row, 0, sizeof row);
        snprintf(row.game_id, sizeof row.game_id, "%s", f->game_id);
        snprintf(row.player_id, sizeof row.player_id, "%s", f->seats[i].user_id);
        row.view = (uint8_t *)f->seats[i].envelope;
        row.view_len = f->seats[i].envelope_len;
        row.version = f->version;
        snprintf(row.status, sizeof row.status, "%s", f->status);
        row.updated_at = f->updated_at;
        rt_publish_view(rt, f->seats[i].user_id, &row, false);
    }
    for (int i = 0; i < f->n_left; i++) {
        // The seat is gone: the view with no envelope.
        cb_view_row row;
        memset(&row, 0, sizeof row);
        snprintf(row.game_id, sizeof row.game_id, "%s", f->game_id);
        snprintf(row.player_id, sizeof row.player_id, "%s", f->left[i]);
        rt_publish_view(rt, f->left[i], &row, true);
    }
    for (int i = 0; i < f->n_seats; i++) {
        if (!f->seats[i].user_id[0] || !f->seats[i].cast) continue;
        rt_publish_cast(rt, f->seats[i].user_id, f->game_id, f->version, f->seats[i].cast, f->seats[i].cast_len);
    }
    if (f->spectator_cast) rt_publish_cast(rt, NULL, f->game_id, f->version, f->spectator_cast, f->spectator_cast_len);
    cb_rt_batch_end(rt);
}
