// router.h - which handler answers which request, and where it runs.
//
// A route is a method, a path pattern and a handler. Patterns are exact
// ("/health"), a prefix ending in "/*" ("/v1/items/*" matches "/v1/items/ab12"
// and "/v1/items/"), or have PARAMETERS: a segment ":name" ([A-Za-z0-9_]+, the
// whole segment) matches any one non-empty path segment ("/decks/:id/notes/:nid"
// matches "/decks/7/notes/a%2Fb", id "7", nid "a/b"), at most
// CB_HTTP_MAX_PARAMS of them, and not with "/*". Method "*" matches any method.
//
// WHICH ROUTE WINS, whatever order they were added in, and the same every time:
//   1. an exact pattern, always;
//   2. then a parameter pattern; between two that fit the same path, the one
//      whose first segment that differs is literal ("/decks/new/:x" beats
//      "/decks/:id/notes" for "/decks/new/notes");
//   3. then a prefix, the longest first.
// A route whose method does not fit is passed over for the next one that does
// (405 when none does). Two parameter patterns of the same shape under one
// method ("/a/:id" and "/a/:key") are CB_E_CONFLICT, as the same pattern is.
// Paths are matched raw (percent-escapes and all), so "%2F" never splits a
// segment; cb_route_param decodes the value.
//
// WHERE A HANDLER RUNS. By default on the REQUEST POOL (util/pool.h): it may
// block on SQLite, bcrypt or a document's lock, and the network loop keeps serving
// everyone else meanwhile. CB_ROUTE_LOOP runs it on the loop thread instead:
// required for a WebSocket upgrade (ws.h cb_ws_upgrade), and allowed for
// handlers that only read memory (/health). A LOOP handler must never block.
//
// WHAT THE ROUTER DOES ITSELF, so no module repeats it:
//   * CORS, for the origins the app declares (cbase/app.h cors_origins) and the
//     deployment adds (config.h CBASE_ALLOWED_ORIGINS), and no other. An OPTIONS
//     request to any routed path from a listed Origin is answered 200 "ok" with
//     Access-Control-Allow-Origin (that origin), -Headers (what the web client
//     sends: authorization, content-type), -Methods and Max-Age 7200; from any
//     other origin, or with none, 403 "origin not allowed" and no Access-Control-*
//     header. Every other response from a listed origin gets
//     Access-Control-Allow-Origin unless the handler set its own, and every
//     response whose CORS depends on the Origin says Vary: Origin. "*" listed is
//     any origin: every answer and preflight says Access-Control-Allow-Origin: *.
//     The app's cors_credentials adds Access-Control-Allow-Credentials: true to an
//     echoed origin; never to "*", which boot refuses beside it.
//   * 404 for no route, 405 for a path routed only under other methods, 503
//     when the request pool is saturated or gone.
//   * HEAD. A path with no HEAD route of its own matches its GET route, and
//     cb_handle marks the response `head` so it goes out with GET's headers
//     and Content-Length and no body. CloudFront and link crawlers send HEAD.
#ifndef CB_ROUTER_H
#define CB_ROUTER_H

#include "ctx.h"
#include "http.h"

#include <stdbool.h>

typedef void (*cb_handler)(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res);

#define CB_ROUTE_POOL   0x0   // run on the request pool (the default)
#define CB_ROUTE_LOOP   0x1   // run on the loop thread; must not block
#define CB_ROUTE_INLINE 0x2   // a pool route that may run on the loop thread instead, when the
                              // server's gate is open (net/server.h cb_server_inline_ok); it must
                              // be short, and never wait: one that would (a lock, the store)
                              // answers to the pool instead (http.h cb_http_res.to_pool)
#define CB_ROUTE_HOME   0x4   // with CB_ROUTE_INLINE: the loop is the route's home, not an
                              // optimization, so the valve never sends it to the pool (there it is
                              // slower for everyone: a hand-off in, a wait for the loop's journal,
                              // a post out). Only the pass budget and its own to_pool do

typedef struct cb_route_s {
    const char *method;    // "GET", "POST", ..., or "*"
    const char *pattern;   // "/exact" or "/prefix/*"
    unsigned    flags;     // CB_ROUTE_*
    cb_handler  fn;
    int         inline_us; // CB_ROUTE_INLINE: the most thread CPU time a run on the loop should take
                           // (0: the server's inline_budget_us); more counts toward the valve
} cb_route;

cb_router *cb_router_new(void);
void cb_router_free(cb_router *r);
// Adds a route. The strings must outlive the router (string literals).
// CB_E_CONFLICT for the same method and pattern (or parameter shape) twice,
// CB_E_INVALID for a pattern that does not start with '/' or a malformed parameter.
int cb_router_add(cb_router *r, const char *method, const char *pattern, unsigned flags, cb_handler fn);
// Adds routes[0..n) in order, each with its inline budget: CB_OK, or the first
// refusal (the routes before it stay added).
int cb_router_add_all(cb_router *r, const cb_route *routes, int n);

// The route for method + path, or NULL. When NULL, *path_known says whether
// the path matched a route under another method (405 rather than 404).
const cb_route *cb_router_match(const cb_router *r, int method, const char *path, bool *path_known);   // method: CB_M_*

// Sets req's path parameters from `route`, the route cb_router_match answered for req->path
// (none for a route without them). Whoever runs a route's handler calls it first: cb_handle,
// and a socket's REQ (rt_local.c). No allocation: the parameters point into the pattern and
// into req->path.
void cb_router_bind(const cb_route *route, cb_http_req *req);

// The path parameter `name` of the request's route, percent-decoded ('+' stays '+') into out
// with a NUL: true, or false when the route has no such parameter, its value has a bad escape or
// decodes to a NUL byte, or it does not fit in cap-1 bytes.
//   char id[32]; if (!cb_route_param(req, "id", id, sizeof id)) { 400 }
bool cb_route_param(const cb_http_req *req, const char *name, char *out, size_t cap);

// The router's own answers, used by handler.c around the handler. Exposed so a
// test can drive them without sockets.
// Answers an OPTIONS preflight into res.
void cb_router_preflight(const cb_ctx *ctx, const cb_http_req *req, cb_http_res *res);
// Adds the CORS headers to a finished response if it has no Access-Control-Allow-Origin.
void cb_router_cors(const cb_ctx *ctx, const cb_http_req *req, cb_http_res *res);
// Boot's check of the CORS declaration: every app origin shaped as a browser sends it
// (scheme://host[:port]) or "*", and no credentials beside "*" in either list. CB_OK, or
// CB_E_INVALID with a sentence in err.
int cb_router_cors_check(const cb_app *app, const cb_config *cfg, char *err, size_t cap);
// Writes a plain-text error body with `status`. Clients read the status; the
// text is for people and logs (a refusal a client acts on is the kernel's
// AskRefusal, written by the route itself).
void cb_router_error(cb_http_res *res, int status, const char *msg);

#endif
