// realtime.h - the push stream's publish side, with one protocol (stream.h)
// and two transports.
//
//   stream.c    the protocol: client frames in, replies out, the topic policy.
//   rt_local.c  `cbase serve`: an in-process WebSocket (net/ws.h) at
//               GET /v1/stream; subscriptions live in memory.
//   rt_apigw.c  Lambda: API Gateway's WebSocket API in front ($connect,
//               $disconnect, $default events); subscriptions are store CONN
//               rows; a push is a POST to the management API per connection
//               (aws/apigw_mgmt.h), and 410 Gone deletes the row.
//
// TOPICS (stream.h has who may hear each):
//   pv-<uid>         the user's views of every game they sit at
//   gu-<game>-<uid>  one seat's casts
//   game-<game>      the spectators' casts
//   chat:<game>      a game's chat
//
// PUBLISH from any thread (a request handler, a bot drive). Under serve the
// call copies the frame and posts it to the loop; under Lambda it posts to API
// Gateway before returning. Nothing is retried: a client that missed a push
// asks for the game again by version when it resubscribes (the catch-up).
#ifndef CB_REALTIME_H
#define CB_REALTIME_H

#include "app/table_ops.h"
#include "ctx.h"
#include "store/store.h"

#include <stddef.h>
#include <stdint.h>

// The transport: deliver one encoded message to every connection subscribed to
// `topic`. The bytes are the message's; the transport decides how they travel
// (rt_local: a binary frame; rt_apigw: base64 text, because what API Gateway's
// @connections posts arrive as is not documented). Returns how many deliveries
// failed (a connection not reached, or the subscribers not read: 1), having
// logged each; 0 when all went out.
typedef struct cb_rt_transport {
    const char *name;   // "local", "apigw"
    int (*deliver)(cb_rt *rt, const char *topic, const uint8_t *frame, size_t len);
    void (*free)(cb_rt *rt);
    // Ends every connection whose user is `user_id` (cb_rt_forget_user):
    // `topics` are the ones it could have joined, for a transport that finds
    // connections only by topic. NULL: the transport keeps no connections.
    void (*forget)(cb_rt *rt, const char *user_id, const char *const *topics, int n);
} cb_rt_transport;

struct cb_rt {
    const cb_rt_transport *t;
    cb_ctx *ctx;
    void *impl;
};

// pv-<uid>: NW_WS_VIEW {game, version, envelope}; `gone` sends the view with no
// envelope (the seat is no longer the user's).
void rt_publish_view(cb_rt *rt, const char *user_id, const cb_view_row *row, bool gone);
// gu-<game>-<uid> or game-<game>: NW_WS_CAST {game, version, cast}.
void rt_publish_cast(cb_rt *rt, const char *topic, const char *game_id, int64_t version, const uint8_t *cast, size_t len);
// chat:<game>: NW_WS_CHAT {game, the message}.
void rt_publish_chat(cb_rt *rt, const char *game_id, const cb_chat_row *row);

// The failed deliveries this thread's publishes counted since the last take
// (app/after.c logs those that happened after an answer was sent).
int cb_rt_failures_take(void);

// Account deletion: every connection the user holds is ended, so a page left
// open hears nothing more of the games it sat at (their seats keep the id:
// table.h table_redact). `game_ids` are the games they sat at. A connection
// made later with the same, still unexpired, token is refused: the stream's
// token check reads the account (rt_deps.h verify_token).
void cb_rt_forget_user(cb_rt *rt, const char *user_id, const char (*game_ids)[CB_GAME_ID_MAX + 1], int n);

void cb_rt_free(cb_rt *rt);

#endif
