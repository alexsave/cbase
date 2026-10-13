// ctx.h - the handles every request handler is given.
//
// One cb_ctx exists per process, built by main.c in this order and torn down
// in reverse: config, log, loop, request pool, db, auth, realtime, app (the
// resident tables), bot scheduler, router, server. A module that has not been
// built yet (its constructor returns CB_E_NOTIMPL) leaves its handle NULL, and
// every handler must answer 503 rather than dereference a NULL handle.
//
// Handlers receive it as their first argument (router.h cb_handler); nothing
// else is global.
#ifndef CB_CTX_H
#define CB_CTX_H

#include "config.h"

#include <stdatomic.h>
#include <stdint.h>

typedef struct cb_loop   cb_loop;     // net/loop.h
typedef struct cb_pool   cb_pool;     // util/pool.h
typedef struct cb_server cb_server;   // net/server.h
typedef struct cb_router cb_router;   // net/router.h
typedef struct cb_db     cb_db;       // store/db.h
typedef struct cb_auth   cb_auth;     // auth/auth.h
typedef struct cb_rt     cb_rt;       // realtime/realtime.h
typedef struct cb_app    cb_app;      // app/table_ops.h
typedef struct cb_bots   cb_bots;     // app/bots_sched.h

// Counters the net layer keeps for the admin Overview panel. Relaxed atomics:
// they are read for display, never to decide anything.
typedef struct {
    atomic_ullong requests;        // HTTP requests answered
    atomic_ullong responses_5xx;
    atomic_int    http_conns;      // open HTTP connections (not upgraded)
    atomic_int    ws_conns;        // open WebSockets
} cb_stats;

typedef struct cb_ctx {
    const cb_config *cfg;
    const char *version;      // CB_VERSION
    int64_t     started_ms;   // cb_now_ms() at start
    cb_loop    *loop;
    cb_pool    *req_pool;
    cb_server  *server;
    cb_router  *router;
    cb_db      *db;
    cb_auth    *auth;
    cb_rt      *rt;
    cb_app     *app;
    cb_bots    *bots;
    cb_stats    stats;
} cb_ctx;

#define CB_VERSION "0.1.0"

#endif
