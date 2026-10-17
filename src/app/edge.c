// edge.c - the edge cache's invalidations (edge.h).
#include "edge.h"

#include "aws/cloudfront.h"

#include "util/clock.h"
#include "util/err.h"
#include "util/log.h"
#include "util/uuid.h"

#include <stdatomic.h>
#include <stdio.h>

static atomic_uint g_pending;
static atomic_ullong g_marks[2];

void cb_edge_stale(unsigned paths) {
    paths &= CB_EDGE_ALL;
    if (paths & CB_EDGE_BOARD) atomic_fetch_add_explicit(&g_marks[0], 1, memory_order_relaxed);
    if (paths & CB_EDGE_BOTS) atomic_fetch_add_explicit(&g_marks[1], 1, memory_order_relaxed);
    atomic_fetch_or_explicit(&g_pending, paths, memory_order_relaxed);
}

unsigned cb_edge_pending(void) { return atomic_load_explicit(&g_pending, memory_order_relaxed); }

uint64_t cb_edge_marks(unsigned path) {
    return path == CB_EDGE_BOARD ? atomic_load(&g_marks[0]) : path == CB_EDGE_BOTS ? atomic_load(&g_marks[1]) : 0;
}

int cb_edge_paths(unsigned mask, const char *out[2]) {
    int n = 0;
    if (mask & CB_EDGE_BOARD) out[n++] = "/v1/leaderboard";
    if (mask & CB_EDGE_BOTS) out[n++] = "/v1/bots";
    return n;
}

int cb_edge_flush(const cb_config *cfg) {
    unsigned mask = atomic_exchange_explicit(&g_pending, 0u, memory_order_relaxed);
    if (!mask || !cfg || !cfg->cf_distribution[0]) return CB_OK;
    const char *paths[2];
    int n = cb_edge_paths(mask, paths);
    // Unique per flush: CloudFront answers a repeated reference with the first
    // batch (or refuses it for other paths), so a reused one would invalidate nothing.
    char id[CB_UUID_LEN + 1];
    cb_uuid_v4(id);
    char ref[96];
    snprintf(ref, sizeof ref, "cbase-%lld-%s", (long long)cb_now_ms(), id);
    int64_t t0 = cb_mono_ms();
    int rc = cb_cloudfront_invalidate(cfg->cf_distribution, paths, n, ref);
    if (rc == CB_OK) CB_INFO("edge", "invalidated %s%s%s (%lld ms)", paths[0], n > 1 ? " " : "", n > 1 ? paths[1] : "",
                             (long long)(cb_mono_ms() - t0));
    return rc;
}
