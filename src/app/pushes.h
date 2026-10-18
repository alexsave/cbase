// pushes.h - what foolish publishes on the push stream (realtime.h): its four
// topic families, and the messages (netwire.h's stream frames) it writes.
//
//   CB_RT_VIEWS  u/{user}/v          NW_WS_VIEW: the user's views of every game they sit at
//   CB_RT_SEAT   u/{user}/c/{doc}    NW_WS_CAST: one seat's casts
//   CB_RT_WATCH  s/{doc}             NW_WS_CAST: the spectators' casts
//   CB_RT_CHAT   u/{user}/m/{doc}    NW_WS_CHAT: a game's chat, once per member
#ifndef CB_PUSHES_H
#define CB_PUSHES_H

#include "realtime/realtime.h"
#include "store/store.h"
#include "table_ops.h"

#include "netwire.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

extern const cb_topic_family cb_fam_views, cb_fam_seat, cb_fam_watch, cb_fam_chat;
#define CB_RT_VIEWS (&cb_fam_views)
#define CB_RT_SEAT  (&cb_fam_seat)
#define CB_RT_WATCH (&cb_fam_watch)
#define CB_RT_CHAT  (&cb_fam_chat)
// The four, for the app (cbase/app.h topics).
extern const cb_topic_family *const cb_foolish_topics[4];

// CB_RT_VIEWS: NW_WS_VIEW {game, version, envelope}; `gone` sends the view
// with no envelope (the seat is no longer the user's).
void rt_publish_view(cb_rt *rt, const char *user_id, const cb_view_row *row, bool gone);
// CB_RT_SEAT for user_id, or CB_RT_WATCH when it is NULL: NW_WS_CAST {game, version, cast}.
void rt_publish_cast(cb_rt *rt, const char *user_id, const char *game_id, int64_t version, const uint8_t *cast, size_t len);
// CB_RT_CHAT: NW_WS_CHAT {game, the message}, to each of the game's human
// members (`members`, n of them) on their own topic.
void rt_publish_chat(cb_rt *rt, const char *game_id, const cb_chat_row *row, const cb_uuid *members, int n);

// A stored chat row as the stream and GET /v1/chat/<id> carry it.
void cb_chat_to_nw(const cb_chat_row *r, NwChat *out);

#endif
