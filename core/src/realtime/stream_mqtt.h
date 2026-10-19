// stream_mqtt.h - the push stream over MQTT 3.1.1 (shared/c/mqtt): who may
// connect, what they may subscribe to, and the hub's side of a connection.
//
// THE POLICY. One rule set, answered two ways: AWS IoT Core asks our custom
// authorizer at CONNECT and enforces the IoT policy it gets back
// (aws_json.h cb_iot_auth_answer, from cb_mqtt_authorize below), and the
// hub `cbase serve` runs (rt_local.c) enforces the same rules itself, so a
// client behaves the same against either:
//   CONNECT    a token that verifies, of an account that still exists, and a
//              client id "<the token's user>-<1 or more bytes>". The token is
//              the CONNECT's password, else the WebSocket URL's token=
//              parameter. The connection ends when the token expires (but no
//              sooner than CB_MQTT_MIN_LIFE_S after it opened).
//   SUBSCRIBE  a filter that starts "u/<that user>/" or "s/", nothing else
//              (realtime.h has which topics live under each).
//   PUBLISH    never: no client may publish.
// A CONNECT refused, a SUBSCRIBE outside the policy, a PUBLISH, and any
// packet that does not parse each END THE CONNECTION, with no CONNACK and no
// SUBACK refusal code: that is what IoT Core does, so the hub does it too, and
// a client handles one rule ("closed before CONNACK: refresh, then reconnect").
//
// The hub's connection runs on the request pool (rt_local.c says why), from a
// copy of cb_mqtt_conn that the loop takes back, as the nw1 stream's row is.
#ifndef CB_STREAM_MQTT_H
#define CB_STREAM_MQTT_H

// The longest access token (a JWT) a connection may present, bytes: the
// clients' own cap (foolish's netwire.h NW_ACCESS_MAX, held equal there).
#define CB_RT_TOKEN_MAX 2048

#include "ctx.h"
#include "store/store.h"

#include "../../../../shared/c/mqtt/mqtt.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CB_MQTT_PACKET_MAX 4096   // the largest packet a client may send (a CONNECT with its token)
#define CB_MQTT_SUBS       32     // filters one connection holds; one more ends it
#define CB_MQTT_MIN_LIFE_S 300    // a connection made with a token about to expire still lives this long (IoT's floor)
#define CB_MQTT_MAX_LIFE_S 86400  // IoT Core's own cap on a connection (disconnectAfterInSeconds)
#define CB_MQTT_KEEPALIVE_MAX 1200   // IoT Core's: a CONNECT asking for more is refused

// The topics the policy lets a user subscribe under: every filter must start
// with one of these (CB_MQTT_USER_PREFIX followed by the user id and '/').
#define CB_MQTT_USER_PREFIX "u/"
#define CB_MQTT_WATCH_PREFIX "s/"

// Where a reply goes: one whole packet, which the transport sends as one
// binary WebSocket message.
typedef void (*cb_rt_send_fn)(void *ud, const char *frame, size_t len);

typedef struct {
    cb_uuid user_id;
    int64_t expires_s;   // when the connection ends: the token's exp, seconds since the epoch
} cb_mqtt_grant;

// CONNECT's verdict: CB_OK and *out (the user, and when the connection must
// end: the token's exp, at least CB_MQTT_MIN_LIFE_S from now and at most
// CB_MQTT_MAX_LIFE_S), CB_E_AUTH (no token, one that does not verify, an
// account that is gone, a client id that is not "<user>-..."), or another
// CB_E_* when the check itself could not run (the store). The account is read
// (rt_deps.h verify_token), so a deleted account's unexpired token connects
// nothing. client_id NULL: the caller has none to check, and the broker holds
// the client id to the user itself (IoT's iot:Connect resource).
int cb_mqtt_authorize(cb_ctx *ctx, const char *token, size_t token_len, const char *client_id, size_t id_len,
                      int64_t now_ms, cb_mqtt_grant *out);

// 1 when `user_id` may subscribe to filter f[0..n) (a valid MQTT filter): it
// starts "u/<user_id>/" or "s/". 0 otherwise.
int cb_mqtt_may_subscribe(const char *user_id, const char *f, int n);

// A user id the policy can be written for: a canonical lower-case UUID
// (8-4-4-4-12 hex). Anything else could add a level or a wildcard to a topic
// or an IoT policy resource, so it is refused before either is written.
bool cb_mqtt_user_ok(const char *user_id);

// ---- the hub's side of one connection ----------------------------------------------------

enum { CB_MQTT_AWAIT_CONNECT = 0, CB_MQTT_OPEN, CB_MQTT_CLOSED };

// The connection is small (about 300 bytes) and owns its variable parts on the
// heap: the filters it holds (a game holds a handful, 50 bytes each), the
// upgrade URL's token until CONNECT, and the unfinished packet when a message
// ends inside one. The 4 KB packet buffer is a per-thread scratch of
// cb_mqtt_on_bytes, not a field, and the old fixed arrays (subs[32][257], the
// 2 KB token, the packet buffer) were 14 KB per connection, copied per frame.
typedef struct {
    uint8_t  state;              // CB_MQTT_*
    cb_uuid  user_id;            // once open
    int64_t  expires_s;          // once open: the connection ends then
    uint16_t keepalive_s;
    int      n_subs;
    char    *subs[CB_MQTT_SUBS]; // malloc'd, NUL-terminated; [0..n_subs)
    char    *query_token;        // the upgrade URL's token=, NULL for none (cb_mqtt_conn_set_token)
    int      in_len;             // a packet begun in one WebSocket message and not yet ended
    uint8_t *carry;              // those in_len bytes, malloc'd
} cb_mqtt_conn;

// c must be zeroed memory (calloc) or a connection from before: what the old
// one owned is released, then c is a fresh AWAIT_CONNECT connection.
void cb_mqtt_conn_init(cb_mqtt_conn *c);
// Releases what c owns; c is then a fresh connection again.
void cb_mqtt_conn_free(cb_mqtt_conn *c);
// dst (zeroed memory) becomes a deep copy of src. CB_OK or CB_E_NOMEM (dst is then freed).
int  cb_mqtt_conn_clone(cb_mqtt_conn *dst, const cb_mqtt_conn *src);
// Keeps a copy of the upgrade URL's token ("" for none). false when out of memory.
bool cb_mqtt_conn_set_token(cb_mqtt_conn *c, const char *token);

// The bytes one WebSocket message carried (part of a packet, one, or several,
// as MQTT over WebSocket allows), handled in order, each reply (CONNACK,
// SUBACK, UNSUBACK, PINGRESP) through `send` as one message. CB_OK: the
// connection stays. Anything else: the caller ends it (CB_E_AUTH a refused
// CONNECT or an expired connection, CB_E_FORBIDDEN a subscription outside the
// policy or a PUBLISH, CB_E_INVALID bytes that are not this protocol or a
// packet out of order, CB_E_FULL a packet past CB_MQTT_PACKET_MAX or a
// subscription past CB_MQTT_SUBS, CB_E_STATE a DISCONNECT, another CB_E_* a
// CONNECT whose check could not run).
int cb_mqtt_on_bytes(cb_ctx *ctx, cb_mqtt_conn *c, const uint8_t *bytes, size_t len, int64_t now_ms,
                     cb_rt_send_fn send, void *ud);

// Does the connection hear topic t[0..n)? Some filter it holds matches.
bool cb_mqtt_hears(const cb_mqtt_conn *c, const char *t, int n);

// The token= parameter of an upgrade's query, into out ("" for none). false
// only when the query does not parse or the token does not fit.
bool cb_mqtt_query_token(const char *query, char *out, size_t cap);

#endif
