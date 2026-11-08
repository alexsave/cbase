// cbc_posix.h - THE C SDK'S NATIVE HOST: a cbase client (cbase/client.h) run by a native C app's
// event loop. The app's loop owns the socket, the HTTP client and the timers (cbc_posix_io); the
// shared host (client/hosts/common/cbc_host.h) owns everything cbase/client.h asks of a host on top
// of them, and this file is its environment (the io, the clock, the store, the seed):
//
//   socket   while cbc_stream_wanted: io.ws_open(cbc_stream_path), then cbc_opened when the loop
//            says it opened (cbc_posix_ws_opened); every message to cbc_in; each cbc_next_out frame
//            to io.ws_send (-1: io.ws_close); on its close (cbc_posix_ws_closed), cbc_closed, and
//            the next open after the ms it answers (an io.timer)
//   HTTP     each cbc_next_request to io.http (the path as the request target, "Bearer ..." as the
//            Authorization value), answered by cbc_on_response (status 0: it never arrived); a
//            borrowed body (CBC_RQ_REF, cbc_call_ref) goes to io.http as the caller's own pointer
//   timer    one io.timer after cbc_due_ms, calling cbc_tick, then drain both outputs again; at
//            most one armed for the ticks and one for the reopen
//   time     io.now, else clock_gettime(CLOCK_REALTIME): wall-clock epoch ms
//   events   handed to the app's callback right after the call that raised them, with the bytes
//            their spans are into (the frame, the body; NULL after a tick or a close)
//   session  io.load at start (cbc_session_load), io.save on every CBC_EV_SESSION (n 0: forget it)
//   seed     cfg.seed from io.entropy, else getentropy()
//
// THE LOOP'S SIDE: every callback this file hands io (an HTTP answer, a timer) and every
// cbc_posix_ws_* the loop calls comes later, from the loop, never from inside the io call that
// started it; and after the app calls into the client (cbc_sign_in, cbc_call, cbc_sub, ...) it
// calls cbc_posix_pump. Nothing here allocates or keeps a global: the app owns the cbc_posix (one
// per client, about 45 KB), and a process may run any number.
//
// client/test/host/host_suite.h is the suite every host passes (unit_host_posix.c runs it on this
// one); a new host proves itself with the same scripted server.
#ifndef CBC_POSIX_H
#define CBC_POSIX_H

#include <stddef.h>
#include <stdint.h>

#include "cbase/client.h"
#include "cbc_host.h"   // client/hosts/common

typedef void (*cbc_posix_done_fn)(void *ctx, int status, const uint8_t *body, size_t len);
typedef void (*cbc_posix_fire_fn)(void *ctx);

typedef struct {
    void *ud;   // handed back to every call below
    // The socket: an id (> 0), or 0 when none can be opened (the host waits retry_first_ms and
    // asks again). Its open, messages and close come back through cbc_posix_ws_*.
    uint32_t (*ws_open)(void *ud, const char *path);
    void     (*ws_send)(void *ud, uint32_t id, const uint8_t *frame, size_t len);
    void     (*ws_close)(void *ud, uint32_t id);
    // One HTTP request ("GET" or "POST"; auth NULL when none; a body of body_len bytes, sent as
    // application/octet-stream), answered once through done: the status (0: it never arrived)
    // and the body. path, auth and body last only for this call (the io sends or copies them),
    // except a borrowed body (cbc_call_ref's), which is the app's and lasts until done is called.
    void (*http)(void *ud, const char *method, const char *path, size_t path_len, const char *auth, size_t auth_len,
                 const uint8_t *body, size_t body_len, cbc_posix_done_fn done, void *ctx);
    // A one-shot timer: an id (> 0); stop cancels one that has not fired.
    uint32_t (*timer)(void *ud, uint32_t ms, cbc_posix_fire_fn fire, void *ctx);
    void     (*timer_stop)(void *ud, uint32_t id);
    // The session's bytes (cbc_session_save's): load answers their length, 0 for none; save with
    // len 0 forgets them. Both NULL: nothing is kept.
    size_t (*load)(void *ud, uint8_t *out, size_t cap);
    void   (*save)(void *ud, const uint8_t *p, size_t len);
    // NULL for the defaults: clock_gettime(CLOCK_REALTIME) in ms, getentropy().
    int64_t (*now)(void *ud);
    int     (*entropy)(void *ud, void *out, size_t len);   // 0, or -1
} cbc_posix_io;

typedef void (*cbc_posix_event_fn)(void *ctx, cbc *c, const cbc_event *e, const uint8_t *bytes);

// The fields are this file's; the app allocates the struct and reads none of them.
typedef struct cbc_posix {
    cbc_host host;    // the shared host (client/hosts/common/cbc_host.h), on the environment below
    cbc_posix_io io;
} cbc_posix;

// Starts the host: cfg (NULL: cbc_cfg_default; the seed is always the host's entropy), the app's
// event callback. Loads the session and pumps. 0, or -1 when there is no entropy for the seed.
int  cbc_posix_start(cbc_posix *h, const cbc_posix_io *io, const cbc_cfg *cfg, cbc_posix_event_fn fn, void *ctx);
cbc *cbc_posix_client(cbc_posix *h);
// Sends what the client has waiting and sets its timer: after every call the app makes into it.
void cbc_posix_pump(cbc_posix *h);
// The epoch ms every cbc call takes.
int64_t cbc_posix_now(const cbc_posix *h);

// What the loop reports of the socket io.ws_open gave (another id is not the client's: ignored).
void cbc_posix_ws_opened(cbc_posix *h, uint32_t id);
void cbc_posix_ws_message(cbc_posix *h, uint32_t id, const uint8_t *frame, size_t len);
void cbc_posix_ws_closed(cbc_posix *h, uint32_t id);

#endif
