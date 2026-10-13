// router.h - which handler answers which request, and where it runs.
//
// A route is a method, a path pattern and a handler. Patterns are exact
// ("/health") or a prefix ending in "/*" ("/rest/v1/*" matches "/rest/v1/bots"
// and "/rest/v1/"). The longest matching pattern wins; an exact pattern beats a
// prefix of the same length. Method "*" matches any method.
//
// WHERE A HANDLER RUNS. By default on the REQUEST POOL (util/pool.h): it may
// block on SQLite, bcrypt or a game's lock, and the network loop keeps serving
// everyone else meanwhile. CB_ROUTE_LOOP runs it on the loop thread instead:
// required for a WebSocket upgrade (ws.h cb_ws_upgrade), and allowed for
// handlers that only read memory (/health). A LOOP handler must never block.
//
// WHAT THE ROUTER DOES ITSELF, so no module repeats it:
//   * CORS. An OPTIONS request to any routed path is answered 200 "ok" with
//     Access-Control-Allow-Origin (CBASE_ALLOWED_ORIGINS), -Headers (the set
//     supabase-js and supabase-swift send), -Methods and Max-Age 7200, exactly
//     as the edge functions' cors.ts did. Every other response gets
//     Access-Control-Allow-Origin unless the handler set its own.
//   * The anon key. A CB_ROUTE_APIKEY route is refused 401 unless the request
//     carries CBASE_ANON_KEY as the `apikey` header or `?apikey=` (Realtime
//     sends it in the query). Off when CBASE_ANON_KEY is unset.
//   * 404 for no route, 405 for a path routed only under other methods, 503
//     when the request pool is saturated or gone.
#ifndef CB_ROUTER_H
#define CB_ROUTER_H

#include "ctx.h"
#include "http.h"

#include <stdbool.h>

typedef void (*cb_handler)(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res);

#define CB_ROUTE_POOL   0x0   // run on the request pool (the default)
#define CB_ROUTE_LOOP   0x1   // run on the loop thread; must not block
#define CB_ROUTE_APIKEY 0x2   // require the anon key

typedef struct {
    const char *method;    // "GET", "POST", ..., or "*"
    const char *pattern;   // "/exact" or "/prefix/*"
    unsigned    flags;     // CB_ROUTE_*
    cb_handler  fn;
} cb_route;

cb_router *cb_router_new(void);
void cb_router_free(cb_router *r);
// Adds a route. The strings must outlive the router (string literals).
// CB_E_CONFLICT for the same method and pattern twice, CB_E_INVALID for a
// pattern that does not start with '/'.
int cb_router_add(cb_router *r, const char *method, const char *pattern, unsigned flags, cb_handler fn);

// The route for method + path, or NULL. When NULL, *path_known says whether
// the path matched a route under another method (405 rather than 404).
const cb_route *cb_router_match(const cb_router *r, const char *method, const char *path, bool *path_known);

// The router's own answers, used by server.c around the handler. Exposed so a
// test can drive them without sockets.
// Answers an OPTIONS preflight into res.
void cb_router_preflight(const cb_ctx *ctx, const cb_http_req *req, cb_http_res *res);
// true when the request passes the anon-key check (or the check is off).
bool cb_router_apikey_ok(const cb_ctx *ctx, const cb_http_req *req);
// Adds Access-Control-Allow-Origin to a finished response if it has none.
void cb_router_cors(const cb_ctx *ctx, const cb_http_req *req, cb_http_res *res);
// Writes a small JSON error body {"message": msg} with `status`: the shape the
// Supabase gateway answers with, which both SDKs surface as the error text.
void cb_router_error(cb_http_res *res, int status, const char *msg);

#endif
