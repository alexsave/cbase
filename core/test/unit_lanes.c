// unit_lanes.c - util/lanes.h: byte-bounded priority lanes over a pool. Order (lane 0 before
// lane 1 whatever the order of put), the caps (CB_E_FULL, and room again once an item is taken),
// the sequence numbers and where they start, the copies (the caller's bytes may change after
// put), the depths, the refusals, and a free with items still queued.
//
// NO SLEEP DECIDES AN OUTCOME: the worker is held at a gate inside run() on a first "plug" item,
// and every test waits for it to be there (g_entered) before it puts what it observes, so what
// is queued is exactly what was put after the plug.
#include "t.h"

#include "util/err.h"
#include "util/lanes.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define T_CHECK_MSG(cond, ...) do { if (!(cond)) { fprintf(stderr, "  "); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } T_CHECK(cond); } while (0)

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static bool g_open = true;
static uint64_t g_seen_seq[256];
static int g_seen_lane[256];
static size_t g_seen_len[256];
static uint8_t g_seen_first[256];
static int64_t g_seen_put_us[256];
static atomic_int g_seen, g_entered;

static void run(void *ud, int lane, const uint8_t *bytes, size_t len, uint64_t seq, int64_t put_us) {
    T_CHECK(ud == (void *)0x1234);
    atomic_fetch_add(&g_entered, 1);
    pthread_mutex_lock(&g_mu);
    while (!g_open) pthread_cond_wait(&g_cv, &g_mu);
    pthread_mutex_unlock(&g_mu);
    int i = atomic_fetch_add(&g_seen, 1);
    if (i < 256) {
        g_seen_seq[i] = seq;
        g_seen_lane[i] = lane;
        g_seen_len[i] = len;
        g_seen_first[i] = len ? bytes[0] : 0;
        g_seen_put_us[i] = put_us;
    }
}

static void gate(bool open) {
    pthread_mutex_lock(&g_mu);
    g_open = open;
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mu);
}

// Polls for a count a worker thread raises; the bound only keeps a broken build from hanging.
static void wait_for(atomic_int *v, int n) {
    for (int k = 0; atomic_load(v) < n && k < 5000; k++) usleep(1000);
}

static void reset(void) {
    atomic_store(&g_seen, 0);
    atomic_store(&g_entered, 0);
    memset(g_seen_seq, 0, sizeof g_seen_seq);
}

static cb_lanes_opts opts(int threads, int n_lanes, const size_t *caps, cb_lane_run_fn fn, void *ud) {
    return (cb_lanes_opts){ .threads = threads, .name = "t", .n_lanes = n_lanes, .caps = caps, .run = fn, .ud = ud };
}

static void refusals(void) {
    cb_lanes *l = (cb_lanes *)1;
    size_t caps[2] = { 100, 200 };
    cb_lanes_opts o = opts(1, 0, caps, run, (void *)0x1234);
    T_EQ_INT(cb_lanes_new(&l, &o), CB_E_INVALID);
    T_CHECK(l == NULL);
    l = (cb_lanes *)1;
    T_EQ_INT(cb_lanes_new(&l, NULL), CB_E_INVALID);
    T_CHECK(l == NULL);
    o.n_lanes = CB_LANES_MAX + 1;
    T_EQ_INT(cb_lanes_new(&l, &o), CB_E_INVALID);
    o = opts(0, 2, caps, run, (void *)0x1234);
    T_EQ_INT(cb_lanes_new(&l, &o), CB_E_INVALID);
    o = opts(1, 2, caps, NULL, (void *)0x1234);
    T_EQ_INT(cb_lanes_new(&l, &o), CB_E_INVALID);
    o = opts(1, 2, caps, run, (void *)0x1234);
    T_EQ_INT(cb_lanes_new(&l, &o), CB_OK);
    T_CHECK(l != NULL);
    T_EQ_INT(cb_lanes_n(l), 2);
    T_EQ_INT(cb_lanes_threads(l), 1);
    uint8_t b[8] = { 1 };
    T_EQ_INT(cb_lanes_put(l, 2, b, 8, NULL), CB_E_INVALID);
    T_EQ_INT(cb_lanes_put(l, -1, b, 8, NULL), CB_E_INVALID);
    T_EQ_INT(cb_lanes_put(l, 0, NULL, 8, NULL), CB_E_INVALID);
    T_EQ_INT(cb_lanes_put(NULL, 0, b, 8, NULL), CB_E_INVALID);
    T_EQ_INT(cb_lanes_depth(l, 5), 0);
    T_EQ_INT(cb_lanes_depth(l, -1), 0);
    T_EQ_INT(cb_lanes_bytes(NULL, 0), 0);
    T_EQ_INT(cb_lanes_threads(NULL), 0);
    T_EQ_INT(cb_lanes_n(NULL), 0);
    cb_lanes_free(l);
    cb_lanes_free(NULL);
}

static void priority_and_caps(void) {
    reset();
    gate(false);
    cb_lanes *l = NULL;
    size_t caps[2] = { 64, 32 };   // lane 0: live, 64 bytes; lane 1: backfill, 32 bytes
    cb_lanes_opts o = opts(1, 2, caps, run, (void *)0x1234);
    T_EQ_INT(cb_lanes_new(&l, &o), CB_OK);
    uint8_t b[64];
    uint64_t seq = 0;
    // The plug: taken at once, the worker holds it at the gate, so what follows waits.
    memset(b, 0x01, sizeof b);
    T_EQ_INT(cb_lanes_put(l, 1, b, 32, &seq), CB_OK);
    T_EQ_INT(seq, 1);
    wait_for(&g_entered, 1);
    T_EQ_INT(atomic_load(&g_entered), 1);
    T_EQ_INT(cb_lanes_depth(l, 1), 0);
    T_EQ_INT(cb_lanes_bytes(l, 1), 0);   // a taken item's bytes are free again
    // Backfill first (three of 10 bytes: 30 of 32), then live (six of 10: 60 of 64); the fourth
    // backfill is refused on bytes, the seventh live too.
    for (int i = 0; i < 3; i++) { memset(b, 0x10 + i, sizeof b); T_EQ_INT(cb_lanes_put(l, 1, b, 10, &seq), CB_OK); T_EQ_INT(seq, i + 2); }
    memset(b, 0x13, sizeof b);
    T_EQ_INT(cb_lanes_put(l, 1, b, 10, &seq), CB_E_FULL);
    T_EQ_INT(seq, 4);   // a refusal leaves *seq alone and takes no number
    T_EQ_INT(cb_lanes_put(l, 1, b, 2, &seq), CB_OK);   // two bytes still fit (32 exactly)
    T_EQ_INT(seq, 5);
    T_EQ_INT(cb_lanes_put(l, 1, b, 1, &seq), CB_E_FULL);
    for (int i = 0; i < 6; i++) { memset(b, 0x20 + i, sizeof b); T_EQ_INT(cb_lanes_put(l, 0, b, 10, &seq), CB_OK); T_EQ_INT(seq, 6 + i); }
    T_EQ_INT(cb_lanes_put(l, 0, b, 10, &seq), CB_E_FULL);
    T_EQ_INT(cb_lanes_put(l, 0, b, 5, &seq), CB_E_FULL);   // 65 of 64
    T_EQ_INT(cb_lanes_put(l, 0, b, 0, &seq), CB_OK);   // an empty item costs no bytes
    T_EQ_INT(seq, 12);
    T_EQ_INT(cb_lanes_depth(l, 0), 7);
    T_EQ_INT(cb_lanes_depth(l, 1), 4);
    T_EQ_INT(cb_lanes_bytes(l, 0), 60);
    T_EQ_INT(cb_lanes_bytes(l, 1), 32);
    // The caller's buffer is not what the lanes hold.
    memset(b, 0xEE, sizeof b);
    // Open the gate: one worker takes every item, the live ones first whatever the order of put.
    gate(true);
    wait_for(&g_seen, 12);
    T_EQ_INT(atomic_load(&g_seen), 12);
    // The plug, then live seqs 6..12, then backfill 2..5; the bytes are each item's own.
    static const uint64_t want_seq[12] = { 1, 6, 7, 8, 9, 10, 11, 12, 2, 3, 4, 5 };
    static const int want_lane[12] = { 1, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1 };
    static const uint8_t want_first[12] = { 0x01, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0, 0x10, 0x11, 0x12, 0x13 };
    static const size_t want_len[12] = { 32, 10, 10, 10, 10, 10, 10, 0, 10, 10, 10, 2 };
    for (int i = 0; i < 12; i++) {
        T_CHECK_MSG(g_seen_seq[i] == want_seq[i] && g_seen_lane[i] == want_lane[i], "item %d: seq %llu lane %d", i,
                    (unsigned long long)g_seen_seq[i], g_seen_lane[i]);
        T_CHECK_MSG(g_seen_first[i] == want_first[i] && g_seen_len[i] == want_len[i], "item %d bytes", i);
        T_CHECK(g_seen_put_us[i] > 0 && (i == 0 || g_seen_put_us[i] >= g_seen_put_us[0]));
    }
    T_EQ_INT(cb_lanes_depth(l, 0), 0);
    T_EQ_INT(cb_lanes_depth(l, 1), 0);
    T_EQ_INT(cb_lanes_bytes(l, 0), 0);
    T_EQ_INT(cb_lanes_bytes(l, 1), 0);
    // Room again: the lane that was full takes a whole cap's worth.
    T_EQ_INT(cb_lanes_put(l, 1, b, 32, &seq), CB_OK);
    T_EQ_INT(seq, 13);
    wait_for(&g_seen, 13);
    T_EQ_INT(atomic_load(&g_seen), 13);
    T_EQ_INT(g_seen_first[12], 0xEE);
    cb_lanes_free(l);
}

// Sequence numbers continue from first_seq (an app's journal), and NULL caps is unbounded.
static void first_seq(void) {
    reset();
    gate(true);
    cb_lanes *l = NULL;
    cb_lanes_opts o = opts(1, 1, NULL, run, (void *)0x1234);
    o.first_seq = 1000;
    T_EQ_INT(cb_lanes_new(&l, &o), CB_OK);
    static uint8_t big[1 << 20];
    uint64_t seq = 0;
    T_EQ_INT(cb_lanes_put(l, 0, big, sizeof big, &seq), CB_OK);
    T_EQ_INT(seq, 1000);
    T_EQ_INT(cb_lanes_put(l, 0, big, sizeof big, &seq), CB_OK);
    T_EQ_INT(seq, 1001);
    wait_for(&g_seen, 2);
    T_EQ_INT(atomic_load(&g_seen), 2);
    T_EQ_INT(g_seen_seq[0], 1000);
    T_EQ_INT(g_seen_seq[1], 1001);
    cb_lanes_free(l);
}

// Two workers, many items, every one run exactly once, and the live ones ahead of a backfill
// flood that arrived earlier: both workers are held on plugs while the flood and the live items
// are put, so when the gate opens the live items are the next two workers take.
static atomic_int g_ran[2], g_order, g_live_last_at;
static void run_count(void *ud, int lane, const uint8_t *bytes, size_t len, uint64_t seq, int64_t put_us) {
    (void)ud; (void)bytes; (void)len; (void)seq; (void)put_us;
    atomic_fetch_add(&g_entered, 1);
    pthread_mutex_lock(&g_mu);
    while (!g_open) pthread_cond_wait(&g_cv, &g_mu);
    pthread_mutex_unlock(&g_mu);
    int at = atomic_fetch_add(&g_order, 1);
    if (lane == 0) atomic_store(&g_live_last_at, at);
    atomic_fetch_add(&g_ran[lane], 1);
}

static void two_workers(void) {
    reset();
    gate(false);
    cb_lanes *l = NULL;
    size_t caps[2] = { 0, 0 };   // unbounded
    cb_lanes_opts o = opts(2, 2, caps, run_count, NULL);
    T_EQ_INT(cb_lanes_new(&l, &o), CB_OK);
    uint8_t b[4] = { 0 };
    T_EQ_INT(cb_lanes_put(l, 1, b, 4, NULL), CB_OK);
    T_EQ_INT(cb_lanes_put(l, 1, b, 4, NULL), CB_OK);
    wait_for(&g_entered, 2);
    T_EQ_INT(atomic_load(&g_entered), 2);   // both workers hold a plug
    for (int i = 0; i < 200; i++) T_EQ_INT(cb_lanes_put(l, 1, b, 4, NULL), CB_OK);
    for (int i = 0; i < 20; i++) T_EQ_INT(cb_lanes_put(l, 0, b, 4, NULL), CB_OK);
    T_EQ_INT(cb_lanes_depth(l, 1), 200);
    T_EQ_INT(cb_lanes_depth(l, 0), 20);
    gate(true);
    wait_for(&g_order, 222);
    T_EQ_INT(atomic_load(&g_ran[0]), 20);
    T_EQ_INT(atomic_load(&g_ran[1]), 202);
    // The two plugs ran first (orders 0 and 1); the 20 live items were all taken before any of
    // the flood, so the last live one ran by order 21, give or take the two workers' interleaving.
    T_CHECK_MSG(atomic_load(&g_live_last_at) <= 23, "the last live item ran at %d", atomic_load(&g_live_last_at));
    cb_lanes_free(l);
}

// Freed with items queued: the one being run finishes, the queued are dropped (none runs, though
// the pool still runs a task for each), no leak (ASan's lane checks that), and a put once the
// free has begun is refused with CB_E_STATE. The plug's run waits, inside the worker, until a put
// says the lanes are stopping, so the free has begun before any queued item could be taken.
static cb_lanes *g_freeing;
static atomic_int g_free_runs, g_state_seen, g_probe;
static void run_until_stopping(void *ud, int lane, const uint8_t *bytes, size_t len, uint64_t seq, int64_t put_us) {
    (void)ud; (void)lane; (void)bytes; (void)len; (void)put_us;
    atomic_fetch_add(&g_free_runs, 1);
    if (seq != 1) return;
    atomic_fetch_add(&g_entered, 1);
    while (!atomic_load(&g_probe)) usleep(1000);   // until the test has filled lane 1
    const uint8_t one = 1;
    for (int k = 0; k < 5000; k++) {   // lane 1 is full: CB_E_FULL until the free, CB_E_STATE after
        int rc = cb_lanes_put(g_freeing, 1, &one, 1, NULL);
        if (rc == CB_E_STATE) { atomic_store(&g_state_seen, 1); return; }
        T_EQ_INT(rc, CB_E_FULL);
        usleep(1000);
    }
}

static void free_with_queue(void) {
    reset();
    atomic_store(&g_free_runs, 0);
    atomic_store(&g_probe, 0);
    cb_lanes *l = NULL;
    size_t caps[2] = { 0, 1 };
    cb_lanes_opts o = opts(1, 2, caps, run_until_stopping, NULL);
    T_EQ_INT(cb_lanes_new(&l, &o), CB_OK);
    g_freeing = l;
    uint8_t b[4] = { 7 };
    T_EQ_INT(cb_lanes_put(l, 0, b, 4, NULL), CB_OK);   // the plug, seq 1
    wait_for(&g_entered, 1);
    for (int i = 0; i < 4; i++) T_EQ_INT(cb_lanes_put(l, 0, b, 4, NULL), CB_OK);
    T_EQ_INT(cb_lanes_put(l, 1, b, 1, NULL), CB_OK);   // lane 1 now full
    T_EQ_INT(cb_lanes_depth(l, 0), 4);
    T_EQ_INT(cb_lanes_depth(l, 1), 1);
    atomic_store(&g_probe, 1);
    cb_lanes_free(l);
    T_EQ_INT(atomic_load(&g_state_seen), 1);
    T_EQ_INT(atomic_load(&g_free_runs), 1);   // the plug alone; the five queued were dropped
}

int main(void) {
    T_RUN(refusals);
    T_RUN(priority_and_caps);
    T_RUN(first_seq);
    T_RUN(two_workers);
    T_RUN(free_with_queue);
    return T_DONE();
}
