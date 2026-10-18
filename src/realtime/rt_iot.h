// rt_iot.h - realtime under Lambda: AWS IoT Core, MQTT over WebSocket.
//
// IoT Core holds the clients' connections and their subscriptions, and
// decides at CONNECT, through our custom authorizer (main_lambda.c answers it:
// stream_mqtt.h has the policy), which topics each may hear. So this
// transport keeps nothing: deliver() is one HTTPS publish per target, to the
// target's MQTT topic (realtime.h cb_rt_topic), at QoS 0, with the message as
// the payload (aws/iot_data.h), and IoT fans it out. A CHAT target with no
// user (nw1's whole-game chat) has no MQTT topic and is not sent. forget is
// NULL: IoT names no connection we could end, and a deleted user's open
// connections end at their token's expiry (realtime.h cb_rt_forget_user).
// The endpoint is CBASE_IOT_ENDPOINT; boot.c makes this transport only when it
// is set.
#ifndef CB_RT_IOT_H
#define CB_RT_IOT_H

#include "aws/event.h"
#include "realtime.h"

int cb_rt_iot_new(cb_ctx *ctx, cb_rt **out);

// THE CUSTOM AUTHORIZER: IoT Core's request at an MQTT CONNECT (aws/event.h
// cb_iot_auth_event), answered into `out` (json_surface.h cb_iot_auth_encode):
// the policy for the token's user when stream_mqtt.h cb_mqtt_authorize grants
// it and the connection came through the edge (X-Cbase-Origin, as every HTTP
// request must), "not authenticated" otherwise. The connection's lifetime is
// the grant's, so it ends when the token does. Region and account are the
// function's own (its ARN: `function_arn`). CB_OK with an answer written
// (allowed or not); another CB_E_* when the check itself could not run (the
// store): the invocation then fails, and IoT refuses the CONNECT.
int cb_iot_authorize(cb_ctx *ctx, const cb_iot_auth_event *ev, const char *function_arn, int64_t now_ms, cb_buf *out,
                     bool *allowed);

#endif
