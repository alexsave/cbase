// rt_apigw.h - realtime under Lambda: API Gateway's WebSocket API.
//
// API Gateway holds the sockets and invokes the function per event
// (aws/event.h CB_EVENT_WS):
//   $connect     require proto=nw1; store a CONN row (no user, expires in
//                2 h). 200 accepts, 400 refuses.
//   $disconnect  delete the CONN row.
//   $default     one stream frame (base64 text): load the CONN row,
//                cb_nw_on_frame, store the row back if it changed; replies are
//                POSTed to the connection through the management API
//                (aws/apigw_mgmt.h).
// deliver() reads store conns_by_topic and POSTs the message to each; a 410 Gone
// deletes the row. The endpoint is CBASE_WS_ENDPOINT.
#ifndef CB_RT_APIGW_H
#define CB_RT_APIGW_H

#include "aws/event.h"
#include "realtime.h"

int cb_rt_apigw_new(cb_ctx *ctx, cb_rt **out);
// Handles one WebSocket event; fills the status API Gateway is answered with.
int cb_rt_apigw_event(cb_ctx *ctx, const cb_ws_event *ev, int *status_out);

#endif
