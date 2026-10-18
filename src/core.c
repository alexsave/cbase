// core.c - the entries a provider's ingress calls (core.h).
#include "core.h"

#include "app/after.h"
#include "handler.h"
#include "jobs.h"
#include "util/err.h"
#include "util/log.h"

#include <stdatomic.h>

void cb_core_http(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    cb_handle(ctx, req, res);
    atomic_fetch_add_explicit(&ctx->stats.requests, 1, memory_order_relaxed);
    if (res->status >= 500) atomic_fetch_add_explicit(&ctx->stats.responses_5xx, 1, memory_order_relaxed);
}

int cb_core_connect(cb_ctx *ctx, const char *token, size_t token_len, const char *client_id, size_t id_len,
                    const char *origin, int64_t now_ms, cb_mqtt_grant *out) {
    if (ctx->cfg && !cb_config_origin_proven(ctx->cfg, origin)) return CB_E_AUTH;   // not through the edge
    return cb_mqtt_authorize(ctx, token, token_len, client_id, id_len, now_ms, out);
}

int cb_core_job(cb_ctx *ctx, const char *job, const char *key) {
    int rc = cb_job_run(ctx, job, key);
    // Nothing to do (a drive's game is gone, or no bot can act on it).
    if (rc == CB_E_NOT_FOUND) {
        CB_INFO("jobs", "%s:%s: nothing to do", job, key);
        rc = CB_OK;
    }
    return rc;
}

int cb_core_schedule(cb_ctx *ctx, const char *name, int64_t now_ms) { return cb_schedule_run(ctx, name, now_ms); }

void cb_core_after(cb_ctx *ctx) { cb_after_run(ctx); }
