// rt_local.h - realtime for `cbase serve`: the in-process WebSocket.
//
// GET /realtime/v1/websocket?apikey=..&vsn=.. (CB_ROUTE_LOOP | CB_ROUTE_APIKEY)
// upgrades (net/ws.h) and keeps a cb_phx_conn per socket on the loop thread.
// deliver() posts the frame to the loop (loop.h cb_loop_post), which sends it
// to every socket joined to the topic.
#ifndef CB_RT_LOCAL_H
#define CB_RT_LOCAL_H

#include "realtime.h"

int cb_rt_local_new(cb_ctx *ctx, cb_rt **out);
int cb_rt_local_routes(cb_ctx *ctx);

#endif
