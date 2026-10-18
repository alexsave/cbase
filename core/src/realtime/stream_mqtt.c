// stream_mqtt.c - the push stream over MQTT (stream_mqtt.h).
#include "stream_mqtt.h"

#include "rt_deps.h"
#include "net/http.h"
#include "util/err.h"
#include "util/log.h"
#include "util/str.h"

#include <stdio.h>
#include <string.h>

// ---- the policy --------------------------------------------------------------------------

bool cb_mqtt_user_ok(const char *u) {
    if (!u || strlen(u) != 36) return false;
    for (int i = 0; i < 36; i++) {
        char c = u[i];
        bool dash = i == 8 || i == 13 || i == 18 || i == 23;
        if (dash ? c != '-' : !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

int cb_mqtt_authorize(cb_ctx *ctx, const char *token, size_t token_len, const char *client_id, size_t id_len,
                      int64_t now_ms, cb_mqtt_grant *out) {
    memset(out, 0, sizeof *out);
    if (!token || token_len == 0 || token_len > CB_RT_TOKEN_MAX) return CB_E_AUTH;
    char tok[CB_RT_TOKEN_MAX + 1];
    memcpy(tok, token, token_len);
    tok[token_len] = 0;
    if (memchr(tok, 0, token_len)) return CB_E_AUTH;
    cb_user u;
    memset(&u, 0, sizeof u);
    int rc = cb_rt_deps_g.verify_token(ctx, tok, &u);
    if (rc != CB_OK) return rc;
    if (!cb_mqtt_user_ok(u.id)) return CB_E_AUTH;
    // "<user>-" and at least one byte after it (NULL: the broker holds it to that itself).
    size_t ul = strlen(u.id);
    if (client_id && (id_len < ul + 2 || memcmp(client_id, u.id, ul) != 0 || client_id[ul] != '-')) return CB_E_AUTH;
    int64_t now_s = now_ms / 1000;
    int64_t until = u.expires_at;
    if (until < now_s + CB_MQTT_MIN_LIFE_S) until = now_s + CB_MQTT_MIN_LIFE_S;
    if (until > now_s + CB_MQTT_MAX_LIFE_S) until = now_s + CB_MQTT_MAX_LIFE_S;
    memcpy(out->user_id, u.id, ul + 1);
    out->expires_s = until;
    return CB_OK;
}

int cb_mqtt_may_subscribe(const char *user_id, const char *f, int n) {
    if (!f || n <= 0) return 0;
    int wl = (int)strlen(CB_MQTT_WATCH_PREFIX);
    if (n > wl && memcmp(f, CB_MQTT_WATCH_PREFIX, (size_t)wl) == 0) return 1;
    if (!cb_mqtt_user_ok(user_id)) return 0;
    int pl = (int)strlen(CB_MQTT_USER_PREFIX), ul = (int)strlen(user_id);
    return n > pl + ul + 1 && memcmp(f, CB_MQTT_USER_PREFIX, (size_t)pl) == 0 &&
           memcmp(f + pl, user_id, (size_t)ul) == 0 && f[pl + ul] == '/';
}

// ---- one connection -----------------------------------------------------------------------

void cb_mqtt_conn_init(cb_mqtt_conn *c) { memset(c, 0, sizeof *c); }

bool cb_mqtt_hears(const cb_mqtt_conn *c, const char *t, int n) {
    if (c->state != CB_MQTT_OPEN) return false;
    for (int i = 0; i < c->n_subs; i++)
        if (mqtt_topic_match(c->subs[i], (int)strlen(c->subs[i]), t, n)) return true;
    return false;
}

static int sub_index(const cb_mqtt_conn *c, const uint8_t *f, int n) {
    for (int i = 0; i < c->n_subs; i++)
        if ((int)strlen(c->subs[i]) == n && memcmp(c->subs[i], f, (size_t)n) == 0) return i;
    return -1;
}

static void reply(cb_rt_send_fn send, void *ud, const uint8_t *b, int n) {
    if (n > 0) send(ud, (const char *)b, (size_t)n);
}

static int on_connect(cb_ctx *ctx, cb_mqtt_conn *c, const MqttPacket *p, int64_t now_ms, cb_rt_send_fn send, void *ud) {
    if (c->state != CB_MQTT_AWAIT_CONNECT) return CB_E_INVALID;   // a second CONNECT (3.1.0-2)
    if (p->keepalive_s > CB_MQTT_KEEPALIVE_MAX) return CB_E_INVALID;
    const char *tok = p->has_password ? (const char *)p->password : c->query_token;
    size_t tl = p->has_password ? p->password_len : strlen(c->query_token);
    cb_mqtt_grant g;
    int rc = cb_mqtt_authorize(ctx, tok, tl, (const char *)p->client_id, p->client_id_len, now_ms, &g);
    if (rc != CB_OK) {
        if (rc != CB_E_AUTH) CB_WARN("mqtt", "connect: the token check failed: %s", cb_strerror(rc));
        return rc;
    }
    c->state = CB_MQTT_OPEN;
    memcpy(c->user_id, g.user_id, sizeof c->user_id);
    c->expires_s = g.expires_s;
    c->keepalive_s = p->keepalive_s;
    uint8_t out[4];
    reply(send, ud, out, mqtt_write_connack(out, sizeof out, 0, MQTT_RC_ACCEPTED));
    return CB_OK;
}

static int on_subscribe(cb_mqtt_conn *c, const MqttPacket *p, cb_rt_send_fn send, void *ud) {
    // Every filter is checked before any is taken: one outside the policy ends
    // the connection, as IoT ends it, and nothing it asked for is kept.
    int pos = 0, fresh = 0;
    const uint8_t *f;
    uint16_t fl;
    uint8_t qos, codes[MQTT_ITEMS_MAX];
    int k = 0;
    while (mqtt_next_filter(p, &pos, &f, &fl, &qos)) {
        if (!cb_mqtt_may_subscribe(c->user_id, (const char *)f, fl)) return CB_E_FORBIDDEN;
        if (sub_index(c, f, fl) < 0) fresh++;
        codes[k++] = 0;   // granted at QoS 0: the hub never sends anything else
    }
    if (c->n_subs + fresh > CB_MQTT_SUBS) return CB_E_FULL;
    pos = 0;
    while (mqtt_next_filter(p, &pos, &f, &fl, &qos)) {
        if (sub_index(c, f, fl) >= 0) continue;   // the same filter again replaces itself (3.8.4)
        memcpy(c->subs[c->n_subs], f, fl);
        c->subs[c->n_subs][fl] = 0;
        c->n_subs++;
    }
    uint8_t out[4 + MQTT_ITEMS_MAX];
    reply(send, ud, out, mqtt_write_suback(out, sizeof out, p->packet_id, codes, k));
    return CB_OK;
}

static void on_unsubscribe(cb_mqtt_conn *c, const MqttPacket *p, cb_rt_send_fn send, void *ud) {
    int pos = 0;
    const uint8_t *f;
    uint16_t fl;
    uint8_t qos;
    while (mqtt_next_filter(p, &pos, &f, &fl, &qos)) {
        int i = sub_index(c, f, fl);
        if (i < 0) continue;
        if (i + 1 < c->n_subs) memmove(c->subs[i], c->subs[i + 1], (size_t)(c->n_subs - i - 1) * sizeof c->subs[0]);
        c->n_subs--;
        memset(c->subs[c->n_subs], 0, sizeof c->subs[0]);
    }
    uint8_t out[4];
    reply(send, ud, out, mqtt_write_unsuback(out, sizeof out, p->packet_id));
}

// One whole packet.
static int on_packet(cb_ctx *ctx, cb_mqtt_conn *c, const MqttPacket *p, int64_t now_ms, cb_rt_send_fn send, void *ud) {
    if (p->type == MQTT_CONNECT) return on_connect(ctx, c, p, now_ms, send, ud);
    if (c->state != CB_MQTT_OPEN) return CB_E_INVALID;   // anything before CONNECT (3.1.0-1)
    // The connection ends when its token does (IoT's disconnectAfterInSeconds);
    // a live client pings well inside that, so this is checked on its packets.
    if (now_ms / 1000 >= c->expires_s) return CB_E_AUTH;
    switch (p->type) {
    case MQTT_SUBSCRIBE: return on_subscribe(c, p, send, ud);
    case MQTT_UNSUBSCRIBE: on_unsubscribe(c, p, send, ud); return CB_OK;
    case MQTT_PINGREQ: {
        uint8_t out[2];
        reply(send, ud, out, mqtt_write_bare(out, sizeof out, MQTT_PINGRESP));
        return CB_OK;
    }
    case MQTT_PUBACK: return CB_OK;           // the hub never sends QoS 1: nothing to match
    case MQTT_DISCONNECT: return CB_E_STATE;
    case MQTT_PUBLISH: return CB_E_FORBIDDEN;  // no client publishes
    default: return CB_E_INVALID;
    }
}

int cb_mqtt_on_bytes(cb_ctx *ctx, cb_mqtt_conn *c, const uint8_t *bytes, size_t len, int64_t now_ms,
                     cb_rt_send_fn send, void *ud) {
    if (c->state == CB_MQTT_CLOSED) return CB_E_STATE;
    size_t at = 0;
    for (;;) {
        // Top the carried bytes up from the message, one packet's worth at most.
        size_t room = sizeof c->in - (size_t)c->in_len, take = len - at < room ? len - at : room;
        memcpy(c->in + c->in_len, bytes + at, take);
        c->in_len += (int)take;
        at += take;
        MqttPacket p;
        int k = mqtt_parse(c->in, c->in_len, CB_MQTT_PACKET_MAX, MQTT_FROM_CLIENT, &p);
        if (k < 0) {
            // Malformed, or announcing more than CB_MQTT_PACKET_MAX.
            c->state = CB_MQTT_CLOSED;
            return c->in_len >= 2 && mqtt_parse(c->in, c->in_len, 1 << 28, MQTT_FROM_CLIENT, &p) >= 0 ? CB_E_FULL : CB_E_INVALID;
        }
        if (k == 0) {
            if (at < len) {   // the buffer is full and still no packet: past the limit
                c->state = CB_MQTT_CLOSED;
                return CB_E_FULL;
            }
            return CB_OK;     // the rest comes in a later message
        }
        int rc = on_packet(ctx, c, &p, now_ms, send, ud);
        memmove(c->in, c->in + k, (size_t)(c->in_len - k));
        c->in_len -= k;
        if (rc != CB_OK) {
            c->state = CB_MQTT_CLOSED;
            return rc;
        }
        if (at == len && c->in_len == 0) return CB_OK;
    }
}

// ---- the upgrade's token ------------------------------------------------------------------

typedef struct {
    char  *out;
    size_t cap;
    bool   ok;
} token_scan;

static bool token_fn(const char *key, const char *value, void *ud) {
    if (strcmp(key, "token") != 0) return true;
    token_scan *t = ud;
    t->ok = cb_strlcpy(t->out, value, t->cap) < t->cap;
    if (!t->ok) t->out[0] = 0;
    return false;
}

bool cb_mqtt_query_token(const char *query, char *out, size_t cap) {
    if (!out || !cap) return false;
    out[0] = 0;
    token_scan t = { out, cap, true };
    if (cb_http_query_each(query ? query : "", token_fn, &t) < 0) return false;
    return t.ok;
}
