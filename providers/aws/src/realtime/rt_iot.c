// rt_iot.c - realtime under Lambda: AWS IoT Core (rt_iot.h).
#include "rt_iot.h"

#include "aws/iot_data.h"
#include "core.h"
#include "realtime/stream_mqtt.h"
#include "aws_json.h"
#include "util/err.h"
#include "util/log.h"
#include "util/str.h"

#include "../../../../../shared/c/mqtt/mqtt.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

int (*cb_rt_iot_publish)(const char *endpoint, const char *topic, const void *data, size_t len) = cb_iot_publish;

static int iot_deliver(cb_rt *rt, const cb_rt_to *to, const uint8_t *frame, size_t len) {
    const cb_config *cfg = rt->ctx->cfg;
    if (!cfg || !cfg->iot_endpoint[0]) return 0;
    char topic[MQTT_TOPIC_MAX + 1];
    if (cb_rt_topic(to, topic, sizeof topic) <= 0) return 0;   // no topic: an id the pattern needs is missing
    int st = cb_rt_iot_publish(cfg->iot_endpoint, topic, frame, len);
    if (st == 200) return 0;
    CB_WARN("realtime", "push %s: IoT answered %d", topic, st);
    return 1;
}

// ---- a batch's lanes at once (realtime.h deliver_lanes) -------------------------------
//
// Each lane is one thread doing its publishes in order, the first on the
// caller's thread: a publish is one HTTPS POST that waits about 8 ms for IoT
// at p50 against well under 1 ms of CPU, so a commit's five publishes, one
// after another, held the invocation (billed) about 39 ms. On their own
// threads they take one publish's time. IoT's data plane has no batch publish
// (one topic per request), and HTTP/1.1 pipelining on one connection is not
// something it documents; https.c's pool keeps a connection per lane warm
// instead, so after a container's first fan-out no publish dials.
#define LANES_MAX 32

typedef struct {
    cb_rt                 *rt;
    const cb_rt_to        *to;
    const uint8_t *const  *frames;
    const size_t          *lens;
    const int             *lane;
    int                    n, me, failed;
} lane_job;

static void *lane_run(void *arg) {
    lane_job *j = arg;
    for (int i = 0; i < j->n; i++)
        if (j->lane[i] == j->me) j->failed += iot_deliver(j->rt, &j->to[i], j->frames[i], j->lens[i]);
    return NULL;
}

static int iot_deliver_lanes(cb_rt *rt, const cb_rt_to *to, const uint8_t *const *frames, const size_t *lens,
                             const int *lane, int n) {
    int n_lanes = 0;
    for (int i = 0; i < n; i++)
        if (lane[i] + 1 > n_lanes) n_lanes = lane[i] + 1;
    if (n_lanes > LANES_MAX) n_lanes = LANES_MAX;   // never: realtime.c holds at most 32 messages
    lane_job job[LANES_MAX];
    pthread_t th[LANES_MAX];
    bool started[LANES_MAX] = { false };
    for (int l = 0; l < n_lanes; l++) job[l] = (lane_job){ rt, to, frames, lens, lane, n, l, 0 };
    for (int l = 1; l < n_lanes; l++) started[l] = pthread_create(&th[l], NULL, lane_run, &job[l]) == 0;
    lane_run(&job[0]);
    int failed = job[0].failed;
    for (int l = 1; l < n_lanes; l++) {
        if (started[l]) pthread_join(th[l], NULL);
        else lane_run(&job[l]);   // no thread to be had: this lane, here, after the others
        failed += job[l].failed;
    }
    return failed;
}

static void iot_free(cb_rt *rt) { free(rt); }

static const cb_rt_transport IOT_T = { "iot", iot_deliver, iot_free, NULL, iot_deliver_lanes };

int cb_rt_iot_new(cb_ctx *ctx, cb_rt **out) {
    *out = NULL;
    cb_rt *rt = calloc(1, sizeof *rt);
    if (!rt) return CB_E_NOMEM;
    rt->t = &IOT_T;
    rt->ctx = ctx;
    *out = rt;
    return CB_OK;
}

// arn:aws:lambda:<region>:<account>:function:<name>: the region and the account.
static bool arn_parts(const char *arn, char *region, size_t rcap, char *account, size_t acap) {
    const char *f[6];
    int k = 0;
    f[k++] = arn;
    for (const char *p = arn; *p && k < 6; p++)
        if (*p == ':') f[k++] = p + 1;
    if (k < 6) return false;
    return cb_strncpy0(region, rcap, f[3], (size_t)(f[4] - f[3] - 1)) &&
           cb_strncpy0(account, acap, f[4], (size_t)(f[5] - f[4] - 1));
}

int cb_iot_authorize(cb_ctx *ctx, const cb_iot_auth_event *ev, const char *function_arn, int64_t now_ms, cb_buf *out,
                     bool *allowed) {
    *allowed = false;
    cb_buf_reset(out);
    char region[40], account[40];
    cb_mqtt_grant g;
    int rc = CB_E_AUTH;
    if (!arn_parts(function_arn ? function_arn : "", region, sizeof region, account, sizeof account)) {
        CB_ERROR("iot", "authorizer: the function ARN \"%s\" names no region and account", function_arn ? function_arn : "");
        rc = CB_E_STATE;
    } else {
        // No client id in the request: the policy's iot:Connect resource holds it to the user.
        rc = cb_core_connect(ctx, ev->token, ev->token_len, ev->client_id[0] ? ev->client_id : NULL,
                             strlen(ev->client_id), ev->has_origin ? ev->origin : NULL, now_ms, &g);
    }
    if (rc != CB_OK && rc != CB_E_AUTH) return rc;
    int64_t life = rc == CB_OK ? g.expires_s - now_ms / 1000 : 0;
    int w = cb_iot_auth_encode(rc == CB_OK, rc == CB_OK ? g.user_id : NULL, life, region, account, out);
    if (w != CB_OK) return w;
    *allowed = rc == CB_OK;
    return CB_OK;
}
