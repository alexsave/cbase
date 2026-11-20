#include "handler.h"

#include "config.h"

#include "net/router.h"
#include "cbase/app.h"

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
