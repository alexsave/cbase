// fanout.h - after a winning commit, who hears about it.
//
// On the push stream (realtime.h):
//   * for every HUMAN seat: its view (realtime.h CB_RT_VIEWS, the dashboard),
//     and its cast (CB_RT_SEAT, table_cast(seat, version));
//   * for the spectators: CB_RT_WATCH, table_cast(-1, version);
//   * the casts and the views on every commit (table_ops.c says why a commit
//     that moves the version and casts nothing is a refetch on every page);
//     the deletion of an empty table writes no version and casts nothing;
//   * a seat that left: its views stream hears the view with no envelope.
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
    int         n_seats;
    struct {
        cb_uuid        user_id;    // "" for a bot seat (nothing is sent to it)
        const uint8_t *envelope;   // the seat's view (CB_RT_VIEWS)
        size_t         envelope_len;
        const uint8_t *cast;       // the seat's cast (CB_RT_SEAT)
        size_t         cast_len;
    } seats[CB_MAX_SEATS];
    const uint8_t *spectator_cast; // the spectators' (CB_RT_WATCH)
    size_t         spectator_cast_len;
    int     n_joined, n_left;      // membership changes this commit
    cb_uuid joined[CB_MAX_SEATS], left[CB_MAX_SEATS];
} cb_fanout;

// Publishes everything above through ctx->rt (nothing when it is NULL), at
// once, or once the answer is out when the invocation's window is open
// (app/after.h). Never fails the commit: a push that cannot be delivered is
// logged; clients refetch on rejoin.
void cb_fanout_commit(cb_ctx *ctx, const cb_fanout *f);
// The publishing itself (after.c's drain; cb_fanout_commit's when no window).
void cb_fanout_publish(cb_ctx *ctx, const cb_fanout *f);

// Queues a copy of f when the invocation's window is open (after.h): true,
// and it is published once the answer is out; else false (cb_fanout_commit
// publishes it then).
bool cb_after_fanout(const cb_fanout *f);

// TEST SEAM: when set, called with every fan-out as it is published, so the
// app's unit tests can see who would hear what without a realtime transport.
extern void (*cb_fanout_observer)(const cb_fanout *f);

#endif
