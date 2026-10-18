// realtime.h - the push stream's publish side: who hears a push, and the
// transports that carry it.
//
//   stream.c       the nw1 protocol: client frames in, replies out, its topic policy.
//   stream_mqtt.c  the MQTT protocol (shared/c/mqtt): who may connect and
//                  subscribe, for the hub and the IoT authorizer alike.
//   rt_local.c     `cbase serve`: one in-process hub holding the WebSockets,
//                  GET /v1/stream (nw1) and GET /mqtt (MQTT); subscriptions
//                  live in memory.
//   rt_apigw.c     Lambda: API Gateway's WebSocket API in front ($connect,
//                  $disconnect, $default events); subscriptions are store CONN
//                  rows; a push is a POST to the management API per connection.
//   rt_iot.c       Lambda: AWS IoT Core holds the MQTT connections and the
//                  subscriptions; a push is one HTTPS publish to the topic.
// While the clients move from nw1 to MQTT, Lambda runs apigw and iot side by
// side (cb_rt.also): every push goes out on both.
//
// WHO HEARS A PUSH is a target (cb_rt_to), not a topic string: each transport
// names the target in its own protocol's topic (cb_rt_topic for MQTT,
// cb_rt_nw_topic for nw1). The families:
//   CB_RT_VIEWS  user        the user's views of every game they sit at
//                            MQTT u/<user>/v            nw1 pv-<user>
//   CB_RT_SEAT   user, game  one seat's casts
//                            MQTT u/<user>/c/<game>     nw1 gu-<game>-<user>
//   CB_RT_WATCH  game        the spectators' casts
//                            MQTT s/<game>              nw1 game-<game>
//   CB_RT_CHAT   user, game  a game's chat as one member hears it
//                            MQTT u/<user>/m/<game>     nw1 chat:<game>
// Under MQTT every private topic starts with the user it is for, so who may
// hear it is decided once, at CONNECT, by a static policy (stream_mqtt.h): a
// connection may subscribe under u/<its user>/ and s/, and nothing else. A
// seat's casts and a game's chat are therefore published once per member, to
// that member's own topic. nw1's chat:<game> is one topic for every member:
// rt_publish_chat delivers it once as a CB_RT_CHAT target with no user, which
// only the nw1 transports take.
//
// PUBLISH from any thread (a request handler, a bot drive). Under serve the
// call copies the frame and posts it to the loop; under Lambda it posts to API
// Gateway and IoT before returning. Nothing is retried: a client that missed a
// push asks for the game again when it resubscribes (the catch-up). The bytes
// are the nw1 message either way (netwire.h NW_WS_VIEW, NW_WS_CAST,
// NW_WS_CHAT): MQTT carries them as a PUBLISH's payload.
#ifndef CB_REALTIME_H
#define CB_REALTIME_H

#include "app/table_ops.h"
#include "ctx.h"
#include "store/store.h"

#include <stddef.h>
#include <stdint.h>

typedef enum {
    CB_RT_VIEWS = 1,
    CB_RT_SEAT,
    CB_RT_WATCH,
    CB_RT_CHAT,
} cb_rt_family;

typedef struct {
    cb_rt_family family;
    const char  *user_id;   // VIEWS, SEAT, CHAT (NULL on CHAT: the whole game's, nw1 only)
    const char  *game_id;   // SEAT, WATCH, CHAT
} cb_rt_to;

// The MQTT topic of a target into out: its length, or -1 when the target has
// no MQTT topic (a CHAT with no user), an id is missing, or an id holds a byte
// a topic level cannot (only A-Z a-z 0-9 '-' '_' pass, so no id can add a
// level or a wildcard).
int cb_rt_topic(const cb_rt_to *to, char *out, size_t cap);
// The nw1 topic of a target (stream.h): its length, or -1 (a SEAT or CHAT is
// the nw1 topic whatever its user, but a CHAT target WITH a user has none: the
// game's chat goes once, as the target with no user).
int cb_rt_nw_topic(const cb_rt_to *to, char *out, size_t cap);

// The transport: deliver one encoded message to every connection that hears
// `to`. Returns how many deliveries failed (a connection not reached, the
// subscribers not read, a publish refused: 1 each), having logged each; 0 when
// all went out, or when the target is not one this transport carries.
typedef struct cb_rt_transport {
    const char *name;   // "local", "apigw", "iot"
    int (*deliver)(cb_rt *rt, const cb_rt_to *to, const uint8_t *frame, size_t len);
    void (*free)(cb_rt *rt);
    // Ends every connection whose user is `user_id` (cb_rt_forget_user):
    // `targets` are the ones it could have heard, for a transport that finds
    // connections only by topic. NULL: the transport keeps no connections.
    void (*forget)(cb_rt *rt, const char *user_id, const cb_rt_to *targets, int n);
} cb_rt_transport;

struct cb_rt {
    const cb_rt_transport *t;
    cb_ctx *ctx;
    void *impl;
    cb_rt *also;   // a second transport every push also goes to (Lambda: iot beside apigw), owned
};

// CB_RT_VIEWS: NW_WS_VIEW {game, version, envelope}; `gone` sends the view
// with no envelope (the seat is no longer the user's).
void rt_publish_view(cb_rt *rt, const char *user_id, const cb_view_row *row, bool gone);
// CB_RT_SEAT for user_id, or CB_RT_WATCH when it is NULL: NW_WS_CAST {game, version, cast}.
void rt_publish_cast(cb_rt *rt, const char *user_id, const char *game_id, int64_t version, const uint8_t *cast, size_t len);
// CB_RT_CHAT: NW_WS_CHAT {game, the message}, to each of the game's human
// members (`members`, n of them) on their own topic, and once to the game's
// nw1 chat topic.
void rt_publish_chat(cb_rt *rt, const char *game_id, const cb_chat_row *row, const cb_uuid *members, int n);

// The failed deliveries this thread's publishes counted since the last take
// (app/after.c logs those that happened after an answer was sent).
int cb_rt_failures_take(void);

// Account deletion: every connection the user holds is ended, so a page left
// open hears nothing more of the games it sat at (their seats keep the id:
// table.h table_redact). `game_ids` are the games they sat at. A connection
// made later with the same, still unexpired, token is refused: the stream's
// token check reads the account (rt_deps.h verify_token). IoT Core keeps no
// connection we can name, so there the user's open connections end at their
// token's expiry (stream_mqtt.h, disconnectAfterInSeconds); until then they
// hear nothing private, because nothing is published to a deleted user's
// topics any more.
void cb_rt_forget_user(cb_rt *rt, const char *user_id, const char (*game_ids)[CB_GAME_ID_MAX + 1], int n);

void cb_rt_free(cb_rt *rt);

#endif
