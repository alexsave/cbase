#include "router.h"

#include "util/err.h"
#include "util/json.h"

#include <stdlib.h>
#include <string.h>

struct cb_router {
    cb_route *r;
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

int cb_router_add(cb_router *r, const char *method, const char *pattern, unsigned flags, cb_handler fn) {
    if (!pattern || pattern[0] != '/' || !method || !fn) return CB_E_INVALID;
    for (int i = 0; i < r->n; i++)
        if (strcmp(r->r[i].method, method) == 0 && strcmp(r->r[i].pattern, pattern) == 0) return CB_E_CONFLICT;
    if (r->n == r->cap) {
        int cap = r->cap ? r->cap * 2 : 16;
        cb_route *x = realloc(r->r, (size_t)cap * sizeof *x);
        if (!x) return CB_E_NOMEM;
        r->r = x;
        r->cap = cap;
    }
    r->r[r->n++] = (cb_route){ method, pattern, flags, fn };
    return CB_OK;
}

// How well `pattern` matches `path`: -1 not at all, else a score where longer
// wins and exact beats prefix.
static long score(const char *pattern, const char *path) {
    size_t stem;
    if (is_prefix(pattern, &stem)) return strncmp(path, pattern, stem) == 0 ? (long)stem * 2 : -1;
    return strcmp(path, pattern) == 0 ? (long)stem * 2 + 1 : -1;
}

static const cb_route *match(const cb_router *r, const char *method, const char *path, bool *path_known) {
    const cb_route *best = NULL;
    long best_score = -1;
    bool known = false;
    for (int i = 0; i < r->n; i++) {
        long s = score(r->r[i].pattern, path);
        if (s < 0) continue;
        known = true;
        if (strcmp(r->r[i].method, "*") != 0 && strcmp(r->r[i].method, method) != 0) continue;
        if (s > best_score) { best = &r->r[i]; best_score = s; }
    }
    if (path_known) *path_known = known;
    return best;
}

const cb_route *cb_router_match(const cb_router *r, const char *method, const char *path, bool *path_known) {
    const cb_route *best = match(r, method, path, path_known);
    // HEAD is GET without the body (RFC 9110 9.3.2): a path with no HEAD route
    // of its own answers through its GET route, and handler.c drops the body.
    if (!best && strcmp(method, "HEAD") == 0) best = match(r, "GET", path, NULL);
    return best;
}

static const char *allow_origin(const cb_ctx *ctx, const cb_http_req *req) {
    const char *origin = cb_http_header_get(req, "origin");
    if (strcmp(ctx->cfg->allowed_origins, "*") == 0) return "*";
    if (origin && cb_config_origin_allowed(ctx->cfg, origin)) return origin;
    return NULL;
}

void cb_router_cors(const cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    if (res->upgrade || cb_res_has_header(res, "Access-Control-Allow-Origin")) return;
    const char *o = allow_origin(ctx, req);
    if (!o) return;
    cb_res_header(res, "Access-Control-Allow-Origin", o);
    if (strcmp(o, "*") != 0) cb_res_header(res, "Vary", "Origin");
    // supabase-js reads Content-Range for counts; harmless when absent.
    cb_res_header(res, "Access-Control-Expose-Headers", "Content-Range, X-Supabase-Api-Version");
}

void cb_router_preflight(const cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    res->status = 200;
    const char *o = allow_origin(ctx, req);
    if (o) {
        cb_res_header(res, "Access-Control-Allow-Origin", o);
        if (strcmp(o, "*") != 0) cb_res_header(res, "Vary", "Origin");
    }
    // The functions' cors.ts list, plus what PostgREST and GoTrue calls send.
    cb_res_header(res, "Access-Control-Allow-Headers",
                  "authorization, x-client-info, apikey, content-type, prefer, range, accept-profile, "
                  "content-profile, x-supabase-api-version, x-region");
    cb_res_header(res, "Access-Control-Allow-Methods", "GET, POST, PUT, PATCH, DELETE, OPTIONS");
    cb_res_header(res, "Access-Control-Max-Age", "7200");
    cb_res_header(res, "Content-Type", "text/plain; charset=utf-8");
    cb_res_body(res, "ok", 2);
}

bool cb_router_apikey_ok(const cb_ctx *ctx, const cb_http_req *req) {
    const char *want = ctx->cfg->anon_key;
    if (!want[0]) return true;
    const char *got = cb_http_header_get(req, "apikey");
    char q[1024];
    if (!got && cb_http_query_get(req, "apikey", q, sizeof q)) got = q;
    if (!got) return false;
    // Length-independent comparison: the key is a secret of sorts.
    size_t a = strlen(want), b = strlen(got);
    unsigned diff = (unsigned)(a ^ b);
    for (size_t i = 0; i < a; i++) diff |= (unsigned)(want[i] ^ (i < b ? got[i] : 0));
    return diff == 0;
}

void cb_router_error(cb_http_res *res, int status, const char *msg) {
    cb_buf b;
    cb_buf_init(&b);
    cb_jw w;
    cb_jw_init(&w, &b);
    cb_jw_obj(&w);
    cb_jw_kstr(&w, "message", msg);
    cb_jw_end(&w);
    cb_res_send(res, status, "application/json", b.p, b.len);
    cb_buf_free(&b);
}
