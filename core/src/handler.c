#include "handler.h"

#include "admin/admin.h"
#include "net/router.h"
#include "cbase/app.h"
#include "cbase/provider.h"
#include "store/store.h"
#include "store/items.h"
#include "util/clock.h"
#include "util/err.h"
#include "util/log.h"

#include <stdio.h>
#include <string.h>

// The parser sets the request's method id; a request built by hand (a test, the
// lambda event) has only the text, and is named here, once per handler entry.
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

void cb_handle(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    cb_store_tally st;
    cb_store_tally_take(&st);   // this request's count starts here
    const int m = meth_of(req);
    res->head = m == CB_M_HEAD;
    if (ctx->cfg && !is_local_probe(req) && !cb_config_origin_proven(ctx->cfg, cb_http_header_get(req, CB_ORIGIN_HEADER))) {
        // Not through the edge (config.h CBASE_ORIGIN_SECRET): nothing else runs.
        cb_router_error(res, 403, "forbidden");
        return;
    } else if (m == CB_M_OPTIONS) {
        bool known = false;
        cb_router_match(ctx->router, CB_M_ANY, req->path, &known);
        if (known) cb_router_preflight(ctx, req, res);
        else cb_router_error(res, 404, "not found");
    } else {
        bool known = false;
        const cb_route *r = cb_router_match(ctx->router, m, req->path, &known);
        if (!r) cb_router_error(res, known ? 405 : 404, known ? "method not allowed" : "not found");
        else r->fn(ctx, req, res);
    }
    cb_router_cors(ctx, req, res);
    cb_store_tally_take(&st);
    if (st.reads || st.writes)
        CB_DEBUG("store", "%s %s %d: %d reads, %d writes, %g RCU, %g WCU (%s)", req->method, req->path, res->status,
                 st.reads, st.writes, st.rcu, st.wcu, st.detail);
}

bool cb_handle_on_loop(const cb_ctx *ctx, const cb_http_req *req) {
    const int m = meth_of(req);
    if (m == CB_M_OPTIONS) return true;
    const cb_route *r = cb_router_match(ctx->router, m, req->path, NULL);
    return !r || (r->flags & CB_ROUTE_LOOP);
}

bool cb_handle_inline_ok(const cb_ctx *ctx, const cb_http_req *req) {
    const cb_route *r = cb_router_match(ctx->router, meth_of(req), req->path, NULL);
    return r && (r->flags & CB_ROUTE_INLINE);
}

// GET /health: liveness for the load balancer, the smoke tests and the deploy
// check. Plain text, never JSON: it is read by people and by `grep`.
static void h_health(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    char b[256];
    snprintf(b, sizeof b, "ok cbase %s mode=%s uptime_s=%lld\n", ctx->version,
             ctx->prov ? ctx->prov->mode : "none",
             (long long)((cb_now_ms() - ctx->started_ms) / 1000));
    cb_res_text(res, 200, b);
    cb_res_header(res, "Cache-Control", "no-store");
}

// GET /ready: readiness. /health says the process answers; this says the
// store does too: one item read (a SQLite SELECT, a DynamoDB GetItem) of a key
// that is never written, so CB_E_NOT_FOUND is the healthy answer. A full disk,
// a corrupt or locked database or a dead table answers 503. Runs on the
// request pool, not the loop: the read may wait (SQLite's busy timeout).
static void h_ready(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    (void)req;
    char b[256];
    int rc = ctx->store ? CB_OK : CB_E_NOTIMPL;
    if (ctx->store) {
        cb_item it;
        cb_item_init(&it);
        rc = ctx->store->ops->item_get(ctx->store, (cb_ikey){ "READY#probe", "PROBE" }, true, &it);
        cb_item_free(&it);
        if (rc == CB_E_NOT_FOUND) rc = CB_OK;
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
    int rc = cb_router_add(ctx->router, "GET", "/health", CB_ROUTE_LOOP, h_health);
    if (rc == CB_OK) rc = cb_router_add(ctx->router, "GET", "/ready", CB_ROUTE_POOL, h_ready);
    if (rc == CB_OK) rc = reg("admin", cb_admin_routes(ctx));
    if (rc == CB_OK && ctx->prov && ctx->prov->routes) rc = reg(ctx->prov->name, ctx->prov->routes(ctx));
    // Then the app's, every list in order (cbase/app.h).
    const cb_app *app = ctx->app;
    for (int l = 0; rc == CB_OK && app && l < app->n_route_lists; l++)
        rc = reg(app->name, cb_router_add_all(ctx->router, app->routes[l]->routes, app->routes[l]->n));
    return rc;
}
