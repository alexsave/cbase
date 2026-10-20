// server.h - the listening socket and every connection on it.
//
// Ties the loop, the HTTP parser, the router, the request pool and the
// WebSocket codec together. Owned by the loop thread: every function here is
// loop-thread-only. Other threads reach a WebSocket by posting to the loop
// (loop.h cb_loop_post), which is how realtime's publish calls get from a bot
// worker to a socket (rt_local.c keeps its sockets' ids).
//
// One connection at a time per socket: a request is parsed, its handler runs
// (on the pool, the socket paused meanwhile), its response is written, and only
// then is the next request on that socket read. Keep-alive is honoured.
#ifndef CB_SERVER_H
#define CB_SERVER_H

#include "ctx.h"
#include "router.h"
#include "ws.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    long    max_body;      // request body bytes (CBASE_MAX_BODY)
    int64_t idle_ms;       // an HTTP connection with nothing in flight (default 60 s)
    int64_t head_ms;       // a request that has started but not finished arriving (default 15 s)
    int64_t ws_idle_ms;    // a WebSocket that has sent nothing, a PONG included (default 120 s)
    int64_t ws_ping_ms;    // a WebSocket silent this long is sent a PING (default 30 s)
    int     max_conns;     // refused past this (default 4096; CBASE_MAX_CONNS), each refusal counted and logged once a minute
    int     backlog;       // listen(2)'s queue (default 512; CBASE_LISTEN_BACKLOG)
    // INLINE DISPATCH: a CB_ROUTE_INLINE request may run on the loop thread (cb_server_inline_ok).
    int     inline_budget_us;   // a run longer than its route's budget (router.h cb_route.inline_us) or, for a
                                // route with none, than this, is over budget (default 2000; 0 = never
                                // inline; CBASE_INLINE_BUDGET_US)
    int     inline_trips;       // this many runs over budget within inline_trip_window_ms trip the valve
    int     inline_trip_window_ms;   // (default 3 in 1000 ms): one slow run is a hiccup, not a trend
    int     inline_off_ms;      // how long the tripped valve keeps inline dispatch off (default 5000)
    int     inline_pass_us;     // inline handler time one loop pass may spend; past it the rest of the
                                // pass's requests go to the pool, so a burst cannot starve the other
                                // sockets (default 4000)
} cb_server_opts;

void cb_server_opts_default(cb_server_opts *o);

// Uses ctx->loop, ctx->router and ctx->req_pool, and counts into ctx->stats.
cb_server *cb_server_new(cb_ctx *ctx, const cb_server_opts *opts);
// Binds and listens. *bound_port gets the port (useful with port 0).
// CB_OK, or CB_E_IO (the log says why).
int cb_server_listen(cb_server *s, const char *host, int port, int *bound_port);
// Graceful stop: closes the listener, lets in-flight requests finish and their
// responses go out, closes idle connections and every WebSocket (1001), then
// calls cb_loop_stop once nothing is in flight or `timeout_ms` has passed.
void cb_server_drain(cb_server *s, int64_t timeout_ms);
// Frees every connection (on_close runs for each WebSocket). Call after the
// loop has stopped AND the request pool has been freed, so no handler is
// still running against a connection.
void cb_server_free(cb_server *s);

// INLINE DISPATCH (loop thread). A CB_ROUTE_INLINE handler may run on the loop thread instead of
// the pool when cb_server_inline_ok says so now; whoever runs it there (dispatch for HTTP,
// realtime's socket hub for a REQ frame) times it and reports it with cb_server_inline_done,
// which feeds the pass budget and the valve.
bool cb_server_inline_ok(cb_server *s);
void cb_server_inline_done(cb_server *s, const cb_route *r, int64_t took_us);
// Inline dispatch counters (loop thread, or after it has stopped): requests
// run on the loop thread, runs over their budget, and times the valve tripped.
typedef struct { int64_t ran, over, tripped; } cb_inline_stats;
void cb_server_inline_stats(const cb_server *s, cb_inline_stats *out);
// ANY THREAD: send() calls made so far (a socket's answer and pushes of one pass share one).
int64_t cb_server_sends(const cb_server *s);

// Calls fn for every open WebSocket. fn may send, and may close the one it is given.
// Loop thread: wakes every request parked under `key` (http.h park_ms): each runs its handler again.
void cb_server_wake(cb_server *s, const char *key);
void cb_server_ws_each(cb_server *s, void (*fn)(cb_ws *ws, void *ud), void *ud);

#endif
