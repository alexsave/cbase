// cbc_cdom.h - THE C SDK'S BROWSER HOST: a cbase client (cbase/client.h) run by cdom, in the app's
// own wasm module. client/hosts/posix does the same for a native app's event loop.
//
// THE HOST CONTRACT IT IMPLEMENTS (cbase/client.h, top of file), each line one cdom call:
//   socket   while cbc_stream_wanted: cd_ws_open_cb(ws[s]://<page host><cbc_stream_path>), then
//            cbc_opened on its CD_WS_OPEN; every CD_WS_MSG to cbc_in; each cbc_next_out frame to
//            cd_ws_send (-1: cd_ws_close); on its CD_WS_CLOSE, cbc_closed, and the next open after
//            the ms it answers (a cd_timer_cb)
//   HTTP     each cbc_next_request to cd_http_cb (same origin, the path as the URL, "Bearer ..." as
//            the Authorization header), answered by cbc_on_response (CD_HTTP_FAILED, and
//            CD_HTTP_TIMEOUT past cfg.call_wait_ms: status 0); a CBC_RQ_REF request's body_len
//            bytes are the caller's, sent from cbc_req_body (cbc_call_ref), never copied
//   timer    one cd_timer_cb after cbc_due_ms, calling cbc_tick, then drain both outputs again; at
//            most one armed for the ticks and one for the reopen, and none re-armed at 0 ms while
//            the socket's close is on its way (the close's report pumps)
//   time     cd_epoch_ms() (the server's expiry times are epoch ms)
//   events   handed to the app's callback right after the call that raised them, with the bytes
//            their spans are into (the frame, the body; NULL after a tick or a close)
//   session  in cdom's store (cdom_store.h, IndexedDB) under a key the app names, written as the TS
//            SDK writes it to localStorage: standard base64 with padding (btoa) of cbc_session_save's
//            bytes. Loaded at start (cd_store_get_cb), saved on every CBC_EV_SESSION
//            (cd_store_put_cb; signed out: cd_store_del_cb). An anonymous client keeps none.
//   seed     cfg.seed from cd_random_bytes (crypto.getRandomValues)
//
// THE LOAD IS ASYNCHRONOUS (IndexedDB), so a host with a session key starts LOADING: until the
// store answers (a hit, a miss, or CD_STORE_DEADLINE_MS) the pump sends nothing and opens nothing,
// and the app's calls wait in the client. Then the host calls ready(ctx, c) once, where the app
// reads cbc_user_of(c)->signed_in and signs in if it must; without a key (or anonymous) ready is
// called before cbc_cdom_start returns.
//
// WHAT IT OWNS IN THE MODULE: nothing global. Its socket, requests, timers and store answers are
// cdom's _cb forms, in cdom's token range, so they never reach the app's hooks (cd_on_ws_*,
// cd_on_http, cd_on_timer, cd_on_store*) or meet the app's tokens. The app owns the cbc_cdom (one
// per client, about 50 KB: keep it static) and may run several, each under its own key.
//
// NEEDS: cdom's src/cdom.c, cdom_net.c, cdom_store.c, cdom_random.c and mini.c in the module,
// -isystem cdom/src/libc (the program's string.h), and cbase's client.mk (CBC_SRC, CBC_INC) plus
// lib/b64/b64.c (already in CBC_OIDC_SRC: list it once).
#ifndef CBC_CDOM_H
#define CBC_CDOM_H
#include <stdint.h>

#include "cbase/client.h"

// An event, as cbase/client.h raises it: bytes is what e->off/e->len index (NULL when none).
typedef void (*cbc_cdom_event_fn)(void *ctx, cbc *c, const cbc_event *e, const uint8_t *bytes);
// The session is known (loaded, or none kept): the app's first moment to sign in.
typedef void (*cbc_cdom_ready_fn)(void *ctx, cbc *c);

#define CBC_CDOM_FRAME_CAP 8192    // a HELLO (a token, SF_TOKEN_MAX) or a REQ (a path and an arena's body)
#define CBC_CDOM_REQ_CAP   16384   // cbc_req_hdr, a path, "Bearer <jwt>", an arena's body (an OIDC sign-in's
                                   // ID token); a borrowed body is never in it
#define CBC_CDOM_HDR_CAP   2200    // the request's header lines, written behind it in the same buffer
#define CBC_CDOM_SESS_CAP  4096    // the session's bytes: two tokens, a user id, a name
#define CBC_CDOM_TEXT_CAP  5464    // their base64: 4 * ceil(CBC_CDOM_SESS_CAP / 3)
#define CBC_CDOM_PEND_MAX  (CBC_CALLS_MAX + 2)   // every call over HTTP at once, and the refresh

typedef struct cbc_cdom cbc_cdom;
typedef struct {
    cbc_cdom *h;
    uint32_t id;   // the request's (cbc_req_hdr.id)
    uint8_t used;
} cbc_cdom_pend;

// The fields are this file's; the app allocates the struct and reads none of them.
struct cbc_cdom {
    cbc c;
    cbc_cdom_event_fn fn;
    cbc_cdom_ready_fn ready;
    void *ctx;
    const char *key;    // the store key (NULL: none kept)
    uint32_t klen;
    uint32_t sock;      // the client's socket id + 1; 0: none
    uint8_t open;       // its CD_WS_OPEN was heard
    uint8_t closing;    // cd_ws_close sent, its CD_WS_CLOSE not yet heard
    uint8_t loading;    // the session's cd_store_get_cb not yet answered
    uint8_t pad;
    uint32_t tick_timer, reopen_timer;
    cbc_cdom_pend pend[CBC_CDOM_PEND_MAX];
    uint8_t frame[CBC_CDOM_FRAME_CAP];
    uint8_t req[CBC_CDOM_REQ_CAP];
    uint8_t sess[CBC_CDOM_SESS_CAP];
    char text[CBC_CDOM_TEXT_CAP + 1];
};

// Starts the host on h: cfg (NULL: cbc_cfg_default; the seed is always the browser's), the store key
// the session lives under (NULL: none kept; ignored when cfg->anonymous), the app's event callback,
// and ready (NULL: none). After any call the app makes into the client (cbc_sign_in, cbc_call,
// cbc_sub, ...) it calls cbc_cdom_pump.
void cbc_cdom_start(cbc_cdom *h, const cbc_cfg *cfg, const char *session_key, cbc_cdom_event_fn fn,
                    cbc_cdom_ready_fn ready, void *ctx);
cbc *cbc_cdom_client(cbc_cdom *h);
// Sends what the client has waiting and sets its timer: after every call the app makes into it.
void cbc_cdom_pump(cbc_cdom *h);
// epoch ms, the `now` every cbc call takes.
int64_t cbc_cdom_now(void);
#endif
