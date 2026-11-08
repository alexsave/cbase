#include "router.h"

#include "cbase/app.h"
#include "util/err.h"
#include "util/str.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// A route with its method as an int (http.h CB_M_*), named once at add. `route`
// is first, so the entry's address is the route's.
// The pattern is measured once too: `len` is its length, `stem` the bytes a path
// must start with when it is a prefix route ("/a/*" -> "/a/"), `kind` which of
// the three it is, and `score` its rank (router.h WHICH ROUTE WINS).
enum { K_PREFIX, K_PARAM, K_EXACT };
typedef struct { cb_route route; uint8_t mid, kind; size_t len, stem; int64_t score; } entry;

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

static bool name_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

#define MAX_SEGMENTS 32   // a parameter pattern's segments: one bit each in its rank

// A parameter pattern's rank among those that fit one path: bit (31 - i) set when segment i is
// literal, so the leftmost literal segment decides. 0 with *ok false for a malformed pattern (a
// ':' segment with a bad or repeated name, too many parameters or segments, or "/*").
static int64_t param_rank(const char *p, bool *ok) {
    int64_t rank = 0;
    int seg = 0, params = 0;
    const char *names[CB_HTTP_MAX_PARAMS];
    size_t lens[CB_HTTP_MAX_PARAMS];
    *ok = false;
    for (size_t i = 0; p[i];) {   // p[i] == '/': a segment starts at i + 1
        size_t s = i + 1, e = s;
        while (p[e] && p[e] != '/') e++;
        if (seg >= MAX_SEGMENTS) return 0;
        if (p[s] == ':') {
            if (e - s < 2 || params == CB_HTTP_MAX_PARAMS) return 0;
            for (size_t k = s + 1; k < e; k++) if (!name_char(p[k])) return 0;
            for (int k = 0; k < params; k++)
                if (lens[k] == e - s - 1 && memcmp(names[k], p + s + 1, lens[k]) == 0) return 0;
            names[params] = p + s + 1;
            lens[params++] = e - s - 1;
        } else {
            if (e - s == 1 && p[s] == '*') return 0;   // no prefix beside parameters
            rank |= (int64_t)1 << (31 - seg);
        }
        seg++;
        i = e;
    }
    *ok = true;
    return rank;
}

static bool has_params(const char *p) { return strstr(p, "/:") != NULL; }

// The same shape: equal byte for byte, but for parameter names ("/a/:id" and "/a/:key" are).
static bool same_shape(const char *a, const char *b) {
    while (*a && *b) {
        if (*a == ':' && *b == ':' && a[-1] == '/' && b[-1] == '/') {
            while (*a && *a != '/') a++;
            while (*b && *b != '/') b++;
            continue;
        }
        if (*a++ != *b++) return false;
    }
    return *a == *b;
}

static int add(cb_router *r, const cb_route *rt) {
    const char *method = rt->method, *pattern = rt->pattern;
    if (!pattern || pattern[0] != '/' || !method || !rt->fn) return CB_E_INVALID;
    entry e = { *rt, (uint8_t)cb_http_method_id(method), K_EXACT, strlen(pattern), 0, 0 };
    if (has_params(pattern)) {
        bool ok;
        e.kind = K_PARAM;
        e.stem = e.len;
        // Rank first by kind (exact, then parameters, then prefixes), then within it.
        e.score = ((int64_t)K_PARAM << 48) | param_rank(pattern, &ok);
        if (!ok) return CB_E_INVALID;
    } else {
        e.kind = is_prefix(pattern, &e.stem) ? K_PREFIX : K_EXACT;
        // Within a kind, longer wins (exact patterns are unique per method, so their order is moot).
        e.score = ((int64_t)e.kind << 48) | (int64_t)e.stem;
    }
    for (int i = 0; i < r->n; i++) {
        const entry *x = &r->r[i];
        if (strcmp(x->route.method, method) != 0) continue;
        if (strcmp(x->route.pattern, pattern) == 0) return CB_E_CONFLICT;
        if (x->kind == K_PARAM && e.kind == K_PARAM && same_shape(x->route.pattern, pattern)) return CB_E_CONFLICT;
    }
    if (r->n == r->cap) {
        int cap = r->cap ? r->cap * 2 : 16;
        entry *x = realloc(r->r, (size_t)cap * sizeof *x);
        if (!x) return CB_E_NOMEM;
        r->r = x;
        r->cap = cap;
    }
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

// Walks a parameter pattern over path[0..plen): true when it fits, each parameter's value
// recorded into req (when not NULL).
static bool fit_params(const char *p, const char *path, size_t plen, cb_http_req *req) {
    size_t i = 0;   // into path
    int n = 0;
    for (size_t k = 0; p[k];) {
        if (p[k] == ':' && p[k - 1] == '/') {   // k > 0: every pattern starts with '/'
            const size_t ns = k + 1;
            while (p[k] && p[k] != '/') k++;
            const size_t vs = i;
            while (i < plen && path[i] != '/') i++;
            if (i == vs || i - vs > UINT16_MAX) return false;   // a whole, non-empty segment
            if (req && n < CB_HTTP_MAX_PARAMS)
                req->params[n++] = (cb_http_param){ p + ns, path + vs, (uint16_t)(k - ns), (uint16_t)(i - vs) };
            continue;
        }
        if (i >= plen || path[i] != p[k]) return false;
        i++;
        k++;
    }
    if (i != plen) return false;
    if (req) req->n_params = (uint8_t)n;
    return true;
}

static bool fits(const entry *e, const char *path, size_t plen) {
    switch (e->kind) {
    case K_EXACT: return plen == e->len && memcmp(path, e->route.pattern, plen) == 0;
    case K_PREFIX: return plen >= e->stem && memcmp(path, e->route.pattern, e->stem) == 0;
    default: return fit_params(e->route.pattern, path, plen, NULL);
    }
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
        if (!fits(e, path, plen)) continue;
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

void cb_router_bind(const cb_route *route, cb_http_req *req) {
    req->n_params = 0;
    if (route && req->path && has_params(route->pattern)) fit_params(route->pattern, req->path, strlen(req->path), req);
}

bool cb_route_param(const cb_http_req *req, const char *name, char *out, size_t cap) {
    const size_t nl = strlen(name);
    for (int i = 0; i < req->n_params && i < CB_HTTP_MAX_PARAMS; i++) {
        const cb_http_param *p = &req->params[i];
        if (p->name_len != nl || memcmp(p->name, name, nl) != 0) continue;
        int n = cb_url_decode(p->value, p->value_len, out, cap, false);
        return n >= 0 && strlen(out) == (size_t)n;   // a "%00" would cut the value short: refused
    }
    return false;
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
