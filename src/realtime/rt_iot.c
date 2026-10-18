// rt_iot.c - realtime under Lambda: AWS IoT Core (rt_iot.h).
#include "rt_iot.h"

#include "rt_deps.h"
#include "stream_mqtt.h"
#include "json_surface.h"
#include "util/err.h"
#include "util/log.h"
#include "util/str.h"

#include "../../../shared/c/mqtt/mqtt.h"

#include <stdlib.h>
#include <string.h>

static int iot_deliver(cb_rt *rt, const cb_rt_to *to, const uint8_t *frame, size_t len) {
    const cb_config *cfg = rt->ctx->cfg;
    if (!cfg || !cfg->iot_endpoint[0]) return 0;
    char topic[MQTT_TOPIC_MAX + 1];
    if (cb_rt_topic(to, topic, sizeof topic) <= 0) return 0;   // not an MQTT target (nw1's whole-game chat)
    int st = cb_rt_deps_g.iot_publish(cfg->iot_endpoint, topic, frame, len);
    if (st == 200) return 0;
    CB_WARN("realtime", "push %s: IoT answered %d", topic, st);
    return 1;
}

static void iot_free(cb_rt *rt) { free(rt); }

static const cb_rt_transport IOT_T = { "iot", iot_deliver, iot_free, NULL };

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
    if (ctx->cfg && !cb_config_origin_proven(ctx->cfg, ev->has_origin ? ev->origin : NULL)) {
        rc = CB_E_AUTH;   // not through the edge
    } else if (!arn_parts(function_arn ? function_arn : "", region, sizeof region, account, sizeof account)) {
        CB_ERROR("iot", "authorizer: the function ARN \"%s\" names no region and account", function_arn ? function_arn : "");
        rc = CB_E_STATE;
    } else {
        // No client id in the request: the policy's iot:Connect resource holds it to the user.
        rc = cb_mqtt_authorize(ctx, ev->token, ev->token_len, ev->client_id[0] ? ev->client_id : NULL,
                               strlen(ev->client_id), now_ms, &g);
    }
    if (rc != CB_OK && rc != CB_E_AUTH) return rc;
    int64_t life = rc == CB_OK ? g.expires_s - now_ms / 1000 : 0;
    int w = cb_iot_auth_encode(rc == CB_OK, rc == CB_OK ? g.user_id : NULL, life, region, account, out);
    if (w != CB_OK) return w;
    *allowed = rc == CB_OK;
    return CB_OK;
}
