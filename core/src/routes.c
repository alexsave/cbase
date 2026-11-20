// routes.c - the platform's own routes (handler.h cb_routes_register): /health, /ready, the
// account routes, the console, /metrics and the provider's, then the app's. Apart from handler.c,
// so a program that serves through cb_handle with routes of its own (tools/fake_stripe) links the
// dispatch without the store, the accounts and the console these name.
#include "handler.h"

#include "config.h"
#include "metrics.h"

#include "admin/admin.h"
#include "auth/auth_http.h"
#include "net/router.h"
#include "net/server.h"
#include "cbase/app.h"
#include "cbase/provider.h"
#include "store/store.h"
#include "util/clock.h"
#include "util/err.h"
#include "util/log.h"

#include <stdio.h>

// GET /health: liveness for the load balancer, the smoke tests and the deploy
// check. Plain text, never JSON: it is read by people and by `grep`.
static void h_health(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    char b[256];
    int n = snprintf(b, sizeof b, "ok cbase %s mode=%s uptime_s=%lld", ctx->version,
                     ctx->prov ? ctx->prov->mode : "none",
                     (long long)((cb_now_ms() - ctx->started_ms) / 1000));
    // Under serve (this runs on the loop thread, which owns the counters): requests run inline,
    // runs over their budget, and trips of the valve (net/server.h); a trip is a handler that
    // keeps stalling the loop, which no normal play should cause.
    if (ctx->server) {
        cb_inline_stats st;
        cb_server_inline_stats(ctx->server, &st);
        n += snprintf(b + n, sizeof b - (size_t)n, " inline=%lld to_pool=%lld over=%lld tripped=%lld", (long long)st.ran,
                      (long long)st.to_pool, (long long)st.over, (long long)st.tripped);
    }
    snprintf(b + n, sizeof b - (size_t)n, "\n");
    cb_res_text(res, 200, b);
    cb_res_header(res, "Cache-Control", "no-store");
}

// GET /ready: readiness. /health says the process answers; this says the
// store does too: one point read (store.h ready). A full disk,
// a corrupt or locked database or a dead table answers 503. Runs on the
// request pool, not the loop: the read may wait (the store's busy timeout).
static void h_ready(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    (void)req;
    char b[256];
    if (!ctx->store && ctx->app && ctx->app->storeless) {   // nothing of the platform's to wait for
        snprintf(b, sizeof b, "ready cbase %s mode=%s store=none\n", ctx->version, ctx->prov ? ctx->prov->mode : "none");
        cb_res_text(res, 200, b);
        cb_res_header(res, "Cache-Control", "no-store");
        return;
    }
    int rc = ctx->store ? CB_OK : CB_E_NOTIMPL;
    if (ctx->store) {
        rc = ctx->store->ops->ready ? ctx->store->ops->ready(ctx->store) : CB_E_NOTIMPL;
    }
    if (rc == CB_OK) snprintf(b, sizeof b, "ready cbase %s mode=%s store=%s\n", ctx->version,
                              ctx->prov ? ctx->prov->mode : "none", ctx->store->ops->name);
    else snprintf(b, sizeof b, "not ready: store %s\n", ctx->store ? cb_strerror(rc) : "missing");
    cb_res_text(res, rc == CB_OK ? 200 : 503, b);
    cb_res_header(res, "Cache-Control", "no-store");
}

// A module that is not built yet answers its registration with CB_E_NOTIMPL;
// that is logged and skipped, so the skeleton starts and serves /health.
static int reg(const char *what, int rc) {
    if (rc == CB_E_NOTIMPL) { CB_WARN("boot", "%s routes: not implemented yet", what); return CB_OK; }
    if (rc != CB_OK) CB_ERROR("boot", "%s routes: %s", what, cb_strerror(rc));
    return rc;
}

int cb_routes_register(cb_ctx *ctx) {
    // The platform's own routes, only those the app declares (cbase/app.h): none with no app.
    const cb_app *app = ctx->app;
    int rc = CB_OK;
    if (app && app->health_route) rc = cb_router_add(ctx->router, "GET", "/health", CB_ROUTE_LOOP, h_health);
    if (rc == CB_OK && app && app->ready_route) rc = cb_router_add(ctx->router, "GET", "/ready", CB_ROUTE_POOL, h_ready);
    if (rc == CB_OK && app && app->auth_routes) rc = reg("auth", cb_auth_routes(ctx));
    if (rc == CB_OK && app && app->admin_routes) rc = reg("admin", cb_admin_routes(ctx));
    else if (rc == CB_OK && ctx->cfg && ctx->cfg->admin_token[0])
        CB_WARN("boot", "CBASE_ADMIN_TOKEN is set, but the app declares no admin routes: no console is served");
    if (rc == CB_OK) rc = reg("metrics", cb_metrics_routes(ctx));
    if (rc == CB_OK && ctx->prov && ctx->prov->routes) rc = reg(ctx->prov->name, ctx->prov->routes(ctx));
    // Then the app's, every list in order (cbase/app.h).
    for (int l = 0; rc == CB_OK && app && l < app->n_route_lists; l++)
        rc = reg(app->name, cb_router_add_all(ctx->router, app->routes[l]->routes, app->routes[l]->n));
    return rc;
}
