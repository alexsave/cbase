// fanout.h - after a winning commit, who hears about it.
//
// Exactly what table_io.ts did after commit_table (lines 406-412) plus what the
// player_views publication did on its own:
//   * for every HUMAN seat: realtime pv-<uid> postgres_changes UPDATE with the
//     seat's view row (the dashboard), and a gu-<game>-<uid> broadcast
//     animation_events {b: base64(table_cast(seat, version))};
//   * for the spectators: game-<game> broadcast of table_cast(-1, version);
//   * the broadcasts only when n_events > 0 or goods_changed (a good flies no
//     card but is a move); the pv- update on every commit;
//   * a membership change: pv-<uid> INSERT for a new seat, DELETE (primary key
//     only) for one that left.
// Called inside the kernel section's aftermath with the bytes already copied
// out (casts and envelopes), so it never touches the kernel.
#ifndef CB_FANOUT_H
#define CB_FANOUT_H

#include "ctx.h"
#include "table_ops.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char *game_id;
    int64_t     version;
    const char *status;            // cb_table_status_label
    int64_t     updated_at;
    bool        broadcast;         // n_events > 0 || goods_changed
    int         n_seats;
    struct {
        cb_uuid        user_id;    // "" for a bot seat (nothing is sent to it)
        const uint8_t *envelope;   // the seat's view (pv- row)
        size_t         envelope_len;
        const uint8_t *cast;       // the seat's cast (gu- broadcast)
        size_t         cast_len;
    } seats[CB_MAX_SEATS];
    const uint8_t *spectator_cast; // the game- broadcast
    size_t         spectator_cast_len;
    int     n_joined, n_left;      // membership changes this commit
    cb_uuid joined[CB_MAX_SEATS], left[CB_MAX_SEATS];
} cb_fanout;

// Publishes everything above through ctx->rt. Never fails the commit: a push
// that cannot be delivered is logged; clients refetch on rejoin.
void cb_fanout_commit(cb_ctx *ctx, const cb_fanout *f);

#endif
