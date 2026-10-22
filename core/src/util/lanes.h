// lanes.h - byte-bounded priority lanes in front of a pool of workers: an item is opaque bytes
// in one of N lanes, each lane holds at most its cap of bytes (CB_E_FULL past it, which a route
// answers 503, so a flood costs memory up to the caps and no more), and a worker always takes the
// first item of the lowest-numbered lane that has one, so a backlog in lane 1 (a user's years of
// history) never delays lane 0 (today's ride).
//
// THE WORKERS ARE A cb_pool's THREADS (util/pool.h), which is not changed for this: each put
// queues one pool task that says "take the next item by priority", so the pool's FIFO carries
// how many items wait and the lanes carry which goes first. Started before the loop pins its
// CPUs (an app's serve_open), the threads inherit the background mask and the loop keeps its
// core. What is queued when the lanes are freed is dropped: nothing was acknowledged as done.
//
// WHY HERE AND NOT IN shared/c: shared/c holds what both c_base and the products' other hosts
// (wasm, iOS, the web) compile. This is built on cb_pool and answers in CB_E_* for a route's 503,
// so only a product running on c_base can use it; it is the platform's, beside the pool it uses.
#ifndef CB_LANES_H
#define CB_LANES_H

#include <stddef.h>
#include <stdint.h>

#define CB_LANES_MAX 8

typedef struct cb_lanes cb_lanes;

// Runs for one item on a worker thread: its lane, its bytes (the lanes' copy, gone after the
// call), its sequence number and when it was put (cb_mono_us), for an end-to-end measure. State
// a run needs per thread (a matcher's scratch) is the app's, in a _Thread_local.
typedef void (*cb_lane_run_fn)(void *ud, int lane, const uint8_t *bytes, size_t len, uint64_t seq, int64_t put_us);

typedef struct {
    int            threads;       // workers (>= 1), named "<name>-<i>"
    const char    *name;
    int            n_lanes;       // 1..CB_LANES_MAX
    const size_t  *caps;          // n_lanes byte caps (0: unbounded); NULL: every lane unbounded
    uint64_t       first_seq;     // the first put's sequence number (0: 1); an app whose ids
                                  // outlive a restart continues them from its journal
    cb_lane_run_fn run;
    void          *ud;
} cb_lanes_opts;

// CB_OK, CB_E_INVALID, CB_E_NOMEM, CB_E_STATE (a thread could not start); *out is NULL on any
// refusal.
int  cb_lanes_new(cb_lanes **out, const cb_lanes_opts *o);
// Stops taking items, drops what is queued, waits for the items being run, joins the workers.
// Must not be called from a run.
void cb_lanes_free(cb_lanes *l);

// Queues a copy of bytes in `lane`. CB_OK and *seq (may be NULL) its sequence number, one more
// than the last accepted put's in any lane; CB_E_FULL when the lane's bytes would pass its cap,
// CB_E_INVALID for a lane out of range, CB_E_NOMEM, CB_E_STATE once freed.
int  cb_lanes_put(cb_lanes *l, int lane, const uint8_t *bytes, size_t len, uint64_t *seq);
// Items waiting in a lane (taken items are not waiting), and their bytes: relaxed loads, a count
// for a display that may be stale at once.
uint32_t cb_lanes_depth(const cb_lanes *l, int lane);
size_t   cb_lanes_bytes(const cb_lanes *l, int lane);
int      cb_lanes_threads(const cb_lanes *l);
int      cb_lanes_n(const cb_lanes *l);

#endif
