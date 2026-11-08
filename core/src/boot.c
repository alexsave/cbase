#include "boot.h"

#include "metrics.h"

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

// A STORELESS APP (cbase/app.h storeless) is refused at boot, with a sentence, when it or the
// configuration asks for something that lives in the platform's store.
static int storeless_check(const cb_ctx *ctx, char *err, size_t cap) {
    const cb_app *app = ctx->app;
    const char *what = app->store ? "declares store rows (cb_app.store)"
                     : app->account_domain ? "names an account domain (cb_app.account_domain), and accounts live in the platform's store"
                     : app->delete_account ? "deletes accounts (cb_app.delete_account), and accounts live in the platform's store"
                     : NULL;
    if (what) {
        snprintf(err, cap, "the app %s keeps no platform store (cb_app.storeless) but %s", app->name ? app->name : "", what);
        return CB_E_INVALID;
    }
    if (ctx->cfg->store[0]) {
        snprintf(err, cap, "CBASE_STORE=%s names a store, and the app %s keeps none (cb_app.storeless)", ctx->cfg->store,
                 app->name ? app->name : "");
        return CB_E_INVALID;
    }
    if (ctx->cfg->apple_client_ids[0]) {
        snprintf(err, cap, "CBASE_APPLE_CLIENT_IDS turns on Sign in with Apple, whose accounts live in the platform's store, "
                 "and the app %s keeps none (cb_app.storeless)", app->name ? app->name : "");
        return CB_E_INVALID;
    }
    return CB_OK;
}

// A storeless app was opened (serve_open ran, or it has none): serve_close is owed at unboot.
static bool g_storeless_open;

int cb_boot(cb_ctx *ctx, char *err, size_t cap) {
    const cb_config *cfg = ctx->cfg;
    ctx->version = CB_VERSION;
    ctx->started_ms = cb_now_ms();
    if (!ctx->prov) { snprintf(err, cap, "no provider"); return CB_E_INVALID; }
    const cb_provider *prov = ctx->prov;
    // A test knob production never runs with (config.h CBASE_CLOCK_SPEEDUP).
    if (!(prov->flags & CB_PROV_TEST_CLOCK) && cfg->clock_speedup != 1) {
        snprintf(err, cap, "CBASE_CLOCK_SPEEDUP=%d is for cbase serve's tests: Lambda runs every timer at its own pace",
                 cfg->clock_speedup);
        return CB_E_INVALID;
    }
    int rc = ctx->app && ctx->app->selftest ? ctx->app->selftest(err, cap) : CB_OK;
    if (rc != CB_OK) return rc;
    if (ctx->app && (rc = cb_rt_families_check(ctx->app->topics, ctx->app->n_topics, err, cap)) != CB_OK) return rc;
    if (ctx->app && ctx->app->n_metrics > 0 && (rc = cb_metrics_new(ctx->app->metrics, ctx->app->n_metrics, &ctx->metrics)) != CB_OK) {
        snprintf(err, cap, "the app's metrics table: %s", cb_strerror(rc));
        return rc;
    }
    cb_edge_setup(ctx->app ? ctx->app->cached_paths : NULL, ctx->app ? ctx->app->n_cached_paths : 0, prov->edge_purge);

    const bool storeless = ctx->app && ctx->app->storeless;
    if (storeless) {
        if ((rc = storeless_check(ctx, err, cap)) != CB_OK) return rc;
        CB_INFO("boot", "the app keeps no platform store: none is opened, and accounts and the console's store views are off");
    } else {
        const char *store = cfg->store[0] ? cfg->store : prov->store;
        if ((prov->flags & CB_PROV_ONE_PROCESS) && (rc = claim_instance(cfg->db_path, err, cap)) != CB_OK) return rc;
        rc = cb_store_open_named(store, prov->store, cfg->db_path, cfg->table, &ctx->store);
        char what[40];
        snprintf(what, sizeof what, "store %s", store);
        if ((rc = soft(what, rc, err, cap)) != CB_OK) return rc;
    }
    if ((prov->flags & CB_PROV_ONE_PROCESS) && ctx->app && ctx->app->serve_open &&
        (rc = ctx->app->serve_open(ctx, err, cap)) != CB_OK)
        return rc;
    g_storeless_open = storeless;
    // Accounts live in the store: a storeless app has none, and every auth route answers 503.
    if (!storeless && (rc = soft("auth", cb_auth_new(ctx, &ctx->auth), err, cap)) != CB_OK) return rc;
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
    // The app closes FIRST, with everything it was opened with still there: an app's own threads
    // (a load simulator's players) count into ctx->metrics and publish through ctx->rt until its
    // serve_close joins them (unit_config.c serve_close_runs_first).
    if ((ctx->store || g_storeless_open) && ctx->prov && (ctx->prov->flags & CB_PROV_ONE_PROCESS) && ctx->app &&
        ctx->app->serve_close)
        ctx->app->serve_close(ctx);
    g_storeless_open = false;
    cb_router_free(ctx->router);
    ctx->router = NULL;
    cb_metrics_free(ctx->metrics);
    ctx->metrics = NULL;
    cb_rt_free(ctx->rt);
    ctx->rt = NULL;
    cb_auth_free(ctx->auth);
    ctx->auth = NULL;
    cb_store_close(ctx->store);
    ctx->store = NULL;
    release_instance();
}
