// main_lambda.c - `bootstrap`: c_base as an AWS Lambda custom runtime
// (provided.al2023, arm64, statically linked; `make bootstrap`).
//
// Start: read the config, cb_boot (kernel self-test, store, auth, realtime
// over API Gateway, routes); a failure is reported to /runtime/init/error and
// the process exits. Then forever: GET the next invocation, decode the event
// (aws/event.h), dispatch it, POST the response:
//   HTTP       cb_handle -> cb_event_encode_http
//   WS         cb_rt_apigw_event -> cb_event_encode_ws
//   SCHEDULE   cb_bots_scan                                      -> {}
//   BOT_DRIVE  cb_bots_drive, budgeted against ctx->deadline_ms
//              (Lambda-Runtime-Deadline-Ms)                     -> {}
// An event that does not decode, or a handler that cannot answer, is POSTed
// to the invocation's /error. Lambda retries a failed ASYNC invocation (the
// self-invoke, the schedule) twice, so a drive for a game that no longer
// exists answers {} rather than an error: retrying it cannot help. The process holds nothing between invocations
// but what a warm container may cache (app/table_ops.h).
#include "app/bots.h"
#include "aws/event.h"
#include "aws/runtime_api.h"
#include "boot.h"
#include "config.h"
#include "ctx.h"
#include "handler.h"
#include "realtime/rt_apigw.h"
#include "store/store.h"
#include "util/clock.h"
#include "util/err.h"
#include "util/log.h"

#include <stdio.h>
#include <string.h>

static int dispatch(cb_ctx *ctx, const cb_invocation *inv, cb_buf *out, char *why, size_t cap) {
    cb_event ev;
    int rc = cb_event_decode((const char *)inv->event.p, inv->event.len, &ev);
    if (rc != CB_OK) { snprintf(why, cap, "event decode: %s", cb_strerror(rc)); cb_event_free(&ev); return rc; }
    switch (ev.kind) {
    case CB_EVENT_HTTP: {
        cb_http_res res;
        cb_res_init(&res);
        cb_handle(ctx, &ev.http, &res);
        atomic_fetch_add_explicit(&ctx->stats.requests, 1, memory_order_relaxed);
        if (res.status >= 500) atomic_fetch_add_explicit(&ctx->stats.responses_5xx, 1, memory_order_relaxed);
        rc = cb_event_encode_http(&res, out);
        cb_res_free(&res);
        break;
    }
    case CB_EVENT_WS: {
        int status = 500;
        cb_rt_apigw_event(ctx, &ev.ws, &status);
        rc = cb_event_encode_ws(status, out);
        break;
    }
    case CB_EVENT_SCHEDULE:
        rc = cb_bots_scan(ctx, cb_now_ms());
        if (rc >= 0) { cb_buf_puts(out, "{}"); rc = CB_OK; }
        break;
    case CB_EVENT_BOT_DRIVE:
        rc = cb_bots_drive(ctx, ev.game_id);
        if (rc == CB_E_NOT_FOUND) { CB_INFO("lambda", "bot_drive:%s: no such game", ev.game_id); rc = CB_OK; }
        if (rc >= 0) { cb_buf_puts(out, "{}"); rc = CB_OK; }
        break;
    default:
        rc = CB_E_INVALID;
    }
    if (rc != CB_OK) snprintf(why, cap, "dispatch: %s", cb_strerror(rc));
    cb_event_free(&ev);
    return rc;
}

int main(void) {
    char host[256];
    int port = 0;
    if (cb_runtime_init(host, sizeof host, &port) != CB_OK) {
        fprintf(stderr, "bootstrap: AWS_LAMBDA_RUNTIME_API is not set (this binary runs under Lambda; use `cbase serve` locally)\n");
        return 2;
    }
    cb_config cfg;
    char err[512];
    if (cb_config_from_env(&cfg, err, sizeof err) != CB_OK) {
        cb_runtime_init_error(host, port, "Config", err);
        return 1;
    }
    cb_log_init(cfg.log_level, 0);
    static cb_ctx ctx;
    ctx.cfg = &cfg;
    ctx.mode = CB_MODE_LAMBDA;
    if (cb_boot(&ctx, err, sizeof err) != CB_OK) {
        CB_ERROR("lambda", "%s", err);
        cb_runtime_init_error(host, port, "Boot", err);
        return 1;
    }
    CB_INFO("lambda", "ready, runtime API at %s:%d", host, port);
    for (;;) {
        cb_invocation inv;
        if (cb_runtime_next(host, port, &inv) != CB_OK) { cb_invocation_free(&inv); break; }
        ctx.deadline_ms = inv.deadline_ms;
        cb_store_set_deadline(inv.deadline_ms);   // a throttled request stops retrying before it
        cb_buf out;
        cb_buf_init(&out);
        char why[256];
        if (dispatch(&ctx, &inv, &out, why, sizeof why) == CB_OK) {
            cb_runtime_respond(host, port, inv.request_id, out.p, out.len);
        } else {
            CB_WARN("lambda", "invocation %s: %s", inv.request_id, why);
            cb_runtime_error(host, port, inv.request_id, "Handler", why);
        }
        ctx.deadline_ms = 0;
        cb_store_set_deadline(0);
        cb_buf_free(&out);
        cb_invocation_free(&inv);
    }
    cb_unboot(&ctx);
    return 0;
}
