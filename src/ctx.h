// ctx.h - the handles every request handler is given.
//
// c_base runs two ways (DESIGN.md "Two mains, one handler"):
//   * `bootstrap`, an AWS Lambda custom runtime: one request per invocation, no
//     state between invocations but a warm container's cache;
//   * `cbase serve`, a local HTTP + WebSocket server for development and tests.
// Both build one cb_ctx (src/boot.c) and hand every request to the same
// handler (src/handler.h). What differs is only behind three seams in here:
// the store backend (SQLite or DynamoDB), the realtime transport (an
// in-process WebSocket or API Gateway's management API), and how a bot drive
// is started (a local worker pool or an async self-invoke).
//
// A module whose constructor returns CB_E_NOTIMPL leaves its handle NULL, and
// every handler answers 503 rather than dereference a NULL handle.
#ifndef CB_CTX_H
#define CB_CTX_H

#include "config.h"

#include <stdatomic.h>
#include <stdint.h>

typedef struct cb_loop   cb_loop;     // net/loop.h        (serve only)
typedef struct cb_pool   cb_pool;     // util/pool.h       (serve only)
typedef struct cb_server cb_server;   // net/server.h      (serve only)
typedef struct cb_router cb_router;   // net/router.h
typedef struct cb_store  cb_store;    // store/store.h
typedef struct cb_auth   cb_auth;     // auth/auth.h
typedef struct cb_rt     cb_rt;       // realtime/realtime.h
typedef struct cb_ctx    cb_ctx;

// How a bot drive is started for a game (app/bots.h). serve: submit
// cb_bots_drive to the local bot pool. lambda: an async self-invoke
// (aws/lambda_invoke.h) carrying the JSON string literal "bot_drive:<id>". Fire and
// forget: a lost kick is caught by the scan (cb_bots_scan).
typedef void (*cb_kick_fn)(cb_ctx *ctx, const char *game_id);

// Counters for the admin Overview panel. Relaxed atomics, read for display only.
// Under Lambda they count this container's invocations, not the function's.
typedef struct {
    atomic_ullong requests;        // requests answered
    atomic_ullong responses_5xx;
    atomic_int    http_conns;      // serve: open HTTP connections (not upgraded)
    atomic_int    ws_conns;        // serve: open WebSockets
} cb_stats;

typedef enum { CB_MODE_SERVE = 1, CB_MODE_LAMBDA = 2 } cb_mode;

struct cb_ctx {
    const cb_config *cfg;
    cb_mode     mode;
    const char *version;      // CB_VERSION
    int64_t     started_ms;   // cb_now_ms() at start (container start, under Lambda)
    cb_router  *router;       // every route, both modes (handler.h)
    cb_store   *store;
    cb_auth    *auth;
    cb_rt      *rt;
    cb_kick_fn  kick_bots;
    // serve only (NULL under Lambda)
    cb_loop    *loop;
    cb_pool    *req_pool;
    cb_pool    *bot_pool;
    cb_server  *server;
    // lambda only: the current invocation's deadline (Lambda-Runtime-Deadline-Ms,
    // epoch ms), 0 under serve. A bot drive budgets against it.
    int64_t     deadline_ms;
    cb_stats    stats;
};

#define CB_VERSION "0.1.0"

#endif
