// realtime.h - the push stream's publish side: who hears a push, and the
// transport that carries it. The stream is MQTT 3.1.1 over WebSocket
// (shared/c/mqtt), QoS 0, at /mqtt:
//
//   stream_mqtt.c  the policy (who may connect and subscribe) and the hub's
//                  side of a connection, for the hub and the IoT authorizer alike.
//   rt_local.c     `cbase serve`: the in-process hub holding the WebSockets;
//                  subscriptions live in memory.
//   rt_iot.c       Lambda: AWS IoT Core holds the connections and the
//                  subscriptions; a push is one HTTPS publish to the topic, and
//                  the custom authorizer answers IoT's CONNECTs.
//
// WHO HEARS A PUSH is a target (cb_rt_to): one of the app's TOPIC FAMILIES
// (cbase/app.h cb_topic_family), with its {user} and {doc} filled in by
// cb_rt_topic. Foolish's (app/pushes.h):
//   views  u/{user}/v          the user's views of every game they sit at
//   seat   u/{user}/c/{doc}    one seat's casts
//   watch  s/{doc}             the spectators' casts
//   chat   u/{user}/m/{doc}    a game's chat as one member hears it
// Every private topic starts with the user it is for, so who may hear it is
// decided once, at CONNECT, by a static policy (stream_mqtt.h): a connection
// may subscribe under u/<its user>/ and s/, and nothing else. A family's
// pattern must therefore start with one of those two (cb_rt_families_check),
// and a product publishes a member's copy to each member's own topic.
//
// PUBLISH from any thread (a request handler, a bot drive). Under serve the
// call copies the frame and posts it to the loop; under Lambda it publishes to
// IoT before returning. Nothing is retried: a client takes its state whole
// again when its subscription goes live. The payload is opaque bytes: the
// product's own frame (foolish's are netwire.h's NW_WS_VIEW, NW_WS_CAST,
// NW_WS_CHAT, written in app/pushes.c).
#ifndef CB_REALTIME_H
#define CB_REALTIME_H

#include "cbase/app.h"
#include "ctx.h"

#include <stddef.h>
#include <stdint.h>

typedef struct {
    const cb_topic_family *family;
    const char            *user_id;   // {user}, when the pattern has it
    const char            *doc_id;    // {doc}, when the pattern has it
} cb_rt_to;

// The MQTT topic of a target into out (its family's pattern, {user} and
// {doc} filled in): its length, or -1 when the family or an id the pattern
// needs is missing, or an id holds a byte a topic level cannot (only A-Z a-z
// 0-9 '-' '_' pass, so no id can add a level or a wildcard).
int cb_rt_topic(const cb_rt_to *to, char *out, size_t cap);

// The app's families as the policy needs them: each pattern starts "u/{user}/"
// or "s/", holds no '+' or '#', and names nothing but {user} and {doc}.
// CB_OK, or CB_E_INVALID with the family in err (boot refuses to serve).
int cb_rt_families_check(const cb_topic_family *const *f, int n, char *err, size_t cap);

// Delivers one message, as it is, to each of to[0..n) through rt's transport
// (in a batch when n > 1, so the targets hear it at once). Nothing when rt is
// NULL. The failures count toward cb_rt_failures_take.
void cb_rt_publish(cb_rt *rt, const cb_rt_to *to, int n, const uint8_t *frame, size_t len);

// The transport: deliver one encoded message to every connection that hears
// `to`. Returns how many deliveries failed (a publish refused: 1), having
// logged it; 0 when it went out.
typedef struct cb_rt_transport {
    const char *name;   // "local", "iot"
    int (*deliver)(cb_rt *rt, const cb_rt_to *to, const uint8_t *frame, size_t len);
    void (*free)(cb_rt *rt);
    // Ends the connections of `user_id` (cb_rt_end_sessions). NULL: the
    // transport keeps no connections it can name.
    void (*forget)(cb_rt *rt, const char *user_id, const char *session_id, const char *keep_session);
    // A batch's deliveries at once (cb_rt_batch_end): n messages, each in
    // lane lane[i]; the lanes run concurrently and each lane in the order
    // given. Returns how many failed. NULL: the transport delivers each
    // message as it is published (deliver), and a batch changes nothing.
    int (*deliver_lanes)(cb_rt *rt, const cb_rt_to *to, const uint8_t *const *frames, const size_t *lens,
                         const int *lane, int n);
} cb_rt_transport;

struct cb_rt {
    const cb_rt_transport *t;
    cb_ctx *ctx;
    void *impl;
};

// A BATCH: the publishes this thread makes between begin and end are held
// (each message copied) and handed to the transport at end, all at once, when
// it can (deliver_lanes): one lane per user and one for the spectators, so a
// user hears its pushes in the order they were published (a view before the
// cast of the same version, which a host showing boards relies on:
// app/pushes.c) while different users' pushes overlap. Under IoT
// that is one publish's time for a commit's fan-out instead of the sum of
// them. Batches nest; the outermost end sends. A publish the batch cannot
// hold (full, or no memory) is delivered at once. Not for a transport
// without deliver_lanes, where both calls do nothing.
void cb_rt_batch_begin(cb_rt *rt);
void cb_rt_batch_end(cb_rt *rt);

// The failed deliveries this thread's publishes counted since the last take
// (app/after.c logs those that happened after an answer was sent).
int cb_rt_failures_take(void);
// Counts n failed deliveries on this thread (a message that could not even be written).
void cb_rt_failures_add(int n);

// Account deletion: every connection the user holds is ended, so a page left
// open hears nothing more of the games it sat at (their seats keep the id:
// table.h table_redact). A connection
// made later with the same, still unexpired, token is refused: the stream's
// token check reads the account (rt_deps.h verify_token). IoT Core keeps no
// connection we can name, so there the user's open connections end at their
// token's expiry (stream_mqtt.h, disconnectAfterInSeconds); until then they
// hear nothing private, because nothing is published to a deleted user's
// topics any more.
void cb_rt_forget_user(cb_rt *rt, const char *user_id);

// A session ended (auth.c: sign-out, revocation, a password changed elsewhere):
// the user's open connections of that session are closed, so a socket never
// outlives the login that opened it. session_id NULL: every session of the
// user; keep_session non-NULL: all but that one (a password change keeps the
// caller). Under serve the sockets close on the loop, soon, and nothing a
// closed connection still had queued is handled.
void cb_rt_end_sessions(cb_rt *rt, const char *user_id, const char *session_id, const char *keep_session);

void cb_rt_free(cb_rt *rt);

#endif
