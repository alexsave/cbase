// cbc_cdom.h - THE CDOM ADAPTER: the C SDK's browser adapter, a cbase client (cbase/client.h) run by cdom
// (github.com/alexsave/cdom), in the app's own wasm module. The shared host
// (client/hosts/common/cbc_host.h) owns everything cbase/client.h asks of a host; this file is its
// environment on cdom's I/O, one cdom call each:
//
//   socket   cd_ws_open_cb(ws[s]://<the page's host><cbc_stream_path>), ws:// on an http: page and
//            wss:// on an https: one; on any other page (file:, a blob:, an extension's) no socket
//            is made and the app gets a CBC_EV_SOCKET_FAILED (CBC_SOCK_UNSUPPORTED), again after
//            each reconnect wait. CD_WS_OPEN, CD_WS_MSG and CD_WS_CLOSE go to cbc_host_ws_*, with
//            cdom's socket id as it is (0 is an id)
//   HTTP     cd_http_cb on the page's own origin (the path as the URL), "Authorization: Bearer ..."
//            and "Content-Type: application/octet-stream" as its header lines, the core's deadline
//            (cbc_req_hdr.wait_ms) as its deadline_ms; CD_HTTP_FAILED and CD_HTTP_TIMEOUT are
//            status 0. A borrowed body (cbc_call_ref) is read from the caller's buffer, never copied
//   timer    cd_timer_cb and cd_timer_stop
//   time     cd_epoch_ms (the server's expiry times are epoch ms)
//   session  in cdom's store (cdom_store.h, IndexedDB) under a key the app names, written as the TS
//            SDK writes it to localStorage: standard base64 with padding (btoa) of
//            cbc_session_save's bytes. Saved on every CBC_EV_SESSION (cd_store_put_cb; signed out:
//            cd_store_del_cb). An anonymous client keeps none
//   seed     cfg.seed from cd_random_bytes (crypto.getRandomValues)
//
// THE LOAD IS ASYNCHRONOUS (IndexedDB): with a key, cbc_cdom_start asks the store and holds the host
// (cbc_host_hold) until it answers (a hit, a miss, or CD_STORE_DEADLINE_MS): the app's calls wait in
// the client meanwhile. Then the host calls ready(ctx, c) once, where the app reads
// cbc_user_of(c)->signed_in and signs in if it must, and pumps. Without a key (or anonymous), ready is
// called before cbc_cdom_start returns.
//
// A STOP AND A NEW START (cbc_host.h, A STOP AND RESTART): cbc_cdom_stop is the shared host's
// cbc_host_stop (its timers cancelled, its socket closed, nothing more sent or told the app), from the
// loop or from inside the app's own callback (an event, ready). A start is from the loop only: one
// from inside a callback traps, as the call that callback came from still runs on the handle. cdom cannot cancel a cd_http_cb request or a store get, so every one is stamped
// with the handle's generation, which each start moves on, in a slot of the handle's own that stays
// reserved until cdom answers; an answer of an older generation is dropped there. A late socket event
// is the shared host's to ignore (cdom does not hand an id out again before its close is delivered).
// Starting a live handle (started, not stopped) traps. The handle must be zeroed before its first start
// (static storage is): its slots outlive a stop.
//
// THE HANDLE LIVES FOR THE PAGE: never free it, nor let it go out of scope, stopped or not. Every
// context the adapter hands cdom points into it (a slot of owed[], the handle itself, the shared
// host's), and cdom may call back into it after any stop: a request's answer up to its wait_ms
// later, and with cbc_cfg http_wait_ms 0 (no deadline) with no limit at all; a store get up to
// CD_STORE_DEADLINE_MS; a socket's close when the browser reports it. Keep it static.
//
// WHAT IT OWNS IN THE MODULE: nothing global. Its socket, requests, timers and store answers are
// cdom's _cb forms, in cdom's token range, so they never reach the app's hooks (cd_on_ws_*,
// cd_on_http, cd_on_timer, cd_on_store*) or meet the app's tokens. The app owns the cbc_cdom (one per
// client, about 50 KB, static: THE HANDLE LIVES FOR THE PAGE above) and may run several, each under its
// own key.
//
// NEEDS: cdom's src/cdom.c, cdom_net.c, cdom_store.c, cdom_random.c and mini.c in the module,
// -isystem <cdom>/src/libc (the program's string.h), -I<cdom>/src, and cbase's client.mk (CBC_SRC,
// CBC_HOST_SRC, CBC_INC, CBC_HOST_INC) plus lib/b64/b64.c (in CBC_OIDC_SRC too: list it once).
// README.md here: how to build and test it, and the cdom it is pinned to.
#ifndef CBC_CDOM_H
#define CBC_CDOM_H

#include <stddef.h>
#include <stdint.h>

#include "cbase/client.h"
#include "cbc_host.h"   // client/hosts/common

// An event, as cbase/client.h raises it: bytes is what e->off/e->len index (NULL when none).
typedef void (*cbc_cdom_event_fn)(void *ctx, cbc *c, const cbc_event *e, const uint8_t *bytes);
// The session is known (loaded, or none kept): the app's first moment to sign in.
typedef void (*cbc_cdom_ready_fn)(void *ctx, cbc *c);

#define CBC_CDOM_KEY_MAX  128    // the store key's bytes, kept in the handle
#define CBC_CDOM_TEXT_CAP (4 * ((CBC_HOST_SESS_CAP + 2) / 3))   // the session's base64
// The request's header lines: "Authorization: Bearer <access>\n" and the body's Content-Type line.
#define CBC_CDOM_HDR_CAP  (sizeof "Authorization: Bearer \n" - 1 + CBW_ACCESS_MAX + \
                           sizeof "Content-Type: application/octet-stream\n" - 1)
// The answers cdom may still owe: every request a generation can have out and its load, twice over,
// so a new start finds room while the last one's answers come in. Past that, a request is answered
// status 0 (and a load a miss) from a 0 ms timer, and the slot table never grows.
#define CBC_CDOM_OWED_MAX (2 * (CBC_HOST_PEND_MAX + 1))

typedef struct cbc_cdom cbc_cdom;
typedef struct {
    cbc_cdom *h;
    void     *tag;    // the shared host's (cbc_host_http_answered); NULL: the session's load
    uint32_t  gen;    // the start it was asked under
    uint8_t   used;   // cdom has not answered it yet
    uint8_t   pad[3];
} cbc_cdom_owed;

// The fields are this file's; the app allocates the struct (zeroed) and reads none of them.
struct cbc_cdom {
    cbc_host host;    // the shared host, on the environment cbc_cdom.c gives it
    cbc_cdom_ready_fn ready;
    void *ctx;
    uint32_t gen;     // moved on by every start
    uint8_t  live;    // started and not stopped
    uint8_t  in_ready;   // ready is running
    uint8_t  pad[2];
    uint32_t klen;    // the store key's length (0: no session kept)
    char     key[CBC_CDOM_KEY_MAX];
    cbc_cdom_owed owed[CBC_CDOM_OWED_MAX];
    void    *late[CBC_HOST_PEND_MAX + 1];   // answered status 0 (NULL: the load a miss) by late_timer
    uint32_t n_late, late_timer;
    char     text[CBC_CDOM_TEXT_CAP + 1];
    char     hdr[CBC_CDOM_HDR_CAP];
};

// Starts the host on h (zeroed, or stopped): cfg (NULL: cbc_cfg_default; the seed is always the
// browser's), the store key the session lives under (NULL: none kept; ignored when cfg->anonymous;
// copied, at most CBC_CDOM_KEY_MAX bytes), the app's event callback, and ready (NULL: none). 0, or -1
// (nothing started) when the key is longer or the page has no crypto.getRandomValues. Traps on a live
// handle, and inside the app's event callback or ready (start from the loop only). After any call the
// app makes into the client (cbc_sign_in, cbc_call, cbc_sub, ...) it calls cbc_cdom_pump.
int  cbc_cdom_start(cbc_cdom *h, const cbc_cfg *cfg, const char *session_key, cbc_cdom_event_fn fn,
                    cbc_cdom_ready_fn ready, void *ctx);
// Stops it, from the loop or from inside the app's callback: its timers cancelled, its socket closed,
// every answer cdom still owes it dropped when it comes. Nothing more is sent or told the app (not the
// rest of the events of a callback it is called from); a stopped handle may be started again, from the
// loop. A no-op on a handle that is not live.
void cbc_cdom_stop(cbc_cdom *h);
cbc *cbc_cdom_client(cbc_cdom *h);
// Sends what the client has waiting and sets its timer: after every call the app makes into it.
void cbc_cdom_pump(cbc_cdom *h);
// Epoch ms, the `now` every cbc call takes.
int64_t cbc_cdom_now(void);

#endif
