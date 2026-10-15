// stream.h - the push stream (DESIGN.md "Binary surface"): the kernel's
// netwire frames over two transports, an in-process WebSocket under `cbase
// serve` (rt_local.h) and API Gateway's WebSocket API under Lambda (rt_apigw.h).
//
// A connection asks for it with `proto=nw1` on the upgrade or $connect URL
// (cb_conn_row.vsn == CB_VSN_NW) and keeps its user and topics in its row. The
// client sends NW_WS_AUTH, NW_WS_SUB, NW_WS_UNSUB and NW_WS_PING; the server
// answers ACK and PONG, and pushes VIEW, CAST and CHAT (realtime.c builds each
// once per publish).
//
// Subscriptions map onto topics, each with its policy (cb_rt_may_hear):
//   NW_SUB_GAMES          pv-<user>
//   NW_SUB_GAME  <game>   gu-<game>-<user> for a member, game-<game> otherwise
//   NW_SUB_CHAT  <game>   chat:<game> (members only)
// A game subscription whose `since` is below the game's version is followed,
// after its ACK, by the caller's current view of the game: the catch-up.
//
// Frames handed to `send` are the message bytes; the transport carries them
// as a binary frame (rt_local) or base64 text (rt_apigw).
#ifndef CB_STREAM_H
#define CB_STREAM_H

#include "ctx.h"
#include "store/store.h"

#include "netwire.h"

#include <stddef.h>
#include <stdint.h>

#define CB_VSN_NW 3

// The protocol a connect URL's query asks for: CB_VSN_NW for proto=nw1,
// CB_E_INVALID for no proto or any other.
int cb_nw_proto_of_query(const char *query);

// Where a frame goes: the message's bytes, which the transport carries.
typedef void (*cb_rt_send_fn)(void *ud, const char *frame, size_t len);

// The topic policy: may `user_id` (NULL or "" for nobody) hear the topic?
// pv-<uid> only that user; gu-<game>-<uid> that user, and only a member of
// <game>; game-<game> any signed-in user; chat:<game> members. 1 yes, 0 no,
// CB_E_* on error (CB_E_STATE without a store).
int cb_rt_may_hear(cb_ctx *ctx, const char *user_id, const char *topic);

// Handles one client message for `conn` (its user and topics updated), every
// reply through `send`. CB_OK, or CB_E_INVALID for bytes that are not a client
// frame (the caller may close the socket).
int cb_nw_on_frame(cb_ctx *ctx, cb_conn_row *conn, const uint8_t *msg, size_t len, cb_rt_send_fn send, void *ud);

// A stored chat row as the stream and GET /v1/chat/<id> carry it.
void cb_chat_to_nw(const cb_chat_row *r, NwChat *out);

#endif
