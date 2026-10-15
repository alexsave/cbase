// rt_local.h - realtime for `cbase serve`: the in-process WebSocket.
//
// GET /v1/stream?proto=nw1 (CB_ROUTE_LOOP) upgrades (net/ws.h) and keeps a
// cb_conn_row per socket on the loop thread. deliver() posts the message to the
// loop (loop.h cb_loop_post), which sends it to every socket subscribed to the
// topic.
#ifndef CB_RT_LOCAL_H
#define CB_RT_LOCAL_H

#include "realtime.h"

int cb_rt_local_new(cb_ctx *ctx, cb_rt **out);
int cb_rt_local_routes(cb_ctx *ctx);

#endif
