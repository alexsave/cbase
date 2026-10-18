// core.h - the entries a provider's ingress calls (docs/PLATFORM.md 8.3.1).
//
// A provider owns main and the shape its work arrives in (a Lambda event, a
// socket, a Cloud Tasks POST); core sees none of it. Each kind of work is one
// entry:
//
//   cb_core_http       one HTTP request in, one response out (handler.h)
//   cb_core_connect    a stream CONNECT's verdict, for a broker that asks
//                      (IoT Core's custom authorizer); the hub answers its own
//                      CONNECTs with the same rule (stream_mqtt.h)
//   cb_core_job        the app's job "<job>:<key>" arriving (jobs.h)
//   cb_core_schedule   the app's schedule arriving (jobs.h)
//   cb_core_after      the answer-first window's work, once the answer is out
//                      (app/after.h)
//
// The process ingress (ingress_process.h) is core too: a long-lived HTTP and
// WebSocket server, which is `cbase serve`'s main and would be a Cloud Run
// service's.
#ifndef CB_CORE_H
#define CB_CORE_H

#include "ctx.h"
#include "net/http.h"
#include "realtime/stream_mqtt.h"

#include <stddef.h>
#include <stdint.h>

// cb_handle, counted in ctx->stats (requests, 5xx).
void cb_core_http(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res);

// A CONNECT's verdict: CB_OK and the grant (stream_mqtt.h cb_mqtt_authorize),
// CB_E_AUTH (refused; also a CONNECT that did not come through the edge:
// `origin` is X-Cbase-Origin's value, NULL when absent, config.h
// CBASE_ORIGIN_SECRET), or another CB_E_* when the check could not run.
int cb_core_connect(cb_ctx *ctx, const char *token, size_t token_len, const char *client_id, size_t id_len,
                    const char *origin, int64_t now_ms, cb_mqtt_grant *out);

// The app's job: CB_OK (also for nothing to do, CB_E_NOT_FOUND, which a retry
// cannot help and is logged), or the job's error.
int cb_core_job(cb_ctx *ctx, const char *job, const char *key);

// The app's schedule ("" every one): >= 0, or its error.
int cb_core_schedule(cb_ctx *ctx, const char *name, int64_t now_ms);

void cb_core_after(cb_ctx *ctx);

#endif
