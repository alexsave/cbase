// fanout.c - after a winning commit, who hears about it (fanout.h).
//
// The two things today's backend did after commit_table: the player_views
// publication (a pv-<uid> postgres_changes event per human row the commit
// upserted or deleted, on EVERY commit), and table_io.ts's broadcastPushes
// (lines 406-412: a gu-<game>-<uid> cast per human seat and a game-<game> cast
// for the spectators, only when the operation moved something a viewer sees).
#include "fanout.h"

#include "realtime/realtime.h"
#include "util/log.h"

#include <stdio.h>
#include <string.h>

void (*cb_fanout_observer)(const cb_fanout *f);

static bool listed(const cb_uuid *list, int n, const char *id) {
    for (int i = 0; i < n; i++) if (strcmp(list[i], id) == 0) return true;
    return false;
}

void cb_fanout_commit(cb_ctx *ctx, const cb_fanout *f) {
    if (cb_fanout_observer) cb_fanout_observer(f);
    cb_rt *rt = ctx->rt;
    if (!rt) {
        CB_DEBUG("fanout", "game %s v%lld: no realtime, nothing published", f->game_id, (long long)f->version);
        return;
    }
    char topic[CB_TOPIC_MAX];
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
        int ev = listed(f->joined, f->n_joined, f->seats[i].user_id) ? CB_RT_INSERT : CB_RT_UPDATE;
        rt_publish_pv_update(rt, f->seats[i].user_id, ev, &row);
    }
    for (int i = 0; i < f->n_left; i++) {
        // A DELETE carries the primary key only.
        cb_view_row row;
        memset(&row, 0, sizeof row);
        snprintf(row.game_id, sizeof row.game_id, "%s", f->game_id);
        snprintf(row.player_id, sizeof row.player_id, "%s", f->left[i]);
        rt_publish_pv_update(rt, f->left[i], CB_RT_DELETE, &row);
    }
    if (!f->broadcast) return;
    for (int i = 0; i < f->n_seats; i++) {
        if (!f->seats[i].user_id[0] || !f->seats[i].cast) continue;
        snprintf(topic, sizeof topic, "gu-%s-%s", f->game_id, f->seats[i].user_id);
        rt_publish_cast(rt, topic, f->game_id, f->version, f->seats[i].cast, f->seats[i].cast_len);
    }
    if (f->spectator_cast) {
        snprintf(topic, sizeof topic, "game-%s", f->game_id);
        rt_publish_cast(rt, topic, f->game_id, f->version, f->spectator_cast, f->spectator_cast_len);
    }
}
