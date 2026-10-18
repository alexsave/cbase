// unit_mqtt.c - the MQTT 3.1.1 subset both sides of the push stream speak
// (shared/c/mqtt/mqtt.h): every packet against hand-written bytes (MQTT
// 3.1.1's own figures where it has one), every malformed shape refused, every
// prefix of a good packet incomplete, from exactly-sized heap copies so an
// over-read is an ASan report, and topic filter matching (4.7).
#include "t.h"

#include "../../../shared/c/mqtt/mqtt.h"

#include <stdlib.h>

// mqtt_parse on an exact-size heap copy of b[0..n).
static int parse(const uint8_t *b, int n, int max, int from, MqttPacket *p) {
    uint8_t *c = malloc(n ? (size_t)n : 1);
    if (!c) return -9;
    memcpy(c, b, (size_t)n);
    int k = mqtt_parse(c, n, max, from, p);
    // The spans must point into the copy; re-point them at b before it is freed.
    if (k > 0) {
#define REPOINT(f) if (p->f) p->f = b + (p->f - c)
        REPOINT(client_id); REPOINT(username); REPOINT(password); REPOINT(topic); REPOINT(payload); REPOINT(items);
#undef REPOINT
    }
    free(c);
    return k;
}

// Every strict prefix of a whole packet is "not yet", never a packet or an error.
static void prefixes_wait(const uint8_t *b, int n, int from) {
    MqttPacket p;
    for (int k = 0; k < n; k++) T_EQ_INT(parse(b, k, 4096, from, &p), 0);
}

static void connect_written_and_read(void) {
    uint8_t out[512];
    static const uint8_t pass[] = { 'j', 'w', 't', 0x00, 0xFF };
    int n = mqtt_write_connect(out, sizeof out, "u1-a", 4, 60, "x", 1, pass, sizeof pass);
    static const uint8_t want[] = {
        0x10, 26,                                  // CONNECT, remaining 26
        0, 4, 'M', 'Q', 'T', 'T', 4, 0xC2, 0, 60,  // name, level 4, user+pass+clean, keep-alive 60
        0, 4, 'u', '1', '-', 'a',                  // client id
        0, 1, 'x',                                 // username
        0, 5, 'j', 'w', 't', 0x00, 0xFF,           // password (binary)
    };
    T_EQ_MEM(out, n, want, sizeof want);
    MqttPacket p;
    T_EQ_INT(parse(out, n, 4096, MQTT_FROM_CLIENT, &p), n);
    T_EQ_INT(p.type, MQTT_CONNECT);
    T_EQ_INT(p.clean_session, 1);
    T_EQ_INT(p.keepalive_s, 60);
    T_EQ_MEM(p.client_id, p.client_id_len, "u1-a", 4);
    T_EQ_INT(p.has_username, 1);
    T_EQ_MEM(p.username, p.username_len, "x", 1);
    T_EQ_INT(p.has_password, 1);
    T_EQ_MEM(p.password, p.password_len, pass, sizeof pass);
    prefixes_wait(out, n, MQTT_FROM_CLIENT);
    // A client never reads a CONNECT.
    T_EQ_INT(parse(out, n, 4096, MQTT_FROM_SERVER, &p), -1);

    // No credentials at all.
    n = mqtt_write_connect(out, sizeof out, "c", 1, 0, NULL, 0, NULL, 0);
    static const uint8_t bare[] = { 0x10, 13, 0, 4, 'M', 'Q', 'T', 'T', 4, 0x02, 0, 0, 0, 1, 'c' };
    T_EQ_MEM(out, n, bare, sizeof bare);
    T_EQ_INT(parse(out, n, 4096, MQTT_FROM_CLIENT, &p), n);
    T_EQ_INT(p.has_username, 0);
    T_EQ_INT(p.has_password, 0);

    // The writer refuses what the reader would.
    T_EQ_INT(mqtt_write_connect(out, sizeof out, "", 0, 60, NULL, 0, NULL, 0), -1);         // empty id
    T_EQ_INT(mqtt_write_connect(out, sizeof out, "a b", 3, 60, NULL, 0, NULL, 0), -1);      // a space
    T_EQ_INT(mqtt_write_connect(out, sizeof out, "c", 1, 60, NULL, 0, pass, 5), -1);         // password, no user
    T_EQ_INT(mqtt_write_connect(out, 14, "c", 1, 0, NULL, 0, NULL, 0), -1);                  // one byte short
    T_EQ_INT(mqtt_write_connect(out, 15, "c", 1, 0, NULL, 0, NULL, 0), 15);                  // exactly enough
    char longid[MQTT_ID_MAX + 1];
    memset(longid, 'a', sizeof longid);
    T_EQ_INT(mqtt_write_connect(out, sizeof out, longid, MQTT_ID_MAX + 1, 60, NULL, 0, NULL, 0), -1);
    T_CHECK(mqtt_write_connect(out, sizeof out, longid, MQTT_ID_MAX, 60, NULL, 0, NULL, 0) > 0);
}

static void connect_refused_shapes(void) {
    MqttPacket p;
    // Each is the bare CONNECT above with one thing wrong.
    uint8_t b[15] = { 0x10, 13, 0, 4, 'M', 'Q', 'T', 'T', 4, 0x02, 0, 0, 0, 1, 'c' };
    T_EQ_INT(parse(b, 15, 4096, MQTT_FROM_CLIENT, &p), 15);
    struct { int at; uint8_t v; const char *why; } bad[] = {
        { 0, 0x11, "flags set" },        { 5, 'X', "protocol name" },   { 8, 3, "level 3 (MQTT 3.1)" },
        { 8, 5, "level 5" },             { 9, 0x03, "reserved bit" },   { 9, 0x06, "a will" },
        { 9, 0x0A, "will QoS, no will" }, { 9, 0x22, "will retain" },   { 9, 0x42, "password, no user" },
        { 9, 0x82, "user flag, no user" }, { 13, 0, "empty client id" }, { 14, ' ', "id not printable" },
        { 14, 0x80, "id not ASCII" },
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        uint8_t c[15];
        memcpy(c, b, sizeof c);
        c[bad[i].at] = bad[i].v;
        int got = parse(c, bad[i].at == 13 ? 15 : 15, 4096, MQTT_FROM_CLIENT, &p);
        if (got != -1) fprintf(stderr, "     %s: got %d\n", bad[i].why, got);
        T_EQ_INT(got, -1);
    }
    // Trailing bytes inside the remaining length.
    uint8_t t[16] = { 0x10, 14, 0, 4, 'M', 'Q', 'T', 'T', 4, 0x02, 0, 0, 0, 1, 'c', 'x' };
    T_EQ_INT(parse(t, 16, 4096, MQTT_FROM_CLIENT, &p), -1);
    // A client id longer than the packet.
    uint8_t s[15] = { 0x10, 13, 0, 4, 'M', 'Q', 'T', 'T', 4, 0x02, 0, 0, 0, 2, 'c' };
    T_EQ_INT(parse(s, 15, 4096, MQTT_FROM_CLIENT, &p), -1);
    // A password with no username flag, the password itself present (3.1.2.9).
    uint8_t pw[18] = { 0x10, 16, 0, 4, 'M', 'Q', 'T', 'T', 4, 0x42, 0, 0, 0, 1, 'c', 0, 1, 'p' };
    T_EQ_INT(parse(pw, 18, 4096, MQTT_FROM_CLIENT, &p), -1);
    pw[9] = 0x82;   // with the username flag the same bytes are a username: read
    T_EQ_INT(parse(pw, 18, 4096, MQTT_FROM_CLIENT, &p), 18);
    T_EQ_MEM(p.username, p.username_len, "p", 1);
    // Shorter than the variable header.
    uint8_t h[11] = { 0x10, 9, 0, 4, 'M', 'Q', 'T', 'T', 4, 0x02, 0 };
    T_EQ_INT(parse(h, 11, 4096, MQTT_FROM_CLIENT, &p), -1);
}

static void connack_suback_unsuback_pingresp(void) {
    uint8_t out[64];
    MqttPacket p;
    int n = mqtt_write_connack(out, sizeof out, 0, 0);
    static const uint8_t ca[] = { 0x20, 2, 0, 0 };
    T_EQ_MEM(out, n, ca, sizeof ca);
    T_EQ_INT(parse(out, n, 64, MQTT_FROM_SERVER, &p), 4);
    T_EQ_INT(p.type, MQTT_CONNACK);
    T_EQ_INT(p.return_code, 0);
    T_EQ_INT(parse(out, n, 64, MQTT_FROM_CLIENT, &p), -1);   // a server never reads one
    prefixes_wait(out, n, MQTT_FROM_SERVER);
    n = mqtt_write_connack(out, sizeof out, 1, MQTT_RC_NOT_AUTHORIZED);
    T_EQ_INT(parse(out, n, 64, MQTT_FROM_SERVER, &p), 4);
    T_EQ_INT(p.session_present, 1);
    T_EQ_INT(p.return_code, 5);
    T_EQ_INT(mqtt_write_connack(out, sizeof out, 0, 6), -1);
    T_EQ_INT(mqtt_write_connack(out, sizeof out, 2, 0), -1);
    T_EQ_INT(mqtt_write_connack(out, 3, 0, 0), -1);
    uint8_t rc6[] = { 0x20, 2, 0, 6 }, sp2[] = { 0x20, 2, 2, 0 }, long3[] = { 0x20, 3, 0, 0, 0 }, fl[] = { 0x21, 2, 0, 0 };
    T_EQ_INT(parse(rc6, 4, 64, MQTT_FROM_SERVER, &p), -1);
    T_EQ_INT(parse(sp2, 4, 64, MQTT_FROM_SERVER, &p), -1);
    T_EQ_INT(parse(long3, 5, 64, MQTT_FROM_SERVER, &p), -1);
    T_EQ_INT(parse(fl, 4, 64, MQTT_FROM_SERVER, &p), -1);

    static const uint8_t codes[] = { 0, 1, MQTT_SUBACK_FAILURE };
    n = mqtt_write_suback(out, sizeof out, 0x1234, codes, 3);
    static const uint8_t sa[] = { 0x90, 5, 0x12, 0x34, 0, 1, 0x80 };
    T_EQ_MEM(out, n, sa, sizeof sa);
    T_EQ_INT(parse(out, n, 64, MQTT_FROM_SERVER, &p), n);
    T_EQ_INT(p.packet_id, 0x1234);
    T_EQ_INT(p.n_items, 3);
    T_EQ_MEM(p.items, p.n_items, codes, 3);
    prefixes_wait(out, n, MQTT_FROM_SERVER);
    uint8_t bad_code[] = { 0x90, 3, 0, 1, 3 }, no_code[] = { 0x90, 2, 0, 1 }, id0[] = { 0x90, 3, 0, 0, 0 };
    T_EQ_INT(parse(bad_code, 5, 64, MQTT_FROM_SERVER, &p), -1);
    T_EQ_INT(parse(no_code, 4, 64, MQTT_FROM_SERVER, &p), -1);
    T_EQ_INT(parse(id0, 5, 64, MQTT_FROM_SERVER, &p), -1);
    static const uint8_t three[] = { 3 };
    T_EQ_INT(mqtt_write_suback(out, sizeof out, 1, three, 1), -1);
    T_EQ_INT(mqtt_write_suback(out, sizeof out, 0, codes, 1), -1);
    uint8_t many[MQTT_ITEMS_MAX + 1] = { 0 };
    T_EQ_INT(mqtt_write_suback(out, sizeof out, 1, many, MQTT_ITEMS_MAX + 1), -1);
    n = mqtt_write_suback(out, sizeof out, 1, many, MQTT_ITEMS_MAX);
    T_EQ_INT(parse(out, n, 64, MQTT_FROM_SERVER, &p), n);
    uint8_t seventeen[2 + 2 + MQTT_ITEMS_MAX + 1] = { 0x90, 2 + MQTT_ITEMS_MAX + 1, 0, 1 };
    T_EQ_INT(parse(seventeen, sizeof seventeen, 64, MQTT_FROM_SERVER, &p), -1);

    n = mqtt_write_unsuback(out, sizeof out, 7);
    static const uint8_t ua[] = { 0xB0, 2, 0, 7 };
    T_EQ_MEM(out, n, ua, sizeof ua);
    T_EQ_INT(parse(out, n, 64, MQTT_FROM_SERVER, &p), 4);
    T_EQ_INT(p.packet_id, 7);
    T_EQ_INT(parse(out, n, 64, MQTT_FROM_CLIENT, &p), -1);
    T_EQ_INT(mqtt_write_unsuback(out, sizeof out, 0), -1);

    n = mqtt_write_bare(out, sizeof out, MQTT_PINGRESP);
    static const uint8_t pr[] = { 0xD0, 0 };
    T_EQ_MEM(out, n, pr, 2);
    T_EQ_INT(parse(out, n, 64, MQTT_FROM_SERVER, &p), 2);
    T_EQ_INT(p.type, MQTT_PINGRESP);
    T_EQ_INT(parse(out, n, 64, MQTT_FROM_CLIENT, &p), -1);
    uint8_t pr1[] = { 0xD0, 1, 0 };
    T_EQ_INT(parse(pr1, 3, 64, MQTT_FROM_SERVER, &p), -1);
    T_EQ_INT(mqtt_write_bare(out, sizeof out, MQTT_CONNACK), -1);
    T_EQ_INT(mqtt_write_bare(out, 1, MQTT_PINGREQ), -1);
}

static void client_packets(void) {
    uint8_t out[512];
    MqttPacket p;
    MqttTopic t[2] = { { "u/a/v", 5, 0 }, { "s/g1", 4, 1 } };
    int n = mqtt_write_subscribe(out, sizeof out, 10, t, 2);
    static const uint8_t sub[] = { 0x82, 17, 0, 10, 0, 5, 'u', '/', 'a', '/', 'v', 0, 0, 4, 's', '/', 'g', '1', 1 };
    T_EQ_MEM(out, n, sub, sizeof sub);
    T_EQ_INT(parse(out, n, 4096, MQTT_FROM_CLIENT, &p), n);
    T_EQ_INT(p.type, MQTT_SUBSCRIBE);
    T_EQ_INT(p.packet_id, 10);
    T_EQ_INT(p.n_items, 2);
    int pos = 0;
    const uint8_t *f;
    uint16_t fl;
    uint8_t q;
    T_EQ_INT(mqtt_next_filter(&p, &pos, &f, &fl, &q), 1);
    T_EQ_MEM(f, fl, "u/a/v", 5);
    T_EQ_INT(q, 0);
    T_EQ_INT(mqtt_next_filter(&p, &pos, &f, &fl, &q), 1);
    T_EQ_MEM(f, fl, "s/g1", 4);
    T_EQ_INT(q, 1);
    T_EQ_INT(mqtt_next_filter(&p, &pos, &f, &fl, &q), 0);
    prefixes_wait(out, n, MQTT_FROM_CLIENT);
    T_EQ_INT(parse(out, n, 4096, MQTT_FROM_SERVER, &p), -1);

    n = mqtt_write_unsubscribe(out, sizeof out, 11, t, 2);
    static const uint8_t uns[] = { 0xA2, 15, 0, 11, 0, 5, 'u', '/', 'a', '/', 'v', 0, 4, 's', '/', 'g', '1' };
    T_EQ_MEM(out, n, uns, sizeof uns);
    T_EQ_INT(parse(out, n, 4096, MQTT_FROM_CLIENT, &p), n);
    pos = 0;
    T_EQ_INT(mqtt_next_filter(&p, &pos, &f, &fl, &q), 1);
    T_EQ_INT(mqtt_next_filter(&p, &pos, &f, &fl, &q), 1);
    T_EQ_MEM(f, fl, "s/g1", 4);
    T_EQ_INT(q, 0);
    T_EQ_INT(mqtt_next_filter(&p, &pos, &f, &fl, &q), 0);

    // Refused shapes.
    uint8_t c[sizeof sub];
    memcpy(c, sub, sizeof c);
    c[0] = 0x80;   // SUBSCRIBE's flags must be 0010
    T_EQ_INT(parse(c, sizeof c, 4096, MQTT_FROM_CLIENT, &p), -1);
    memcpy(c, sub, sizeof c);
    c[18] = 3;     // QoS 3
    T_EQ_INT(parse(c, sizeof c, 4096, MQTT_FROM_CLIENT, &p), -1);
    c[18] = 0x41;  // reserved bits of the QoS byte
    T_EQ_INT(parse(c, sizeof c, 4096, MQTT_FROM_CLIENT, &p), -1);
    memcpy(c, sub, sizeof c);
    c[2] = c[3] = 0;   // packet id 0
    T_EQ_INT(parse(c, sizeof c, 4096, MQTT_FROM_CLIENT, &p), -1);
    memcpy(c, sub, sizeof c);
    c[8] = '#';        // "u/#/v": '#' not last
    T_EQ_INT(parse(c, sizeof c, 4096, MQTT_FROM_CLIENT, &p), -1);
    memcpy(c, sub, sizeof c);
    c[7] = '+';        // "u+a/v": '+' not a whole level
    T_EQ_INT(parse(c, sizeof c, 4096, MQTT_FROM_CLIENT, &p), -1);
    uint8_t nofilter[] = { 0x82, 2, 0, 1 };
    T_EQ_INT(parse(nofilter, 4, 4096, MQTT_FROM_CLIENT, &p), -1);
    uint8_t noqos[] = { 0x82, 5, 0, 1, 0, 1, 'a' };
    T_EQ_INT(parse(noqos, 7, 4096, MQTT_FROM_CLIENT, &p), -1);
    uint8_t emptyf[] = { 0x82, 5, 0, 1, 0, 0, 0 };
    T_EQ_INT(parse(emptyf, 7, 4096, MQTT_FROM_CLIENT, &p), -1);
    // Seventeen filters: one past MQTT_ITEMS_MAX.
    MqttTopic many[MQTT_ITEMS_MAX + 1];
    for (int i = 0; i <= MQTT_ITEMS_MAX; i++) many[i] = (MqttTopic){ "s/a", 3, 0 };
    T_EQ_INT(mqtt_write_subscribe(out, sizeof out, 1, many, MQTT_ITEMS_MAX + 1), -1);
    n = mqtt_write_subscribe(out, sizeof out, 1, many, MQTT_ITEMS_MAX);
    T_EQ_INT(parse(out, n, 4096, MQTT_FROM_CLIENT, &p), n);
    uint8_t big[4 + 6 * (MQTT_ITEMS_MAX + 1)];
    memcpy(big, out, (size_t)n);
    big[1] = (uint8_t)(big[1] + 6);
    memcpy(big + n, out + 4, 6);
    T_EQ_INT(parse(big, n + 6, 4096, MQTT_FROM_CLIENT, &p), -1);
    // The writers refuse what the readers would.
    MqttTopic bad[] = { { "a/#/b", 5, 0 } }, q2[] = { { "a", 1, 2 } }, empty[] = { { "", 0, 0 } };
    T_EQ_INT(mqtt_write_subscribe(out, sizeof out, 1, bad, 1), -1);
    T_EQ_INT(mqtt_write_subscribe(out, sizeof out, 1, q2, 1), -1);
    T_EQ_INT(mqtt_write_subscribe(out, sizeof out, 1, empty, 1), -1);
    T_EQ_INT(mqtt_write_subscribe(out, sizeof out, 0, t, 1), -1);
    T_EQ_INT(mqtt_write_subscribe(out, sizeof out, 1, t, 0), -1);
    T_EQ_INT(mqtt_write_subscribe(out, (int)sizeof sub - 1, 10, t, 2), -1);
    T_EQ_INT(mqtt_write_subscribe(out, (int)sizeof sub, 10, t, 2), (int)sizeof sub);

    n = mqtt_write_bare(out, sizeof out, MQTT_PINGREQ);
    static const uint8_t pq[] = { 0xC0, 0 };
    T_EQ_MEM(out, n, pq, 2);
    T_EQ_INT(parse(out, n, 64, MQTT_FROM_CLIENT, &p), 2);
    T_EQ_INT(parse(out, n, 64, MQTT_FROM_SERVER, &p), -1);
    n = mqtt_write_bare(out, sizeof out, MQTT_DISCONNECT);
    static const uint8_t dc[] = { 0xE0, 0 };
    T_EQ_MEM(out, n, dc, 2);
    T_EQ_INT(parse(out, n, 64, MQTT_FROM_CLIENT, &p), 2);
    T_EQ_INT(p.type, MQTT_DISCONNECT);
    n = mqtt_write_puback(out, sizeof out, 0x0102);
    static const uint8_t pa[] = { 0x40, 2, 1, 2 };
    T_EQ_MEM(out, n, pa, 4);
    T_EQ_INT(parse(out, n, 64, MQTT_FROM_CLIENT, &p), 4);
    T_EQ_INT(parse(out, n, 64, MQTT_FROM_SERVER, &p), 4);
    T_EQ_INT(p.packet_id, 0x0102);
    T_EQ_INT(mqtt_write_puback(out, sizeof out, 0), -1);
}

static void publish_both_ways(void) {
    uint8_t out[40000];
    MqttPacket p;
    static const uint8_t body[] = { 1, 0x03, 0xFF, 0 };
    int n = mqtt_write_publish(out, sizeof out, "u/a/v", 5, body, sizeof body);
    static const uint8_t pub[] = { 0x30, 11, 0, 5, 'u', '/', 'a', '/', 'v', 1, 0x03, 0xFF, 0 };
    T_EQ_MEM(out, n, pub, sizeof pub);
    T_EQ_INT(mqtt_publish_len(5, sizeof body), n);
    T_EQ_INT(parse(out, n, 4096, MQTT_FROM_SERVER, &p), n);
    T_EQ_INT(p.type, MQTT_PUBLISH);
    T_EQ_INT(p.qos, 0);
    T_EQ_MEM(p.topic, p.topic_len, "u/a/v", 5);
    T_EQ_MEM(p.payload, p.payload_len, body, sizeof body);
    prefixes_wait(out, n, MQTT_FROM_SERVER);
    // A client's PUBLISH reads too: refusing it is the server's decision, not the codec's.
    T_EQ_INT(parse(out, n, 4096, MQTT_FROM_CLIENT, &p), n);
    // An empty payload.
    n = mqtt_write_publish(out, sizeof out, "s/g", 3, NULL, 0);
    T_EQ_INT(parse(out, n, 4096, MQTT_FROM_SERVER, &p), n);
    T_EQ_INT(p.payload_len, 0);
    // A view-sized payload: a two-byte length, then a three-byte one.
    static uint8_t big[20000];
    for (size_t i = 0; i < sizeof big; i++) big[i] = (uint8_t)i;
    n = mqtt_write_publish(out, sizeof out, "u/a/c/g1", 8, big, 100);
    T_EQ_INT(n, 2 + 2 + 8 + 100);
    T_EQ_INT(mqtt_publish_len(8, 200), 3 + 2 + 8 + 200);
    n = mqtt_write_publish(out, sizeof out, "u/a/c/g1", 8, big, sizeof big);
    T_EQ_INT(n, mqtt_publish_len(8, sizeof big));
    T_EQ_INT(n, 4 + 2 + 8 + (int)sizeof big);
    T_EQ_INT(parse(out, n, 40000, MQTT_FROM_SERVER, &p), n);
    T_EQ_MEM(p.payload, p.payload_len, big, sizeof big);
    // Past the reader's limit: refused from the length alone, before the body.
    T_EQ_INT(parse(out, 5, 4096, MQTT_FROM_SERVER, &p), -1);
    T_EQ_INT(parse(out, n, n - 1, MQTT_FROM_SERVER, &p), -1);
    T_EQ_INT(parse(out, n, n, MQTT_FROM_SERVER, &p), n);
    T_EQ_INT(mqtt_write_publish(out, n - 1, "u/a/c/g1", 8, big, sizeof big), -1);

    // QoS 1: a packet id after the topic.
    uint8_t q1[] = { 0x32, 7, 0, 1, 't', 0x00, 0x09, 'x', 'y' };
    T_EQ_INT(parse(q1, sizeof q1, 64, MQTT_FROM_SERVER, &p), (int)sizeof q1);
    T_EQ_INT(p.qos, 1);
    T_EQ_INT(p.packet_id, 9);
    T_EQ_MEM(p.payload, p.payload_len, "xy", 2);
    uint8_t c[sizeof q1];
    memcpy(c, q1, sizeof c);
    c[5] = c[6] = 0;   // packet id 0
    T_EQ_INT(parse(c, sizeof c, 64, MQTT_FROM_SERVER, &p), -1);
    memcpy(c, q1, sizeof c);
    c[0] = 0x34;       // QoS 2
    T_EQ_INT(parse(c, sizeof c, 64, MQTT_FROM_SERVER, &p), -1);
    c[0] = 0x36;       // QoS 3
    T_EQ_INT(parse(c, sizeof c, 64, MQTT_FROM_SERVER, &p), -1);
    uint8_t dup0[] = { 0x38, 3, 0, 1, 't' };   // DUP at QoS 0
    T_EQ_INT(parse(dup0, 5, 64, MQTT_FROM_SERVER, &p), -1);
    uint8_t retain[] = { 0x31, 3, 0, 1, 't' };
    T_EQ_INT(parse(retain, 5, 64, MQTT_FROM_SERVER, &p), 5);
    T_EQ_INT(p.retain, 1);
    uint8_t wild[] = { 0x30, 3, 0, 1, '#' }, notopic[] = { 0x30, 2, 0, 0 }, longtopic[] = { 0x30, 3, 0, 2, 't' };
    T_EQ_INT(parse(wild, 5, 64, MQTT_FROM_SERVER, &p), -1);
    T_EQ_INT(parse(notopic, 4, 64, MQTT_FROM_SERVER, &p), -1);
    T_EQ_INT(parse(longtopic, 5, 64, MQTT_FROM_SERVER, &p), -1);
    T_EQ_INT(mqtt_write_publish(out, sizeof out, "a/+", 3, body, 1), -1);
    T_EQ_INT(mqtt_write_publish(out, sizeof out, "a", 1, NULL, 1), -1);
    static uint8_t edge_out[20000];
    // mqtt_publish_len agrees with the writer at every length-byte boundary
    // (a remaining length of 127/128 and 16383/16384).
    int edges[] = { 127, 128, 16383, 16384 };
    for (int e = 0; e < 4; e++) {
        int plen = edges[e] - 2 - 8;
        T_EQ_INT(mqtt_publish_len(8, plen), mqtt_write_publish(edge_out, sizeof edge_out, "u/a/c/g1", 8, big, plen));
    }
}

static void lengths_and_types(void) {
    MqttPacket p;
    // A remaining length of five bytes is malformed; of four is read.
    uint8_t five[] = { 0x30, 0x80, 0x80, 0x80, 0x80, 0x01 };
    T_EQ_INT(parse(five, 6, 1 << 30, MQTT_FROM_SERVER, &p), -1);
    uint8_t four[] = { 0x30, 0xFF, 0xFF, 0xFF, 0x7F };
    T_EQ_INT(parse(four, 5, 1 << 30, MQTT_FROM_SERVER, &p), 0);   // 256 MB announced: not here yet
    T_EQ_INT(parse(four, 5, 1 << 20, MQTT_FROM_SERVER, &p), -1);  // and past this reader's limit
    // A length in more bytes than it needs: 3 written as 0x83 0x00.
    uint8_t padded[] = { 0x90, 0x83, 0x00, 0x00, 0x01, 0x00 };
    T_EQ_INT(parse(padded, 6, 64, MQTT_FROM_SERVER, &p), -1);
    uint8_t exact[] = { 0x90, 0x03, 0x00, 0x01, 0x00 };
    T_EQ_INT(parse(exact, 5, 64, MQTT_FROM_SERVER, &p), 5);
    // A limit below the header itself refuses everything.
    uint8_t ping[] = { 0xC0, 0 };
    T_EQ_INT(parse(ping, 2, 1, MQTT_FROM_CLIENT, &p), -1);
    T_EQ_INT(parse(ping, 2, 2, MQTT_FROM_CLIENT, &p), 2);
    // Types outside the subset, from either side.
    for (int type = 0; type < 16; type++) {
        uint8_t b[] = { (uint8_t)(type << 4), 0 };
        int s = parse(b, 2, 64, MQTT_FROM_SERVER, &p), c = parse(b, 2, 64, MQTT_FROM_CLIENT, &p);
        T_EQ_INT(s, type == MQTT_PINGRESP ? 2 : -1);
        T_EQ_INT(c, type == MQTT_PINGREQ || type == MQTT_DISCONNECT ? 2 : -1);
    }
    // A type the side never sends is refused from its first byte alone, before
    // any length or body: a hub never buffers junk waiting for it to end.
    uint8_t first[] = { 0x20 };
    T_EQ_INT(parse(first, 1, 64, MQTT_FROM_CLIENT, &p), -1);
    T_EQ_INT(parse(first, 1, 64, MQTT_FROM_SERVER, &p), 0);
    uint8_t junk[] = { 0x01, 0x7F };   // type 0, a 127-byte "body" announced
    T_EQ_INT(parse(junk, 2, 4096, MQTT_FROM_CLIENT, &p), -1);
    // Two packets in one buffer: the first is read, its length says where the next starts.
    uint8_t two[] = { 0xD0, 0, 0x20, 2, 0, 0 };
    T_EQ_INT(parse(two, 6, 64, MQTT_FROM_SERVER, &p), 2);
    T_EQ_INT(parse(two + 2, 4, 64, MQTT_FROM_SERVER, &p), 4);
    T_EQ_INT(mqtt_parse(NULL, 0, 64, MQTT_FROM_SERVER, &p), 0);
}

static void topics(void) {
    T_EQ_INT(mqtt_topic_ok("u/a/v", 5, 0), 1);
    T_EQ_INT(mqtt_topic_ok("u/a/v", 5, 1), 1);
    T_EQ_INT(mqtt_topic_ok("u/+/v", 5, 0), 0);
    T_EQ_INT(mqtt_topic_ok("u/+/v", 5, 1), 1);
    T_EQ_INT(mqtt_topic_ok("u/#", 3, 1), 1);
    T_EQ_INT(mqtt_topic_ok("#", 1, 1), 1);
    T_EQ_INT(mqtt_topic_ok("+", 1, 1), 1);
    T_EQ_INT(mqtt_topic_ok("u/#/v", 5, 1), 0);
    T_EQ_INT(mqtt_topic_ok("u#", 2, 1), 0);
    T_EQ_INT(mqtt_topic_ok("u+", 2, 1), 0);
    T_EQ_INT(mqtt_topic_ok("+u", 2, 1), 0);
    T_EQ_INT(mqtt_topic_ok("", 0, 1), 0);
    T_EQ_INT(mqtt_topic_ok("a b", 3, 0), 0);
    T_EQ_INT(mqtt_topic_ok("a\x01", 2, 0), 0);
    T_EQ_INT(mqtt_topic_ok(NULL, 3, 0), 0);
    char long_topic[MQTT_TOPIC_MAX + 1];
    memset(long_topic, 'a', sizeof long_topic);
    T_EQ_INT(mqtt_topic_ok(long_topic, MQTT_TOPIC_MAX, 0), 1);
    T_EQ_INT(mqtt_topic_ok(long_topic, MQTT_TOPIC_MAX + 1, 0), 0);

    struct { const char *f, *t; int want; } m[] = {
        { "u/a/v", "u/a/v", 1 },   { "u/a/v", "u/a/w", 0 },     { "u/a/v", "u/a", 0 },     { "u/a", "u/a/v", 0 },
        { "u/a/#", "u/a/v", 1 },   { "u/a/#", "u/a/c/g1", 1 },  { "u/a/#", "u/a", 1 },     { "u/a/#", "u/b/v", 0 },
        { "u/a/#", "u/ab", 0 },    { "u/+/v", "u/a/v", 1 },     { "u/+/v", "u/a/c", 0 },   { "u/+/v", "u/a/b/v", 0 },
        { "u/+", "u/", 1 },        { "+/+", "/x", 1 },          { "+", "a", 1 },           { "+", "a/b", 0 },
        { "#", "a/b/c", 1 },       { "#", "$SYS/x", 0 },        { "+/x", "$SYS/x", 0 },    { "$SYS/#", "$SYS/x", 1 },
        { "s/g1", "s/g12", 0 },    { "s/g12", "s/g1", 0 },      { "a//b", "a//b", 1 },     { "a/+/b", "a//b", 1 },
        { "u/a/c/+", "u/a/c/g1", 1 }, { "u/a/c/+", "u/a/m/g1", 0 },
    };
    for (size_t i = 0; i < sizeof m / sizeof m[0]; i++) {
        int got = mqtt_topic_match(m[i].f, (int)strlen(m[i].f), m[i].t, (int)strlen(m[i].t));
        if (got != m[i].want) fprintf(stderr, "     match(%s, %s)\n", m[i].f, m[i].t);
        T_EQ_INT(got, m[i].want);
    }
}

int main(void) {
    T_RUN(connect_written_and_read);
    T_RUN(connect_refused_shapes);
    T_RUN(connack_suback_unsuback_pingresp);
    T_RUN(client_packets);
    T_RUN(publish_both_ways);
    T_RUN(lengths_and_types);
    T_RUN(topics);
    return T_DONE();
}
