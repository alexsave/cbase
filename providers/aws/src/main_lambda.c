// main_lambda.c - `bootstrap`: c_base as an AWS Lambda custom runtime
// (provided.al2023, arm64, statically linked; `make bootstrap`).
//
// Start: read the config, cb_boot (kernel self-test, store, auth, realtime
// over API Gateway, routes); a failure is reported to /runtime/init/error and
// the process exits. Then forever: GET the next invocation, decode the event
// (aws/event.h), dispatch it, POST the response, then do what the caller does
// not wait for (app/after.h: an HTTP request's pushes and bot kicks, and the
// edge cache's invalidation):
//   HTTP       cb_handle -> cb_event_encode_http
//   IOT_AUTH   cb_iot_authorize (an MQTT CONNECT to IoT Core) -> its policy
//   SCHEDULE   cb_schedule_run, the app's schedule the literal names
//              ("scan"; jobs.h)                                  -> {}
//   JOB        cb_job_run, the app's job "<job>:<key>" names
//              ("bot_drive:<id>"), budgeted against ctx->deadline_ms
//              (Lambda-Runtime-Deadline-Ms)                     -> {}
// An event that does not decode, or a handler that cannot answer, is POSTed
// to the invocation's /error. Lambda retries a failed ASYNC invocation (the
// self-invoke, the schedule) twice, so a job with nothing to do (a drive
// for a game that no longer exists: CB_E_NOT_FOUND) answers {} rather than an error: retrying it cannot help. The process holds nothing between invocations
// but what a warm container may cache (app/table_ops.h).
//
// Every invocation ends with one INFO line, so latency is measured rather
// than estimated (CloudWatch Logs Insights reads it with `parse`):
//   inv: http GET /v1/leaderboard 200 ms=8.4 after=0.0 store=2/7.9/0 push=0/0.0/0 other=0/0.0/0 rid=<request id> ip=<viewer>
// ip is the viewer's address an HTTP request came from (CloudFront's
// CloudFront-Viewer-Address, rate_limits.h cb_viewer_key): with it this line is
// the API's access log, at no cost past the line it already was (finding M9;
// CloudFront's own standard logs are a delivery to set up and pay for).
// ms is decode to response posted, after is the work done once the response
// is out (app/after.h: the pushes, the kicks, the edge invalidation), and each store|push|other
// is calls/ms/new connections (aws/https.h). rid is the invocation's Lambda
// request id (Lambda-Runtime-Aws-Request-Id), the id Lambda's own REPORT line
// carries, so Logs Insights can join the two (billed duration, memory, init).
#include "core.h"
#include "after.h"
#include "rate_limits.h"
#include "aws/event.h"
#include "aws/https.h"
#include "aws/runtime_api.h"
#include "aws/ssm.h"
#include "boot.h"
#include "cbase/app.h"
#include "cbase/provider.h"
#include "config.h"
#include "ctx.h"
#include "handler.h"
#include "aws_json.h"
#include "realtime/rt_iot.h"
#include "store/store.h"
#include "util/clock.h"
#include "util/err.h"
#include "util/log.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "../../../../shared/c/clock.h"

static int64_t mono_us(void) {
    return host_mono_us();
}

// The invocation's timing line (above). `what` names it: kind, route, status.
// The current HTTP invocation's viewer, for its timing line ("" for any other kind).
static char g_viewer[96];

static void log_timing(const char *what, const char *request_id, int64_t t0, int64_t t_resp, int64_t t_end) {
    cb_https_tally t;
    cb_https_tally_take(&t);
    CB_INFO("inv", "%s ms=%.1f after=%.1f store=%d/%.1f/%d push=%d/%.1f/%d other=%d/%.1f/%d rid=%s ip=%s", what,
            (double)(t_resp - t0) / 1000.0, (double)(t_end - t_resp) / 1000.0, t.store.calls, (double)t.store.us / 1000.0,
            t.store.dials, t.push.calls, (double)t.push.us / 1000.0, t.push.dials, t.other.calls,
            (double)t.other.us / 1000.0, t.other.dials, request_id, g_viewer[0] ? g_viewer : "-");
}

static int dispatch(cb_ctx *ctx, const cb_invocation *inv, cb_buf *out, char *why, size_t cap, char *what,
                    size_t what_cap) {
    cb_event ev;
    int rc = cb_event_decode((const char *)inv->event.p, inv->event.len, &ev);
    if (rc != CB_OK) {
        snprintf(why, cap, "event decode: %s", cb_strerror(rc));
        cb_event_log_raw((const char *)inv->event.p, inv->event.len);
        cb_event_free(&ev);
        return rc;
    }
    switch (ev.kind) {
    case CB_EVENT_HTTP: {
        cb_http_res res;
        cb_res_init(&res);
        cb_after_begin();   // the pushes and kicks wait for the answer (app/after.h)
        cb_core_http(ctx, &ev.http, &res);
        rc = cb_event_encode_http(&res, out);
        snprintf(what, what_cap, "http %s %.60s %d", ev.http.method, ev.http.path ? ev.http.path : "", res.status);
        cb_viewer_key(ctx, &ev.http, g_viewer, sizeof g_viewer);
        cb_res_free(&res);
        break;
    }
    case CB_EVENT_IOT_AUTH: {
        bool allowed = false;
        rc = cb_iot_authorize(ctx, &ev.iot, inv->function_arn, cb_now_ms(), out, &allowed);
        snprintf(what, what_cap, "iot connect %s", allowed ? "allow" : "deny");
        break;
    }
    case CB_EVENT_SCHEDULE:
        snprintf(what, what_cap, "%s", ev.schedule[0] ? ev.schedule : "schedule");
        rc = cb_core_schedule(ctx, ev.schedule, cb_now_ms());
        if (rc >= 0) rc = cb_event_encode_none(out);
        break;
    case CB_EVENT_JOB:
        snprintf(what, what_cap, "%s %s", ev.job, ev.key);
        rc = cb_core_job(ctx, ev.job, ev.key);
        if (rc >= 0) rc = cb_event_encode_none(out);
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
    // The secrets from SSM (aws/ssm.h), then the rule every one must meet.
    if ((cfg.secrets_ssm[0] && cb_ssm_load_secrets(&cfg, err, sizeof err) != CB_OK) ||
        cb_config_check_secrets(&cfg, true, err, sizeof err) != CB_OK) {
        cb_runtime_init_error(host, port, "Config", err);
        return 1;
    }
    cb_log_init(cfg.log_level, 0);
    static cb_ctx ctx;
    ctx.cfg = &cfg;
    ctx.prov = &cb_provider_aws;
    ctx.app = cb_main_app;
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
        char why[256], what[96] = "event";
        g_viewer[0] = 0;
        cb_https_tally skip;
        cb_https_tally_take(&skip);   // this invocation's count starts here
        const int64_t t0 = mono_us();
        if (dispatch(&ctx, &inv, &out, why, sizeof why, what, sizeof what) == CB_OK) {
            cb_runtime_respond(host, port, inv.request_id, out.p, out.len);
        } else {
            CB_WARN("lambda", "invocation %s: %s", inv.request_id, why);
            cb_runtime_error(host, port, inv.request_id, "Handler", why);
        }
        const int64_t t_resp = mono_us();
        // After the response is out: the caller waits for none of this.
        cb_core_after(&ctx);
        log_timing(what, inv.request_id, t0, t_resp, mono_us());
        ctx.deadline_ms = 0;
        cb_store_set_deadline(0);
        cb_buf_free(&out);
        cb_invocation_free(&inv);
    }
    cb_unboot(&ctx);
    return 0;
}
