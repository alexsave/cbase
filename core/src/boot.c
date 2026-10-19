#include "boot.h"

#include "edge.h"
#include "rate_limits.h"
#include "auth/auth.h"
#include "cbase/app.h"
#include "cbase/provider.h"
#include "handler.h"
#include "net/router.h"
#include "realtime/realtime.h"
#include "store/store.h"
#include "util/clock.h"
#include "util/err.h"
#include "util/log.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>

// CB_E_NOTIMPL from a module under construction is a warning, not a failure.
static int soft(const char *what, int rc, char *err, size_t cap) {
    if (rc == CB_E_NOTIMPL) { CB_WARN("boot", "%s: not implemented yet, running without it", what); return CB_OK; }
    if (rc != CB_OK) snprintf(err, cap, "%s: %s", what, cb_strerror(rc));
    return rc;
}

// ONE INSTANCE. A provider whose process makes every write (CB_PROV_ONE_PROCESS: the app's state, its
// locks and the sessions are authoritative in its memory) must be the only one on its database: a
// second `cbase serve` on the same file would commit beside it unseen. "<db>.serve-lock" is held with
// an exclusive flock(2) for the life of the boot, taken before the store opens; the second instance
// refuses to start. (The database library's own locks are fcntl locks: a flock is a separate lock,
// so cbase-import and cbase-export beside a running serve are unaffected.)
static int g_instance_fd = -1;

static int claim_instance(const char *db, char *err, size_t cap) {
    if (!db || !db[0] || strcmp(db, ":memory:") == 0) return CB_OK;
    char path[1100];
    snprintf(path, sizeof path, "%s.serve-lock", db);
    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) return CB_OK;   // no file can be made there: the store's open says why, in its words
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        close(fd);
        snprintf(err, cap, "another cbase serve holds %s (one instance per database)", db);
        return CB_E_STATE;
    }
    g_instance_fd = fd;
    return CB_OK;
}

static void release_instance(void) {
    if (g_instance_fd >= 0) close(g_instance_fd);   // the flock goes with the descriptor
    g_instance_fd = -1;
}

int cb_boot(cb_ctx *ctx, char *err, size_t cap) {
    const cb_config *cfg = ctx->cfg;
    ctx->version = CB_VERSION;
    ctx->started_ms = cb_now_ms();
    if (!ctx->prov) { snprintf(err, cap, "no provider"); return CB_E_INVALID; }
    const cb_provider *prov = ctx->prov;
    // A test knob production never runs with (config.h CBASE_BOT_SPEEDUP).
    if (!(prov->flags & CB_PROV_TEST_CLOCK) && cfg->bot_speedup != 1) {
        snprintf(err, cap, "CBASE_BOT_SPEEDUP=%d is for cbase serve's tests: Lambda runs the bots at their own pace",
                 cfg->bot_speedup);
        return CB_E_INVALID;
    }
    int rc = ctx->app && ctx->app->selftest ? ctx->app->selftest(err, cap) : CB_OK;
    if (rc != CB_OK) return rc;
    if (ctx->app && (rc = cb_rt_families_check(ctx->app->topics, ctx->app->n_topics, err, cap)) != CB_OK) return rc;
    cb_edge_setup(ctx->app ? ctx->app->cached_paths : NULL, ctx->app ? ctx->app->n_cached_paths : 0, prov->edge_purge);

    const char *store = cfg->store[0] ? cfg->store : prov->store;
    if ((prov->flags & CB_PROV_ONE_PROCESS) && (rc = claim_instance(cfg->db_path, err, cap)) != CB_OK) return rc;
    rc = cb_store_open_named(store, prov->store, cfg->db_path, cfg->table, &ctx->store);
    char what[40];
    snprintf(what, sizeof what, "store %s", store);
    if ((rc = soft(what, rc, err, cap)) != CB_OK) return rc;
    if ((rc = soft("auth", cb_auth_new(ctx, &ctx->auth), err, cap)) != CB_OK) return rc;
    if (ctx->auth && (prov->flags & CB_PROV_ONE_PROCESS)) cb_auth_hold_sessions(ctx->auth);
    if ((rc = soft("realtime", prov->rt_open ? prov->rt_open(ctx, &ctx->rt) : CB_OK, err, cap)) != CB_OK) return rc;
    ctx->job_start = prov->job_start;
    ctx->job_start_after = prov->job_start_after;
    cb_limits_configure(cfg);

    ctx->router = cb_router_new();
    if (!ctx->router) { snprintf(err, cap, "out of memory"); return CB_E_NOMEM; }
    rc = cb_routes_register(ctx);
    if (rc != CB_OK) { snprintf(err, cap, "route registration: %s", cb_strerror(rc)); return rc; }
    return CB_OK;
}

void cb_unboot(cb_ctx *ctx) {
    cb_router_free(ctx->router);
    ctx->router = NULL;
    cb_rt_free(ctx->rt);
    ctx->rt = NULL;
    cb_auth_free(ctx->auth);
    ctx->auth = NULL;
    cb_store_close(ctx->store);
    ctx->store = NULL;
    release_instance();
}
