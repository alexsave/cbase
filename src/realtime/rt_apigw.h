// rt_apigw.h - realtime under Lambda: API Gateway's WebSocket API.
//
// API Gateway holds the sockets and invokes the function per event
// (aws/event.h CB_EVENT_WS):
//   $connect     require proto=nw1; nothing is stored. 200 accepts, 400 refuses.
//   $disconnect  delete the CONN row and its topic index.
//   $default     one stream message (base64 text): a frame or a batch. Load the
//                CONN row (a ping does not), cb_nw_on_frame, store the row back
//                if it changed; the last reply is the route response
//                (CBASE_WS_ROUTE_REPLY=yes) and any before it are POSTed to the
//                connection through the management API (aws/apigw_mgmt.h).
// deliver() reads store conns_by_topic and POSTs the message to each; a 410 Gone
// deletes the row. The endpoint is CBASE_WS_ENDPOINT.
#ifndef CB_RT_APIGW_H
#define CB_RT_APIGW_H

#include "aws/event.h"
#include "realtime.h"

int cb_rt_apigw_new(cb_ctx *ctx, cb_rt **out);
// Handles one WebSocket event; fills the status API Gateway is answered with,
// and `answer` (NULL: none wanted) with the route response's text, empty for
// none.
int cb_rt_apigw_event(cb_ctx *ctx, const cb_ws_event *ev, int *status_out, cb_buf *answer);

#endif
