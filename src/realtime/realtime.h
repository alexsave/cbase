// realtime.h - the push stream's publish side: who hears a push, and the
// transport that carries it. The stream is MQTT 3.1.1 over WebSocket
// (shared/c/mqtt), QoS 0, at /mqtt:
//
//   stream_mqtt.c  the policy (who may connect and subscribe) and the hub's
//                  side of a connection, for the hub and the IoT authorizer alike.
//   rt_local.c     `cbase serve`: the in-process hub holding the WebSockets;
//                  subscriptions live in memory.
//   rt_iot.c       Lambda: AWS IoT Core holds the connections and the
//                  subscriptions; a push is one HTTPS publish to the topic, and
//                  the custom authorizer answers IoT's CONNECTs.
//
// WHO HEARS A PUSH is a target (cb_rt_to), named as an MQTT topic by
// cb_rt_topic:
//   CB_RT_VIEWS  user        u/<user>/v          the user's views of every game they sit at
//   CB_RT_SEAT   user, game  u/<user>/c/<game>   one seat's casts
//   CB_RT_WATCH  game        s/<game>            the spectators' casts
//   CB_RT_CHAT   user, game  u/<user>/m/<game>   a game's chat as one member hears it
// Every private topic starts with the user it is for, so who may hear it is
// decided once, at CONNECT, by a static policy (stream_mqtt.h): a connection
// may subscribe under u/<its user>/ and s/, and nothing else. A seat's casts
// and a game's chat are therefore published once per member, to that member's
// own topic.
//
// PUBLISH from any thread (a request handler, a bot drive). Under serve the
// call copies the frame and posts it to the loop; under Lambda it publishes to
// IoT before returning. Nothing is retried: a client takes a game whole again
// when its subscription goes live (netclient.h). The payload is the push as
// netwire.h writes it (NW_WS_VIEW, NW_WS_CAST, NW_WS_CHAT).
#ifndef CB_REALTIME_H
#define CB_REALTIME_H

#include "app/table_ops.h"
#include "ctx.h"
#include "store/store.h"

#include "netwire.h"

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
    const char  *user_id;   // VIEWS, SEAT, CHAT
    const char  *game_id;   // SEAT, WATCH, CHAT
} cb_rt_to;

// The MQTT topic of a target into out: its length, or -1 when an id is
// missing, or holds a byte a topic level cannot (only A-Z a-z 0-9 '-' '_'
// pass, so no id can add a level or a wildcard).
int cb_rt_topic(const cb_rt_to *to, char *out, size_t cap);

// The transport: deliver one encoded message to every connection that hears
// `to`. Returns how many deliveries failed (a publish refused: 1), having
// logged it; 0 when it went out.
typedef struct cb_rt_transport {
    const char *name;   // "local", "iot"
    int (*deliver)(cb_rt *rt, const cb_rt_to *to, const uint8_t *frame, size_t len);
    void (*free)(cb_rt *rt);
    // Ends every connection whose user is `user_id` (cb_rt_forget_user).
    // NULL: the transport keeps no connections it can name.
    void (*forget)(cb_rt *rt, const char *user_id);
    // A batch's deliveries at once (cb_rt_batch_end): n messages, each in
    // lane lane[i]; the lanes run concurrently and each lane in the order
    // given. Returns how many failed. NULL: the transport delivers each
    // message as it is published (deliver), and a batch changes nothing.
    int (*deliver_lanes)(cb_rt *rt, const cb_rt_to *to, const uint8_t *const *frames, const size_t *lens,
                         const int *lane, int n);
} cb_rt_transport;

struct cb_rt {
    const cb_rt_transport *t;
    cb_ctx *ctx;
    void *impl;
};

// CB_RT_VIEWS: NW_WS_VIEW {game, version, envelope}; `gone` sends the view
// with no envelope (the seat is no longer the user's).
void rt_publish_view(cb_rt *rt, const char *user_id, const cb_view_row *row, bool gone);
// CB_RT_SEAT for user_id, or CB_RT_WATCH when it is NULL: NW_WS_CAST {game, version, cast}.
void rt_publish_cast(cb_rt *rt, const char *user_id, const char *game_id, int64_t version, const uint8_t *cast, size_t len);
// CB_RT_CHAT: NW_WS_CHAT {game, the message}, to each of the game's human
// members (`members`, n of them) on their own topic.
void rt_publish_chat(cb_rt *rt, const char *game_id, const cb_chat_row *row, const cb_uuid *members, int n);

// A BATCH: the publishes this thread makes between begin and end are held
// (each message copied) and handed to the transport at end, all at once, when
// it can (deliver_lanes): one lane per user and one for the spectators, so a
// user hears its pushes in the order they were published (a view before the
// cast of the same version, which a host showing boards relies on:
// netclient.c NW_WS_CAST) while different users' pushes overlap. Under IoT
// that is one publish's time for a commit's fan-out instead of the sum of
// them. Batches nest; the outermost end sends. A publish the batch cannot
// hold (full, or no memory) is delivered at once. Not for a transport
// without deliver_lanes, where both calls do nothing.
void cb_rt_batch_begin(cb_rt *rt);
void cb_rt_batch_end(cb_rt *rt);

// The failed deliveries this thread's publishes counted since the last take
// (app/after.c logs those that happened after an answer was sent).
int cb_rt_failures_take(void);

// Account deletion: every connection the user holds is ended, so a page left
// open hears nothing more of the games it sat at (their seats keep the id:
// table.h table_redact). A connection
// made later with the same, still unexpired, token is refused: the stream's
// token check reads the account (rt_deps.h verify_token). IoT Core keeps no
// connection we can name, so there the user's open connections end at their
// token's expiry (stream_mqtt.h, disconnectAfterInSeconds); until then they
// hear nothing private, because nothing is published to a deleted user's
// topics any more.
void cb_rt_forget_user(cb_rt *rt, const char *user_id);

void cb_rt_free(cb_rt *rt);

// A stored chat row as the stream and GET /v1/chat/<id> carry it.
void cb_chat_to_nw(const cb_chat_row *r, NwChat *out);

#endif
