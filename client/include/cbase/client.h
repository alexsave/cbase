// cbase/client.h - THE C_BASE CLIENT: the other half of `cbase serve`'s socket (GET /ws and
// GET /ws/public, lib/stream_frame.h) and of the platform's sessions (/v1/auth,
// cbase/auth_wire.h). One client is one user (or one anonymous listener) talking to one app.
// cbase/docs/CLIENT_DESIGN.md is the design; this is the contract.
//
// SANS-IO. The host owns the socket, the HTTP client and one timer, and nothing else. It passes in
// every byte it receives and the time (now, epoch ms), and sends every byte this file hands out:
//
//   socket   while cbc_stream_wanted: open cbc_stream_path (ws or wss, the page's scheme), then
//            cbc_opened; every binary message to cbc_in; send each frame cbc_next_out writes
//            (-1: close the socket); on any close, cbc_closed, and reopen after the ms it answers
//   HTTP     send each request cbc_next_request writes, answer it with cbc_on_response
//            (status 0: it never arrived); a CBC_RQ_REF request's body is the caller's, at
//            cbc_req_body, valid until the call's CBC_EV_DONE (cbc_call_ref)
//   timer    wake after cbc_due_ms and call cbc_tick, then drain both outputs again
//   events   cbc_in, cbc_on_response, cbc_closed and cbc_tick raise events: read them with
//            cbc_n_events / cbc_event_at after each, before calling any of the four again. A span
//            (off, len) is into the bytes passed to the call that raised it (cbc_event).
// client/hosts/<name>/ holds hosts that keep this (posix: a native app's event loop), each the
// shared host (client/hosts/common/cbc_host.h) on its platform's I/O, and every host passes one
// suite against a scripted server (client/test/host/host_suite.h).
//
// ONE AUTHENTICATION PER SOCKET (CLIENT_DESIGN.md 6.4). A session is made over HTTP
// (cbc_sign_in, and the refresh this file sends itself), the socket says HELLO with its token once,
// and every call after that rides the socket as SF_REQ with no token. A call made while the socket
// is down waits for READY. HTTP is left for /v1/auth, an anonymous client's reads (/ws/public
// carries no REQ), and a call flagged CBC_C_HTTP. A socket never outlives its token: the client
// refreshes refresh_early_ms before expiry and then rolls the socket (cbc_next_out answers -1 once
// no socket call is in flight, and cbc_closed answers 0 for that close).
//
// TIME. Every `now` is wall-clock epoch ms (the server's token expiries are epoch ms: a host
// passes Date.now(), clock_gettime(CLOCK_REALTIME), cd_epoch_ms(), never a monotonic clock). If it
// steps back, each timer waits on until the clock passes its old deadline again, and a refresh
// comes late; a token the server already finds expired is answered 401 and refreshed then.
//
// REQUIRES (client/client.mk CBC_REQUIRES; client/test/requires_gate.sh links the core's objects
// against these alone, and fails on anything else they leave undefined):
//   <string.h>  memcpy memmove memset memcmp strlen
//   <limits.h> <stddef.h> <stdint.h>
// and nothing more: no allocator, no stdio, no clock, no other C library function. A freestanding
// program (a wasm32 module built -nostdlib) defines the five itself.
//
// NO ALLOCATION, NO CLOCK, NO GLOBALS. The caller owns the cbc (CBC_SIZE bytes, at most
// CBC_SIZE_BUDGET), so a load generator holds thousands in one process; randomness is the
// reconnect jitter's, from cfg.seed (lib/rng.h). The structs a host reads (cbc_event,
// cbc_req_hdr, cbc_user) and writes (cbc_cfg) are read in other languages through the accessors
// structgen generates from this header; no host spells a byte offset.
#ifndef CBASE_CLIENT_H
#define CBASE_CLIENT_H

#include <stddef.h>
#include <stdint.h>

#include "cbase/auth_wire.h"

#define CBC_SUBS_MAX    16     // rooms held at once
#define CBC_CALLS_MAX   16     // calls in flight or waiting, of every kind
#define CBC_EVENTS_MAX  (CBC_CALLS_MAX + 4)   // what one call can raise: every call finished, and a few
#define CBC_ARENA       4096   // the paths and bodies of the calls waiting (a call's own bytes)
#define CBC_PATH_MAX    256    // == SF_PATH_MAX
#define CBC_ROOM_MAX    64     // == SF_GID_MAX
#define CBC_SIZE_BUDGET 12288  // sizeof(cbc), pinned below
#define CBC_HTTP_MIN_BPS 16000 // the slowest upload a body's deadline makes room for: 16 kB/s (128 kbit/s)

// ---- configuration -----------------------------------------------------------------------------
typedef struct {
    uint32_t ping_ms;           // a PING when nothing went out this long (30000)
    uint32_t pong_ms;           // after a PING, no frame of any kind this long: half-open, close it (10000)
    uint32_t ack_wait_ms;       // a SUB, or the HELLO, unanswered this long is given up on (10000)
    uint32_t retry_first_ms;    // every backoff's first wait, and the reconnect floor (500)
    uint32_t retry_max_ms;      // every backoff's cap (10000)
    uint32_t refresh_early_ms;  // refresh the access token this long before it expires (60000)
    uint32_t force_refresh_ms;  // a socket refused before READY refreshes at most this often (600000)
    uint32_t call_wait_ms;      // a socket call unanswered this long is lost (15000)
    // THE HTTP DEADLINE (cbc_req_hdr.wait_ms), the core's for every host: a request is given
    // http_wait_ms, and its body the time it takes at CBC_HTTP_MIN_BPS on top, so a large borrowed
    // upload on a slow uplink is not cut off while it is still going (64 KB: 4 s more; 100 MB: 104
    // min). 0: no deadline. At most 2^31.
    uint32_t http_wait_ms;      // (15000)
    uint32_t pad0;
    uint64_t seed;              // the reconnect jitter's generator: per client, from the host's entropy
    // 1: no session and every call over HTTP; a socket (GET /ws/public, an empty HELLO) is open
    // only while a room is held (cbc_sub), so an anonymous client that holds none is plain HTTP.
    // WHICH MODE: an app with no accounts sets anonymous; an app with accounts leaves it 0 and
    // signs in (cbc_sign_in), its socket open (GET /ws) while a user is signed in. Before that a
    // call without CBC_C_AUTH goes over HTTP at once and waits for no socket.
    uint8_t  anonymous;
    uint8_t  pad[7];
} cbc_cfg;

// The defaults above, with seed 0 (a host sets its own).
void cbc_cfg_default(cbc_cfg *cfg);

// ---- what the host reads -----------------------------------------------------------------------
enum {
    CBC_EV_READY       = 1,   // the socket's HELLO was taken
    CBC_EV_PUSH        = 2,   // an app frame: push is its kind (frame[1]); the span is the whole frame
    CBC_EV_SUB_LIVE    = 3,   // what/room went live (an app reads what it missed now)
    CBC_EV_SUB_REFUSED = 4,   // what/room was refused (status); it is asked again by itself
    CBC_EV_DONE        = 5,   // call `ticket` finished: status, why (0 = done), and its body's span
    CBC_EV_SESSION     = 6,   // the session changed (signed in, refreshed, ended): save it again
    CBC_EV_BAD         = 7,   // a frame that is not this protocol's
    // Raised by a host, never by the core (the core never sees a socket that did not open): no
    // socket could be opened, why a CBC_SOCK_*. The next try is after the reconnect wait, as
    // after any close; one event a try.
    CBC_EV_SOCKET_FAILED = 8,
};

// Why a host could not open a socket (CBC_EV_SOCKET_FAILED.why).
#define CBC_SOCK_UNSUPPORTED 1   // none can be opened here (a page's scheme, no socket API)
#define CBC_SOCK_REFUSED     2   // the platform refused to make one (a construction error)
#define CBC_SOCK_FAILED      3   // made, and it failed or closed before it opened (refused, no answer)

// Why a call finished without its answer (CBC_EV_DONE.why). The server's own are the AskRefusal
// whys (auth_wire.h CBW_WHY_*); CBC_WHY_LOST is this file's: it never arrived, or a write may have
// landed and is not sent again.
#define CBC_WHY_LOST 64

typedef struct {
    uint8_t  kind;      // CBC_EV_*
    uint8_t  what;      // SUB_*: the room's kind (the app's, as in SF_SUB)
    uint8_t  push;      // PUSH: the app frame's kind
    uint8_t  why;       // DONE: 0, CBW_WHY_*, or CBC_WHY_LOST
    uint16_t status;    // DONE: the HTTP status (0: never arrived); SUB_REFUSED: the REPLY's
    uint8_t  room_len;
    uint8_t  pad;
    uint32_t ticket;    // DONE
    int32_t  code;      // DONE: the refusal's code and detail
    int32_t  detail;
    uint32_t off, len;  // PUSH, DONE: the span (below)
    char     room[CBC_ROOM_MAX];   // SUB_*
} cbc_event;
// THE SPAN. A PUSH's and a DONE's body is off/len into the bytes passed to the call that raised
// the event: cbc_in's frame, or cbc_on_response's body. Those bytes last only until that call
// returns. A host that keeps the I/O itself (client/hosts/common/cbc_host.h, and every adapter on
// it under client/hosts/) hands its app those same bytes with every event, as `bytes`, so
// the body is len bytes at bytes + off. bytes is NULL when the call that raised the event had none
// (cbc_tick, cbc_closed, a body cbc_on_response was not given), and a span is read only when it is
// not NULL.

// A request cbc_next_request writes: this head, then path_len bytes of path, auth_len bytes of the
// Authorization value ("Bearer ..."; none when 0), body_len bytes of body. With CBC_RQ_REF the body
// is not written: its body_len bytes are the caller's, at cbc_req_body (cbc_call_ref). wait_ms is
// its deadline, which the host hands its HTTP client as is (fetch's abort, a curl timeout): no
// answer this long after it was sent is status 0. 0: none (cbc_cfg http_wait_ms).
// CBC_RQ_BODY is about what the CORE reads, never about what the app gets. The core reads a 2xx
// body only for its own requests (a sign-in's session, the refresh), so only those set it; an app
// call's 2xx body is the app's, and the core needs only its length: its CBC_EV_DONE is the span
// (0, len) into the bytes the host holds. A host still hands the app the whole body (THE SPAN
// above): the shared host passes cbc_host_http_answered's body to the app with the DONE whatever
// this flag says (client/test/host/host_suite.c holds it), and the TypeScript and Swift layers
// resolve the call with it. What the flag saves is a copy into the core's buffer (the wasm
// module's io), so a large answer is never limited by it.
#define CBC_RQ_BODY 0x1   // the core reads the answer's body: pass it to cbc_on_response (else it needs
                          // a 2xx body's length only, and a non-2xx body, a refusal, as always)
#define CBC_RQ_REF  0x2   // the body is borrowed: send cbc_req_body(id)'s body_len bytes after the head
typedef struct {
    uint32_t id;        // cbc_on_response's
    uint8_t  method;    // CBC_GET, CBC_POST
    uint8_t  flags;     // CBC_RQ_*
    uint16_t path_len;
    uint16_t auth_len;
    uint16_t pad;
    uint32_t body_len;
    uint32_t wait_ms;   // the deadline: cbc_cfg http_wait_ms, and the body's time at CBC_HTTP_MIN_BPS
} cbc_req_hdr;

typedef struct {
    uint8_t  signed_in;
    uint8_t  user_id_len;
    uint8_t  name_len;
    uint8_t  pad[5];
    int64_t  expires_at;    // the access token's, epoch ms
    char     user_id[CBW_ID_MAX];
    char     name[CBW_NAME_MAX];
} cbc_user;

// ---- the client --------------------------------------------------------------------------------
// The fields are this file's; a host reads none of them (the readers below do), and only allocates.
typedef struct {
    uint8_t  state;          // CBC_SUB_* (client.c)
    uint8_t  what;
    uint8_t  room_len;
    uint8_t  holds;
    uint32_t id;             // its SUB's request id (top bit set)
    uint32_t backoff;        // the refusal's last wait
    int64_t  at;             // SENT: when; REFUSED: when to ask again
    char     room[CBC_ROOM_MAX];
} cbc_sub_slot;

typedef struct {
    uint8_t  state;          // CBC_CALL_* (client.c)
    uint8_t  kind;           // CBC_K_* (client.c): the app's call, a sign-in, a sign-out
    uint8_t  method;
    uint8_t  flags;          // CBC_C_*
    uint8_t  tries;          // sent again after a loss
    uint8_t  busy;           // sent again after a 503 busy
    uint8_t  reauthed;       // sent again after a 401 and a refresh
    uint8_t  pad;
    uint32_t ticket;
    uint32_t id;             // the HTTP request's or the SF_REQ's
    uint16_t path_len;
    uint16_t at_arena;       // path then body, in the arena (a borrowed body is not)
    uint32_t body_len;
    int64_t  at;             // SENT: when; WAIT: when to send again
    const uint8_t *ref;      // cbc_call_ref's borrowed body (NULL: the body is in the arena)
} cbc_call_slot;

typedef struct cbc {
    cbc_cfg  cfg;
    uint64_t rng;
    int64_t  forced_last;       // the last pre-READY close that forced a refresh (kept past a session)
    // the session (the NW_SESSION fields, auth_wire.h), user to refresh: cleared as one span
    cbc_user user;
    uint16_t access_len;
    uint8_t  refresh_len;
    uint8_t  ending;            // a sign-out is out: no new call, the socket closes
    uint8_t  refresh_out;       // the refresh is sent and unanswered
    uint8_t  refresh_forced;    // refresh now, whatever the expiry says
    uint8_t  refresh_tries;
    uint8_t  pad0;
    uint32_t refresh_id;
    int64_t  refresh_at;        // a failed refresh's next try
    char     access[CBW_ACCESS_MAX];
    char     refresh[CBW_REFRESH_MAX];
    // the socket
    uint8_t  sock;              // CBC_SOCK_* (client.c)
    uint8_t  roll;              // the close cbc_next_out asked for is the token's planned roll
    uint8_t  closing;           // cbc_next_out asked for the close (-1); cbc_closed not yet called
    uint8_t  pad1;
    uint32_t next_sock_id, next_sub_id, next_http_id, next_ticket;
    uint32_t reconnect_prev;    // the last reconnect wait (0: none since READY)
    int64_t  hello_at, last_out, ping_at;
    int64_t  hello_exp;         // the expiry of the token the socket's HELLO carried
    cbc_sub_slot  subs[CBC_SUBS_MAX];
    cbc_call_slot calls[CBC_CALLS_MAX];
    uint16_t arena_used;
    int16_t  err;               // the last refusal of a call into this file (CBC_E_*)
    int32_t  n_events;
    cbc_event events[CBC_EVENTS_MAX];
    uint8_t  arena[CBC_ARENA];
    // a provider's sign-in (cbc_oidc_begin): what it kept, all NUL-terminated
    uint8_t  oidc_on;
    char     oidc_nonce[44];    // the raw nonce, base64url of 32 bytes
    char     oidc_hash[65];     // its SHA-256, lowercase hex: the token's nonce claim
    char     oidc_state[23];    // base64url of 16 bytes
} cbc;

#define CBC_SIZE sizeof(cbc)
_Static_assert(sizeof(cbc) <= CBC_SIZE_BUDGET, "cbc grew past its budget (CLIENT_DESIGN.md 3.1)");

// Refusals of a call into this file (cbc_error).
#define CBC_E_ARG   (-1)   // a path, room or body past its bound, a bad method, or a GET with a body
#define CBC_E_FULL  (-2)   // CBC_CALLS_MAX calls or CBC_SUBS_MAX rooms already, or the arena is full
#define CBC_E_AUTH  (-3)   // the call needs a user and nobody is signed in (or the session is ending)
#define CBC_E_CAP   (-4)   // the host's output buffer is too small for what is waiting
#define CBC_E_STATE (-5)   // a provider's answer with no sign-in begun, or with another state than it

void cbc_init(cbc *c, const cbc_cfg *cfg);
int  cbc_error(const cbc *c);

// ---- the session -------------------------------------------------------------------------------
// The session as a blob to keep (the NW_SESSION bytes) and back: save answers its length, 0 when
// signed out, CBC_E_CAP; load answers 0, or -1 for bytes that are not one (the client is then
// signed out).
int  cbc_session_save(const cbc *c, uint8_t *out, size_t cap);
int  cbc_session_load(cbc *c, const uint8_t *p, size_t n);
const cbc_user *cbc_user_of(const cbc *c);

// Sign up or in (CBW_SIGNUP, CBW_SIGNIN) with a name as typed (the host upper-cases it) and a
// password: a ticket, finished by a CBC_EV_DONE (and a CBC_EV_SESSION when it worked), or 0.
uint32_t cbc_sign_in(cbc *c, int op, const char *name, size_t name_len, const char *pw, size_t pw_len, int64_t now);
// Sign out this session (global 0) or every session of the user (1): no call is taken after it,
// the socket closes, and the session ends when the server answers, whatever it answers.
uint32_t cbc_sign_out(cbc *c, int global, int64_t now);

// ---- sign-in with an identity provider (cbase/auth_wire.h CBW_OIDC) -----------------------------
// OPT-IN: these exist in a build with CBC_OIDC defined (the native library, and the wasm module's
// oidc variant, cbase_client_oidc.wasm); the base module of an app that signs nobody in through a
// provider stays without them and their SHA-256. The begin's state below is in cbc in every build.
// It begins before the provider's own button does anything: from CBC_OIDC_ENTROPY bytes of the
// host's entropy the client makes the raw nonce (32 bytes) and the state (16 bytes) and keeps both.
// The host hands the provider cbc_oidc_nonce (the raw nonce's SHA-256, lowercase hex, which comes
// back as the ID token's nonce claim) and cbc_oidc_state; the raw nonce never leaves the client
// but in the sign-in's body, where the server checks the token against it.
#define CBC_OIDC_ENTROPY 48
#ifdef CBC_OIDC
int cbc_oidc_begin(cbc *c, const uint8_t *entropy, size_t n);   // 0, or CBC_E_ARG for n != 48
const char *cbc_oidc_nonce(const cbc *c);   // "" before a begin
const char *cbc_oidc_state(const cbc *c);   // "" before a begin
// What the provider answered, sent to the server (POST /v1/auth/oidc): the state it echoed (it must
// be the kept one), its ID token, its authorization code (or none), and for a new account the name
// it takes, as the person typed it (the server's case step makes it the account's). flags CBW_OIDC_LINK adds the identity to the
// signed-in account instead (the bearer goes with it; no session comes back). A ticket finished by a
// CBC_EV_DONE (and a CBC_EV_SESSION when a session came), or 0: CBC_E_STATE (no begin, another
// state), CBC_E_AUTH (a link with nobody signed in, or a sign-out under way), CBC_E_ARG (a field past
// its bound: cbase/auth_wire.h), CBC_E_FULL. The begin is kept until a sign-in or link succeeds, so
// CBW_ACCOUNT_E_NEEDS_NAME or EXISTS is answered by sending the same token again with a name.
uint32_t cbc_oidc_sign_in(cbc *c, int idp, int flags, const char *state, size_t state_len, const char *id_token,
                          size_t token_len, const char *code, size_t code_len, const char *name, size_t name_len,
                          int64_t now);
// PKCE (RFC 7636, S256) for a provider's redirect flow: from 32 bytes of the host's entropy, the
// verifier (base64url, 43 characters) the token request carries and the challenge (base64url of its
// SHA-256, 43 characters) the authorization request carries, each NUL-terminated. 0, or CBC_E_ARG.
// (Apple's flows take a nonce and a state, not PKCE; a provider that takes PKCE takes it from here.)
#define CBC_PKCE_ENTROPY 32
int cbc_pkce(const uint8_t *entropy, size_t n, char verifier[44], char challenge[44]);
#endif   // CBC_OIDC

// ---- calls -------------------------------------------------------------------------------------
#define CBC_GET  1   // == SF_METH_GET
#define CBC_POST 2   // == SF_METH_POST
// TWO SEPARATE QUESTIONS, two flags. CBC_C_IDEMPOTENT: may the call be sent again? CBC_C_FAIL_FAST:
// how soon is a dead server reported?
#define CBC_C_IDEMPOTENT 0x1   // safe to send again: after a loss (below) and after a 503 busy
#define CBC_C_FAIL_FAST  0x8   // a loss finishes the call at once, idempotent or not (below)
// HOW LONG A DEAD SERVER TAKES TO BE HEARD. A call that gets no answer (status 0 over HTTP: a
// refused or reset connection, or its deadline passed; or the socket closed under it) is LOST.
// A lost call that is not CBC_C_IDEMPOTENT is sent once: its CBC_EV_DONE says CBC_WHY_LOST at once
// (a write may have landed). A lost CBC_C_IDEMPOTENT call is sent CBC_TRIES times in all, then its
// DONE says CBC_WHY_LOST. Over HTTP the sends are retry_first_ms apart, doubling, capped at
// retry_max_ms (no jitter): with the defaults 500, 1000 and 2000 ms, so an app hears a server that
// refuses connections about 3.5 s after the call, and one that never answers after
// 4 x http_wait_ms + 3.5 s (63.5 s). On the socket a lost idempotent call goes again at the next
// READY, so it waits for the socket to come back. CBC_C_FAIL_FAST sends a call once whatever
// CBC_C_IDEMPOTENT says, so a UI that shows "the server is down" hears a refused connection at once
// (and one that never answers after one http_wait_ms); an idempotent fail-fast call is still sent
// again after a 503 busy, which a server that is up answered. A 503 busy idempotent call is sent
// again at most CBC_BUSY_TRIES times, after CBC_BUSY_FIRST_MS doubling (250, 500, 1000 ms).
#define CBC_TRIES          4     // an idempotent call is sent at most this often in all after losses
#define CBC_BUSY_TRIES     3     // and sent again at most this often after a 503 busy
#define CBC_BUSY_FIRST_MS  250   // the first busy wait, doubling
#define CBC_C_AUTH 0x2   // needs the user: refused at once when nobody is signed in
#define CBC_C_HTTP 0x4   // always over HTTP, with the bearer when signed in (CLIENT_DESIGN.md 6.4)

// A call of an app route: method (CBC_GET, CBC_POST), the path (and query), a body (none for a
// GET: CBC_E_ARG, as a browser refuses one). A ticket, finished by exactly one CBC_EV_DONE, or 0
// (cbc_error).
uint32_t cbc_call(cbc *c, int method, int flags, const char *path, size_t path_len,
                  const uint8_t *body, size_t body_len, int64_t now);

// A LARGE BODY, BORROWED (a bulk write: POST /notes/bulk and its kind). cbc_call's body is copied
// into the arena (CBC_ARENA bytes for every waiting call); this one's is not. The client keeps body
// (the pointer) and body_len (up to UINT32_MAX) and never reads, copies or frees the bytes: THE
// CALLER KEEPS THEM VALID AND UNCHANGED UNTIL THE CALL'S CBC_EV_DONE, and a read sent again after a
// loss or a 503 goes from the same bytes. It always goes over HTTP (a big body never holds the
// socket's frames up behind it). The host sends it from two places: the head, path and
// Authorization that cbc_next_request writes (flags CBC_RQ_REF, body_len the whole length, nothing
// of the body in out), then body_len bytes from cbc_req_body. Only the path is copied (into the
// arena). A ticket, finished by exactly one CBC_EV_DONE, or 0 (cbc_error: CBC_E_ARG for a NULL body
// or one past UINT32_MAX, CBC_E_AUTH, CBC_E_FULL). body may be a host's opaque handle instead of a
// pointer (a page's key to bytes of its own): the client hands it back exactly as it got it.
uint32_t cbc_call_ref(cbc *c, int method, int flags, const char *path, size_t path_len,
                      const uint8_t *body, size_t body_len, int64_t now);

// The next HTTP request to send into out (cbc_req_hdr, then its bytes): its length, 0 for none,
// CBC_E_CAP. The refresh comes first when it is due. A CBC_RQ_REF request needs room for its head,
// path and Authorization only.
int  cbc_next_request(cbc *c, uint8_t *out, size_t cap, int64_t now);
// The borrowed body of HTTP request id while it is out (cbc_req_hdr.flags CBC_RQ_REF): the pointer
// cbc_call_ref was given. NULL for a request that is not out or whose body is in its own bytes.
const uint8_t *cbc_req_body(const cbc *c, uint32_t id);
// The answer to request id. body may be NULL when the request did not set CBC_RQ_BODY and the
// status is 2xx (the core does not read it; the host keeps it for the app, THE SPAN); len is the
// body's length either way. Answers the events raised.
int  cbc_on_response(cbc *c, uint32_t id, int status, const uint8_t *body, size_t len, int64_t now);

// ---- the socket --------------------------------------------------------------------------------
int  cbc_stream_wanted(const cbc *c);      // 1: keep a socket open
const char *cbc_stream_path(const cbc *c); // "/ws", or "/ws/public" for an anonymous client

// A room's kind for the platform's room families ("s/{doc}"), as SUB says it: == SF_WHAT_ROOM.
#define CBC_ROOM 2

// Hold or release a room (counted: the SUB goes once, the UNSUB when the last hold goes; an
// anonymous client's socket closes instead when its last room goes). 0, or CBC_E_ARG, CBC_E_FULL.
int  cbc_sub(cbc *c, int what, const char *room, size_t room_len);
int  cbc_unsub(cbc *c, int what, const char *room, size_t room_len);

void     cbc_opened(cbc *c, int64_t now);
// The socket closed, for any reason: the ms to wait before opening the next one (decorrelated
// jitter, CLIENT_DESIGN.md 6.2; 0 for the token's planned roll). Raises the events of the socket
// calls it ends.
uint32_t cbc_closed(cbc *c, int64_t now);
// One binary message from the server. Answers the events raised.
int  cbc_in(cbc *c, const uint8_t *frame, size_t len, int64_t now);
// The next frame to send into out: its length, 0 for none, -1: close the socket now (half-open, a
// HELLO never answered, a session that ended, the token's roll), CBC_E_CAP. After a -1 it answers
// 0 until cbc_closed, and cbc_due_ms counts nothing of the socket meanwhile (only its other
// timers: HTTP retries, waits, the refresh), so a host arms its timer as always while the close is
// on its way.
int  cbc_next_out(cbc *c, uint8_t *out, size_t cap, int64_t now);

// ---- time and events ---------------------------------------------------------------------------
int  cbc_tick(cbc *c, int64_t now);        // the timers: answers the events raised
int  cbc_due_ms(const cbc *c, int64_t now); // when to call cbc_tick and drain again (-1: never)
int  cbc_n_events(const cbc *c);
const cbc_event *cbc_event_at(const cbc *c, int i);

#endif
