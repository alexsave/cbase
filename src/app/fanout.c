// fanout.c - after a winning commit, who hears about it (fanout.h).
//
// Two things after every winning commit: each human seat's view on its own
// views stream (realtime.h CB_RT_VIEWS, on EVERY commit; a seat that left hears
// the view with no envelope), and the casts, one per human seat (CB_RT_SEAT)
// and one for the spectators (CB_RT_WATCH), on every commit that wrote them
// (all of them but an empty table's deletion, which has no board to cast).
#include "fanout.h"

#include "after.h"
#include "realtime/realtime.h"
#include "util/log.h"

#include <stdio.h>
#include <string.h>

void (*cb_fanout_observer)(const cb_fanout *f);

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
}
