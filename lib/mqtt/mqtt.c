// mqtt.c - the MQTT 3.1.1 subset of a subscribe-only push stream (mqtt.h).
#include "mqtt.h"
#include "../be_bytes.h"

#include <string.h>

// ---- small pieces ----------------------------------------------------------------


static int printable(const uint8_t *s, int n) {
    for (int i = 0; i < n; i++)
        if (s[i] < 0x21 || s[i] > 0x7E) return 0;
    return 1;
}

int mqtt_topic_ok(const char *t, int n, int filter) {
    if (!t || n < 1 || n > MQTT_TOPIC_MAX || !printable((const uint8_t *)t, n)) return 0;
    for (int i = 0; i < n; i++) {
        char c = t[i];
        if (c != '+' && c != '#') continue;
        if (!filter) return 0;
        // A wildcard is a whole level: '/' (or an end) on each side, and '#' is the last.
        if (i > 0 && t[i - 1] != '/') return 0;
        if (c == '#' && i != n - 1) return 0;
        if (c == '+' && i + 1 < n && t[i + 1] != '/') return 0;
    }
    return 1;
}

int mqtt_topic_match(const char *f, int fn, const char *t, int tn) {
    if ((f[0] == '+' || f[0] == '#') && t[0] == '$') return 0;
    int i = 0, j = 0;
    for (;;) {
        if (i < fn && f[i] == '#') return 1;   // this level and below
        if (i < fn && f[i] == '+') {
            // One whole level of the topic, whatever it holds (empty included).
            while (j < tn && t[j] != '/') j++;
            i++;
        } else {
            while (i < fn && j < tn && f[i] != '/' && t[j] != '/') {
                if (f[i] != t[j]) return 0;
                i++, j++;
            }
            // Both levels must end together.
            if ((i < fn && f[i] != '/') || (j < tn && t[j] != '/')) return 0;
        }
        if (i == fn && j == tn) return 1;
        // "a/#" matches "a": the filter's next level is a lone '#'.
        if (j == tn && i + 2 == fn && f[i] == '/' && f[i + 1] == '#') return 1;
        if (i == fn || j == tn) return 0;
        i++, j++;   // past the '/' on both sides
    }
}

// ---- reading -----------------------------------------------------------------------

// A u16-length string at v[*at], within rem: 1 and the span, else 0.
static int str_at(const uint8_t *v, int rem, int *at, const uint8_t **s, uint16_t *len) {
    if (rem - *at < 2) return 0;
    int n = be_get_u16(v + *at);
    if (rem - *at - 2 < n) return 0;
    *s = v + *at + 2;
    *len = (uint16_t)n;
    *at += 2 + n;
    return 1;
}

static int parse_connect(const uint8_t *v, int rem, MqttPacket *p) {
    static const uint8_t name[] = { 0, 4, 'M', 'Q', 'T', 'T' };
    if (rem < 10 || memcmp(v, name, sizeof name) != 0 || v[6] != 4) return -1;
    uint8_t fl = v[7];
    // Reserved bit 0 clear; no will (bit 2), so no will QoS or retain either.
    if (fl & 0x01 || fl & 0x04 || fl & 0x18 || fl & 0x20) return -1;
    int has_user = (fl >> 7) & 1, has_pass = (fl >> 6) & 1;
    if (has_pass && !has_user) return -1;
    p->clean_session = (fl >> 1) & 1;
    p->keepalive_s = (uint16_t)be_get_u16(v + 8);
    int at = 10;
    if (!str_at(v, rem, &at, &p->client_id, &p->client_id_len)) return -1;
    if (p->client_id_len < 1 || p->client_id_len > MQTT_ID_MAX || !printable(p->client_id, p->client_id_len)) return -1;
    if (has_user) {
        if (!str_at(v, rem, &at, &p->username, &p->username_len)) return -1;
        p->has_username = 1;
    }
    if (has_pass) {
        if (!str_at(v, rem, &at, &p->password, &p->password_len)) return -1;
        p->has_password = 1;
    }
    return at == rem ? 0 : -1;
}

static int parse_publish(const uint8_t *v, int rem, int flags, MqttPacket *p) {
    p->dup = (uint8_t)((flags >> 3) & 1);
    p->qos = (uint8_t)((flags >> 1) & 3);
    p->retain = (uint8_t)(flags & 1);
    if (p->qos > 1 || (p->qos == 0 && p->dup)) return -1;
    int at = 0;
    if (!str_at(v, rem, &at, &p->topic, &p->topic_len)) return -1;
    if (!mqtt_topic_ok((const char *)p->topic, p->topic_len, 0)) return -1;
    if (p->qos) {
        if (rem - at < 2) return -1;
        p->packet_id = (uint16_t)be_get_u16(v + at);
        if (p->packet_id == 0) return -1;
        at += 2;
    }
    p->payload = v + at;
    p->payload_len = rem - at;
    return 0;
}

// SUBSCRIBE (with_qos) or UNSUBSCRIBE: a packet id, then 1 to MQTT_ITEMS_MAX filters.
static int parse_filters(const uint8_t *v, int rem, int with_qos, MqttPacket *p) {
    if (rem < 2) return -1;
    p->packet_id = (uint16_t)be_get_u16(v);
    if (p->packet_id == 0) return -1;
    p->items = v + 2;
    p->items_len = rem - 2;
    int at = 2, n = 0;
    while (at < rem) {
        const uint8_t *f;
        uint16_t fl;
        if (!str_at(v, rem, &at, &f, &fl) || !mqtt_topic_ok((const char *)f, fl, 1)) return -1;
        if (with_qos) {
            if (at >= rem || v[at] > 2) return -1;   // reserved bits clear, QoS 0 to 2
            at++;
        }
        if (++n > MQTT_ITEMS_MAX) return -1;
    }
    if (n == 0) return -1;
    p->n_items = (uint8_t)n;
    return 0;
}

// Whether the side `from` ever sends a packet of this type: known from the
// first byte, so junk is refused before a byte of its "body" is waited for.
static int sends(int type, int from) {
    static const uint16_t SERVER = 1u << MQTT_CONNACK | 1u << MQTT_PUBLISH | 1u << MQTT_PUBACK | 1u << MQTT_SUBACK |
                                   1u << MQTT_UNSUBACK | 1u << MQTT_PINGRESP;
    static const uint16_t CLIENT = 1u << MQTT_CONNECT | 1u << MQTT_PUBLISH | 1u << MQTT_PUBACK | 1u << MQTT_SUBSCRIBE |
                                   1u << MQTT_UNSUBSCRIBE | 1u << MQTT_PINGREQ | 1u << MQTT_DISCONNECT;
    return ((from == MQTT_FROM_SERVER ? SERVER : CLIENT) >> type) & 1;
}

int mqtt_parse(const uint8_t *buf, int n, int max, int from, MqttPacket *p) {
    if (!buf || n < 1) return 0;
    if (!sends(buf[0] >> 4, from)) return -1;
    if (n < 2) return 0;
    int rem = 0, i = 1, shift = 0;
    for (;;) {
        if (i > 4) return -1;          // a fifth length byte
        if (i >= n) return 0;
        uint8_t b = buf[i++];
        rem |= (b & 0x7F) << shift;
        if (!(b & 0x80)) break;
        shift += 7;
    }
    if (i > 2 && buf[i - 1] == 0) return -1;   // a length in more bytes than it needs (MQTT 5 1.5.5 says so outright)
    if (rem > max - i) return -1;      // longer than the reader takes (max may be below i: refused too)
    if (n - i < rem) return 0;
    const uint8_t *v = buf + i;
    int type = buf[0] >> 4, flags = buf[0] & 0x0F;
    memset(p, 0, sizeof *p);
    p->type = (uint8_t)type;
    int server = from == MQTT_FROM_SERVER;
    int ok = -1;
    switch (type) {
    case MQTT_CONNECT:
        if (!server && flags == 0) ok = parse_connect(v, rem, p);
        break;
    case MQTT_CONNACK:
        if (server && flags == 0 && rem == 2 && !(v[0] & 0xFE) && v[1] <= MQTT_RC_NOT_AUTHORIZED) {
            p->session_present = v[0];
            p->return_code = v[1];
            ok = 0;
        }
        break;
    case MQTT_PUBLISH:
        ok = parse_publish(v, rem, flags, p);
        break;
    case MQTT_PUBACK:
    case MQTT_UNSUBACK:
        if (flags == 0 && rem == 2 && (type == MQTT_PUBACK || server)) {
            p->packet_id = (uint16_t)be_get_u16(v);
            ok = p->packet_id ? 0 : -1;
        }
        break;
    case MQTT_SUBSCRIBE:
        if (!server && flags == 0x2) ok = parse_filters(v, rem, 1, p);
        break;
    case MQTT_UNSUBSCRIBE:
        if (!server && flags == 0x2) ok = parse_filters(v, rem, 0, p);
        break;
    case MQTT_SUBACK:
        if (server && flags == 0 && rem >= 3 && rem - 2 <= MQTT_ITEMS_MAX) {
            p->packet_id = (uint16_t)be_get_u16(v);
            p->items = v + 2;
            p->items_len = rem - 2;
            p->n_items = (uint8_t)(rem - 2);
            ok = p->packet_id ? 0 : -1;
            for (int k = 2; k < rem && ok == 0; k++)
                if (v[k] > 2 && v[k] != MQTT_SUBACK_FAILURE) ok = -1;
        }
        break;
    case MQTT_PINGREQ:
    case MQTT_DISCONNECT:
        if (!server && flags == 0 && rem == 0) ok = 0;
        break;
    case MQTT_PINGRESP:
        if (server && flags == 0 && rem == 0) ok = 0;
        break;
    default:
        break;                         // 0, 15, and the QoS 2 flow (5, 6, 7): not this subset
    }
    return ok == 0 ? i + rem : -1;
}

int mqtt_next_filter(const MqttPacket *p, int *pos, const uint8_t **filter, uint16_t *len, uint8_t *qos) {
    if (!p || (p->type != MQTT_SUBSCRIBE && p->type != MQTT_UNSUBSCRIBE) || *pos >= p->items_len) return 0;
    int at = *pos;
    if (!str_at(p->items, p->items_len, &at, filter, len)) return 0;
    *qos = 0;
    if (p->type == MQTT_SUBSCRIBE) {
        if (at >= p->items_len) return 0;
        *qos = p->items[at++];
    }
    *pos = at;
    return 1;
}

// ---- writing -----------------------------------------------------------------------

// The fixed header for a body of `rem` bytes: the type and flags byte, then the
// remaining length in one to four bytes. Its length, or -1 when the header and
// body cannot both fit.
static int head(uint8_t *out, int cap, int type, int flags, int rem) {
    if (rem < 0 || rem > 268435455 || cap < 2) return -1;
    int n = 0;
    out[n++] = (uint8_t)(type << 4 | flags);
    do {
        if (n >= cap) return -1;
        uint8_t b = (uint8_t)(rem & 0x7F);
        rem >>= 7;
        out[n++] = rem ? (uint8_t)(b | 0x80) : b;
    } while (rem);
    return n;
}

static int fits(int h, int rem, int cap) { return h >= 0 && rem <= cap - h; }

static int put_str(uint8_t *p, const void *s, int n) {
    be_put_u16(p, n);
    if (n) memcpy(p + 2, s, (size_t)n);
    return 2 + n;
}

int mqtt_write_connect(uint8_t *out, int cap, const char *client_id, int id_len, uint16_t keepalive_s,
                       const char *username, int username_len, const uint8_t *password, int password_len) {
    if (!out || !client_id || id_len < 1 || id_len > MQTT_ID_MAX || !printable((const uint8_t *)client_id, id_len)) return -1;
    if (password && !username) return -1;
    if (username && (username_len < 0 || username_len > 0xFFFF)) return -1;
    if (password && (password_len < 0 || password_len > 0xFFFF)) return -1;
    int rem = 10 + 2 + id_len + (username ? 2 + username_len : 0) + (password ? 2 + password_len : 0);
    int h = head(out, cap, MQTT_CONNECT, 0, rem);
    if (!fits(h, rem, cap)) return -1;
    uint8_t *p = out + h;
    static const uint8_t vh[] = { 0, 4, 'M', 'Q', 'T', 'T', 4 };
    memcpy(p, vh, sizeof vh);
    p[7] = (uint8_t)(0x02 | (username ? 0x80 : 0) | (password ? 0x40 : 0));   // clean session
    be_put_u16(p + 8, keepalive_s);
    int at = 10;
    at += put_str(p + at, client_id, id_len);
    if (username) at += put_str(p + at, username, username_len);
    if (password) at += put_str(p + at, password, password_len);
    return h + rem;
}

// SUBSCRIBE (with_qos) or UNSUBSCRIBE.
static int write_filters(uint8_t *out, int cap, int type, uint16_t packet_id, const MqttTopic *t, int n, int with_qos) {
    if (!out || !t || packet_id == 0 || n < 1 || n > MQTT_ITEMS_MAX) return -1;
    int rem = 2;
    for (int k = 0; k < n; k++) {
        if (!mqtt_topic_ok(t[k].p, t[k].len, 1) || (with_qos && t[k].qos > 1)) return -1;
        rem += 2 + t[k].len + (with_qos ? 1 : 0);
    }
    int h = head(out, cap, type, 0x2, rem);
    if (!fits(h, rem, cap)) return -1;
    uint8_t *p = out + h;
    be_put_u16(p, packet_id);
    int at = 2;
    for (int k = 0; k < n; k++) {
        at += put_str(p + at, t[k].p, t[k].len);
        if (with_qos) p[at++] = t[k].qos;
    }
    return h + rem;
}

int mqtt_write_subscribe(uint8_t *out, int cap, uint16_t packet_id, const MqttTopic *topics, int n) {
    return write_filters(out, cap, MQTT_SUBSCRIBE, packet_id, topics, n, 1);
}

int mqtt_write_unsubscribe(uint8_t *out, int cap, uint16_t packet_id, const MqttTopic *topics, int n) {
    return write_filters(out, cap, MQTT_UNSUBSCRIBE, packet_id, topics, n, 0);
}

static int write_id(uint8_t *out, int cap, int type, uint16_t packet_id) {
    if (!out || cap < 4 || packet_id == 0) return -1;
    out[0] = (uint8_t)(type << 4);
    out[1] = 2;
    be_put_u16(out + 2, packet_id);
    return 4;
}

int mqtt_write_puback(uint8_t *out, int cap, uint16_t packet_id) { return write_id(out, cap, MQTT_PUBACK, packet_id); }
int mqtt_write_unsuback(uint8_t *out, int cap, uint16_t packet_id) { return write_id(out, cap, MQTT_UNSUBACK, packet_id); }

int mqtt_write_bare(uint8_t *out, int cap, int type) {
    if (!out || cap < 2 || (type != MQTT_PINGREQ && type != MQTT_PINGRESP && type != MQTT_DISCONNECT)) return -1;
    out[0] = (uint8_t)(type << 4);
    out[1] = 0;
    return 2;
}

int mqtt_write_connack(uint8_t *out, int cap, int session_present, int return_code) {
    if (!out || cap < 4 || (session_present != 0 && session_present != 1) || return_code < 0 ||
        return_code > MQTT_RC_NOT_AUTHORIZED)
        return -1;
    out[0] = MQTT_CONNACK << 4;
    out[1] = 2;
    out[2] = (uint8_t)session_present;
    out[3] = (uint8_t)return_code;
    return 4;
}

int mqtt_write_suback(uint8_t *out, int cap, uint16_t packet_id, const uint8_t *codes, int n) {
    if (!out || !codes || packet_id == 0 || n < 1 || n > MQTT_ITEMS_MAX) return -1;
    for (int k = 0; k < n; k++)
        if (codes[k] > 2 && codes[k] != MQTT_SUBACK_FAILURE) return -1;
    int rem = 2 + n;
    int h = head(out, cap, MQTT_SUBACK, 0, rem);
    if (!fits(h, rem, cap)) return -1;
    be_put_u16(out + h, packet_id);
    memcpy(out + h + 2, codes, (size_t)n);
    return h + rem;
}

int mqtt_publish_len(int topic_len, int payload_len) {
    if (topic_len < 1 || topic_len > MQTT_TOPIC_MAX || payload_len < 0 || payload_len > 268435455 - 2 - topic_len)
        return -1;
    int rem = 2 + topic_len + payload_len;
    int h = rem < 128 ? 2 : rem < 16384 ? 3 : rem < 2097152 ? 4 : 5;
    return h + rem;
}

int mqtt_write_publish(uint8_t *out, int cap, const char *topic, int topic_len, const uint8_t *payload, int payload_len) {
    if (!out || !mqtt_topic_ok(topic, topic_len, 0) || payload_len < 0 || (payload_len && !payload)) return -1;
    if (mqtt_publish_len(topic_len, payload_len) < 0) return -1;
    int rem = 2 + topic_len + payload_len;
    int h = head(out, cap, MQTT_PUBLISH, 0, rem);
    if (!fits(h, rem, cap)) return -1;
    int at = h + put_str(out + h, topic, topic_len);
    if (payload_len) memcpy(out + at, payload, (size_t)payload_len);
    return h + rem;
}
