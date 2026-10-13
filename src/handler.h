// handler.h - one request in, one response out: the core both mains share.
//
// `cbase serve` parses a request off a socket (net/server.c); `bootstrap`
// decodes it from a Lambda event (aws/event.h). Either way it becomes a
// cb_http_req, goes through cb_handle, and the cb_http_res that comes back is
// written to the socket or encoded into the Lambda response. Nothing below this
// line knows which main is running, except through the three seams in ctx.h.
//
// cb_handle does what every route needs before the handler (router.h): CORS
// preflight, the anon-key check, 404/405, and Access-Control-Allow-Origin on the
// way out. It never blocks the caller's event loop by itself; the HANDLER may
// (SQLite, bcrypt, DynamoDB), which is why serve runs it on the request pool.
#ifndef CB_HANDLER_H
#define CB_HANDLER_H

#include "ctx.h"
#include "net/http.h"

#include <stdbool.h>

// Answers `req` into `res` (cb_res_init'ed by the caller).
void cb_handle(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res);

// serve only: true when cb_handle for this request must run on the loop thread
// (a CB_ROUTE_LOOP route, such as the WebSocket upgrade) or costs nothing (a
// preflight, a 404); false sends it to the request pool.
bool cb_handle_on_loop(const cb_ctx *ctx, const cb_http_req *req);

// Builds ctx->router: GET /health here, then every module's routes
// (auth_http.h, postgrest.h, functions_http.h, admin.h, and under serve
// rt_local.h's WebSocket upgrade). CB_OK, or the first registration error.
int cb_routes_register(cb_ctx *ctx);

#endif
