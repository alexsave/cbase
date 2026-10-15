// stream.h - the binary push stream (DESIGN.md "Binary surface"): the
// kernel's netwire frames over the same two transports as Phoenix.
//
// A connection that asked for it (`proto=nw1` on the upgrade or $connect URL)
// has cb_conn_row.vsn == CB_VSN_NW, keeps its user and topics in the same row,
// and hears the same topics; what differs is the frames. The client sends
// NW_WS_AUTH, NW_WS_SUB, NW_WS_UNSUB and NW_WS_PING; the server answers ACK and
// PONG, and pushes VIEW, CAST and CHAT (realtime.c builds them beside the
// Phoenix frames, once per publish).
//
// Subscriptions map onto the topics the Phoenix policy already guards:
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

#include "phoenix.h"

#include "netwire.h"

#define CB_VSN_NW 3

// The protocol a connect URL's query asks for: CB_VSN_NW for proto=nw1, 0 for
// no proto (a Phoenix client, see cb_phx_vsn_of_query), CB_E_INVALID for any
// other proto.
int cb_nw_proto_of_query(const char *query);

// Handles one client message for `conn` (its user and topics updated), every
// reply through `send`. CB_OK, or CB_E_INVALID for bytes that are not a client
// frame (the caller may close the socket).
int cb_nw_on_frame(cb_ctx *ctx, cb_conn_row *conn, const uint8_t *msg, size_t len, cb_phx_send_fn send, void *ud);

// A stored chat row as the stream and GET /v1/chat/<id> carry it.
void cb_chat_to_nw(const cb_chat_row *r, NwChat *out);

#endif
