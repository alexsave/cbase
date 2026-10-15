// realtime.h - Supabase Realtime's surface: Phoenix channels over WebSocket,
// with one protocol module and two transports.
//
//   phoenix.c   the protocol, shared: frames in and out (both serializers),
//               join/leave/heartbeat/access_token, topic authorization, and the
//               shapes of the three things the server pushes.
//   rt_local.c  `cbase serve`: an in-process WebSocket (net/ws.h) at
//               GET /realtime/v1/websocket; subscriptions live in memory.
//   rt_apigw.c  Lambda: API Gateway's WebSocket API in front ($connect,
//               $disconnect, $default events); subscriptions are store CONN
//               rows; a push is a POST to the management API per connection
//               (aws/apigw_mgmt.h), and 410 Gone deletes the row.
//
// TOPICS AND WHO MAY JOIN (the realtime.messages policies, in C):
//   pv-<uid>         postgres_changes on player_views filter player_id=eq.<uid>; only <uid>
//   gu-<game>-<uid>  private broadcast; only <uid>, and only a member of <game>
//   game-<game>      private broadcast; any authenticated user (spectators)
//   chat:<game>      private broadcast of chat INSERTs; members of <game>
// Clients send "realtime:<topic>"; the bare topic is what is stored and matched.
//
// PUBLISH from any thread (a request handler, a bot drive). Under serve the
// call copies the frame and posts it to the loop; under Lambda it posts to API
// Gateway before returning. Nothing is retried and nothing is caught up: a
// client that missed a push refetches its view row when it rejoins, as today.
#ifndef CB_REALTIME_H
#define CB_REALTIME_H

#include "app/table_ops.h"
#include "ctx.h"
#include "store/store.h"

#include <stddef.h>
#include <stdint.h>

// The transport: deliver one already-encoded Phoenix frame to every
// connection joined to `topic`, each in its own serializer (the frame is
// built per vsn by the caller through phoenix.h, so `frames[vsn - 1]`).
typedef struct cb_rt_transport {
    const char *name;   // "local", "apigw"
    void (*deliver)(cb_rt *rt, const char *topic, const char *const frames[2], const size_t lens[2]);
    void (*free)(cb_rt *rt);
} cb_rt_transport;

struct cb_rt {
    const cb_rt_transport *t;
    cb_ctx *ctx;
    void *impl;
};

enum { CB_RT_INSERT = 1, CB_RT_UPDATE = 2, CB_RT_DELETE = 3 };

// pv-<uid>: a postgres_changes event for the user's player_views row, shaped
// as Supabase sends it ({data:{type, schema:"public", table:"player_views",
// commit_timestamp, record:{game_id, player_id, view:"\\x..", version, status,
// updated_at}, old_record}, ids:[<binding id>]}). DELETE carries the primary
// key only.
void rt_publish_pv_update(cb_rt *rt, const char *user_id, int event, const cb_view_row *row);
// gu-<game>-<uid> or game-<game>: broadcast event "animation_events" with
// payload {b: base64(cast)}.
void rt_publish_cast(cb_rt *rt, const char *topic, const uint8_t *cast, size_t len);
// chat:<game>: broadcast event "INSERT" with the row, as broadcast_changes did.
void rt_publish_chat(cb_rt *rt, const char *game_id, const cb_chat_row *row);

void cb_rt_free(cb_rt *rt);

#endif
