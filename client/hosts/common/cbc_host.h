// cbc_host.h - THE SHARED HOST: everything cbase/client.h asks of a host that does not depend on
// the platform, written once for every C host (client/hosts/posix for a native event loop, and any
// host an app or another repository writes). A host is this file plus an ENVIRONMENT (cbc_host_env):
// the platform's socket, HTTP client, timers, clock and session store, a few lines each.
//
//   socket   while cbc_stream_wanted: env.ws_open(cbc_stream_path), then cbc_opened when the
//            platform says it opened (cbc_host_ws_opened); every message to cbc_in; each
//            cbc_next_out frame to env.ws_send (-1: env.ws_close); on its close
//            (cbc_host_ws_closed), cbc_closed, and the next open after the ms it answers
//   HTTP     each cbc_next_request to env.http (cbc_host_req: the path as the request target,
//            "Bearer ..." as the Authorization value, the deadline the core set, which the
//            environment hands its HTTP client as is and never invents), answered once by
//            cbc_host_http_answered (status 0: it never arrived); a borrowed body (CBC_RQ_REF,
//            cbc_call_ref) is handed over as the caller's own pointer
//   timer    one env.timer after cbc_due_ms, calling cbc_tick, then drain both outputs again; at
//            most one armed for the ticks and one for the reopen
//   events   handed to the app's callback right after the call that raised them, with the bytes
//            their spans are into (the frame, the body; NULL after a tick or a close)
//   session  env.save on every CBC_EV_SESSION (len 0: forget it); the load is the environment's
//            (cbc_session_load on cbc_host_client before the first pump, or later behind
//            cbc_host_hold when the platform's store answers later)
//   seed     the environment's, in the cfg it passes cbc_host_init
//
// THE PLATFORM'S SIDE: every answer the environment hands back (an HTTP answer, a timer, a
// socket's open, message or close) comes later, from the platform's loop, never from inside the
// env call that started it; and after the app calls into the client (cbc_sign_in, cbc_call,
// cbc_sub, ...) it calls cbc_host_pump. The app may call into the client and pump from inside its
// own event callback. Nothing here allocates or keeps a global: the host's struct embeds a
// cbc_host, and a process may run any number.
//
// A STOP AND RESTART: there is no stop call. A host stops when the environment stops calling in,
// and restarts with cbc_host_init on the same struct. Everything owed to the old one must then be
// dropped by the environment, never handed to the new one: its timers stopped and its socket
// closed before the init, and every HTTP answer still out at that moment dropped when it comes
// (stamp each env.http call with a generation the environment bumps at the init, and pass on only
// an answer of the current one). The shared host cannot tell: the tag it hands env.http is a slot
// in the struct, which the new client reuses for its own requests. Such an answer may come as late
// as the request's wait_ms, and with cbc_cfg http_wait_ms 0 (wait_ms 0) the core sets no bound on
// how long it stays out.
//
// client/test/host/host_suite.h is the suite every host passes; a new environment proves itself
// with the same scripted server.
//
// WRITING A HOST (a browser framework's adapter, an app's own): nothing here names a platform, so a
// new host lives in its own tree, client/hosts/posix being the model:
//   1. the environment: a cbc_host_env whose calls each map onto one platform call (its socket,
//      HTTP client, timer, store and clock), cbc_host_init with the platform's seed, the session
//      loaded into h->sess, then cbc_host_pump;
//   2. the platform's callbacks into cbc_host_ws_opened/_message/_closed and
//      cbc_host_http_answered (with the tag env.http got), each from the platform's loop;
//   3. its adapter onto the host suite (cbhs_host, as client/test/unit_host_posix.c does, a line
//      a call), which runs every case the other hosts pass against the scripted server.
// It builds with CBC_HOST_SRC and CBC_HOST_INC (client/client.mk).
#ifndef CBC_HOST_H
#define CBC_HOST_H

#include <stddef.h>
#include <stdint.h>

#include "cbase/client.h"

typedef void (*cbc_host_fire_fn)(void *ctx);
typedef void (*cbc_host_event_fn)(void *ctx, cbc *c, const cbc_event *e, const uint8_t *bytes);

// One HTTP request, as env.http gets it. Every pointer lasts only for that call (the environment
// sends or copies them), except a borrowed body (cbc_call_ref's), which is the app's and lasts
// until the request is answered.
typedef struct {
    const char    *method;     // "GET" or "POST"
    const char    *path;       // the request target, path_len bytes (not NUL-terminated)
    size_t         path_len;
    const char    *auth;       // the Authorization value ("Bearer ..."), auth_len bytes; NULL: none
    size_t         auth_len;
    const uint8_t *body;       // body_len bytes, sent as application/octet-stream (0: no body)
    size_t         body_len;
    uint32_t       wait_ms;    // the deadline, the core's (cbc_req_hdr.wait_ms): no answer this long
                               // after it was sent is status 0 (abort it); 0: none
} cbc_host_req;

typedef struct {
    // The socket: 0 and its id in *id, or nonzero when none can be opened (the host waits
    // retry_first_ms and asks again). The id is the platform's own, passed as is: any value, 0
    // too (the host keeps whether it has one apart from the id). Its open, messages and close
    // come back through cbc_host_ws_*.
    int      (*ws_open)(void *ud, const char *path, uint32_t *id);
    void     (*ws_send)(void *ud, uint32_t id, const uint8_t *frame, size_t len);
    void     (*ws_close)(void *ud, uint32_t id);
    // One HTTP request, answered once, later, by cbc_host_http_answered(tag, ...).
    void     (*http)(void *ud, const cbc_host_req *r, void *tag);
    // A one-shot timer: an id (> 0); stop cancels one that has not fired (a stale id is harmless).
    uint32_t (*timer)(void *ud, uint32_t ms, cbc_host_fire_fn fire, void *ctx);
    void     (*timer_stop)(void *ud, uint32_t id);
    // The session's bytes (cbc_session_save's) to keep; len 0: forget them. NULL: nothing is kept.
    void     (*save)(void *ud, const uint8_t *p, size_t len);
    // Wall-clock epoch ms (cbase/client.h TIME).
    int64_t  (*now)(void *ud);
} cbc_host_env;

#define CBC_HOST_FRAME_CAP 8192    // a HELLO (a token) or a REQ (a path and an arena's body)
#define CBC_HOST_REQ_CAP   16384   // cbc_req_hdr, a path, "Bearer <jwt>", a copied body (an OIDC
                                   // sign-in's); a borrowed body is never here
#define CBC_HOST_SESS_CAP  4096    // the session's bytes: two tokens, a user id, a name
#define CBC_HOST_PEND_MAX  (CBC_CALLS_MAX + 2)   // every call over HTTP at once, and the refresh

typedef struct cbc_host cbc_host;
typedef struct {
    cbc_host *h;
    uint32_t  id;    // the request's (cbc_req_hdr.id)
    uint8_t   used;
} cbc_host_pend;

// The fields are this file's; the environment reads only sess (a buffer it may load into).
struct cbc_host {
    cbc c;
    const cbc_host_env *env;
    void *ud;                 // handed to every env call
    cbc_host_event_fn fn;
    void *ctx;
    uint32_t sock;            // the client's socket id (the platform's, any value) while has_sock
    uint8_t  has_sock;        // a socket is the client's: opening, or open
    uint8_t  open;            // its open was heard
    uint8_t  held;            // cbc_host_hold: the pump does nothing
    uint8_t  pad[1];
    uint32_t tick_timer, reopen_timer;
    cbc_host_pend pend[CBC_HOST_PEND_MAX];
    uint8_t frame[CBC_HOST_FRAME_CAP];
    uint8_t req[CBC_HOST_REQ_CAP];
    uint8_t sess[CBC_HOST_SESS_CAP];
};

// The client inited from cfg (its seed the environment's), on env (kept by pointer: it outlives
// the host), handing every event to fn. Nothing is sent: the environment loads the session, then
// pumps.
void cbc_host_init(cbc_host *h, const cbc_host_env *env, void *ud, const cbc_cfg *cfg, cbc_host_event_fn fn, void *ctx);
cbc *cbc_host_client(cbc_host *h);
int64_t cbc_host_now(const cbc_host *h);
// Sends what the client has waiting and sets its timer: after every call the app makes into it.
void cbc_host_pump(cbc_host *h);
// on 1: every pump does nothing until on 0 (a platform whose session store answers later holds
// the host until it has). Does not pump.
void cbc_host_hold(cbc_host *h, int on);

// What the platform reports of the socket env.ws_open gave (another id is not the client's: ignored).
void cbc_host_ws_opened(cbc_host *h, uint32_t id);
void cbc_host_ws_message(cbc_host *h, uint32_t id, const uint8_t *frame, size_t len);
void cbc_host_ws_closed(cbc_host *h, uint32_t id);
// The answer to the env.http call given tag: the status (0: it never arrived) and the body.
void cbc_host_http_answered(void *tag, int status, const uint8_t *body, size_t len);

#endif
