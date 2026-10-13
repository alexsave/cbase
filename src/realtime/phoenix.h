// phoenix.h - the Phoenix channel protocol as @supabase/realtime-js 2.x and
// supabase-swift speak it, independent of the socket underneath.
//
// THE WIRE. A text frame per message. vsn=2.0.0 (realtime-js 2.x) is a JSON
// array [join_ref, ref, topic, event, payload]; vsn=1.0.0 (supabase-swift's
// default) is an object {join_ref, ref, topic, event, payload}. The vsn comes
// from the connect URL's query and is fixed per connection. The executable
// spec is foolish/e2e/helpers/ws_phoenix.ts and e2e/fake_supabase.mts.
//
// WHAT THE SERVER DOES WITH A CLIENT MESSAGE (cb_phx_on_frame):
//   heartbeat (topic "phoenix")  reply ok {}
//   phx_join                     verify payload.access_token (auth.h), then the
//                                topic policy (realtime.h); reply ok with
//                                {postgres_changes:[{id, event, schema, table,
//                                filter}]} echoing each binding with an id (or
//                                realtime-js errors the channel), else reply
//                                error {reason}
//   access_token                 re-verify; a failure leaves the channels
//   phx_leave                    drop the topic, reply ok
//   anything else on a joined topic: ignored (clients do not broadcast here)
#ifndef CB_PHOENIX_H
#define CB_PHOENIX_H

#include "ctx.h"
#include "store/store.h"
#include "util/str.h"

#include <stdbool.h>
#include <stddef.h>

// One parsed client message. Spans point into the frame text.
typedef struct {
    cb_span join_ref;   // empty for null
    cb_span ref;
    char    topic[CB_TOPIC_MAX];   // as sent ("realtime:gu-..."), unescaped
    char    event[32];
    cb_span payload;    // the payload's JSON text
} cb_phx_msg;

// CB_OK, or CB_E_INVALID for a frame that is not a Phoenix message in `vsn`.
int cb_phx_decode(const char *text, size_t len, int vsn, cb_phx_msg *out);

// Frame writers (into `out`, replacing its contents).
void cb_phx_reply(cb_buf *out, int vsn, const cb_phx_msg *to, bool ok, const char *response_json);
void cb_phx_push(cb_buf *out, int vsn, const char *wire_topic, const char *event, const char *payload_json);

// The subscription state of one connection: kept in memory by rt_local, and
// in a store CONN row by rt_apigw (cb_conn_row has the same fields).
typedef cb_conn_row cb_phx_conn;

// Handles one client frame for `conn`, updating its topics and user, and
// sending every reply through `send`. CB_OK, or CB_E_INVALID for a frame that
// does not decode (the caller may close the socket).
typedef void (*cb_phx_send_fn)(void *ud, const char *frame, size_t len);
int cb_phx_on_frame(cb_ctx *ctx, cb_phx_conn *conn, const char *text, size_t len, cb_phx_send_fn send, void *ud);

// The topic policy: may `user_id` join the bare `topic`? 1 yes, 0 no, CB_E_* on error.
int cb_phx_may_join(cb_ctx *ctx, const char *user_id, const char *topic);

#endif
