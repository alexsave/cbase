// ctx.h - the handles every request handler is given.
//
// c_base runs two ways (DESIGN.md "Two mains, one handler"):
//   * `bootstrap`, an AWS Lambda custom runtime: one request per invocation, no
//     state between invocations but a warm container's cache;
//   * `cbase serve`, a local HTTP + WebSocket server for development and tests.
// Both build one cb_ctx (src/boot.c) and hand every request to the same
// handler (src/handler.h). What differs is the PROVIDER (cbase/provider.h:
// the store backend, the realtime transport, how a bot drive is started, the
// edge's viewer header), which each main names, and what is served is the APP
// (cbase/app.h: its routes and its self-test), which each main names too
// (docs/PLATFORM.md).
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
typedef struct cb_app    cb_app;      // cbase/app.h
typedef struct cb_provider cb_provider;   // cbase/provider.h

// How a job is started (jobs.h): the app's job `job` for `key`, run
// elsewhere and not waited for. serve: the job pool (provider_local.c).
// lambda: an async self-invoke carrying the JSON string literal
// "<job>:<key>" (aws/lambda_invoke.h). Fire and forget: CB_OK when it was
// handed on; a lost job is the app's schedule's to catch (foolish's scan).
typedef int (*cb_job_start_fn)(cb_ctx *ctx, const char *job, const char *key);

// Counters for the admin Overview panel. Relaxed atomics, read for display only.
// Under Lambda they count this container's invocations, not the function's.
typedef struct {
    atomic_ullong requests;        // requests answered
    atomic_ullong responses_5xx;
    atomic_int    http_conns;      // serve: open HTTP connections (not upgraded)
    atomic_int    ws_conns;        // serve: open WebSockets
} cb_stats;

struct cb_ctx {
    const cb_config *cfg;
    const cb_provider *prov;  // which runtime this is (cbase/provider.h); never NULL once booted
    const cb_app *app;        // what it serves (cbase/app.h); NULL serves the platform's routes alone
    const char *version;      // CB_VERSION
    int64_t     started_ms;   // cb_now_ms() at start (container start, under Lambda)
    cb_router  *router;       // every route, both modes (handler.h)
    cb_store   *store;
    cb_auth    *auth;
    cb_rt      *rt;
    cb_job_start_fn job_start;   // the provider's (cbase/provider.h), or a test's
    // serve only (NULL under Lambda)
    cb_loop    *loop;
    cb_pool    *req_pool;
    cb_pool    *job_pool;     // the jobs (provider_local.c), CBASE_BOT_WORKERS threads
    cb_server  *server;
    // lambda only: the current invocation's deadline (Lambda-Runtime-Deadline-Ms,
    // epoch ms), 0 under serve. A bot drive budgets against it.
    int64_t     deadline_ms;
    cb_stats    stats;
};

#define CB_VERSION "0.1.0"

#endif
