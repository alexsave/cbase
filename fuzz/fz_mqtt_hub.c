// fz_mqtt_hub.c - the hub's side of an MQTT connection (realtime/stream_mqtt.h
// cb_mqtt_on_bytes): a client's bytes in WebSocket messages of any size, the
// token check faked so the fuzzer reaches every packet past CONNECT. Every
// reply must be a whole packet a client reads; the connection may only ever
// hold filters the policy allows its user; a refused step closes it for good.
#include "fz.h"

#include "realtime/rt_deps.h"
#include "realtime/stream_mqtt.h"
#include "util/err.h"
#include "util/log.h"
#include "util/str.h"

#define UA "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"

static int fake_verify(cb_ctx *ctx, const char *jwt, cb_user *out) {
    if (strcmp(jwt, "tok-db") == 0) return CB_E_DB;
    if (strncmp(jwt, "tok-", 4) != 0 || strlen(jwt + 4) >= sizeof out->id) return CB_E_AUTH;
    memset(out, 0, sizeof *out);
    cb_strlcpy(out->id, jwt + 4, sizeof out->id);
    out->expires_at = 1700000000 + 3600;
    return CB_OK;
}

static int g_replies;
static void reply(void *ud, const char *frame, size_t len) {
    MqttPacket p;
    FZ_CHECK(len >= 2 && len <= 64);
    FZ_CHECK(mqtt_parse((const uint8_t *)frame, (int)len, 64, MQTT_FROM_SERVER, &p) == (int)len);
    FZ_CHECK(p.type == MQTT_CONNACK || p.type == MQTT_SUBACK || p.type == MQTT_UNSUBACK || p.type == MQTT_PINGRESP);
    g_replies++;
}

int LLVMFuzzerInitialize(int *argc, char ***argv) {
    cb_log_init(CB_LOG_ERROR + 1, 0);
    cb_rt_deps_g.verify_token = fake_verify;
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    static cb_ctx ctx;
    static cb_mqtt_conn c;
    cb_mqtt_conn_init(&c);
    fz_in in = fz_in_of(data, size);
    // A query token sometimes, as the upgrade URL's.
    if (fz_u8(&in) & 1) cb_strlcpy(c.query_token, "tok-" UA, sizeof c.query_token);
    int64_t now = 1700000000000LL;
    int closed = 0;
    while (fz_left(&in)) {
        size_t want = fz_u8(&in) * 8 + 1, got;
        const uint8_t *msg = fz_bytes(&in, want, &got);
        uint8_t *copy = fz_dup(msg, got);
        now += fz_u8(&in) * 10000LL;   // time passes between messages (past the token's end, sometimes)
        int rc = cb_mqtt_on_bytes(&ctx, &c, copy, got, now, reply, NULL);
        free(copy);
        FZ_CHECK(c.n_subs >= 0 && c.n_subs <= CB_MQTT_SUBS);
        FZ_CHECK(c.in_len >= 0 && c.in_len < CB_MQTT_PACKET_MAX);
        for (int i = 0; i < c.n_subs; i++) {
            int fl = (int)strlen(c.subs[i]);
            FZ_CHECK(mqtt_topic_ok(c.subs[i], fl, 1));
            FZ_CHECK(cb_mqtt_may_subscribe(c.user_id, c.subs[i], fl));
        }
        if (c.state == CB_MQTT_OPEN) FZ_CHECK(cb_mqtt_user_ok(c.user_id));
        if (rc != CB_OK) {
            FZ_CHECK(c.state == CB_MQTT_CLOSED);
            closed = 1;
        }
        if (closed) FZ_CHECK(rc != CB_OK);   // once closed, nothing is taken again
    }
    return 0;
}
