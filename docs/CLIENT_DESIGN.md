# The cbase client: design

The cbase client is the other half of the platform's protocol: a sans-IO C core that signs in, holds the session and its refresh, keeps the socket to `cbase serve` (`GET /ws`, or `GET /ws/public` for a listener that never signed in), and carries an app's calls over it or over HTTP.
`client/include/cbase/client.h` is the contract; this document is the design and the reasons for it.
The sections keep their numbers because comments in the code cite them.

The design was made on 2026-10-22 from an inventory of the first apps' hand-written clients (foolish's, the most complete, and four smaller ones), and decided by Alex the same day (section 6).
That inventory and the plan to move each app onto the client are history, kept in this repository's git history before 2026-11-08 and in foolish's repository (https://github.com/alexsave/foolish).

## 1. The answer in one paragraph

What every realtime client rebuilds is the same: the frame codec, HELLO and READY, SUB and UNSUB with retries, request and reply correlation, PING and a deadline for its answer, the reconnect delay, the token's lifetime and refresh, an HTTP pump that retries reads and never writes, the session blob, and the host's socket shell.
That is the cbase client, written once in C and reached from every language through generated bindings.
What stays each app's is what its bytes mean: its push kinds and bodies, its endpoints and answers, which subscription needs which catch-up read, its mirror of the server's state and its optimistic rules, and its UI.

## 2. What is the client's, and what is the app's

The client's, in C (`client/src/client.c`):

- The session: sign-up, sign-in and sign-out over the platform's `/v1/auth` (`cbase/auth_wire.h`), the refresh before expiry, and an opaque blob the host persists (`cbc_session_save`, `cbc_session_load`).
- Sign in with Apple, opt-in under `CBC_OIDC` (`cbc_oidc_*`, `cbc_pkce`; [docs/AUTH_APPLE.md](AUTH_APPLE.md)).
- The socket: when it is wanted, its path, HELLO with the token once, counted subscriptions to rooms that are subscribed again on every READY, PING and the half-open close, and the jittered reconnect (6.2).
- Calls: an app's call names its method, path and flags, rides the socket as `SF_REQ` once it is up (6.4), retries a lost or busy read and never a write, and finishes as one `CBC_EV_DONE` event with the status, the refusal decoded and a span of the body.

The app's, in its own layer over the client:

- Its push kinds, declared below `SF_HELLO` in its own wire header; `cbc_in` raises `CBC_EV_PUSH` with the kind and a span of the body, which the app reads through its own generated readers.
- Its endpoints, as the calls its layer builds, and the answers it reads from each `CBC_EV_DONE`.
- Catch-up: `CBC_EV_SUB_LIVE` says a room just went live, so the app starts its own read of the room's state.
- Its mirror, its version gates and its optimistic apply and correction.

## 3. The interface

### 3.1 The rules it follows

- **Sans-IO.** The host owns the socket, the HTTP client and the timer, and passes in bytes and `now`, so one C file runs in a browser, in an iOS app and in a load generator.
- **A context struct, not statics.**
  The caller allocates `cbc`, so a load generator holds thousands per process.
- **No allocation, no clock, no randomness the caller does not pass in.**
  The core calls five C library functions (`memcpy`, `memmove`, `memset`, `memcmp`, `strlen`) and nothing else (`client/test/requires_gate.sh`), so a freestanding wasm32 module links it with five functions of its own.
- **No function-pointer interface.**
  The app's knowledge comes in as data (a call's method, path and flags) and goes out as events the app's own code reads after each call.
- **The output buffer is the caller's.**
  The client's state stays small: `cbc` is held under a budget of 12,288 bytes (`CBC_SIZE_BUDGET`) by a static assert, and the access token and the arena for waiting calls are most of it.
- **A large body is borrowed, not copied.**
  `cbc_call` copies its body into the arena, so a body past 4 KB does not fit.
  `cbc_call_ref` keeps only the caller's pointer and length, and the caller keeps the bytes valid until the call's `CBC_EV_DONE` (a retry sends from them again).
  It always goes over HTTP: `cbc_next_request` writes the head, path and Authorization with `CBC_RQ_REF`, and the host sends the body from `cbc_req_body`, so the core never reads or copies those bytes, and a page may pass an opaque handle instead of a pointer.
  Over HTTP and not the socket because one WebSocket message is one frame: a large one would hold every push and reply up behind it.

### 3.2 The C header

`client/include/cbase/client.h`, as built:

- **One door for everything the host reads.** `cbc_in`, `cbc_on_response`, `cbc_closed` and `cbc_tick` raise events, a finished call among them (`CBC_EV_DONE`), read with `cbc_n_events` and `cbc_event_at` after each.
- **No pointers in what other languages read.** A request is written into the caller's buffer as a `cbc_req_hdr` followed by its path, its Authorization value and its body, as a frame is, so structgen's readers reach every byte.
- **A call names its method, path and flags itself** (`CBC_C_IDEMPOTENT`, `CBC_C_FAIL_FAST`, `CBC_C_AUTH`, `CBC_C_HTTP`); a route table, if an app wants one, is its own layer's data.
- **The host's loop:** while `cbc_stream_wanted`, open `cbc_stream_path` and call `cbc_opened`; hand every message to `cbc_in` and send every frame `cbc_next_out` writes; on a close call `cbc_closed` and reopen after the ms it answers; send each request `cbc_next_request` writes and answer it with `cbc_on_response`; and wake after `cbc_due_ms` to call `cbc_tick`.

### 3.3 How an app plugs in its bodies

The app's code is a thin layer over `cbc`, and it is where the app's knowledge lives (section 2).
A C app links the core into its own program through `client/client.mk` (`CBC_SRC`, `CBC_INC`, and `CBC_OIDC_SRC` with `CBC_B64_SRC` and `CBC_OIDC_CFLAGS` for the provider sign-in), so a digest it already links is shared, never doubled.
Each of lib/'s files is in one of those lists only: `lib/b64/b64.c` is `CBC_B64_SRC`, which the provider sign-in and the cdom host both need, so an app with both has nothing to dedupe.
An app with no C of its own on the client loads the prebuilt module (3.5) and reads its events through the TypeScript shim.

### 3.4 The hosts and the generated surfaces, per language

structgen reads `cbase/client.h` and writes, for each language, the readers of `cbc_req_hdr`, `cbc_event` and `cbc_user`, the writer of `cbc_cfg`, the module's typed exports, and a layout hash the host checks at load.
No host spells a byte offset: every byte is read by C or by a structgen reader.

The hand-written host is the one part per language that cannot be C, and it holds only I/O:

- C: `client/hosts/common/cbc_host.h` is the shared host, everything `client.h` asks of a host that does not depend on the platform; `client/hosts/posix` puts it on a native event loop.
  Every host passes one suite against a scripted server (`client/test/host/host_suite.h`).
- TypeScript: `client/ts/socket.ts` holds one WebSocket, `fetch`, one timer, and `localStorage` for a signed-in client's session; `client/ts/react/SignInWithApple.tsx` is the Apple button.
  `make -C client ts` copies them, the generated files and the module into an app's generated directory.
- Swift: `client/swift/CBaseClient/CbClient.swift` is the HTTP half (URLSession, one timer, the randomness the core asks for), with `AppleSignIn.swift` and `SignInWithAppleView.swift`; the socket half, `CbSocket`, is not written yet.
- Kotlin: structgen emits Kotlin, so the generated surface comes free when an Android host is written.

### 3.5 Wasm packaging and its size budget

Two modules from one source, held by a size gate in CI that prints the numbers and fails past a budget (`make -C client wasm`):

- `cbase_client.wasm`, the base module every page may load: at most 10,240 bytes raw and 5,120 bytes as `gzip -9` sends it.
- `cbase_client_oidc.wasm`, which adds the provider sign-in and its SHA-256, base64url and hex: at most 13,312 and 7,168.

The module is a final program, so it brings the C library its objects require, and `client/wasm/` is all of it (`include/string.h` and `libc.c`); nothing in cbase defines a C library for anything else (`tools/libc_gate.sh`).
It is built with a pinned clang, and the same source gives the same bytes on macOS and Linux.

### 3.6 Static files

The client does not serve an app's site.
An app serves its built pages and assets from disk with `core/src/net/files.h` (docs/PLATFORM.md 2.11), or deploys them to a CDN of its own; which one is the app's choice (6.3).

## 4. Where each piece lives

| Piece | Where |
|---|---|
| The C core | `client/src/client.c`, header `client/include/cbase/client.h` |
| The platform's session wire, read by the server too | `client/include/cbase/auth_wire.h` |
| The frame codec | `lib/stream_frame.h` |
| The make fragment a C app includes | `client/client.mk` |
| The shared host and the native one | `client/hosts/common/`, `client/hosts/posix/` |
| The wasm doors, its libc, and the modules | `client/wasm/`, built into `client/build/wasm/` |
| The TypeScript host and the React button | `client/ts/` |
| The Swift host and an example app | `client/swift/` |
| Tests | `client/test/` (unit, the host suite, the shim's node tests, the mutation script), `client/fuzz/` (a step-program fuzz target and its corpus) |
| structgen | `tools/structgen` |

`client/` sits beside `core/` because it is the other half of cbase's own protocol, not a helper that apps and cbase both happen to use.
The name gate holds it to core's rule: it names no product, no provider and no product's domain.

## 5. Status

Built: the C core with the session, the socket, calls and Sign in with Apple; the shared and POSIX hosts; the wasm modules and their size gate; the TypeScript host and the React Apple button; the Swift HTTP half and its Apple sign-in.
Proof: the unit tests and the host suite, natively and under ASan and UBSan; a step-program fuzz target whose corpus replays in `make check`; the shim's tests over the real module (`make -C client ts-test`); and the mutants of `client/test/mutate.sh`, recorded in [docs/mutation_checks.md](mutation_checks.md).
cbase's `make check` runs the client's check, and CI's client job builds the modules, holds the size gate and runs the shim's tests.
Not built yet: the Swift socket half, the load-test library and an ops panel component (section 7).

## 6. Decisions (Alex, 2026-10-22)

### 6.1 Where it lives

`client/`, under the name gate; the helpers it uses (the frame codec, the generator, the digests) stay in `lib/`.

### 6.2 Reconnect jitter: on from the start

Every reconnect delay is jittered, in every app, from the first commit; there is no flag that turns it off.
The schedule is **decorrelated jitter**: the next delay is drawn uniformly from `[retry_first_ms, 3 x the previous delay]` and capped at `retry_max_ms`, starting from `retry_first_ms` and starting over at every READY.
Why decorrelated and not full jitter:

- Full jitter draws from `[0, window]`, so a share of clients reconnect at once, at 0 ms, into a server that is just coming back; decorrelated jitter never goes below the floor (500 ms).
- It spreads a crowd as well as full jitter does: both were measured within a few percent of each other in total work and time to finish in AWS's study of backoff with contention, and both are far ahead of no jitter, which is what every hand-written client of the first apps did.
- It needs no attempt counter, only the previous delay, which the client already keeps.

The generator is `lib/rng.h`'s splitmix64, its state a field of the client, seeded per client by the host (`cbc_cfg.seed`).
The browser shim seeds it from `crypto.getRandomValues`, a native host from its own entropy, and every test with a constant, so a test's schedule is exact.
Only the reconnect delay is jittered; a refused subscription and a lost idempotent call keep their doubling schedule, since they are one client's retries and not a crowd's.

### 6.3 Static files are a choice each app makes

An app either serves its built site from its own process (`net/files.h`) or deploys it to a CDN and serves no site; cbase decides neither for it.
An in-memory, embedded site table was designed and not built: serving from disk was enough, and a rebuild on disk is what the next request gets.

### 6.4 One login for every app, and one authentication per socket

Every app signs in through the platform's `/v1/auth` (on when the app sets `auth_routes`, docs/PLATFORM.md 2.9) and listens anonymously through `/ws/public`.

**Authenticate once per socket.**
This is the client's default model:

- A session is made over HTTP (`/v1/auth/signup`, `signin`, `refresh`): it has to exist before a socket can say who it is, and the hub refuses `/v1/auth/*` over the socket.
- The socket authenticates once, with HELLO.
- Every call after that rides the socket as `SF_REQ`, answered by `SF_REPLY`, and carries no token: the hub runs the route with the connection's user.
- A call made while the socket is down waits for the next READY. It does not fall back to HTTP, so nothing re-authenticates per request.
- A socket never outlives its token (`providers/local/src/realtime/rt_local.c`).
  So the client refreshes over HTTP `refresh_early_ms` before expiry, and once no socket call is in flight it rolls the socket: it asks the host to close it and reopens at once with the new token, with no backoff and no jitter, since it is planned.

**What stays HTTP**, and why each does:

- `/v1/auth/*`, for the reason above.
- An anonymous client's reads: `/ws/public` carries HELLO, SUB and PING and nothing else, and these reads are public GETs that a CDN or the edge can cache.
- Bytes that are not calls: the site, images, a large file.
- A borrowed body (`cbc_call_ref`, 3.1).
- A call an app flags `CBC_C_HTTP`, which the client then sends over HTTP with the bearer.

### 6.5 TLS

The client is transport-agnostic: it reads and writes frames, and the host's socket may be `ws` or `wss`.
The browser shim takes the scheme from the server URL, which defaults to the page's own origin, so a page on `https` opens `wss` and a page on `http` opens `ws`; an explicit `http(s)` server URL maps the same way.

### 6.6 Swift and Kotlin

Swift's socket half comes with the next iOS client; Kotlin when an Android host exists.

## 7. Later

- **The Swift socket half** (`CbSocket`), with the same host suite as the C hosts.
- **A load-test library** on the same `cbc`: a non-blocking WebSocket and HTTP transport on `cb_loop`, one `cbc` per virtual user (which the context struct makes possible), connect pacing, and percentiles from `lib/stats`, with the app's own `main` reacting to each `(user, cbc_event)` pair, so there are no callbacks.
- **An ops panel component** over the platform's `GET /metrics` (`core/src/metrics.h`), with the platform's rows first and the app's after.
- **More identity providers.** Sign in with Apple is built (2026-10-24): `POST /v1/auth/oidc` with `CBW_IDP_APPLE`, the client's `cbc_oidc_*`, linking only when a signed-in account adds an identity (never by email), and a provider-only account with no password ([docs/AUTH_APPLE.md](AUTH_APPLE.md)).
  Another provider is another `CBW_IDP_*`, its keys fetched and cached by the platform, and the same session answer, so the socket, the refresh and the client do not change.
