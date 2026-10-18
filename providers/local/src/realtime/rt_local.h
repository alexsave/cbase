// rt_local.h - realtime for `cbase serve`: the in-process MQTT hub.
//
// GET /mqtt (CB_ROUTE_LOOP, subprotocol "mqtt") upgrades (net/ws.h) and keeps a
// connection's MQTT state (stream_mqtt.h) per socket on the loop thread.
// deliver() posts the PUBLISH to the loop (loop.h cb_loop_post), which sends it
// to every connection a filter of which matches the topic.
#ifndef CB_RT_LOCAL_H
#define CB_RT_LOCAL_H

#include "realtime/realtime.h"

int cb_rt_local_new(cb_ctx *ctx, cb_rt **out);
int cb_rt_local_routes(cb_ctx *ctx);

#endif
