// rt_local.h - realtime for `cbase serve`: the in-process socket hub (rt_local.c).
//
// GET /ws (CB_ROUTE_LOOP, no subprotocol) upgrades (net/ws.h) to a binary WebSocket that speaks
// shared/c/stream_frame.h's frames: HELLO with a token, SUB a room, REQ a route, PING. GET
// /ws/public, served only when the app has an anonymous family (cbase/app.h), is the same socket
// without a login: an empty HELLO, SUB of rooms, PING. There is no MQTT here (that is IoT's
// transport under Lambda, rt_iot.c): deliver() hands the push to the loop (loop.h cb_loop_post),
// which sends it to the user's connections or to the room's watchers.
#ifndef CB_RT_LOCAL_H
#define CB_RT_LOCAL_H

#include "realtime/realtime.h"

// THE INBOX. While a connection's HELLO or REQ is out on the request pool, its next frames wait in
// its inbox; with one request worker (config.c REQ_WORKERS_DEFAULT) a job may hold it a while. A
// connection whose waiting frames pass either cap is closed with 1008, and the server serves
// everyone else. What a real client ever has waiting (the product's client library): at most 24
// REQs in flight, 16 SUBs and 16 UNSUBs, and a PING, so under 60 frames of at most 4096 bytes,
// under 250 KiB. Both caps are twice that and more.
#define CB_RT_INBOX_MAX        (1u << 20)   // heap one connection's waiting frames may hold (each charged its real cost)
#define CB_RT_INBOX_FRAMES_MAX 128          // frames one connection may have waiting

int cb_rt_local_new(cb_ctx *ctx, cb_rt **out);
int cb_rt_local_routes(cb_ctx *ctx);
// Walks of the by-user table so far (a user's second push in a loop pass reuses the first's).
long cb_rt_local_user_walks(const cb_rt *rt);
// User pushes not sent to a connection in their quiet room (realtime.h cb_rt_to.quiet_room).
long cb_rt_local_quiet_skips(const cb_rt *rt);
// The most heap one connection's waiting frames have held (rt_local.c INBOX_MAX bounds it).
size_t cb_rt_local_inbox_peak(const cb_rt *rt);

#endif
