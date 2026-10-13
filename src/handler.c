#include "handler.h"

#include "admin/admin.h"
#include "app/functions_http.h"
#include "auth/auth_http.h"
#include "net/router.h"
#include "realtime/postgrest.h"
#include "realtime/rt_local.h"
#include "util/clock.h"
#include "util/err.h"
#include "util/log.h"

#include <stdio.h>
#include <string.h>

void cb_handle(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    if (strcmp(req->method, "OPTIONS") == 0) {
        bool known = false;
        cb_router_match(ctx->router, "*", req->path, &known);
        if (known) cb_router_preflight(ctx, req, res);
        else cb_router_error(res, 404, "not found");
    } else {
        bool known = false;
        const cb_route *r = cb_router_match(ctx->router, req->method, req->path, &known);
        if (!r) cb_router_error(res, known ? 405 : 404, known ? "method not allowed" : "not found");
        else if ((r->flags & CB_ROUTE_APIKEY) && !cb_router_apikey_ok(ctx, req)) cb_router_error(res, 401, "Invalid API key");
        else r->fn(ctx, req, res);
    }
    cb_router_cors(ctx, req, res);
}

bool cb_handle_on_loop(const cb_ctx *ctx, const cb_http_req *req) {
    if (strcmp(req->method, "OPTIONS") == 0) return true;
    const cb_route *r = cb_router_match(ctx->router, req->method, req->path, NULL);
    return !r || (r->flags & CB_ROUTE_LOOP);
}

// GET /health: liveness for the load balancer, the smoke tests and the deploy
// check. Plain text, never JSON: it is read by people and by `grep`.
static void h_health(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    char b[256];
    snprintf(b, sizeof b, "ok cbase %s mode=%s uptime_s=%lld\n", ctx->version,
             ctx->mode == CB_MODE_LAMBDA ? "lambda" : "serve",
             (long long)((cb_now_ms() - ctx->started_ms) / 1000));
    cb_res_text(res, 200, b);
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
    if (rc == CB_OK) rc = reg("auth", cb_auth_routes(ctx));
    if (rc == CB_OK) rc = reg("postgrest", cb_postgrest_routes(ctx));
    if (rc == CB_OK) rc = reg("functions", cb_functions_routes(ctx));
    if (rc == CB_OK) rc = reg("admin", cb_admin_routes(ctx));
    if (rc == CB_OK && ctx->mode == CB_MODE_SERVE) rc = reg("realtime", cb_rt_local_routes(ctx));
    return rc;
}
