// cbase/provider.h - where the platform runs: ONE const struct per provider,
// mostly data (docs/PLATFORM.md section 8.3). A binary's main names the one
// it is (ctx->prov), so the provider is chosen at link time; the store and the
// push transport stay vtables (store.h, realtime.h) because the test lanes mix
// them (serve's ingress over the DynamoDB store).
//
//   cb_provider_local   `cbase serve` (provider_local.c): SQLite, the
//                       in-process MQTT hub, the bot pool, no edge
//   cb_provider_aws     `bootstrap` under Lambda (provider_aws.c): DynamoDB,
//                       IoT Core, the async self-invoke, CloudFront
//
// Core asks the provider instead of asking which main is running, so nothing
// outside a provider's file names one.
#ifndef CB_PROVIDER_H
#define CB_PROVIDER_H

#include "ctx.h"
#include "util/str.h"

enum {
    // Work may run after the answer is sent (app/after.h).
    CB_PROV_AFTER_WINDOW = 1u << 0,
    // The process holds the stream's connections (the hub), so it can count
    // them; else a broker holds them (IoT Core) and this process sees none.
    CB_PROV_HELD_SOCKETS = 1u << 1,
    // The edge can purge a path (app/edge.h); else s-maxage alone bounds staleness.
    CB_PROV_EDGE_PURGE   = 1u << 2,
    // CBASE_BOT_SPEEDUP may be other than 1 (config.h): a test runtime only.
    CB_PROV_TEST_CLOCK   = 1u << 3,
};

struct cb_provider {
    const char *name;    // "local", "aws"
    const char *mode;    // the label /health and the admin console print: "serve", "lambda"
    unsigned    flags;   // CB_PROV_*
    // The store this provider runs on ("sqlite", "dynamo"), unless
    // CBASE_STORE names another (store.h cb_store_open_named).
    const char *store;
    // The push transport (realtime.h), *out left NULL when there is none to
    // run (no endpoint configured): CB_OK, or a CB_E_*.
    int (*rt_open)(cb_ctx *ctx, cb_rt **out);
    // Routes only this provider serves (the hub's /mqtt upgrade). NULL: none.
    int (*routes)(cb_ctx *ctx);
    // How a job is started (ctx.h cb_job_start_fn).
    cb_job_start_fn job_start;
    // The request header the edge vouches for the viewer's address in
    // (rate_limits.h cb_viewer_key), or NULL: the socket's peer is the viewer.
    const char *viewer_header;
    // The admin console's links into the provider's own console, written into
    // u[0..3] (the function, its logs, the store, the store's backups), each
    // left empty when there is none. NULL: none at all.
    void (*console_links)(const cb_ctx *ctx, cb_buf u[4]);
    // Purges paths from the provider's edge cache (app/edge.h
    // cb_edge_purge_fn). NULL: no edge (s-maxage alone bounds staleness).
    int (*edge_purge)(const cb_config *cfg, const char *const *paths, int n);
};

extern const cb_provider cb_provider_local;
extern const cb_provider cb_provider_aws;

#endif
