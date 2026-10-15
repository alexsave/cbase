// server.h - the listening socket and every connection on it.
//
// Ties the loop, the HTTP parser, the router, the request pool and the
// WebSocket codec together. Owned by the loop thread: every function here is
// loop-thread-only. Other threads reach a WebSocket by posting to the loop
// (loop.h cb_loop_post) and looking it up by id (cb_server_ws_find), which is
// how realtime's publish calls get from a bot worker to a socket.
//
// One connection at a time per socket: a request is parsed, its handler runs
// (on the pool, the socket paused meanwhile), its response is written, and only
// then is the next request on that socket read. Keep-alive is honoured.
#ifndef CB_SERVER_H
#define CB_SERVER_H

#include "ctx.h"
#include "ws.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    long    max_body;      // request body bytes (CBASE_MAX_BODY)
    int64_t idle_ms;       // an HTTP connection with nothing in flight (default 60 s)
    int64_t head_ms;       // a request that has started but not finished arriving (default 15 s)
    int64_t ws_idle_ms;    // a WebSocket that has sent nothing (default 120 s; Phoenix heartbeats every 25-30 s)
    int     max_conns;     // refused past this (default 4096)
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

// The open WebSocket with that id, or NULL (closed since).
cb_ws *cb_server_ws_find(cb_server *s, uint64_t id);
// Calls fn for every open WebSocket. fn may send, and may close the one it is given.
void cb_server_ws_each(cb_server *s, void (*fn)(cb_ws *ws, void *ud), void *ud);

#endif
