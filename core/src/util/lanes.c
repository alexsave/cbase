// lanes.c - util/lanes.h.
#include "lanes.h"

#include "clock.h"
#include "err.h"
#include "pool.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

typedef struct item {
    struct item *next;
    uint64_t     seq;
    int64_t      put_us;
    size_t       len;
    int          lane;
    uint8_t      bytes[];
} item;

typedef struct {
    item         *head, *tail;
    size_t        bytes, cap;
    uint32_t      n;
    atomic_uint   n_pub;       // n and bytes again, stored under the lock and read without it
    atomic_size_t bytes_pub;
} lane;

struct cb_lanes {
    pthread_mutex_t mu;
    lane            lanes[CB_LANES_MAX];
    int             n_lanes;
    uint64_t        next_seq;
    bool            stopping;
    cb_pool        *pool;
    int             threads;
    cb_lane_run_fn  run;
    void           *ud;
};

static void lane_publish(lane *ln) {
    atomic_store_explicit(&ln->n_pub, ln->n, memory_order_relaxed);
    atomic_store_explicit(&ln->bytes_pub, ln->bytes, memory_order_relaxed);
}

static void lane_push(lane *ln, item *it) {
    it->next = NULL;
    if (ln->tail) ln->tail->next = it;
    else ln->head = it;
    ln->tail = it;
    ln->bytes += it->len;
    ln->n++;
    lane_publish(ln);
}

static item *lane_pop(lane *ln) {
    item *it = ln->head;
    if (!it) return NULL;
    ln->head = it->next;
    if (!ln->head) ln->tail = NULL;
    ln->bytes -= it->len;
    ln->n--;
    lane_publish(ln);
    return it;
}

// One pool task: the next item by priority (lane 0 first), run, freed. Every put queues exactly
// one task after its item, so a task finds an item unless the lanes are stopping.
static void take_one(void *ud) {
    cb_lanes *l = ud;
    pthread_mutex_lock(&l->mu);
    item *it = NULL;
    if (!l->stopping)
        for (int i = 0; i < l->n_lanes && !it; i++) it = lane_pop(&l->lanes[i]);
    pthread_mutex_unlock(&l->mu);
    if (!it) return;
    l->run(l->ud, it->lane, it->bytes, it->len, it->seq, it->put_us);
    free(it);
}

int cb_lanes_new(cb_lanes **out, const cb_lanes_opts *o) {
    *out = NULL;
    if (!o || o->n_lanes < 1 || o->n_lanes > CB_LANES_MAX || o->threads < 1 || !o->run) return CB_E_INVALID;
    cb_lanes *l = calloc(1, sizeof *l);
    if (!l) return CB_E_NOMEM;
    l->n_lanes = o->n_lanes;
    for (int i = 0; i < o->n_lanes; i++) l->lanes[i].cap = o->caps ? o->caps[i] : 0;
    l->next_seq = o->first_seq ? o->first_seq : 1;
    l->run = o->run;
    l->ud = o->ud;
    l->threads = o->threads;
    pthread_mutex_init(&l->mu, NULL);
    if (!(l->pool = cb_pool_new(o->threads, o->name ? o->name : "lanes"))) {
        pthread_mutex_destroy(&l->mu);
        free(l);
        return CB_E_STATE;
    }
    *out = l;
    return CB_OK;
}

void cb_lanes_free(cb_lanes *l) {
    if (!l) return;
    pthread_mutex_lock(&l->mu);
    l->stopping = true;   // a task that starts from here on finds nothing
    pthread_mutex_unlock(&l->mu);
    cb_pool_free(l->pool);   // runs every queued task (each returns at once), waits for the runs, joins
    for (int i = 0; i < l->n_lanes; i++) {
        item *it;
        while ((it = lane_pop(&l->lanes[i]))) free(it);
    }
    pthread_mutex_destroy(&l->mu);
    free(l);
}

int cb_lanes_put(cb_lanes *l, int lane_i, const uint8_t *bytes, size_t len, uint64_t *seq) {
    if (!l || lane_i < 0 || lane_i >= l->n_lanes || (!bytes && len)) return CB_E_INVALID;
    item *it = malloc(sizeof *it + len);
    if (!it) return CB_E_NOMEM;
    it->len = len;
    it->lane = lane_i;
    it->put_us = cb_mono_us();
    if (len) memcpy(it->bytes, bytes, len);
    pthread_mutex_lock(&l->mu);
    lane *ln = &l->lanes[lane_i];
    int rc = l->stopping ? CB_E_STATE : ln->cap && len > ln->cap - ln->bytes ? CB_E_FULL : CB_OK;
    // The task is queued under the lock, before the item is linked: no worker can take the lock
    // in between, so the task finds this item (or one ahead of it), and a refused submit (the
    // pool is out of memory) leaves the lanes as they were.
    if (rc == CB_OK) rc = cb_pool_submit(l->pool, take_one, l);
    if (rc != CB_OK) {
        pthread_mutex_unlock(&l->mu);
        free(it);
        return rc;
    }
    it->seq = l->next_seq++;
    lane_push(ln, it);
    uint64_t s = it->seq;
    pthread_mutex_unlock(&l->mu);
    if (seq) *seq = s;
    return CB_OK;
}

uint32_t cb_lanes_depth(const cb_lanes *l, int lane_i) {
    if (!l || lane_i < 0 || lane_i >= l->n_lanes) return 0;
    return atomic_load_explicit(&((cb_lanes *)l)->lanes[lane_i].n_pub, memory_order_relaxed);
}

size_t cb_lanes_bytes(const cb_lanes *l, int lane_i) {
    if (!l || lane_i < 0 || lane_i >= l->n_lanes) return 0;
    return atomic_load_explicit(&((cb_lanes *)l)->lanes[lane_i].bytes_pub, memory_order_relaxed);
}

int cb_lanes_threads(const cb_lanes *l) { return l ? l->threads : 0; }
int cb_lanes_n(const cb_lanes *l) { return l ? l->n_lanes : 0; }
