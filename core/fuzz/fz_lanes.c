// fz_lanes.c - util/lanes.h under an arbitrary run of puts across lanes with arbitrary caps and
// sizes, against a model: a put is refused exactly when its lane's bytes would pass the cap (an
// unbounded lane never), sequence numbers count every accepted put in order from first_seq, the
// depths and bytes are the model's at every step, every accepted item runs exactly once with its
// own bytes, with one worker the items run lane by lane in put order, and a free with items
// queued drops them and leaks nothing.
//
// DETERMINISTIC: each worker is first handed an empty "plug" item and held inside run() at a
// gate, so every later put stays queued until the gate opens and the model is exact.
//
// Input: [n_lanes][caps: n_lanes x u16][threads][first_seq u16][ops: lane, len (u8), bytes...]...
#include "fz.h"

#include "util/err.h"
#include "util/lanes.h"

#include <pthread.h>
#include <stdatomic.h>
#include <time.h>

#define ITEMS_MAX 512

typedef struct {
    atomic_int ran[ITEMS_MAX];
    atomic_int n_ran, entered, order;
    int        at[ITEMS_MAX];
    uint8_t    first[ITEMS_MAX];
    size_t     len[ITEMS_MAX];
    int        lane[ITEMS_MAX];
    uint64_t   base;
    pthread_mutex_t mu;
    pthread_cond_t  cv;
    bool       open;
} model;

static void run(void *ud, int lane, const uint8_t *bytes, size_t len, uint64_t seq, int64_t put_us) {
    model *m = ud;
    atomic_fetch_add(&m->entered, 1);
    pthread_mutex_lock(&m->mu);
    while (!m->open) pthread_cond_wait(&m->cv, &m->mu);
    pthread_mutex_unlock(&m->mu);
    FZ_CHECK(seq >= m->base && seq < m->base + ITEMS_MAX);
    FZ_CHECK(put_us > 0);
    int i = (int)(seq - m->base);
    FZ_CHECK(atomic_fetch_add(&m->ran[i], 1) == 0);   // once
    FZ_CHECK(m->lane[i] == lane && m->len[i] == len && (len == 0 || m->first[i] == bytes[0]));
    m->at[i] = atomic_fetch_add(&m->order, 1);
    atomic_fetch_add(&m->n_ran, 1);
}

static void open_gate(model *m) {
    pthread_mutex_lock(&m->mu);
    m->open = true;
    pthread_cond_broadcast(&m->cv);
    pthread_mutex_unlock(&m->mu);
}

static void wait_for(atomic_int *v, int n) {
    for (int k = 0; k < 20000 && atomic_load(v) < n; k++) {
        struct timespec ts = { 0, 100000 };
        nanosleep(&ts, NULL);
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    fz_in in = fz_in_of(data, size);
    int n_lanes = fz_u8(&in) % (CB_LANES_MAX + 2);   // 0 and 9 are refused
    size_t caps[CB_LANES_MAX + 1] = { 0 };
    for (int i = 0; i < n_lanes && i < CB_LANES_MAX; i++) caps[i] = fz_u16(&in);
    int threads = fz_u8(&in) % 4;   // 0 refused
    uint64_t first = fz_u16(&in);
    static model m;
    memset(&m, 0, sizeof m);
    pthread_mutex_init(&m.mu, NULL);
    pthread_cond_init(&m.cv, NULL);
    m.base = first ? first : 1;
    cb_lanes *l = (cb_lanes *)1;
    cb_lanes_opts o = { .threads = threads, .name = "fz", .n_lanes = n_lanes, .caps = caps, .first_seq = first, .run = run, .ud = &m };
    int rc = cb_lanes_new(&l, &o);
    if (n_lanes < 1 || n_lanes > CB_LANES_MAX || threads < 1) {
        FZ_CHECK(rc == CB_E_INVALID && l == NULL);
        goto out;
    }
    FZ_CHECK(rc == CB_OK && l && cb_lanes_n(l) == n_lanes && cb_lanes_threads(l) == threads);
    uint64_t next_seq = m.base;
    int accepted = 0;
    // The plugs: one empty item per worker in lane 0, each taken at once and held at the gate.
    for (int t = 0; t < threads; t++) {
        uint64_t seq = 0;
        FZ_CHECK(cb_lanes_put(l, 0, NULL, 0, &seq) == CB_OK && seq == next_seq);
        m.lane[accepted++] = 0;
        next_seq++;
    }
    wait_for(&m.entered, threads);
    FZ_CHECK(atomic_load(&m.entered) == threads);
    size_t bytes[CB_LANES_MAX] = { 0 };
    uint32_t depth[CB_LANES_MAX] = { 0 };
    while (fz_left(&in) >= 2 && accepted < ITEMS_MAX) {
        int lane = (int)(fz_u8(&in) % (CB_LANES_MAX + 2)) - 1;   // -1 and n_lanes.. refused
        size_t want = fz_u8(&in), got;
        const uint8_t *b = fz_bytes(&in, want, &got);
        uint64_t seq = 0;
        rc = cb_lanes_put(l, lane, got ? b : NULL, got, &seq);
        if (lane < 0 || lane >= n_lanes) { FZ_CHECK(rc == CB_E_INVALID && seq == 0); continue; }
        if (caps[lane] && bytes[lane] + got > caps[lane]) { FZ_CHECK(rc == CB_E_FULL && seq == 0); }
        else {
            FZ_CHECK(rc == CB_OK && seq == next_seq);
            int i = (int)(seq - m.base);
            m.lane[i] = lane;
            m.len[i] = got;
            m.first[i] = got ? b[0] : 0;
            next_seq++;
            bytes[lane] += got;
            depth[lane]++;
            accepted++;
        }
        for (int i = 0; i < n_lanes; i++) FZ_CHECK(cb_lanes_depth(l, i) == depth[i] && cb_lanes_bytes(l, i) == bytes[i]);
    }
    FZ_CHECK(cb_lanes_depth(l, n_lanes) == 0 && cb_lanes_bytes(l, -1) == 0);
    FZ_CHECK(atomic_load(&m.entered) == threads);   // nothing past the plugs was taken
    if (size & 1) {
        // Drain: everything runs exactly once.
        open_gate(&m);
        wait_for(&m.n_ran, accepted);
        FZ_CHECK(atomic_load(&m.n_ran) == accepted);
        for (int i = 0; i < n_lanes; i++) FZ_CHECK(cb_lanes_depth(l, i) == 0 && cb_lanes_bytes(l, i) == 0);
        for (int i = 0; i < accepted; i++) FZ_CHECK(atomic_load(&m.ran[i]) == 1);
        // One worker: past the plug, item a ran before item b exactly when a's lane is lower, or
        // the same lane and put first.
        if (threads == 1)
            for (int a = 1; a < accepted; a++)
                for (int b = a + 1; b < accepted; b++)
                    FZ_CHECK((m.at[a] < m.at[b]) == (m.lane[a] < m.lane[b] || (m.lane[a] == m.lane[b])));
        cb_lanes_free(l);
    } else {
        // Free with the queue full: the workers finish their plugs (and perhaps take a few more
        // before the free stops the lanes); the rest is dropped.
        open_gate(&m);
        cb_lanes_free(l);
        int ran = atomic_load(&m.n_ran);
        FZ_CHECK(ran >= threads && ran <= accepted);
        for (int i = 0; i < accepted; i++) FZ_CHECK(atomic_load(&m.ran[i]) <= 1);
    }
out:
    pthread_mutex_destroy(&m.mu);
    pthread_cond_destroy(&m.cv);
    return 0;
}
