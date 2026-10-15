// event.h - the Lambda event, decoded: the kinds of event the one function
// receives and the struct they decode into. The JSON both ways (the decoder,
// the HTTP and WS response encoders, the bot_drive payload, the redaction for
// the log) lives in json_surface.c and is declared in json_surface.h.
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
    const char *query;              // $connect: the raw query ("proto=nw1"), "" otherwise
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

void cb_event_free(cb_event *ev);
// cb_event_redact(json, len, 4096) logged at WARN, in pieces a line can hold.
void cb_event_log_raw(const char *json, size_t len);

#endif
