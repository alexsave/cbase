# c_base design

c_base is the foolish backend written in C, replacing Supabase (Postgres, PostgREST, GoTrue, Realtime, the Deno edge functions, pg_cron, pg_net and Vault).
It runs on AWS Lambda as a custom runtime in production, and as a local server for development and tests.
The shipped web and iOS clients switch to it by changing a URL and an anon key, and nothing else.

This document is the brief the orchestrator wrote (2026-10-13), expanded, with the Lambda addendum folded in.
Each module section names its owner directory and public header, and has an "As built" note its builder fills in.
The decisions here are settled; a builder who finds one impossible says so in the "As built" note rather than quietly working around it.

## Decisions

1. **Language: C.**
   Toolchain: make and clang or gcc through `shared/tools/c_rules.mk`, the same rules `foolish/c` uses.
   Dependencies: SQLite, OpenSSL libcrypto (and libssl for the AWS HTTPS client), pthreads.
   Nothing else.
2. **Shape: one binary per environment, one handler.**
   `bootstrap` is the Lambda custom runtime (provided.al2023, arm64, statically linked).
   `cbase serve` is a plain HTTP and WebSocket server on 127.0.0.1:8080 for development, the tests and the browser acceptance run.
   Both call the same `cb_handle` (src/handler.h).
3. **Store: one interface, two backends.**
   SQLite for `cbase serve`, the tests and the import rehearsal; DynamoDB in production.
   Every write a request makes is done before its response is sent.
4. **Consistency: the Supabase backend's fake consistent server, rebuilt.**
   A compare-and-swap on the game's version, five attempts, a bot lease, a heartbeat scan.
   Clients gate on the version exactly as today.
5. **Bots: no timers.**
   A commit that leaves bot work starts a drive (an async self-invoke under Lambda, a worker pool under serve); a scan every minute catches what a lost kick missed.
6. **Client surface: a Supabase-compatible facade.**
   The four edge functions byte for byte, the GoTrue auth routes, the PostgREST reads the clients make, and Realtime's Phoenix protocol.
   iOS installs cannot be updated in place, so old installs keep calling Supabase until they update; the runbook says so.
7. **Admin console:** one static HTML file served at /admin, its JSON API at /admin/api/*, behind a Bearer token.
8. **Ops:** one CloudFormation template (`ops/cbase.yaml`) Alex uploads in the console; no AWS CLI needed locally.
9. **Migration:** an export through the Supabase Management API, and `cbase-import` writing through the store interface.
10. **Tests in C**, every one mutation-checked.
11. **Conventions:** no em dashes; no JSON outside the list below; no hand-rolled kernel byte layouts (the kernel's own encoders and decoders only); every kernel call takes now_ms from the host; the kernel never sees a socket, a JWT, SQLite or DynamoDB.

## Why Lambda, and what it costs us

Alex chose Lambda because it is free at this scale: 1M requests and 400k GB-seconds a month, DynamoDB's always-free 25 GB, a free Function URL, CloudFront's free plan, free ACM certificates and a free EventBridge Scheduler.
The only line that is not free after the first year is API Gateway's WebSocket API, at $1 per million messages and $0.25 per million connection-minutes, which at our volume is cents a month.
Expected total: about $0 to $0.10 a month.

What it costs is the thing the one-VM design would have deleted.
A long-lived process could hold every game in RAM under a mutex and treat the database as a log.
A Lambda has no process between invocations, so the "one consistent server" is an illusion again, and we rebuild it on AWS primitives, deliberately:

- **The compare-and-swap.**
  A request loads the game row, runs one kernel section, and commits with `UpdateItem` conditioned on `version = :expected` (inside a `TransactWriteItems` with the membership and rating writes).
  A lost race reloads and re-runs, up to five times.
  This is `commit_table`'s `WHERE version = p_expected_version` and `table_io.ts`'s `runTableOp`, moved.
- **The bot lease.**
  A conditional update on `lease_until`, 25 s, renewed every cycle, released on every exit path.
  This is `try_acquire_bot_lease`, `renew_bot_lease` and `release_bot_lease`, moved.
- **The heartbeat.**
  EventBridge Scheduler invokes the scan every minute: games that need bots, untouched for 10 s, active within the hour.
  This is `bot-heartbeat/index.ts` and its pg_cron job, moved.
- **The kick.**
  An async self-invoke (`InvocationType: Event`) per drive, re-invoking itself when its budget ends with work left.
  This is `scheduleBotLoop` with `EdgeRuntime.waitUntil`, moved.
- **A per-container cache** of rows the container just committed, as `game_cache.ts` had: a stale entry costs one CAS conflict, and a refusal computed from the cache is re-checked fresh.

The semantics being preserved live in `foolish/server/impls/supabase/functions/_shared/adapter/table_io.ts`, `bot_actions.ts`, `game_cache.ts`, `finalize.ts` and `functions/bot-heartbeat/index.ts`.
A builder who changes one of these behaviours writes down why.

The other cost is JSON.
The Lambda event envelope, the DynamoDB API, the API Gateway Management API and the Lambda Invoke API are JSON by AWS's design.
That tax is confined: only `src/aws/` and `src/store/store_dynamo.c` read or write it, and nothing above `store.h` and `event.h` knows it exists.

S3 with conditional writes (`If-Match` on an ETag) was the binary-bodied alternative for the store, at about $0.10 a month.
It was not chosen because DynamoDB costs nothing and gives the query indexes (a user's games, a topic's connections, the bot scan) that S3 would have needed a second store for.

## Two mains, one handler

```
                 +-- bootstrap (main_lambda.c) --+     +-- cbase serve (main_serve.c) --+
 event / socket  | runtime API loop, event.h     |     | net/: loop, server, http, ws   |
                 +---------------+---------------+     +----------------+---------------+
                                 |   cb_http_req / cb_http_res          |
                                 +---------------> cb_handle <----------+
                                                  (handler.c: CORS, apikey, 404/405, /health)
                                                      | routes
          auth_http  postgrest  functions_http  admin  (rt_local's upgrade, serve only)
                                                      |
              app/table_ops (load, kernel, CAS)  app/bots  app/fanout -> realtime (phoenix)
                                                      |
                       store.h: store_sqlite | store_dynamo        realtime: rt_local | rt_apigw
```

`ctx.h` holds the three seams: `ctx->store`, `ctx->rt` (with its transport), and `ctx->kick_bots`.
`boot.c` brings a context up the same way for both mains, runs the kernel self-test first, and leaves a module whose constructor answers `CB_E_NOTIMPL` as NULL with a warning, so the skeleton serves /health while modules are being built.
Every handler answers 503 when a handle it needs is NULL.

Under serve, `cb_handle` runs on a request pool for every route but the ones flagged `CB_ROUTE_LOOP` (the WebSocket upgrade, /health), so SQLite and bcrypt never block the network loop.
Under Lambda one invocation is one request.

## Module: util (foundation, complete)

Owner: `src/util/`.
Headers: `err.h` (the process-wide codes), `str.h` (cb_buf, spans, URL decoding), `b64.h`, `hex.h` (with the Postgres bytea spelling), `uuid.h` (the one CSPRNG entry point), `clock.h` (wall and monotonic ms, ISO-8601 both ways), `log.h` (stderr plus a ring buffer the console tails), `json.h` (a small writer and a tolerant path reader), `pool.h` (a thread pool).

As built: complete and tested (`test/unit_util.c`).
The JSON reader validates what it walks, so a document cut after the value asked for still answers; `cb_json_valid` is the strict check.

## Module: net (foundation, complete)

Owner: `src/net/`.
Headers: `loop.h`, `http.h`, `ws.h`, `router.h`, `server.h`.
Used by `cbase serve`, by the fake Runtime API in `test/smoke_lambda.c`, and (`http.h`, `router.h`) by both mains through `cb_handle`.

As built: lifted from `foolish/server/impls/native`.
`httpd.c`'s one-pass header scan and `ws.c`/`shard.c`'s frame codec were made pure functions over a buffer.
Changed on the way: the native Content-Length scan overflowed an int on a long digit string and now stops at the body limit; a second differing Content-Length, any Transfer-Encoding, and a bare CR or LF in a header line are refused; the WebSocket 8-byte length's top bit and the reserved bits are rejected outright; SHA-1 comes from libcrypto.
The loop is poll(2), the same code on macOS and Linux.
A closed connection is freed on the next loop pass, so a callback further up the stack never touches freed memory.
Tests: `unit_ws.c` (the native frame test, ported), `unit_http.c`, `unit_net.c` (a real server over loopback).

## Module: store

Owner: `src/store/` and `sql/schema.sql`.
Header: `src/store/store.h`, one table of functions (`cb_store_ops`) and the `CB_STORE_CALL` wrapper.
Backends: `store_sqlite.c` (local, tests, import rehearsal) and `store_dynamo.c` (production).

SQLite: one file, WAL, foreign keys on, `sql/schema.sql` (embedded into the binary at build time) applied in one transaction at every open, `PRAGMA user_version` set to `CB_SCHEMA_VERSION`, and a database newer than the binary refused.
`game_commit` is one `BEGIN IMMEDIATE` transaction whose `UPDATE games ... WHERE version = ?` is the CAS.

DynamoDB: one table `cbase`, PAY_PER_REQUEST, point-in-time recovery on, TTL on `expires_at`.
Items, by partition key and sort key:

| pk | sk | what |
|---|---|---|
| `USER#<id>` | `PROFILE` | email, bcrypt hash, username, created, last sign-in |
| `USER#<id>` | `RATING` | elo_rating, previous_elo, games_played, username (the leaderboard row) |
| `EMAIL#<lowercase email>` | `USER` | the user id (signup's uniqueness is a conditional put on this) |
| `REFRESH#<sha256 hex>` | `TOKEN` | user, session, created, revoked |
| `GAME#<id>` | `ROW` | state, roster, logs_packed, seed (Binary); version, round_epoch, status, needs_bots, created/updated/last_commit; lease_token, lease_until |
| `GAME#<id>` | `MEMBER#<user>` | a seated human; GSI1 `USERGAMES#<user>` / updated_at gives the dashboard |
| `GAME#<id>` | `BOT#<bot id>` | a seated bot |
| `BOT#<id>` | `BOT` | the 49 bots, with their ratings |
| `SNAP#<game id>` | `<created>#<id>` | a replay; one GSI2 item per player `PLAYER#<user>` for "my snapshots" |
| `CHAT#<game id>` | `<created>#<id>` | a chat message |
| `CONN#<connection id>` | `CONN` | a realtime connection: user, vsn, topics, expires_at (TTL) |
| `TOPIC#<topic>` | `CONN#<connection id>` | the reverse index fan-out queries |
| `SCAN` | `GAME#<id>` | present while a game needs bots (sparse; the heartbeat's scan reads one partition) |

`game_commit` is one `TransactWriteItems`: the game update conditioned on the version, the membership puts and deletes, the rating updates, the SCAN marker.

The 49 bots: `sql/schema.sql` seeds the nicknames and strategy keys of `seed.sql` with FIXED ids (UUIDv5 of the nickname), because `seed.sql` leaves bot ids to `uuid_generate_v4()` and so they differ per database.
A migrated store replaces them with the hosted rows, since the hosted rosters name the hosted ids.

As built: (store builder)

## Module: auth

Owner: `src/auth/`.
Headers: `auth.h` (users, sessions, verify a bearer), `jwt.h` (HS256 in GoTrue's claim shape), `bcrypt.h` (to embed: a public-domain OpenBSD-derived bcrypt with its test vectors), `auth_http.h` (the six GoTrue routes).

Email and password only; the clients derive the email from the username.
Access tokens are HS256 JWTs signed with `CBASE_JWT_SECRET`, 3600 s; refresh tokens are 32 random bytes, stored hashed, rotated on use.
GoTrue's bcrypt hashes import unchanged, so existing players keep their passwords.
A username with '%' is refused.
Error bodies follow GoTrue's shape only as far as supabase-js and supabase-swift read them.

As built: (auth builder)

## Module: app

Owner: `src/app/`.
Headers: `table_ops.h` (one operation per request: load, kernel section, CAS commit with retries, finalize, views on demand), `bots.h` (drive, scan, the two kick functions), `fanout.h` (after a winning commit, who hears), `functions_http.h` (create, action, meta, delete-account, byte for byte).

The kernel section uses per-thread scratch (a `Game` of about 37 KB and `TableSnaps` of about 46 KB).
`table_commit_products` takes a const table, so when its products ask for ratings the C host reads them, calls `table_rate` and commits again without re-running the operation.
Views (player_views, spectator_views) are computed from the stored row on demand, never stored.

As built: (app builder)

## Module: realtime and PostgREST

Owner: `src/realtime/`.
Headers: `realtime.h` (the publish calls and the transport interface), `phoenix.h` (the protocol, shared), `rt_local.h` (serve's in-process WebSocket), `rt_apigw.h` (API Gateway's WebSocket API and CONN rows), `postgrest.h` (the reads the clients make, and the chat insert).

Both Phoenix serializers are needed: realtime-js 2.x sends `vsn=2.0.0` arrays, supabase-swift `vsn=1.0.0` objects.
Topic policy in C mirrors the `realtime.messages` policies: `pv-<uid>` only that user, `gu-<game>-<uid>` that user if a member, `game-<game>` any authenticated user, `chat:<game>` members.
There is no catch-up, as today: a client that missed a push refetches its view row when it rejoins.
The web client reaches API Gateway through CloudFront at the same domain, `/realtime/v1/websocket` rewritten to the stage by a CloudFront Function; if that proves fragile, the fallback is a client-side realtime URL override, documented, not built.

As built: (realtime builder)

## Module: aws

Owner: `src/aws/`.
Headers: `runtime_api.h` (complete: the Lambda Runtime API client), `event.h` (the event envelope in, the response out), `sigv4.h` (signing and credentials), `https.h` (a TLS client on OpenSSL), `apigw_mgmt.h` (post to a connection), `lambda_invoke.h` (the async self-invoke).
The stubs live together in `aws_stubs.c`; each builder file replaces its part and deletes it there.

As built: `runtime_api.c` is real and exercised by `test/smoke_lambda.c` against the real `bootstrap`.
The rest: (aws builder)

## Module: admin

Owner: `src/admin/` and `admin/console.html`.
Header: `src/admin/admin.h`.
One HTML file, vanilla JS, light and dark, under about 1500 lines, embedded at build time like the schema.
Panels: Overview, Tables (DynamoDB by key prefix, SQLite by table), Query (PartiQL or SQL, SELECT only unless confirmed), Users, Games, Bots, Logs (this process's ring, plus a CloudWatch Logs Insights link under Lambda), Config (secrets redacted), Backups (point-in-time recovery explained; SQLite `VACUUM INTO`).

As built: (admin builder)

## Module: ops and migrate

Owner: `ops/` (`cbase.yaml`, `package.sh`, the GitHub workflow) and `migrate/` (`export.sh`, `import.c`, `out/` gitignored).
`make bootstrap` already produces `dist/bootstrap` and `dist/bootstrap.zip`: a stripped static Linux arm64 binary built in an arm64 Alpine container pinned by digest (`tools/build_bootstrap.sh`), byte-for-byte reproducible across runs on this Mac.
Alpine (musl) rather than Ubuntu because glibc's static `getaddrinfo` still loads NSS modules at run time.
`ops/package.sh` should call it rather than repeat it.
The export holds password hashes: it lives under `migrate/out/`, is gitignored, and is deleted after the rehearsal.

As built: (ops builder)

## Tests

`make test` builds and runs every `test/unit_*.c` as its own program; `make smoke` runs both smoke lanes; `make check` is both; `make check-asan` rebuilds everything, the kernel included, under ASan and UBSan.
Test files are found by wildcard, and every `test/lib_*.c` is linked into every test program (`lib_client.c` is an HTTP and WebSocket client, `lib_proc.c` starts a binary as a child).
`test/smoke.c` starts `cbase serve`; `test/smoke_lambda.c` is a fake Runtime API that drives the real `bootstrap`.
The smoke builders extend `smoke.c` to the brief's full scenario: two humans, a bot, a whole game over HTTP and WebSocket, ratings changed, a snapshot stored, pushes with monotone versions, a restart, the game reloaded.
Every test is mutation-checked: break the code, see the right assertion fail, restore by re-editing (never `git checkout`), and remember that make compares whole seconds, so delete the objects after restoring.

## Kernel thread safety

`cbase serve` runs kernel calls on several threads, so the kernel is compiled with its `_Thread_local` qualifiers intact (foolish/c's own Makefile strips them for its single-threaded programs).
One kernel global is not per-thread: `bot_drive_pre_action_hook` (bot_drive.c), which `table_bot_drive` sets and restores around each drive.
Two concurrent drives race on the restore, and one of them can lose its per-decision seeding.
So serve's bot pool defaults to one thread (`CBASE_BOT_WORKERS=1`) until the kernel makes that hook `_Thread_local`, a one-word kernel change outside c_base.
Lambda runs one invocation per container at a time and is not affected.
Human moves and lobby edits do not touch the hook.

## Where JSON is tolerated, and why

The kernel's bytes never become JSON.
JSON appears only where something outside c_base speaks it and cannot be changed:

1. GoTrue's auth bodies, because supabase-js and supabase-swift send and read them (`src/auth/auth_http.c`).
2. PostgREST's rows and the chat insert, because both clients read rows through supabase-js and supabase-swift (`src/realtime/postgrest.c`); the view bytes inside them are `\x` hex, as today.
3. Phoenix frames, because realtime-js and supabase-swift speak them (`src/realtime/phoenix.c`); a cast inside them is base64, as today.
4. The admin console's API, because it is a browser page we own and JSON is what `fetch` reads (`src/admin/`).
5. AWS's APIs: the Lambda event and response envelope, the Runtime API's error body, DynamoDB, the API Gateway Management API and Lambda Invoke (`src/aws/`, `src/store/store_dynamo.c`).

The edge function bodies (create, action, meta, delete-account) stay binary, as `e2e/validation/no_json_validation.test.ts` holds them today.

## Phase 2: the binary surface, and deleting the facade (not built now)

Items 1 to 3 above are a compatibility tax paid only because the shipped clients speak Supabase.
Phase 2 replaces them:

- binary auth endpoints (sign up, sign in, refresh, sign out, change password) whose bodies are kernel-style packed structs with generated writers;
- binary reads (my games with their envelopes, one game's envelope, chat history, the leaderboard, snapshots) in the same style;
- a binary push stream on the WebSocket: each message a version and a kernel cast or envelope, so a reconnecting client can ask for what it missed by version (the catch-up the Phoenix facade never had);
- the web and iOS clients moved onto them, behind their existing version gates;
- then the facade (`auth_http.c`, `postgrest.c`, `phoenix.c`) deleted, once the last client that speaks it has aged out (iOS builds in the wild are the long pole).

The AWS JSON (item 5) stays, because AWS sets it.

## Corrections to the brief, found while building the skeleton

- `seed.sql` does not give the bots fixed ids (it uses `uuid_generate_v4()`), so "copy the ids" was impossible; parity is of nicknames and strategy keys, which is what `e2e/bot_roster_parity.test.ts` checks, and the ids are fixed here instead (see Store).
- realtime-js 2.x speaks Phoenix `vsn=2.0.0` (arrays), not `1.0.0` (objects) as the brief said; supabase-swift may use either, so both are required (see Realtime).
- PostgREST lives under `src/realtime/` as the brief's layout said, though it is not realtime; it stays there to keep the builders' directories disjoint.

## Open defects carried over from Supabase

Kept on purpose, so behaviour does not change under the clients; each is a later decision, not an accident: no rate limiting (create is unlimited, BUMP is open to non-members), ratings are world-readable, `game_snapshots.player_ids` survives account deletion, nothing cleans up stale lobbies or old snapshots, the game id space is 24 bits with five redraws, a snapshot failure is logged and swallowed, and there is no realtime catch-up.

## Suggested builder split

Five builders, disjoint directories, each coding against the headers above:

1. store: `src/store/`, `sql/`, `migrate/`.
2. auth: `src/auth/`.
3. app: `src/app/`, and the smoke scenario in `test/smoke.c`.
4. realtime and aws: `src/realtime/`, `src/aws/` (they meet at `rt_apigw.c`).
5. admin and ops: `src/admin/`, `admin/`, `ops/`, `.github/workflows/c_base.yml`.

A builder who needs a header outside its directory changed asks the orchestrator first; the header is the contract the other four are coding against.
