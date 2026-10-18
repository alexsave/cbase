// fz_mqtt.c - the MQTT 3.1.1 codec (shared/c/mqtt/mqtt.h), both directions:
// what a client reads from a broker and what the backend's hub reads from a
// client, and topic filter matching. Every packet read must lie inside the
// input, its filters must walk to exactly its count, and what is read must
// write back to a packet that reads the same.
#include "fz.h"

#include "../../shared/c/mqtt/mqtt.h"

static void check_packet(const uint8_t *p, int n, int k, const MqttPacket *m, int from) {
    FZ_CHECK(k > 0 && k <= n);
    const uint8_t *end = p + k;
#define IN(ptr, len) FZ_CHECK(!(ptr) || ((ptr) >= p && (ptr) + (len) <= end))
    IN(m->client_id, m->client_id_len);
    IN(m->username, m->username_len);
    IN(m->password, m->password_len);
    IN(m->topic, m->topic_len);
    IN(m->payload, m->payload_len);
    IN(m->items, m->items_len);
#undef IN
    uint8_t out[1 << 17];
    int w = -2;
    MqttPacket r;
    switch (m->type) {
    case MQTT_CONNECT:
        FZ_CHECK(from == MQTT_FROM_CLIENT);
        FZ_CHECK(m->client_id_len >= 1 && m->client_id_len <= MQTT_ID_MAX);
        w = mqtt_write_connect(out, sizeof out, (const char *)m->client_id, m->client_id_len, m->keepalive_s,
                               m->has_username ? (const char *)m->username : NULL, m->username_len,
                               m->has_password ? m->password : NULL, m->password_len);
        FZ_CHECK(w > 0);
        FZ_CHECK(mqtt_parse(out, w, sizeof out, MQTT_FROM_CLIENT, &r) == w);
        FZ_CHECK(r.keepalive_s == m->keepalive_s && r.client_id_len == m->client_id_len &&
                 memcmp(r.client_id, m->client_id, m->client_id_len) == 0);
        FZ_CHECK(r.has_password == m->has_password && r.password_len == m->password_len);
        break;
    case MQTT_SUBSCRIBE:
    case MQTT_UNSUBSCRIBE: {
        FZ_CHECK(from == MQTT_FROM_CLIENT);
        MqttTopic t[MQTT_ITEMS_MAX];
        int pos = 0, c = 0;
        const uint8_t *f;
        uint16_t fl;
        uint8_t q;
        while (mqtt_next_filter(m, &pos, &f, &fl, &q)) {
            FZ_CHECK(c < MQTT_ITEMS_MAX);
            FZ_CHECK(mqtt_topic_ok((const char *)f, fl, 1));
            FZ_CHECK(q <= 2);
            t[c++] = (MqttTopic){ (const char *)f, fl, (uint8_t)(q > 1 ? 1 : q) };
        }
        FZ_CHECK(c == m->n_items && pos == m->items_len);
        w = m->type == MQTT_SUBSCRIBE ? mqtt_write_subscribe(out, sizeof out, m->packet_id, t, c)
                                      : mqtt_write_unsubscribe(out, sizeof out, m->packet_id, t, c);
        FZ_CHECK(w > 0);
        FZ_CHECK(mqtt_parse(out, w, sizeof out, MQTT_FROM_CLIENT, &r) == w && r.n_items == c && r.packet_id == m->packet_id);
        break;
    }
    case MQTT_PUBLISH:
        FZ_CHECK(mqtt_topic_ok((const char *)m->topic, m->topic_len, 0));
        FZ_CHECK(m->qos <= 1);
        if (m->qos == 0) {
            w = mqtt_write_publish(out, sizeof out, (const char *)m->topic, m->topic_len, m->payload, m->payload_len);
            FZ_CHECK(w == mqtt_publish_len(m->topic_len, m->payload_len));
            FZ_CHECK(mqtt_parse(out, w, sizeof out, MQTT_FROM_SERVER, &r) == w && r.payload_len == m->payload_len);
        }
        break;
    case MQTT_SUBACK:
        FZ_CHECK(from == MQTT_FROM_SERVER && m->n_items >= 1 && m->n_items <= MQTT_ITEMS_MAX);
        w = mqtt_write_suback(out, sizeof out, m->packet_id, m->items, m->n_items);
        FZ_CHECK(w == k);   // fixed sizes: the same bytes
        FZ_CHECK(memcmp(out, p, (size_t)k) == 0);
        break;
    case MQTT_CONNACK:
        FZ_CHECK(from == MQTT_FROM_SERVER);
        w = mqtt_write_connack(out, sizeof out, m->session_present, m->return_code);
        FZ_CHECK(w == 4 && memcmp(out, p, 4) == 0);
        break;
    default:
        break;
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 1 || size > (1 << 16)) return 0;
    int mode = data[0] & 3;
    uint8_t *p = fz_dup(data + 1, size - 1);
    int n = (int)size - 1;
    if (mode < 2) {
        // A stream of packets, as a reader walks it.
        int from = mode == 0 ? MQTT_FROM_SERVER : MQTT_FROM_CLIENT;
        int max = 1 + (data[0] >> 2) * 512;
        int at = 0;
        while (at < n) {
            MqttPacket m;
            int k = mqtt_parse(p + at, n - at, max, from, &m);
            if (k <= 0) break;
            FZ_CHECK(k <= max);
            check_packet(p + at, n - at, k, &m, from);
            at += k;
        }
    } else {
        // Topic matching: a filter and a name split at the first NUL.
        const uint8_t *z = memchr(p, 0, (size_t)n);
        int fn = z ? (int)(z - p) : n / 2, tn = n - fn - (z ? 1 : 0);
        const char *f = (const char *)p, *t = (const char *)p + fn + (z ? 1 : 0);
        if (mqtt_topic_ok(f, fn, 1) && mqtt_topic_ok(t, tn, 0)) {
            int r = mqtt_topic_match(f, fn, t, tn);
            // A filter with no wildcard matches exactly itself.
            if (!memchr(f, '+', (size_t)fn) && !memchr(f, '#', (size_t)fn))
                FZ_CHECK(r == (fn == tn && memcmp(f, t, (size_t)fn) == 0));
            if (fn == 1 && f[0] == '#') FZ_CHECK(r == (t[0] != '$'));
        }
    }
    free(p);
    return 0;
}
