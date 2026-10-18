// fz_iot_auth.c - IoT Core's custom authorizer request (aws/event.h
// CB_EVENT_IOT_AUTH, decoded in core_json.c or aws_json.c) and its answer
// (realtime/rt_iot.h cb_iot_authorize): any bytes as the event, the token
// check faked so "tok-<uuid>" is a live account. The answer is always valid
// JSON; an allowed one names exactly the token's user, and its policy never
// grants a publish, a wildcard user or another account's topics.
#include "fz.h"

#include "aws/event.h"
#include "aws_json.h"
#include "realtime/rt_deps.h"
#include "realtime/rt_iot.h"
#include "realtime/stream_mqtt.h"
#include "util/err.h"
#include "util/json.h"
#include "util/log.h"
#include "util/str.h"

static int fake_verify(cb_ctx *ctx, const char *jwt, cb_user *out) {
    // The real check hands back the token's own subject, never a cut one.
    if (strncmp(jwt, "tok-", 4) != 0 || strlen(jwt + 4) >= sizeof out->id) return CB_E_AUTH;
    memset(out, 0, sizeof *out);
    cb_strlcpy(out->id, jwt + 4, sizeof out->id);
    out->expires_at = 1700000000 + 3600;
    return CB_OK;
}

int LLVMFuzzerInitialize(int *argc, char ***argv) {
    cb_log_init(CB_LOG_ERROR + 1, 0);
    cb_rt_deps_g.verify_token = fake_verify;
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size > (1 << 16)) return 0;
    char *json = fz_cstr(data, size);
    cb_event ev;
    int rc = cb_event_decode(json, size, &ev);
    if (rc == CB_OK && ev.kind == CB_EVENT_IOT_AUTH) {
        FZ_CHECK(ev.iot.token_len < sizeof ev.iot.token && strlen(ev.iot.token) == ev.iot.token_len);
        static cb_ctx ctx;
        static cb_config cfg;
        cfg.origin_secret[0] = 0;
        if (size & 1) cb_strlcpy(cfg.origin_secret, "0123456789abcdef0123456789abcdef", sizeof cfg.origin_secret);
        ctx.cfg = &cfg;
        cb_buf out;
        cb_buf_init(&out);
        bool ok = false;
        int a = cb_iot_authorize(&ctx, &ev.iot, "arn:aws:lambda:us-east-1:487337210065:function:cbase",
                                 1700000000000LL, &out, &ok);
        if (a == CB_OK) {
            const char *j = cb_buf_cstr(&out);
            FZ_CHECK(cb_json_valid(j, out.len));
            bool authed = !ok;
            FZ_CHECK(cb_json_get_bool(j, out.len, "isAuthenticated", &authed) && authed == ok);
            if (ok) {
                // The token's user, and only theirs.
                FZ_CHECK(ev.iot.token_len > 4 && cb_mqtt_user_ok(ev.iot.token + 4));
                char pol[4096];
                FZ_CHECK(cb_json_get_str(j, out.len, "policyDocuments.0", pol, sizeof pol));
                FZ_CHECK(cb_json_valid(pol, strlen(pol)));
                FZ_CHECK(!strstr(pol, "iot:Publish"));
                FZ_CHECK(!strstr(pol, "u/*") && !strstr(pol, "u/+") && !strstr(pol, "\"*\""));
                char want[96];
                snprintf(want, sizeof want, "topicfilter/u/%.36s/*", ev.iot.token + 4);   // a checked UUID: 36 bytes
                FZ_CHECK(strstr(pol, want) != NULL);
            } else {
                FZ_CHECK(strcmp(j, "{\"isAuthenticated\":false}") == 0);
            }
        } else {
            FZ_CHECK(!ok);
        }
        cb_buf_free(&out);
    }
    if (rc == CB_OK) cb_event_free(&ev);
    free(json);
    return 0;
}
