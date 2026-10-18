// mqtt.h - the MQTT 3.1.1 subset of a subscribe-only push stream, both sides.
//
// A server (a broker such as AWS IoT Core, or a hub inside a server process)
// pushes PUBLISH at QoS 0 to clients that only ever CONNECT, SUBSCRIBE,
// UNSUBSCRIBE, PINGREQ and DISCONNECT. This file is the one owner of those
// packets' bytes: the writers each side sends and the readers each side checks
// what arrives with. A client and a server built on it cannot disagree about a
// byte, because there is no second copy of the rules.
//
// Pure: no socket, clock or allocation; memcpy and memset only. Every length is
// an int bounded by the caller's `cap` or `max`, so nothing here can be walked
// past a buffer by a length on the wire.
//
// THE SUBSET. MQTT 3.1.1 (protocol level 4) only. A CONNECT with a will is
// refused (nothing here needs one, and a broker that took it would publish on a
// client's behalf). QoS 2 is refused in both directions. A PUBLISH at QoS 1 is
// read (a broker may upgrade nothing, but a misconfigured one could send it), so
// a client can answer it with PUBACK. Topic names, filters and client ids are
// printable ASCII (0x21 to 0x7E) here, stricter than the specification's UTF-8:
// every topic this stream uses is ASCII, and anything else is a client that is
// not ours.
#ifndef SHARED_MQTT_H
#define SHARED_MQTT_H

#include <stdint.h>

#define MQTT_CONNECT     1
#define MQTT_CONNACK     2
#define MQTT_PUBLISH     3
#define MQTT_PUBACK      4
#define MQTT_SUBSCRIBE   8
#define MQTT_SUBACK      9
#define MQTT_UNSUBSCRIBE 10
#define MQTT_UNSUBACK    11
#define MQTT_PINGREQ     12
#define MQTT_PINGRESP    13
#define MQTT_DISCONNECT  14

// CONNACK return codes (3.2.2.3).
#define MQTT_RC_ACCEPTED       0
#define MQTT_RC_BAD_PROTOCOL   1
#define MQTT_RC_ID_REJECTED    2
#define MQTT_RC_UNAVAILABLE    3
#define MQTT_RC_BAD_CREDENTIALS 4
#define MQTT_RC_NOT_AUTHORIZED 5

#define MQTT_SUBACK_FAILURE 0x80

// Which side wrote the bytes being read: a client reads what a server sent
// (MQTT_FROM_SERVER) and a server what a client sent (MQTT_FROM_CLIENT). A
// packet of a type that side never sends is malformed.
#define MQTT_FROM_SERVER 0
#define MQTT_FROM_CLIENT 1

#define MQTT_TOPIC_MAX   256   // the longest topic name or filter either side takes
#define MQTT_ID_MAX      128   // the longest client id
#define MQTT_ITEMS_MAX   16    // filters in one SUBSCRIBE or UNSUBSCRIBE, codes in one SUBACK

// One packet, read. Every pointer is into the buffer mqtt_parse was given.
typedef struct {
    uint8_t  type;             // MQTT_*
    // CONNECT
    uint8_t  clean_session;
    uint16_t keepalive_s;
    const uint8_t *client_id;  uint16_t client_id_len;
    uint8_t  has_username;     const uint8_t *username; uint16_t username_len;
    uint8_t  has_password;     const uint8_t *password; uint16_t password_len;
    // CONNACK
    uint8_t  session_present, return_code;
    // PUBLISH
    uint8_t  qos, dup, retain;
    const uint8_t *topic;      uint16_t topic_len;
    const uint8_t *payload;    int payload_len;
    // PUBLISH at QoS 1, PUBACK, SUBSCRIBE, SUBACK, UNSUBSCRIBE, UNSUBACK
    uint16_t packet_id;
    // SUBSCRIBE and UNSUBSCRIBE: the filters, walked with mqtt_next_filter;
    // SUBACK: n_items return codes at `items`.
    const uint8_t *items;      int items_len;
    uint8_t  n_items;
} MqttPacket;

// Reads one packet from buf[0..n). `max` is the largest whole packet the reader
// takes (fixed header included). Returns:
//   > 0  that many bytes were one whole packet, *p filled
//   0    not a whole packet yet (and nothing read so far is wrong)
//   -1   malformed, or longer than `max` (known as soon as the length is read,
//        before its bytes arrive), or a type the side `from` never sends (known
//        from the first byte)
// Checked: the remaining length (at most four bytes, and no more than it
// needs: one length has one encoding), each type's fixed flags
// (SUBSCRIBE and UNSUBSCRIBE 0010, the rest 0000 but PUBLISH's), exact sizes of
// the fixed-size packets, CONNECT's protocol name "MQTT" and level 4 with the
// reserved flag clear, no will, a password only with a username, a client id of
// 1 to MQTT_ID_MAX printable bytes; PUBLISH QoS 0 or 1 (DUP clear at QoS 0), a
// topic name with no wildcard, packet ids not 0; SUBSCRIBE and UNSUBSCRIBE 1 to
// MQTT_ITEMS_MAX valid filters (requested QoS 0 to 2, its reserved bits clear);
// SUBACK 1 to MQTT_ITEMS_MAX codes of 0, 1, 2 or 0x80; CONNACK's reserved bits
// clear and a return code of 0 to 5; and that every packet's fields fill its
// remaining length exactly.
int mqtt_parse(const uint8_t *buf, int n, int max, int from, MqttPacket *p);

// The next filter of a SUBSCRIBE or UNSUBSCRIBE that mqtt_parse accepted:
// 1 with the filter (and for SUBSCRIBE its requested QoS; 0 for UNSUBSCRIBE),
// 0 after the last. *pos starts at 0.
int mqtt_next_filter(const MqttPacket *p, int *pos, const uint8_t **filter, uint16_t *len, uint8_t *qos);

// ---- writers -----------------------------------------------------------------
// Each returns the packet's length, or -1 when it does not fit `cap` or an
// argument is out of range (a length, a QoS, a packet id of 0, a topic that is
// not valid for its use). Nothing is written past `cap` either way.

typedef struct {
    const char *p;
    int         len;
    uint8_t     qos;           // SUBSCRIBE's requested QoS (0 or 1); ignored by UNSUBSCRIBE
} MqttTopic;

// CONNECT, clean session. `username` and `password` NULL for none (a password
// needs a username: 3.1.2.9).
int mqtt_write_connect(uint8_t *out, int cap, const char *client_id, int id_len, uint16_t keepalive_s,
                       const char *username, int username_len, const uint8_t *password, int password_len);
int mqtt_write_subscribe(uint8_t *out, int cap, uint16_t packet_id, const MqttTopic *topics, int n);
int mqtt_write_unsubscribe(uint8_t *out, int cap, uint16_t packet_id, const MqttTopic *topics, int n);
int mqtt_write_puback(uint8_t *out, int cap, uint16_t packet_id);
// PINGREQ, PINGRESP or DISCONNECT: the packets with no body.
int mqtt_write_bare(uint8_t *out, int cap, int type);
int mqtt_write_connack(uint8_t *out, int cap, int session_present, int return_code);
int mqtt_write_suback(uint8_t *out, int cap, uint16_t packet_id, const uint8_t *codes, int n);
int mqtt_write_unsuback(uint8_t *out, int cap, uint16_t packet_id);
// PUBLISH at QoS 0 (no packet id), never DUP or RETAIN.
int mqtt_write_publish(uint8_t *out, int cap, const char *topic, int topic_len, const uint8_t *payload, int payload_len);
// The bytes mqtt_write_publish writes for these lengths (-1: too long).
int mqtt_publish_len(int topic_len, int payload_len);

// ---- topics ------------------------------------------------------------------

// 1 when t[0..n) is a valid topic name (filter 0) or topic filter (filter 1):
// 1 to MQTT_TOPIC_MAX printable bytes; a name holds no '+' or '#'; in a filter
// '+' fills a whole level and '#' is the whole last level.
int mqtt_topic_ok(const char *t, int n, int filter);
// 1 when the valid filter f matches the valid topic name t (4.7): '+' one
// level, '#' this level and every one below (and the parent: "a/#" matches
// "a"), and a filter starting with a wildcard never matches a topic starting
// with '$'.
int mqtt_topic_match(const char *f, int fn, const char *t, int tn);

#endif
