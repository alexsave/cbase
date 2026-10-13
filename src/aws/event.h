// event.h - Lambda's event JSON in, and the response JSON out.
//
// The kinds of event the one function receives:
//   HTTP       a Function URL (payload format 2.0, same as an HTTP API):
//              requestContext.http.method, rawPath, rawQueryString, headers
//              (lowercase names; repeated values comma-joined), cookies (joined
//              into one "cookie" header, "; " between), body, isBase64Encoded.
//              Decoded into a cb_http_req for cb_handle.
//   WS         an API Gateway WebSocket event: requestContext.routeKey
//              ($connect | $disconnect | $default), connectionId, domainName,
//              stage, queryStringParameters ($connect; re-encoded into `query`),
//              body ($default; base64 when isBase64Encoded).
//   SCHEDULE   the bot scan: an EventBridge event ("source":"aws.events"), or
//              the JSON string literal "scan" (the Scheduler's configured input).
//   BOT_DRIVE  our self-invoke: the JSON string literal "bot_drive:<game id>".
// The two literals are valid JSON for Lambda and are read by stripping the
// quotes: no object parsing on that path (decision record, 2026-10-13).
// The HTTP response is {statusCode, headers:{..}, body, isBase64Encoded} plus
// "cookies" for any Set-Cookie; a body goes base64 when its Content-Type is
// application/octet-stream (every function answer) or it is not valid UTF-8.
// A WS event is answered {statusCode}.
//
// THE JSON TAX STAYS HERE: this file and the rest of src/aws are the only
// places that read or write the Lambda envelope.
#ifndef CB_EVENT_H
#define CB_EVENT_H

#include "net/http.h"
#include "util/str.h"

typedef enum {
    CB_EVENT_UNKNOWN = 0,
    CB_EVENT_HTTP,
    CB_EVENT_WS,
    CB_EVENT_SCHEDULE,
    CB_EVENT_BOT_DRIVE,
} cb_event_kind;

typedef struct {
    char        route_key[16];      // "$connect", "$disconnect", "$default"
    char        connection_id[64];
    char        domain[256];
    char        stage[64];
    const char *query;              // $connect: the raw query ("apikey=..&vsn=2.0.0"), "" otherwise
    const uint8_t *body;            // $default: the frame text
    size_t      body_len;
} cb_ws_event;

typedef struct {
    cb_event_kind kind;
    cb_http_req   http;             // CB_EVENT_HTTP; strings point into `storage`
    cb_ws_event   ws;               // CB_EVENT_WS
    char          game_id[32];      // CB_EVENT_BOT_DRIVE
    cb_buf        storage;          // owns every string above
} cb_event;

// CB_OK and *out (free with cb_event_free), or CB_E_INVALID.
int  cb_event_decode(const char *json, size_t len, cb_event *out);
void cb_event_free(cb_event *ev);
// The response JSON for an HTTP event.
int  cb_event_encode_http(const cb_http_res *res, cb_buf *out);
// The response JSON for a WebSocket event.
int  cb_event_encode_ws(int status, cb_buf *out);

#endif
