# The c_base client: design

Status: approved by Alex on 2026-10-22, with the decisions of section 6.
Step 1 (the library) and step 2 (adventurer) are being built.
Written from origin/main at 7264b94ee, with read-only reads of the branches foolish-birdseye, c-chess, adventurer, lichess-ws and cbase-app-lifts (PR #429).

## 0. The answer in one paragraph

Very little of foolish's website moves.
Of the 23,781 lines in `foolish/src`, about 490 lines of session, socket and route-guard code move, which is 2%.
The rest of `src` is the game: the table, the animations, the oracle, the replays and the styles, and it stays.
What moves is the plumbing under the website.
That is the generic half of the C client (`netclient.c`, `netwire.c`), the TypeScript that loads wasm and moves bytes (`sdk/ts/wasm`, `sdk/ts/net`, most of `src/common/backend.ts`), and the iOS socket shell (`ios/FoolishNet/Stream.swift`, `Backend.swift`).
In lines: about 1,360 of C, 1,250 of TypeScript and 410 of Swift move from `foolish/` into `c_base/client/`.
Foolish keeps about 970 lines of `netclient.c` (its endpoints, row readers, version gates and seat logic) and gains about 120 lines of forwarders and a route table.
The `nc_*` names that the web and iOS hosts call stay the same, so the hosts gain no lines.
Foolish moves last, behind a differential proof that every byte the client sends and every event it raises is unchanged.

## 1. Inventory

### 1.1 What each app has today

A note on the five apps:
- **foolish** has the only complete client.
- **c_chess** has the best structure: a context struct, no I/O, no allocation, one client per bot.
- **birdseye** and **adventurer** each hand-wrote a TypeScript socket loop.
- **lichess_ws** speaks lichess's JSON protocol, so its only reusable client piece is the load generator's socket code.
- uttt, chuiniu, pickemup, tallybones and werewolf have no realtime client.

| Piece | foolish (main) | c_chess | birdseye | adventurer | lichess_ws |
|---|---|---|---|---|---|
| Client core | `foolish/c/src/netclient.c` 1822 + `.h` 411, C, sans-IO, one per process (file statics, netclient.c:48-99) | `c_chess/c/src/client.c` 426 + `.h` 132, C, sans-IO, caller-owned `cc_client` struct | `birdseye/web/src/api.ts:214-336` (123 lines, TS); `birdseye/server/src/client.c` 210 + `.h` 73, C, blocking, for the sim | `adventurer/web/src/api.ts:267-344` (78 lines, TS) | none (JSON protocol) |
| Connection, auth, session | HELLO with token; signup/signin/refresh/signout/password/delete in C (netclient.c:144-333); blob in localStorage `foolish.session` (backend.ts:45-89); cross-tab `storage` sync | HELLO with token from `POST /c/auth` and `/c/refresh` (client.c:97-138); guest account in localStorage as JSON (net.ts:146-159) | `POST /be/session`, a dev-only fixed account (routes.c:94-133), fetched again on every connect | anonymous `GET /ws/public`, empty HELLO (api.ts:253-257) | `lila2` cookie |
| Frame codec | `shared/c/stream_frame.h` in C (netclient.c:1643-1811) | `stream_frame.h` in C | `birdseye/server/wasm/sf_wasm.c` 69 lines, 1,299 B wasm | structgen TS from the packed heads of PR #429 (`gen/stream_frame.ts` 103) | JSON text |
| Request/reply correlation | tickets, 8 host slots + 16 own slots, ids with top bit for SUB (netclient.c:567-833, 1062-1140) | 64-entry table indexed `id % 64` that overwrites and never times out (client.c:49-73) | none in web (SUB REPLY ignored); 8-entry table in sim client.c:109-154 | one SUB id | none |
| Subscribe, resubscribe | ref-counted holds, 16 subs, resubscribed by C on READY, unanswered SUB re-sent after 10 s (netclient.c:1174-1272) | host re-issues `lobby()`/`open()` in `onopen` (net.ts:171-203) | one room, implicit | one room, implicit | n/a |
| Reconnect, backoff | 500 ms doubling to 10 s, reset on READY, no jitter (netclient.c:1313-1315) | 250 ms doubling to 5 s; 50 ms after a pre-READY close (net.ts:52, 190, 201) | 1 s doubling to 15 s, reset on READY (api.ts:220, 229); sim: fixed 2 s | 1 s doubling to 15 s, reset on SUB REPLY 200 (api.ts:274, 302, 327) | none (wsbench: a closed client stays closed) |
| Ping, liveness | PING after 30 s silent out; half-open after 10 s without any frame; host closes on an empty frame (netclient.h:52-53, netclient.c:1755-1777) | none | PING every 30 s, no pong deadline | none (server's own WS ping only) | text `"p"` every 2 s |
| Token refresh | 60 s before expiry; forced on a pre-READY close at most every 10 min; 401 re-sent once (netclient.c:264-274, 1085-1092, 1304) | `renew()` only after a pre-READY close | new session per connect | n/a | none |
| HTTP retries | GET only: lost read 3 retries (500, 1000, 2000); 503 busy 3 tries (250, 500, 1000); writes never (netclient.c:410-415, 781-785) | none | `LIST_RETRY_MS` 2000 in App.tsx | `LIST_RETRY_MS` 2000 | n/a |
| Optimistic apply, correction | `client_table.c` 1392 (optimistic apply 699, push settle 1097, revert 1256); TS `state/optimistic*.ts` | `cc_client_move` + `take_state` resync on gap (client.c:199-328) | none (viewer) | none (viewer) | n/a |
| State mirror | `ServerContext.tsx` 895, version gates 64-room LRU (netclient.c:1142-1172), quiet-room refetch at 60 s + 6 s jitter | `cc_client.g` mirror, `useSyncExternalStore` (use.ts) | snapshot + stream merged by version (App.tsx:97-253) | refetch on push, debounced | n/a |
| Wasm build and load | `web.wasm.gz` (67 KB raw, 31 KB gz per c/Makefile:1463), sync instantiate after vendored gunzip (`sdk/ts/wasm/*` 531 lines) | `chess.wasm` 24,764 B, 10,481 B gz, `instantiateStreaming` (kernel.ts 122) | `sf.wasm` 1,299 B (api.ts:177-212) | none | none |
| Generated bindings | structgen `net_layout.bots.ts`, `kernel.ios.swift`; Kotlin `Kernel.kt` without the net structs | `web_layout.ts` 168 | `wire.ts` 204, `metrics.ts` 114, `sf_wasm.ts` 23 | `wire.ts` 637, `stream_frame.ts` 103 | none |
| Static site serving | `next build` export to S3 + CloudFront (`c_base/providers/aws/ops/site/web.yaml` 454, `web_deploy.sh` 232) | `c_chess/server/src/web.c` 142: reads `CC_WEB_DIR` into the heap at boot, gzip twins, linear lookup, `GET /*` INLINE+HOME | `next start` on its own port | `next dev` | n/a |
| Ops panel | none (c_base admin console only) | none | `Ops.tsx` 135 over c_base `GET /metrics` (generic) | `Ops.tsx` 98 over its own `/adv/ops` | stdout report |
| Load generator | `server/bench/roundtrip.c` 474, codecs only (the client is a singleton) | `server/tools/chessbench.c` 514, one `cc_client` per bot | load comes from in-process `sim.c` 1005 + `league.c` 990 over `lib_client` | `server/gen` 397 + its own `httpc.c` 216, HTTP only | `server/tools/wsbench.c` 603, own non-blocking WS client on `cb_loop` (77-284) |
| Tests of the client | `unit_netclient.c` 1996 (42 tests), `fz_netclient.c` 208 with a 1,548-file corpus, `e2e/stream_dead_socket.test.ts` 86 | `server/test/smoke.c` 475 drives `cc_client` | smoke only | `smoke.c:113-131` opens `/ws/public` | `smoke.c` 345 |

### 1.2 Generic against app-specific

Generic in every app: the frame codec, HELLO and READY, SUB and UNSUB with retries, REQ and REPLY correlation, PING and the pong deadline, the reconnect delay, the token's lifetime, the HTTP pump with read-only retries, the session blob, the event list, the wasm loader with its layout-hash check, the host's socket shell, the metrics reader, and the load generator's socket and pacing code.

App-specific in every app: the push kinds and their bodies, the endpoints and their answers, which subscription needs which catch-up read, the mirror and its optimistic rules, and the UI.

Two pieces are generic in mechanism and app-specific in content, and the design splits them that way:
- The per-room version gate (foolish's 64-room LRU): the LRU is generic, and the app says what a version is.
- The quiet-room refetch (foolish's 60 s + jitter): the timer is generic, and the app says when a room is quiet.

### 1.3 Behavioural differences, and which is best

Foolish's behaviour is the best on every axis but one, and the shared client starts from it.
- Half-open sockets: only foolish detects them in a browser (PING plus a 10 s deadline on any frame).
  adventurer and c_chess never notice a dead socket behind a NAT until the OS gives up.
- Resubscribe: foolish's is in C and counted; c_chess and the two TS clients rebuild it in the host.
- Token refresh: foolish refreshes before expiry and once on a refusal; c_chess only after a refusal; birdseye signs in again on every connect.
- Correlation: c_chess's table overwrites a live slot after 64 requests and never times one out; birdseye's web ignores the SUB's REPLY.
- Write retries: only foolish refuses to resend a write that may have landed.
- **Jitter: no app jitters its reconnect.**
  After a server restart every foolish client reconnects at the same 500 ms, then the same 1 s, and so on.
  That is the one place foolish is not the best, and the shared client jitters from the start (section 6.2).

## 2. What of foolish moves, file by file

Line counts are `wc -l` at origin/main.
"Moves" means the lines leave `foolish/` and land in `c_base/client/`, rewritten from file statics to a context struct, with the same logic.

### 2.1 C

| File | Lines | Moves | Stays | What moves |
|---|---|---|---|---|
| `foolish/c/src/netclient.c` | 1822 | ~850 | ~970 | session 144-215, request builder 216-275, the auth endpoints of 283-333, `nc_retry_ms` 410-415, pump state and core 567-833 (mechanism, not the kind tables), `nc_next_request` 883-929, `refresh_answered` 931-944, `answered` and the pump exits 1062-1140, the gate LRU 1142-1172, subscriptions 1174-1272, socket lifecycle 1274-1322, events 1324-1359, the quiet timer of 1434-1497, `take_suback` 1606-1641, frames in 1643-1693, frames out 1695-1811 |
| `foolish/c/src/netclient.h` | 411 | ~200 | ~210 | the timing constants, `NcRequest`, `NcOut`, `NcSessionInfo`, `NcError`, the session and stream API, the pump loop's contract |
| `foolish/c/src/netwire.c` | 481 | ~145 | ~336 | str8/str16/blob/u48 1-77, the session codec 144-172, the auth request codec of 78-143 |
| `foolish/c/src/netwire.h` | 235 | ~70 | ~165 | `[NW_V1][kind]`, the platform kinds 1-5 and 20, the session layout, the refusal layout (PLATFORM.md 2.6 already fixed this split by range) |
| `foolish/c/wasm/wasm_net_api.c` | 87 | ~70 | ~17 | the inbox and the one-line wasm wrappers |
| `foolish/c/ios/ios_api_net.c` | 33 | ~25 | ~8 | the `fio_net_*` one-liners |
| **C total** | | **~1,360** | | |

Foolish gains about 120 lines of C: the `nc_*` forwarders over one static `cbc` (about 80) and its route table as data (about 40).
`c_base/client` gains about 1,420 (the moved lines plus about 60 of context plumbing and static asserts).
Net change in C across the repo: about +180, which matches PLATFORM.md's estimate of +200 for stage 9.
The tests split as well: about 1,100 of the 1,996 lines of `unit_netclient.c` test generic behaviour and move with it, and `fz_netclient.c` stays in foolish and keeps driving the whole stack.

### 2.2 TypeScript

| File | Lines | Moves | Stays | What moves |
|---|---|---|---|---|
| `foolish/src/common/backend.ts` | 319 | ~240 | ~79 | session restore and cross-tab sync 45-89, pump/send/settle 93-153, `class Stream` 235-319 |
| `foolish/sdk/ts/net/net_client.ts` | 182 | ~150 | ~32 | the call-and-copy wrapper; foolish keeps `NetIntent`, its result types and `ROW_READERS` (26-53) |
| `foolish/sdk/ts/wasm/wasm_asset.ts` | 87 | 87 | 0 | the loader, with the asset URL as an argument |
| `foolish/sdk/ts/wasm/gunzip.ts` | 22 | 22 | 0 | |
| `foolish/sdk/ts/wasm/tiny_inflate.mjs` | 380 | 380 | 0 | vendored inflate |
| `foolish/sdk/ts/wasm/layout_hash.ts` | 42 | 42 | 0 | |
| `foolish/sdk/ts/wire/bytes.ts` | 22 | 22 | 0 | base64 of the session blob |
| `foolish/sdk/ts/wasm/bots.ts` | 647 | ~34 | ~613 | the instantiate path 44-77 |
| `foolish/src/common/session.ts` | 46 | 46 | 0 | the signed-in mirror, with the storage key as an argument |
| `foolish/src/contexts/AuthContext.tsx` | 60 | 60 | 0 | |
| `foolish/src/components/KernelGate.tsx` | 56 | 56 | 0 | |
| `foolish/src/components/ProtectedRoute.tsx`, `UnprotectedRoute.tsx` | 52 + 56 | 108 | 0 | with the redirect paths as props |
| **TS total** | | **~1,250** | | |

Foolish gains about 40 lines of TypeScript: the call that configures the client with foolish's storage key, asset URL and listener kinds.
Net change in TypeScript across the repo: about +40 for foolish alone, and negative once a second app adopts it.
The test kit `e2e/helpers/fake_backend.ts` (107) and `e2e/stream_dead_socket.test.ts` (86) move with it as the client's own tests.

### 2.3 Swift

| File | Lines | Moves | Stays |
|---|---|---|---|
| `foolish/ios/FoolishNet/Stream.swift` | 104 | 104 | 0 |
| `foolish/ios/FoolishNet/Backend.swift` | 156 | ~140 | ~16 (the boards flag) |
| `foolish/sdk/swift/NetClient.swift` | 117 | ~90 | ~27 (row readers) |
| `foolish/ios/FoolishNet/AuthService.swift`, `AccountService.swift` | 62 + 18 | 80 | 0 |
| **Swift total** | | **~410** | |

Net change in Swift: about +20.
The iMessage app never links FoolishNet, and the main iOS app is not a priority, so this step can wait for the next iOS build.

### 2.4 Kotlin

Nothing moves and nothing is written: foolish has no Android client, and `sdk/kotlin/gen/Kernel.kt` does not include `netclient.h`.
structgen already emits Kotlin, so the generated surface comes free when an Android host exists.

### 2.5 What stays in foolish

Everything that knows a card, a seat, a game or a bot:
- The endpoints and answers: `NC_DO_*`, the row readers, `nc_req_action` and the action-over-socket hook.
- The book of games: the cast-skip refetch, the reconcile after a refused move, `nc_set_boards`, the seat learning from the roster trailer, and the quiet predicates that read `GAME_STATUS_GAME_OVER`.
- `client_table.c`, `evwire.c`, `awire.c`, `table_wire.c` and `ask.c`, which are the mirror and its optimistic rules.
- The whole website above `backend.ts`: `ServerContext.tsx`, `state/*`, `AnimationContext.tsx`, the oracle, the replays, the components and the styles.
- The static site stays on S3 and CloudFront (section 6.3).

### 2.6 How zero behaviour change is proven

1. **A differential trace over recorded and fuzzed sessions.**
   A new test tool, `nc_trace`, reads a step program (an intent, an answer, stream bytes, opened, closed, a session blob, each with its `now`) and prints every `NcRequest`, `NcOut`, `NcEvent` and `NcDone` byte for byte.
   It is built twice: once against origin/main's objects and once against the branch.
   It replays the 1,548 programs of `foolish/server/fuzz/corpus/netclient/` and a set of sessions recorded from the e2e suite at the `wasm_net_*` boundary (a recorder compiled only into a test build, so no shipped code grows).
   The two outputs must be identical, except the reconnect delays, which are jittered and are asserted to lie within their bounds instead (section 6.2).
   The client is a pure function of its inputs: time comes in as `now`, and the only randomness is the quiet jitter, which mixes `now`, the user id and the gid.
2. **The 42 tests of `unit_netclient.c` stay green.**
   They are not edited, except to call the moved ones from their new place, and each new test is mutation-checked.
3. **The generated bindings are byte-identical.**
   `net_layout.bots.ts` and `kernel.ios.swift` from structgen must not change, and the layout hash must not change.
   `Nc*` keep their names and layouts in `netclient.h` as foolish's names for the moved structs, and a static assert pins every size and offset.
4. **web.wasm keeps its export list.**
   The list is diffed, and its size is reported; growth over 512 bytes needs a reason.
5. **No c_base server code changes in the client steps.**
   Any step that does touch the server (static serving, section 3.5) runs `c_base/tools/hotpath_proof.sh` with its default hot-path list, and every function on it must be identical.
6. **The full `npm run test:e2e` pass, the live smoke and one played game** run at the end, after the targeted gates above, as the defer-full-e2e rule says.

## 3. The proposed interface

### 3.1 The rules it follows

- **Sans-IO.** The host owns the socket, the HTTP client and the timer, and passes in bytes and `now`.
  This is both foolish's and c_chess's shape, and it is why one C file runs in a browser, in iOS and in a load generator.
- **A context struct, not statics.**
  The caller allocates `cbc`, as c_chess does, so a load generator holds thousands per process.
  Foolish keeps one static instance behind its `nc_*` names.
- **No allocation, no clock, no randomness the caller does not pass in.**
- **No function-pointer interface.**
  The app's knowledge comes in as data (a route table) and goes out as events the app's own C reads after each call, the way the shared lifts so far have done it.
- **The output buffer is the caller's.**
  The client state stays small, and a load generator shares one scratch buffer.
  Built: 10,240 bytes per `cbc`, under a budget of 12,288 that a static assert pins; the access token's 2 KB and the 4 KB arena for waiting calls are most of it.

### 3.2 The C header, sketched

**Built (step 1):** `c_base/client/include/cbase/client.h` is the contract, and it differs from this sketch in four ways, each found while building it.
- **One door for everything the host reads.** `cbc_in`, `cbc_on_response`, `cbc_closed` and `cbc_tick` raise events, a finished call among them (`CBC_EV_DONE`), so there is no `cbc_next_done`.
- **No pointers in what other languages read.** A request is written into the caller's buffer as a `cbc_req_hdr` followed by its path, its Authorization value and its body, as a frame is, so structgen's readers reach every byte.
- **A call names its method, path and flags itself** (`CBC_C_READ`, `CBC_C_AUTH`, `CBC_C_HTTP`); a route table is the app layer's data, from which its forwarders build calls.
- **The mirror helpers (the version gate, the quiet timer) are not built yet.** They come with step 6, whose foolish layer is their first user.

The sketch as it was drafted:

```c
// c_base/client/include/cbase/client.h - the c_base client: the other half of `cbase serve`'s
// /ws and /v1/auth. Sans-IO: the host owns sockets and timers and passes bytes and now_ms.
#include "stream_frame.h"   // shared/c

typedef struct {
    uint32_t ping_ms;          // 30000: a PING when nothing went out this long
    uint32_t pong_ms;          // 10000: no frame of any kind this long after a PING: half-open
    uint32_t ack_wait_ms;      // 10000: an unanswered SUB is asked again
    uint32_t retry_first_ms;   // 500: every doubling backoff starts here
    uint32_t retry_max_ms;     // 10000: and stops here
    uint32_t refresh_early_ms; // 60000: refresh the token this long before it expires
    uint32_t force_refresh_ms; // 600000: a pre-READY close refreshes at most this often
    uint64_t seed;             // the reconnect jitter's generator, per client (section 6.2)
    uint8_t  anonymous;        // 1: GET /ws/public with an empty HELLO, no session
} cbc_cfg;

// One endpoint, as data (PLATFORM.md 2.6: no product switch in the pump).
#define CBC_R_AUTH  0x1   // carries the bearer; waits for a refresh when it is due
#define CBC_R_READ  0x2   // idempotent: a lost answer or a 503 busy is sent again
#define CBC_R_WIRE  0x4   // may ride the live socket as SF_REQ instead of HTTP
typedef struct {
    uint8_t     method;    // SF_METH_GET, SF_METH_POST
    uint8_t     flags;     // CBC_R_*
    uint8_t     answer;    // the app's answer kind, handed back in cbc_done
    const char *path;      // "/v1/games/{gid}": {gid} is the call's room id
} cbc_route;

typedef struct { uint32_t ticket; uint16_t route; uint8_t gid_len; char gid[SF_GID_MAX];
                 uint16_t body_len; const uint8_t *body; } cbc_call;

typedef struct { uint32_t id; uint8_t method; uint8_t is_refresh;
                 uint8_t path_len; char path[160]; uint16_t auth_len; char auth[2055];
                 uint16_t body_len; const uint8_t *body; } cbc_request;

typedef struct { uint32_t ticket; uint16_t route; uint8_t answer; uint8_t why;
                 int32_t code, detail, status; uint32_t body_off, body_len; } cbc_done;

enum { CBC_EV_READY = 1, CBC_EV_PUSH, CBC_EV_SUB_LIVE, CBC_EV_SUB_REFUSED,
       CBC_EV_SESSION, CBC_EV_ROOM_QUIET, CBC_EV_BAD };
typedef struct { uint8_t kind; uint8_t what; uint8_t push_kind; uint8_t why;
                 uint8_t gid_len; char gid[SF_GID_MAX];
                 uint32_t body_off, body_len; } cbc_event;   // spans into the frame passed in

typedef struct cbc cbc;   // the whole client; its size is CBC_SIZE, static-asserted <= CBC_SIZE_BUDGET

void cbc_init(cbc *c, const cbc_cfg *cfg, const cbc_route *routes, int n_routes);

// The session (the platform's /v1/auth: signup, signin, refresh, signout, password, delete).
int  cbc_session_save(const cbc *c, uint8_t *out, size_t cap);
int  cbc_session_load(cbc *c, const uint8_t *p, size_t n);
const cbc_user *cbc_user_of(const cbc *c);
uint32_t cbc_auth(cbc *c, int op, const char *name, size_t nn, const char *pw, size_t pn, int64_t now);

// The HTTP pump.
uint32_t cbc_call_start(cbc *c, const cbc_call *call, int64_t now);   // a ticket, or 0 (cbc_error)
const cbc_request *cbc_next_request(cbc *c, int64_t now);
const cbc_done *cbc_on_response(cbc *c, uint32_t id, int status, const uint8_t *body, size_t n, int64_t now);
const cbc_done *cbc_next_done(cbc *c);

// The stream.
const char *cbc_stream_path(const cbc *c);                 // "/ws" or "/ws/public"
int  cbc_sub(cbc *c, uint8_t what, const char *gid, size_t n);    // counted holds
int  cbc_unsub(cbc *c, uint8_t what, const char *gid, size_t n);
void cbc_opened(cbc *c, int64_t now);
uint32_t cbc_closed(cbc *c, int64_t now);                  // ms to wait before reopening
int  cbc_in(cbc *c, const uint8_t *frame, size_t n, int64_t now);   // events raised, or -1
int  cbc_next_out(cbc *c, uint8_t *out, size_t cap, int64_t now);   // a frame's length; 0 none; -1 close it

// Events of the last call, and the one timer.
const cbc_event *cbc_event_at(const cbc *c, int i);
int  cbc_due_ms(const cbc *c, int64_t now);                // -1: nothing waits

// Mirror helpers the app may use: the per-room version gate and the quiet timer.
int  cbc_gate_pass(cbc *c, const char *gid, size_t n, uint32_t version);  // 1 newer, 0 stale
void cbc_gate_reset(cbc *c, const char *gid, size_t n);
void cbc_quiet_arm(cbc *c, const char *gid, size_t n, uint32_t after_ms, int64_t now);
```

### 3.3 How an app plugs in its bodies

The app's C is a thin layer over `cbc`, and it is where the app's knowledge lives.
- **Pushes.** The app declares its push kinds below `SF_HELLO` in its own wire header (as `foolish/server/src/app/pushes.c:20-23` already asserts).
  `cbc_in` raises `CBC_EV_PUSH` with the push kind and a span of the body, and the app's layer reads the body through its structgen-generated readers.
- **Answers.** Each route names an answer kind; `cbc_done` hands it back with a span of the body, and the app reads it.
- **Catch-up.** `CBC_EV_SUB_LIVE` tells the app a room just went live, so it starts its own refetch call (foolish: read the game whole).
- **Optimistic apply and correction** stay the app's: foolish's `client_table.c`, chess's `take_state`.
  What the platform gives them is the version gate and the quiet timer.
- For foolish, `netclient.c` keeps its `nc_*` API and becomes this layer over one static `cbc`.

### 3.4 The generated surfaces, per language

structgen reads `cbase/client.h` exactly as it reads `netclient.h` today, and it writes:
- **TypeScript:** `client_layout.ts` (the readers of `cbc_request`, `cbc_done`, `cbc_event`, `cbc_user`, and the `cbc_cfg` writer), `client_exports.ts` (`--exports-ts --check-wasm`, as birdseye does), and the layout hash.
- **Swift:** the same readers with the iOS triple, and the hash baked into the xcframework.
- **Kotlin:** the same, written only when an Android host exists.

The hand-written host shim is the one part per language that cannot be C, and it holds only I/O:
- `c_base/client/ts/socket.ts` (about 200 lines): a WebSocket, `fetch`, one `setTimeout`, `localStorage` for the blob, and the cross-tab `storage` listener.
  It replaces foolish's `backend.ts` core, c_chess's `net.ts`, birdseye's `openStream` and adventurer's `openFeed`.
- `c_base/client/ts/wasm.ts`: the loader (foolish's `wasm_asset.ts`, `gunzip.ts`, `tiny_inflate.mjs`, `layout_hash.ts`), with the asset URL and the expected hash as arguments.
- `c_base/client/ts/react/`: `useClient` over `useSyncExternalStore` (c_chess's pattern), `AuthProvider`, `KernelGate` and the route guards, all moved from foolish.
- `c_base/client/swift/`: `CbSocket.swift` and `CbBackend.swift` (URLSession, Keychain blob), from foolish's `Stream.swift` and `Backend.swift`.
- No hand-written byte offsets anywhere: every byte is read by C or by a structgen reader.
  The one exception today, the gzip header in `gunzip.ts:12-18`, is the gzip format, not ours.

### 3.5 Wasm packaging and its size budget

Two packagings, the same objects:
- **Linked into the app's own module** for apps with C on the client (foolish's `web.wasm`, c_chess's `chess.wasm`).
  `c_base/client/client.mk` lists the sources and the exports, as `shared/tools/gen_tools.mk` does for structgen.
- **A prebuilt `cbase_client.wasm`** for apps with no C of their own on the client (birdseye, adventurer).
  It replaces birdseye's 1,299-byte `sf.wasm` with a client that also correlates, retries and pings.

Budget, measured by a CI gate that prints the number and fails past it:
- `cbase_client.wasm` at `-Oz`: at most 10 KB raw and 5 KB gzipped.
- Foolish's `web.wasm`: grows by at most 512 bytes (the context pointer replaces file statics).
- c_chess's `chess.wasm`: no larger than today's 24,764 bytes, since its own correlation code goes away.

### 3.6 Static-site serving

Each app chooses, explicitly, between a CDN and this (section 6.3).
For an app that chooses SERVE, c_base serves the app's static export itself, from memory, in the app's own process.
- **Build.** `c_base/tools/embed_site.c` (C, like `embed.c`) walks the `next build` export, writes a gzip `-9` twin when it is smaller, and emits `site_gen.c`.
  That file holds a sorted path table and the bodies in `.rodata`, with each response's headers precomputed (Content-Type, Content-Length, Content-Encoding, Vary, Cache-Control `immutable` for `/_next/static/` and `no-cache` otherwise, and an ETag from `shared/c/fnv.h`).
  A deploy stays one file.
- **Runtime.** `c_base/core/src/site/site.c` (about 150 lines, lifted from c_chess's `web.c`).
  The app names its mode and its table in `cb_app.site`, and boot registers `GET /*` with `CB_ROUTE_INLINE | CB_ROUTE_HOME` after every other route, so the longest match keeps every API route and `/ws` ahead of it.
  Lookup is a binary search, `If-None-Match` answers 304, and fallbacks (Next's `404.html`, a `/{id}` page such as foolish's `[game_id]`) are rows of data in the table.
  The body is sent from `.rodata` without a copy, which needs a borrowed-body flag on `cb_http_res`, because `cb_res_body` copies today (http.h:67).
- **Each app is its own process.** The table is linked into that app's binary; no process serves two apps.
- **The proof that it does not slow the hot path:**
  - `hotpath_proof.sh` with its default list (`on_message`, `pump`, `cb_rt_publish`, `send_all`, `cb_ws_send`, `flush` and the rest) shows identical machine code.
    The borrowed-body flag touches `cb_res_serialize`, which is not on that list, so that function's diff is read and named in the PR.
  - An A/B run of the app's load generator with the site linked and not linked gives the same p50 and p99 for a push round trip, and the same server CPU per message.
  - A microbench reports the cost of one static hit in nanoseconds on the loop thread.
  - The site's bytes are counted in RSS, since `.rodata` pages in on first touch.

### 3.7 The ops panel

- **One metrics source.** Every app reports through PR #429's registry (`c_base/core/src/metrics.h`) and `GET /metrics`.
  adventurer's `/adv/ops` record becomes registry rows.
- **One reader.** structgen's `metrics.ts` from `metrics.h`, with birdseye's `readMetrics`, moves to `c_base/client/ts/ops/`.
- **One component.** `CbOps` (from birdseye's `Ops.tsx`, 135 lines): the platform's rows first, the app's after, rates as sparklines, histograms as p50/p90/p99.
  An app puts it on a page in one line.
- The admin console that c_base already serves (`core/src/admin/console.html`) gets the same view, so an app with no website still has one.

### 3.8 The load-test harness

`cbase-load` is a C library in `c_base/client/load/`, built natively on the same `cbc`.
- **Transport:** a non-blocking WebSocket and HTTP client on `cb_loop`, lifted from `wsbench.c:77-284` and `chessbench.c:107-304`.
  The upgrade key is random per connection instead of fixed.
- **Many clients per process:** one `cbc` per virtual user, which the context struct makes possible and the foolish singleton never did (`roundtrip.c:20-22` had to use the codecs alone for that reason).
- **Pacing and accounts:** connect rate per tick, account workers, and the session cache file, from chessbench.
- **Measurement:** percentiles from `shared/c/stats/hist.h`, server CPU and RSS from `shared/c/proc.h` and from the server's own `GET /metrics`, and a report in a fixed binary layout plus a text summary.
- **The app's part:** its own `main`, which loops `cbl_poll(L, now)` and reacts to each `(user, cbc_event)` pair, so there are no callbacks here either.
- lichess_ws uses only the transport and pacing, in a text-frame mode, because its protocol is lichess's JSON by contract.
  JSON stays in that test tool and never in a shipped path.

## 4. Where each piece lives, and the name

The name is **the c_base client**: C prefix `cbc_`, header `cbase/client.h`, TS package `@cbase/client`, load library `cbase-load`.

| Piece | Where |
|---|---|
| The C client (`conn.c`, `session.c`, `pump.c`, `gate.c`) | `c_base/client/src/`, header `c_base/client/include/cbase/client.h` |
| The platform's auth wire (`cbase/wire.h`, from `netwire.[ch]`'s platform range) | `c_base/client/include/cbase/wire.h`, read by the server too |
| The frame codec | `shared/c/stream_frame.h` (stays) |
| The wasm libc | `shared/c/wasm/libc.c` (stays) |
| The wasm exports and `cbase_client.wasm` | `c_base/client/wasm/` |
| The TS host shim, loader, React hooks, ops component | `c_base/client/ts/` |
| The Swift host shim | `c_base/client/swift/` |
| The load harness | `c_base/client/load/` |
| Static serving | `c_base/core/src/site/` and `c_base/tools/embed_site.c` |
| Histograms, process readers | `shared/c/stats/hist.h`, `shared/c/proc.h` (stay, from PR #429) |
| structgen, datagen | `shared/tools/` (stay) |
| Tests | `c_base/client/test/` (unit, fuzz with a corpus, and the `nc_trace` differential tool) |

`c_base/client` sits beside `c_base/core` because it is the other half of c_base's own protocol, not a helper that apps and c_base both happen to use.
It falls under the same product-name gate as the rest of `c_base/`.
Alex confirmed the split (section 6.1).

## 5. Migration order

Each step lands as its own PR with its own proof.
PR #429 (the packed frame heads, `/ws/public`, the metrics registry) merges first, since steps 2 to 5 use it.

1. **The library, used by nothing.** Done (2026-10-22): see the status below.
   `c_base/client` with `cbc`, `cbase/wire.h`, the TS shim, the wasm build, unit tests mutation-checked, a fuzz target with its corpus, and the size gate.
   Proof: its own tests, and the product-name gate.
   Deletes nothing.
2. **adventurer**, a read-only anonymous viewer with no wasm today. Done (2026-10-24): see the status below.
   It moves to `cbase_client.wasm` in anonymous mode, `socket.ts`, `CbOps`, and the metrics registry.
   Proof: its smoke's `/ws/public` test, and a browser check that the feed and the ops panel render.
   Deletes: `web/src/api.ts:247-344` (the socket, about 100 lines), `gen/stream_frame.ts` (103, generated), `Ops.tsx` (98), the `/adv/ops` route.
   It gains a client PING and half-open detection.
3. **birdseye**, a read-only viewer plus an in-process simulator.
   The web moves to `cbase_client.wasm`, the dev-only `/be/session` route goes away in favour of `/ws/public`, and `sim.c` and `league.c` run on `cbc` through the load transport.
   Proof: its smoke, the hot-path proof, and the same resync count in the simulator before and after.
   Deletes: `server/wasm/sf_wasm.c` (69), `api.ts:177-336` (about 160), `server/src/client.{c,h}` (283), `Ops.tsx` (135, replaced by `CbOps`), `routes.c:94-133`.
4. **lichess_ws**, only the load transport.
   Proof: `wsbench` reports the same moves per second within noise.
   Deletes: `wsbench.c:77-284` (about 200).
4b. **`/v1/auth` into the platform.**
   The four routes and their wire move from `foolish/server/src/app/v1_http.c` into `c_base/core`, byte for byte.
   Proof: foolish's smokes and `unit_v1` unchanged and green, and `hotpath_proof.sh`.
5. **c_chess**, a full client with an optimistic mirror and static serving.
   `client.c` keeps its chess half on top of `cbc`, `net.ts` and `kernel.ts` go to the shim, `web.c` goes to c_base's site serving, and `chessbench` goes to `cbase-load`.
   Its auth moves to the platform's `/v1/auth` and session blob (section 6.4).
   Proof: `smoke.c` (475 lines) unchanged and green, `chessbench` at or above 89,593 moves per second (DESIGN.md's configuration C), and the hot-path proof for the site step.
   Deletes: about 150 lines of `client.c`, about 180 of `net.ts`, about 70 of `kernel.ts`, `web.c` (142), and about 300 of `chessbench.c`.
   It gains PING, a pending-request timeout, resubscribe in C and write no-retry, which are intended changes to an app that is not live.
6. **foolish C**, the stage-9 split of PLATFORM.md.
   `netclient.c` becomes foolish's layer over one static `cbc`, and the `nc_*` names stay.
   Proof: everything in section 2.6.
   Deletes: the roughly 1,360 lines of section 2.1.
7. **foolish TS**: `backend.ts`, `sdk/ts/wasm`, `sdk/ts/net`, the session and the route guards go to `@cbase/client`.
   Proof: the e2e suite, `stream_dead_socket.test.ts` from its new home, the export-list diff, and one live game on a deploy preview.
   Deletes: the roughly 1,250 lines of section 2.2.
8. **foolish Swift**, with the next iOS build, after an OK on timing.
   Proof: `NetTests.swift`, and a live game on a device.
   Deletes: the roughly 410 lines of section 2.3.

### 5.1 Status

**Step 1, the library (branch `cbase-client`).**
- C: `c_base/client/include/cbase/client.h` and `auth_wire.h`, `src/client.c`; the wasm doors in `wasm/cbc_wasm.c`; the page shim in `ts/socket.ts`.
- The module is 9,819 bytes raw and 4,980 gzipped, under the 10,240 and 5,120 the size gate holds (`make -C c_base/client wasm`), and byte-identical when built on macOS (Homebrew clang 22.1.8) and on Linux (apt.llvm.org clang 22.1.8).
- Proof: 27 unit tests (456 checks) natively and under ASan and UBSan; a step-program fuzz target whose 1,035-input corpus replays in `make check` and which ran 2.2 million inputs on macOS and 709,458 in a 30-second smoke on Linux with no failure; three node tests of the shim over the real module; 40 mutants (34 in C, 6 in the shim), every one caught (`test/mutate.sh`, recorded in `mutation_checks.md`).
- Wiring: c_base's `make check` runs the client's check, the name gate holds `client/` to core's rule, and `c_base.yml` has a `client` job for the module, its size gate and the shim's tests.
- The full `make -C c_base check` and `check-asan` passed on macOS, and `check`, `check-asan` and `fuzz-replay` passed in the Linux CI image.

**Step 2, adventurer (branch `adventurer`).**
- The page's feed and its reads run on the client: the hand-written socket loop and its frame writers left `web/src/api.ts` (425 lines to 386), and the generated `stream_frame.ts` (103) is no longer made.
- The feed gained what it lacked: a client PING with the half-open close, jittered reconnects, and the platform's retries on its reads.
- Proof: `tsc --noEmit` under the page's strict settings, and the live page at 1440x900 (the feed, the ops pushes and every read), then a server killed and restarted under one open page, which came back live with the room held again on the new socket.

## 6. Decisions (Alex, 2026-10-22)

The five open questions of the first draft are answered, and two rules were added.

### 6.1 Where it lives

`c_base/client/`, as proposed, under the product-name gate; the helpers it uses stay in `shared/c`.

### 6.2 Reconnect jitter: on from the start

Every reconnect delay is jittered, in every app, foolish included, from the first commit; there is no flag that turns it off.
The schedule is **decorrelated jitter**: the next delay is drawn uniformly from `[retry_first_ms, 3 x the previous delay]` and capped at `retry_max_ms`, starting from `retry_first_ms` and starting over at every READY.
Why decorrelated and not full jitter:
- Full jitter draws from `[0, window]`, so a share of clients reconnect at once, at 0 ms, into a server that is just coming back; decorrelated jitter never goes below the floor (500 ms).
- It spreads a crowd as well as full jitter does: both were measured within a few percent of each other in total work and time to finish in AWS's study of backoff with contention, and both are far ahead of no jitter, which is what every app has today.
- It needs no attempt counter, only the previous delay, which the client already keeps.

The generator is `shared/c/rng.h`'s splitmix64, its state a field of the client, seeded per client by the host (`cbc_cfg.seed`).
The browser shim seeds it from `crypto.getRandomValues`, a native host from its own entropy, and every test with a constant, so a test's schedule is exact.
Only the reconnect delay is jittered; a refused subscription and a lost read keep their doubling schedule, since they are one client's retries and not a crowd's.

**The zero-change proof for foolish changes accordingly** (section 2.6, item 1): the differential trace compares every request, frame, event and finished call byte for byte, except the reconnect delay `cbc_closed` returns.
For each of those, the trace asserts instead that it lies in `[retry_first_ms, min(retry_max_ms, 3 x the previous delay)]`, with the previous delay the trace's own last one (or the floor after a READY).
Because a delay moves the time the next step happens, the trace program feeds both builds the same `now` values, taken from the program, never from the delay.

### 6.3 Site serving is a choice each app makes, explicitly

Each app declares one of two modes, and there is no default:
- **CDN.** The site is built and deployed on its own (foolish: `next build`, `web_deploy.sh`, S3 and CloudFront, which caches at the edge), and the app's process serves no site.
- **Serve.** The app's process serves its built site from memory (section 3.6).

The choice is written twice, and the two must agree:
- **In the app declaration.** `cb_app.site` is `{ .mode = CB_SITE_CDN }` or `{ .mode = CB_SITE_SERVE, .table = &app_site }`.
  Boot refuses an app whose mode is unset (0), a SERVE app with no table, and a CDN app with one.
- **In the ops and deploy config.** The app's deploy file names `SITE=cdn` or `SITE=serve`.
  For `cdn`, the deploy runs the site's own deploy (foolish's `web_deploy.sh`) and the binary is built without a site table; for `serve`, the build runs `embed_site` and links the table, and no site deploy runs.
  `cbase describe` prints the mode the binary was built with, and the deploy refuses to continue when it is not the one the config names.

Foolish declares CDN and stays on CloudFront.
c_chess, birdseye and adventurer declare SERVE when they move to it.

### 6.4 One login for every app, and one authentication per socket

Every app signs in through the platform's `/v1/auth` and listens anonymously through `/ws/public`.
c_chess's `/c/auth` and its JSON guest, and birdseye's dev-only `/be/session`, go away when those apps move.
One fact the first draft missed: `/v1/auth/*` is still served by foolish (`foolish/server/src/app/v1_http.c:690-693`), not by the platform.
Lifting those routes into `c_base/core`, byte for byte, is therefore a step of its own before the first signed-in app other than foolish moves (step 4b in section 5).

**Authenticate once per socket.**
This is the client's default model:
- A session is made over HTTP (`/v1/auth/signup`, `signin`, `refresh`): it has to exist before a socket can say who it is, and the hub refuses `/v1/auth/*` over the socket (`rt_local.c:431`).
- The socket authenticates once, with HELLO.
- Every call after that rides the socket as `SF_REQ`, answered by `SF_REPLY`, and carries no token: the hub runs the route with the connection's user.
- A call made while the socket is down waits for the next READY. It does not fall back to HTTP, so nothing re-authenticates per request.
- A socket never outlives its token (`rt_local.c`, "A SOCKET NEVER OUTLIVES ITS LOGIN").
  So the client refreshes over HTTP `refresh_early_ms` before expiry, and once no socket call is in flight it rolls the socket: it asks the host to close it and reopens at once with the new token, with no backoff and no jitter, since it is planned.

**What stays HTTP**, and why each does:
- `/v1/auth/*`, for the reason above.
- An anonymous client's reads: `/ws/public` carries HELLO, SUB and PING and nothing else, and these reads are public GETs that a CDN or the edge can cache.
  adventurer's lists are this case.
- Bytes that are not calls: the site, map tiles, a replay file.
- A route an app flags `CBC_R_HTTP`, which the client then sends over HTTP with the bearer.
  It exists for foolish's zero-change move (foolish sends its reads over HTTP today), and a later foolish PR clears the flag route by route.

### 6.5 TLS

The client is transport-agnostic: it reads and writes frames, and the host's socket may be `ws` or `wss`.
The browser shim takes the scheme from the server URL, which defaults to the page's own origin, so a page on `https` opens `wss` and a page on `http` opens `ws`; an explicit `http(s)` server URL maps the same way.
TLS termination in c_base is the c_chess agent's work; the native load harness uses it once it lands.

### 6.6 Swift and Kotlin

Swift with the next iOS build (step 8), Kotlin when an Android host exists.

## 7. Later

**Sign in with Apple, Google and other OAuth/OIDC providers, as platform auth.**
They belong to the platform, not to each app: a route such as `/v1/auth/oidc/<provider>` verifies the provider's ID token (the provider's published keys, fetched and cached by the platform) and answers the same session as signin, so the socket, the refresh and the client do not change.
The client gains one sign-in op that carries the provider's token; an app's config names only which providers it offers and its client ids.
Linking a provider to an existing account, and an account that has only a provider and no password, are the questions to settle then.
