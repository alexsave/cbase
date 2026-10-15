// fanout.h - after a winning commit, who hears about it.
//
// On the push stream (realtime.h):
//   * for every HUMAN seat: its view on pv-<uid> (the dashboard), and its cast
//     on gu-<game>-<uid> (table_cast(seat, version));
//   * for the spectators: game-<game>, table_cast(-1, version);
//   * the casts only when n_events > 0, goods_changed (a good flies no card but
//     is a move) or roster_changed (a redaction renames a seat and flies
//     nothing, yet an open page must show it); the views on every commit;
//   * a seat that left: its pv-<uid> hears the view with no envelope.
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
    bool        broadcast;         // n_events > 0 || goods_changed || roster_changed
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

// Publishes everything above through ctx->rt (nothing when it is NULL). Never
// fails the commit: a push that cannot be delivered is logged; clients refetch
// on rejoin.
void cb_fanout_commit(cb_ctx *ctx, const cb_fanout *f);

// TEST SEAM: when set, called with every fan-out before it is published, so the
// app's unit tests can see who would hear what without a realtime transport.
extern void (*cb_fanout_observer)(const cb_fanout *f);

#endif
