// event.h - the Lambda event, decoded: the kinds of event the one function
// receives and the struct they decode into. The JSON both ways (the decoder,
// the HTTP response encoder, the job payload, the redaction for
// the log) lives in json_surface.c and is declared in json_surface.h.
#ifndef CB_EVENT_H
#define CB_EVENT_H

#include "jobs.h"
#include "net/http.h"
#include "util/str.h"

typedef enum {
    CB_EVENT_UNKNOWN = 0,
    CB_EVENT_HTTP,
    CB_EVENT_SCHEDULE,
    CB_EVENT_JOB,
    CB_EVENT_IOT_AUTH,
} cb_event_kind;


// An AWS IoT Core custom authorizer request (CB_EVENT_IOT_AUTH): what an MQTT
// client presented at CONNECT (realtime/rt_iot.h answers it).
typedef struct {
    // The access token: the MQTT password (base64 on the wire, decoded here),
    // else the event's "token" (IoT's copy of a query parameter), else the
    // query string's token=. Empty: none.
    char        token[2048];
    size_t      token_len;
    // protocolData.mqtt.clientId; empty when the event carries none (the
    // policy's iot:Connect resource still holds the client id to the user).
    char        client_id[160];
    // protocolData.http.headers' X-Cbase-Origin (config.h CBASE_ORIGIN_SECRET);
    // has_origin false when the request carried none.
    bool        has_origin;
    char        origin[128];
} cb_iot_auth_event;

typedef struct {
    cb_event_kind kind;
    cb_http_req   http;             // CB_EVENT_HTTP; strings point into `storage`
    cb_iot_auth_event iot;          // CB_EVENT_IOT_AUTH
    char          job[CB_JOB_NAME_MAX + 1];   // CB_EVENT_JOB: the literal "<job>:<key>" (jobs.h)
    char          key[CB_JOB_KEY_MAX + 1];    // CB_EVENT_JOB
    char          schedule[CB_JOB_NAME_MAX + 1];   // CB_EVENT_SCHEDULE: the literal's name; "" for an EventBridge event (every schedule)
    cb_buf        storage;          // owns every string above
} cb_event;

void cb_event_free(cb_event *ev);
// cb_event_redact(json, len, 4096) logged at WARN, in pieces a line can hold.
void cb_event_log_raw(const char *json, size_t len);

#endif
