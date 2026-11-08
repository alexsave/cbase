#include "router.h"

#include "cbase/app.h"
#include "util/err.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// A route with its method as an int (http.h CB_M_*), named once at add. `route`
// is first, so the entry's address is the route's.
// The pattern is measured once too: `len` is its length, `stem` the bytes a path
// must start with when it is a prefix route ("/a/*" -> "/a/"), `prefix` says which.
typedef struct { cb_route route; uint8_t mid; bool prefix; size_t len, stem; long score; } entry;

struct cb_router {
    entry *r;
    int n, cap;
};

cb_router *cb_router_new(void) { return calloc(1, sizeof(cb_router)); }

void cb_router_free(cb_router *r) {
    if (!r) return;
    free(r->r);
    free(r);
}

static bool is_prefix(const char *pattern, size_t *stem) {
    size_t n = strlen(pattern);
    if (n >= 2 && pattern[n - 2] == '/' && pattern[n - 1] == '*') { *stem = n - 1; return true; }
    *stem = n;
    return false;
}

static int add(cb_router *r, const cb_route *rt) {
    const char *method = rt->method, *pattern = rt->pattern;
    if (!pattern || pattern[0] != '/' || !method || !rt->fn) return CB_E_INVALID;
    for (int i = 0; i < r->n; i++)
        if (strcmp(r->r[i].route.method, method) == 0 && strcmp(r->r[i].route.pattern, pattern) == 0) return CB_E_CONFLICT;
    if (r->n == r->cap) {
        int cap = r->cap ? r->cap * 2 : 16;
        entry *x = realloc(r->r, (size_t)cap * sizeof *x);
        if (!x) return CB_E_NOMEM;
        r->r = x;
        r->cap = cap;
    }
    entry e = { *rt, (uint8_t)cb_http_method_id(method), false, strlen(pattern), 0, 0 };
    e.prefix = is_prefix(pattern, &e.stem);
    // How well the pattern matches a path it matches: longer wins, exact beats
    // prefix at the same length.
    e.score = e.prefix ? (long)e.stem * 2 : (long)e.stem * 2 + 1;
    // The table is kept sorted by score, best first, ties in the order added: the
    // first route that fits a request is the best one.
    int at = r->n;
    while (at > 0 && r->r[at - 1].score < e.score) { r->r[at] = r->r[at - 1]; at--; }
    r->r[at] = e;
    r->n++;
    return CB_OK;
}

int cb_router_add(cb_router *r, const char *method, const char *pattern, unsigned flags, cb_handler fn) {
    const cb_route rt = { method, pattern, flags, fn, 0 };
    return add(r, &rt);
}

int cb_router_add_all(cb_router *r, const cb_route *routes, int n) {
    for (int i = 0; i < n; i++) {
        int rc = add(r, &routes[i]);
        if (rc != CB_OK) return rc;
    }
    return CB_OK;
}

// The first route (best score first) whose pattern matches the path and whose
// method fits. *path_known is whether any pattern matched the path under any
// method (405 rather than 404).
static const cb_route *match(const cb_router *r, int method, const char *path, bool *path_known) {
    const size_t plen = strlen(path);
    bool known = false;
    const cb_route *best = NULL;
    for (int i = 0; i < r->n; i++) {
        const entry *e = &r->r[i];
        if (e->prefix ? (plen < e->stem || memcmp(path, e->route.pattern, e->stem) != 0)
                      : (plen != e->len || memcmp(path, e->route.pattern, plen) != 0))
            continue;
        known = true;
        if (e->mid != CB_M_ANY && e->mid != method) continue;
        best = &e->route;
        break;
    }
    if (path_known) *path_known = known;
    return best;
}

const cb_route *cb_router_match(const cb_router *r, int method, const char *path, bool *path_known) {
    const cb_route *best = match(r, method, path, path_known);
    // HEAD is GET without the body (RFC 9110 9.3.2): a path with no HEAD route
    // of its own answers through its GET route, and handler.c drops the body.
    if (!best && method == CB_M_HEAD) best = match(r, CB_M_GET, path, NULL);
    return best;
}

// ---- CORS (router.h) -----------------------------------------------------------

// What the allow lists say about one Origin.
enum { CORS_DENY, CORS_ANY, CORS_ECHO };

static bool app_lists(const cb_app *app, const char *origin) {
    for (int i = 0; app && i < app->n_cors_origins; i++)
        if (strcmp(app->cors_origins[i], origin) == 0) return true;
    return false;
}

static int cors_of(const cb_ctx *ctx, const char *origin) {
    const cb_app *app = ctx->app;
    // "*" first: it answers "*" whatever the request's Origin, as it always has.
    if (app_lists(app, "*") || (ctx->cfg && cb_config_origin_any(ctx->cfg))) return CORS_ANY;
    if (!origin || !origin[0]) return CORS_DENY;
    if (app_lists(app, origin) || (ctx->cfg && cb_config_origin_allowed(ctx->cfg, origin))) return CORS_ECHO;
    return CORS_DENY;
}

// The headers that let a page read this answer: Allow-Origin, and for an echoed origin
// Allow-Credentials when the app asked for it. Vary: Origin goes on every answer that depends on
// the Origin (an echo, or a refusal of one), so a cache never hands one origin's answer to another.
static void cors_headers(const cb_ctx *ctx, const cb_http_req *req, cb_http_res *res, int kind) {
    if (kind == CORS_ANY) {
        cb_res_header(res, "Access-Control-Allow-Origin", "*");
        return;
    }
    cb_res_header(res, "Vary", "Origin");
    if (kind != CORS_ECHO) return;
    cb_res_header(res, "Access-Control-Allow-Origin", cb_http_header_get(req, "origin"));
    if (ctx->app && ctx->app->cors_credentials) cb_res_header(res, "Access-Control-Allow-Credentials", "true");
}

void cb_router_cors(const cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    if (res->upgrade || cb_res_has_header(res, "Access-Control-Allow-Origin")) return;
    cors_headers(ctx, req, res, cors_of(ctx, cb_http_header_get(req, "origin")));
}

void cb_router_preflight(const cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    const int kind = cors_of(ctx, cb_http_header_get(req, "origin"));
    if (kind == CORS_DENY) {
        // Not an origin the app or the deployment lists: no Access-Control-* header at all, so the
        // browser refuses the page its request (cb_router_cors adds only Vary: Origin after).
        cb_router_error(res, 403, "origin not allowed");
        return;
    }
    res->status = 200;
    cors_headers(ctx, req, res, kind);
    cb_res_header(res, "Access-Control-Allow-Headers", "authorization, content-type");
    cb_res_header(res, "Access-Control-Allow-Methods", "GET, POST, PUT, PATCH, DELETE, OPTIONS");
    cb_res_header(res, "Access-Control-Max-Age", "7200");
    cb_res_header(res, "Content-Type", "text/plain; charset=utf-8");
    cb_res_body(res, "ok", 2);
}

// An origin as a browser sends it: http:// or https://, then a host and any port, with no path,
// query, user, blank or comma.
static bool origin_shaped(const char *o) {
    size_t k = strncmp(o, "https://", 8) == 0 ? 8 : strncmp(o, "http://", 7) == 0 ? 7 : 0;
    if (!k || !o[k]) return false;
    for (const char *p = o + k; *p; p++)
        if (*p == '/' || *p == '?' || *p == '#' || *p == '@' || *p == ',' || *p == ' ' || (unsigned char)*p < 0x21 ||
            (unsigned char)*p > 0x7e)
            return false;
    return true;
}

int cb_router_cors_check(const cb_app *app, const cb_config *cfg, char *err, size_t cap) {
    bool any = cfg && cb_config_origin_any(cfg);
    for (int i = 0; app && i < app->n_cors_origins; i++) {
        const char *o = app->cors_origins[i];
        if (o && strcmp(o, "*") == 0) { any = true; continue; }
        if (!o || !origin_shaped(o)) {
            snprintf(err, cap, "the app's CORS origin %d (\"%s\") is not scheme://host[:port]", i, o ? o : "(null)");
            return CB_E_INVALID;
        }
    }
    if (app && app->cors_credentials && any) {
        snprintf(err, cap, "the app's CORS credentials beside the origin \"*\" (%s): list the origins instead",
                 cfg && cb_config_origin_any(cfg) ? "CBASE_ALLOWED_ORIGINS" : "cors_origins");
        return CB_E_INVALID;
    }
    return CB_OK;
}

void cb_router_error(cb_http_res *res, int status, const char *msg) {
    cb_res_send(res, status, "text/plain; charset=utf-8", msg, strlen(msg));
}
