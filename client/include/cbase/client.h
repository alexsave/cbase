// cbase/client.h - THE C_BASE CLIENT: the other half of `cbase serve`'s socket (GET /ws and
// GET /ws/public, shared/c/stream_frame.h) and of the platform's sessions (/v1/auth,
// cbase/auth_wire.h). One client is one user (or one anonymous listener) talking to one app.
// c_base/docs/CLIENT_DESIGN.md is the design; this is the contract.
//
// SANS-IO. The host owns the socket, the HTTP client and one timer, and nothing else. It passes in
// every byte it receives and the time (now, epoch ms), and sends every byte this file hands out:
//
//   socket   while cbc_stream_wanted: open cbc_stream_path (ws or wss, the page's scheme), then
//            cbc_opened; every binary message to cbc_in; send each frame cbc_next_out writes
//            (-1: close the socket); on any close, cbc_closed, and reopen after the ms it answers
//   HTTP     send each request cbc_next_request writes, answer it with cbc_on_response
//            (status 0: it never arrived)
//   timer    wake after cbc_due_ms and call cbc_tick, then drain both outputs again
//   events   cbc_in, cbc_on_response, cbc_closed and cbc_tick raise events: read them with
//            cbc_n_events / cbc_event_at after each, before calling any of the four again. A span
//            (off, len) is into the bytes passed to the call that raised it.
//
// ONE AUTHENTICATION PER SOCKET (CLIENT_DESIGN.md 6.4). A session is made over HTTP
// (cbc_sign_in, and the refresh this file sends itself), the socket says HELLO with its token once,
// and every call after that rides the socket as SF_REQ with no token. A call made while the socket
// is down waits for READY. HTTP is left for /v1/auth, an anonymous client's reads (/ws/public
// carries no REQ), and a call flagged CBC_C_HTTP. A socket never outlives its token: the client
// refreshes refresh_early_ms before expiry and then rolls the socket (cbc_next_out answers -1 once
// no socket call is in flight, and cbc_closed answers 0 for that close).
//
// NO ALLOCATION, NO CLOCK, NO GLOBALS. The caller owns the cbc (CBC_SIZE bytes, at most
// CBC_SIZE_BUDGET), so a load generator holds thousands in one process; randomness is the
// reconnect jitter's, from cfg.seed (shared/c/rng.h). The structs a host reads (cbc_event,
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
    uint64_t seed;              // the reconnect jitter's generator: per client, from the host's entropy
    uint8_t  anonymous;         // 1: GET /ws/public, an empty HELLO, no session, every call over HTTP
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
};

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
    uint32_t off, len;  // PUSH, DONE: the span, into the bytes passed to the call that raised it
    char     room[CBC_ROOM_MAX];   // SUB_*
} cbc_event;

// A request cbc_next_request writes: this head, then path_len bytes of path, auth_len bytes of the
// Authorization value ("Bearer ..."; none when 0), body_len bytes of body.
#define CBC_RQ_BODY 0x1   // hand the answer's body to cbc_on_response (else only its length is needed,
                          // and its body only when the status is not 2xx: a refusal)
typedef struct {
    uint32_t id;        // cbc_on_response's
    uint8_t  method;    // CBC_GET, CBC_POST
    uint8_t  flags;     // CBC_RQ_*
    uint16_t path_len;
    uint16_t auth_len;
    uint16_t pad;
    uint32_t body_len;
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
    uint16_t body_len;
    uint16_t at_arena;       // path then body, in the arena
    uint16_t pad2;
    int64_t  at;             // SENT: when; WAIT: when to send again
} cbc_call_slot;

typedef struct cbc {
    cbc_cfg  cfg;
    uint64_t rng;
    // the session (the NW_SESSION fields, auth_wire.h)
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
    int64_t  forced_last;       // the last pre-READY close that forced one
    char     access[CBW_ACCESS_MAX];
    char     refresh[CBW_REFRESH_MAX];
    // the socket
    uint8_t  sock;              // CBC_SOCK_* (client.c)
    uint8_t  roll;              // the close cbc_next_out asked for is the token's planned roll
    uint8_t  pad1[2];
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
} cbc;

#define CBC_SIZE sizeof(cbc)
_Static_assert(sizeof(cbc) <= CBC_SIZE_BUDGET, "cbc grew past its budget (CLIENT_DESIGN.md 3.1)");

// Refusals of a call into this file (cbc_error).
#define CBC_E_ARG   (-1)   // a path, room or body past its bound, or a bad method
#define CBC_E_FULL  (-2)   // CBC_CALLS_MAX calls or CBC_SUBS_MAX rooms already, or the arena is full
#define CBC_E_AUTH  (-3)   // the call needs a user and nobody is signed in (or the session is ending)
#define CBC_E_CAP   (-4)   // the host's output buffer is too small for what is waiting

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

// ---- calls -------------------------------------------------------------------------------------
#define CBC_GET  1   // == SF_METH_GET
#define CBC_POST 2   // == SF_METH_POST
#define CBC_C_READ 0x1   // idempotent: sent again after a loss or a 503 busy
#define CBC_C_AUTH 0x2   // needs the user: refused at once when nobody is signed in
#define CBC_C_HTTP 0x4   // always over HTTP, with the bearer when signed in (CLIENT_DESIGN.md 6.4)

// A call of an app route: method (CBC_GET, CBC_POST), the path (and query), a body. A
// ticket, finished by exactly one CBC_EV_DONE, or 0 (cbc_error).
uint32_t cbc_call(cbc *c, int method, int flags, const char *path, size_t path_len,
                  const uint8_t *body, size_t body_len, int64_t now);

// The next HTTP request to send into out (cbc_req_hdr, then its bytes): its length, 0 for none,
// CBC_E_CAP. The refresh comes first when it is due.
int  cbc_next_request(cbc *c, uint8_t *out, size_t cap, int64_t now);
// The answer to request id. body may be NULL when the request did not ask for it (CBC_RQ_BODY)
// and the status is 2xx; len is the body's length either way. Answers the events raised.
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
// HELLO never answered, a session that ended, the token's roll), CBC_E_CAP.
int  cbc_next_out(cbc *c, uint8_t *out, size_t cap, int64_t now);

// ---- time and events ---------------------------------------------------------------------------
int  cbc_tick(cbc *c, int64_t now);        // the timers: answers the events raised
int  cbc_due_ms(const cbc *c, int64_t now); // when to call cbc_tick and drain again (-1: never)
int  cbc_n_events(const cbc *c);
const cbc_event *cbc_event_at(const cbc *c, int i);

#endif
