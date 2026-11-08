// host_suite.h - THE HOST SUITE: one test every host of the cbase client passes (cbase/client.h's
// host contract), driven against a scripted fake server, so a new host proves itself with the
// same cases the others did.
//
// A host is the code between a platform's I/O and the client core: it opens the socket while
// cbc_stream_wanted, hands every message to cbc_in and sends what cbc_next_out writes, sends what
// cbc_next_request writes (with a CBC_RQ_REF request, the head's body_len bytes from cbc_req_body:
// the caller's buffer, a 64 KB case checks it byte for byte and on every retry) and answers it with
// cbc_on_response, keeps one timer on cbc_due_ms and
// calls cbc_tick, keeps the session and seeds the jitter. The suite cannot reach a platform's
// sockets, so it gives the host a WORLD instead: a fake network, clock, timer wheel, store and
// entropy (the cbhs_* calls below), behind which the suite's script plays the server. A host runs
// on it through an ADAPTER (cbhs_host): a few lines that map the host's platform calls onto the
// world (cdom's cd_ws_open, cd_http_cb, cd_timer_cb, cd_epoch_ms; cbc_posix.h's io) and the
// world's deliveries onto the host's entry points.
//
//   #include "host_suite.h"
//   static const cbhs_host my_host = { "mine", start, client, pump, ws_opened, ws_message, ws_closed };
//   int main(void) { return cbhs_run(&my_host); }
//
// THE WORLD'S RULES, which every platform's loop keeps and so every host may count on: nothing is
// delivered from inside the call that started it (an HTTP answer, a timer, a socket's open or
// message comes later, when the script says so), and time moves only when the script moves it,
// firing the timers that fall due in order. Every check is on what crosses the host's two edges:
// what the server received and what the app was told. client/test/unit_host_posix.c runs it on
// client/hosts/posix.
#ifndef CBHS_HOST_SUITE_H
#define CBHS_HOST_SUITE_H

#include <stddef.h>
#include <stdint.h>

#include "cbase/client.h"

typedef struct cbhs_world cbhs_world;

// ---- the world, as a host's I/O reaches it --------------------------------------------------------
int64_t  cbhs_now(cbhs_world *w);                                   // epoch ms
uint32_t cbhs_ws_open(cbhs_world *w, const char *path);              // a socket id (> 0)
void     cbhs_ws_send(cbhs_world *w, uint32_t id, const uint8_t *frame, size_t len);
void     cbhs_ws_close(cbhs_world *w, uint32_t id);
typedef void (*cbhs_done_fn)(void *ctx, int status, const uint8_t *body, size_t len);
// wait_ms: the request's deadline as the core set it (cbc_req_hdr.wait_ms; 0: none), which the
// world keeps as a platform's HTTP client does: unanswered by then, it answers status 0.
void     cbhs_http(cbhs_world *w, const char *method, const char *path, size_t path_len, const char *auth,
                   size_t auth_len, const uint8_t *body, size_t body_len, uint32_t wait_ms, cbhs_done_fn done,
                   void *ctx);
typedef void (*cbhs_fire_fn)(void *ctx);
uint32_t cbhs_timer(cbhs_world *w, uint32_t ms, cbhs_fire_fn fire, void *ctx);   // an id (> 0)
void     cbhs_timer_stop(cbhs_world *w, uint32_t id);
size_t   cbhs_load(cbhs_world *w, uint8_t *out, size_t cap);           // the kept session's bytes
void     cbhs_save(cbhs_world *w, const uint8_t *p, size_t len);       // len 0: forget them
int      cbhs_entropy(cbhs_world *w, void *out, size_t len);           // deterministic: 0

// The app's side: a host hands every event to this, with the bytes its span is into.
void cbhs_event(cbhs_world *w, cbc *c, const cbc_event *e, const uint8_t *bytes);

// ---- the host, as the suite drives it -------------------------------------------------------------
typedef struct {
    const char *name;
    // A host started on w (cfg: the suite's timings; the host takes its seed from cbhs_entropy and
    // its session from cbhs_load), handing every event to cbhs_event. Answers the host's own state.
    void *(*start)(cbhs_world *w, const cbc_cfg *cfg);
    cbc  *(*client)(void *host);
    void  (*pump)(void *host);   // after the app calls into the client
    // The world delivers the host's socket through these (its HTTP answers and timers go to the
    // callbacks the host handed cbhs_http and cbhs_timer).
    void (*ws_opened)(void *host, uint32_t id);
    void (*ws_message)(void *host, uint32_t id, const uint8_t *frame, size_t len);
    void (*ws_closed)(void *host, uint32_t id);
} cbhs_host;

// Every case, on fresh worlds; prints each failure and a summary. Answers the process's exit code.
int cbhs_run(const cbhs_host *h);

#endif
