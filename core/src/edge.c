// edge.c - the edge cache's marks and flush (edge.h).
#include "edge.h"

#include "util/clock.h"
#include "util/err.h"
#include "util/log.h"

#include <stdatomic.h>
#include <stdio.h>

static const char *const *g_paths;
static int g_n;
static cb_edge_purge_fn g_purge;
static atomic_uint g_pending;
static atomic_ullong g_marks[CB_EDGE_MAX];

void cb_edge_setup(const char *const *paths, int n, cb_edge_purge_fn purge) {
    g_paths = paths;
    g_n = paths && n > 0 ? (n > CB_EDGE_MAX ? CB_EDGE_MAX : n) : 0;
    g_purge = purge;
}

static unsigned declared(void) { return g_n >= 32 ? ~0u : (1u << g_n) - 1u; }

void cb_edge_stale(unsigned paths) {
    paths &= declared();
    for (int i = 0; i < g_n; i++)
        if (paths & (1u << i)) atomic_fetch_add_explicit(&g_marks[i], 1, memory_order_relaxed);
    atomic_fetch_or_explicit(&g_pending, paths, memory_order_relaxed);
}

unsigned cb_edge_pending(void) { return atomic_load_explicit(&g_pending, memory_order_relaxed); }

uint64_t cb_edge_marks(unsigned path) {
    for (int i = 0; i < CB_EDGE_MAX; i++)
        if (path == 1u << i) return atomic_load(&g_marks[i]);
    return 0;
}

int cb_edge_paths(unsigned mask, const char *out[CB_EDGE_MAX]) {
    int n = 0;
    for (int i = 0; i < g_n; i++)
        if (mask & (1u << i)) out[n++] = g_paths[i];
    return n;
}

int cb_edge_flush(const cb_config *cfg) {
    unsigned mask = atomic_exchange_explicit(&g_pending, 0u, memory_order_relaxed);
    if (!mask || !g_purge) return CB_OK;
    const char *paths[CB_EDGE_MAX];
    int n = cb_edge_paths(mask, paths);
    if (n == 0) return CB_OK;
    int64_t t0 = cb_mono_ms();
    int rc = g_purge(cfg, paths, n);
    if (rc == CB_OK) CB_DEBUG("edge", "purged %d path(s) (%lld ms)", n, (long long)(cb_mono_ms() - t0));
    return rc;
}
