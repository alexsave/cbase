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
#include "store/items.h"
#include "util/clock.h"
#include "util/err.h"
#include "util/log.h"

#include <stdio.h>
#include <string.h>

// The parser sets the request's method id; a request built by hand (a test, a platform
// event) has only the text, and is named here, once per handler entry.
static int meth_of(const cb_http_req *req) { return req->meth ? req->meth : cb_http_method_id(req->method); }

// A liveness or readiness probe from the machine itself (systemd, a local
// curl, a watchdog): GET or HEAD /health or /ready whose socket peer is
// loopback. The peer is the socket's, not a header, so it cannot be claimed
// from outside; every other request, and these from any other address, still
// needs the origin proof. (A reverse proxy on the same box would make
// outside probes look local: they could then read only these two bodies.)
static bool is_local_probe(const cb_http_req *req) {
    const int m = meth_of(req);
    if (m != CB_M_GET && m != CB_M_HEAD) return false;
    if (strcmp(req->path, "/health") != 0 && strcmp(req->path, "/ready") != 0) return false;
    return strcmp(req->peer, "127.0.0.1") == 0 || strcmp(req->peer, "::1") == 0 ||
           strcmp(req->peer, "::ffff:127.0.0.1") == 0;
}

// What runs before anything else, for every request (cb_handle) and every refusal of a slow one
// (cb_handle_slow_full): G_PASS, or the refusal written into res, G_BARE (no CORS after it) or
// G_CORS.
enum { G_PASS, G_BARE, G_CORS };
static int gate(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    res->head = meth_of(req) == CB_M_HEAD;
    const int host_refused = cb_router_host_refusal(ctx, req);
    if (host_refused) {
        // Not a Host the app answers to (cbase/app.h host_allow): before anything else, on every
        // route, a socket's upgrade and a file among them, and with no CORS header after.
        cb_router_error(res, host_refused, host_refused == 400 ? "bad host" : "host not allowed");
        return G_BARE;
    }
    if (ctx->cfg && !is_local_probe(req) && !cb_config_origin_proven(ctx->cfg, cb_http_header_get(req, CB_ORIGIN_HEADER))) {
        // Not through the edge (config.h CBASE_ORIGIN_SECRET): nothing else runs.
        cb_router_error(res, 403, "forbidden");
        return G_CORS;
    }
    return G_PASS;
}

void cb_handle(cb_ctx *ctx, cb_http_req *req, cb_http_res *res) {
    const int m = meth_of(req);
    const int g = gate(ctx, req, res);
    if (g == G_BARE) return;
    if (g == G_CORS) {
        // refused (gate), with CORS after
    } else if (m == CB_M_OPTIONS) {
        bool known = false;
        cb_router_match(ctx->router, CB_M_ANY, req->path, &known);
        if (known) cb_router_preflight(ctx, req, res);
        else cb_router_error(res, 404, "not found");
    } else {
        bool known = false;
        const cb_route *r = cb_router_match(ctx->router, m, req->path, &known);
        if (!r && known) {
            char allow[128];
            cb_router_allow(ctx->router, req->path, allow, sizeof allow);
            cb_router_not_allowed(res, allow);
        } else if (!r) cb_router_error(res, 404, "not found");
        else if (cb_http_header_get(req, "upgrade") && !cb_router_upgrade_origin_ok(ctx, req)) {
            // A browser's upgrade from a page on an origin neither list names (router.h): refused
            // before the handler runs, so no handshake is ever answered 101 for it.
            cb_router_error(res, 403, "origin not allowed");
        }
        else if (r->max_body && req->body_len > r->max_body) {
            // A body that arrived whole, past the route's own cap: a one-shot platform's, or a socket's REQ.
            // Serve's HTTP never gets here with one: it answers 413 before reading the body
            // (net/server.c body_cap), which this cannot do for a body already in memory.
            cb_router_error(res, 413, "content too large");
        } else {
            cb_router_bind(r, req);
            r->fn(ctx, req, res);
        }
    }
    cb_router_cors(ctx, req, res);
}

bool cb_handle_on_loop(const cb_ctx *ctx, const cb_http_req *req) {
    const int m = meth_of(req);
    if (m == CB_M_OPTIONS) return true;
    const cb_route *r = cb_router_match(ctx->router, m, req->path, NULL);
    return !r || (r->flags & CB_ROUTE_LOOP);
}

const cb_route *cb_handle_route(const cb_ctx *ctx, const cb_http_req *req) {
    return cb_router_match(ctx->router, meth_of(req), req->path, NULL);
}

void cb_handle_slow_full(cb_ctx *ctx, const cb_route *route, cb_http_req *req, cb_http_res *res) {
    const int g = gate(ctx, req, res);
    if (g == G_BARE) return;
    if (g == G_PASS) {
        if (ctx->app && ctx->app->slow_full) {
            cb_router_bind(route, req);
            ctx->app->slow_full(ctx, req, res);
        } else cb_res_slow_full(res);
    }
    cb_router_cors(ctx, req, res);
}

void cb_res_slow_full(cb_http_res *res) {
    cb_router_error(res, 503, "server busy");
    cb_res_header(res, "Retry-After", "5");
}

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
