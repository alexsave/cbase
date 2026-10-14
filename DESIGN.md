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

DynamoDB: one table `cbase`, PAY_PER_REQUEST, point-in-time recovery on, TTL on `expires_at` (epoch seconds).
Two indexes, `gsi1` (`gsi1pk`, `gsi1sk`) and `gsi2` (`gsi2pk`, `gsi2sk`), all four keys String, both projecting ALL.
`src/store/dynamo_wire.h` owns these names (`DW_GSI1`, `DW_GSI2`, `DW_TTL_ATTR`), and `test/unit_dynamo.c` reads `ops/cbase.yaml` and fails when the template disagrees.
Items, by partition key and sort key:

| pk | sk | what |
|---|---|---|
| `USER#<id>` | `PROFILE` | email, bcrypt hash, username, created, last sign-in |
| `USER#<id>` | `RATING` | elo_rating, previous_elo, games_played, username (the leaderboard row) |
| `EMAIL#<lowercase email>` | `USER` | the user id (signup's uniqueness is a conditional put on this) |
| `REFRESH#<sha256 hex>` | `TOKEN` | user, session, created, revoked, revoked_at; gsi1 `USERREFRESH#<user>`, gsi2 `SESSION#<session>` |
| `GAME#<id>` | `ROW` | state, roster, logs_packed, seed (Binary); version, round_epoch, status, needs_bots, created/updated/last_commit; lease_token, lease_until |
| `GAME#<id>` | `MEMBER#<user>` | a seated human; gsi1 `USERGAMES#<user>` / `<updated_at, 15 digits>#<game>` gives the dashboard |
| `GAME#<id>` | `BOT#<bot id>` | a seated bot |
| `BOT#<id>` | `BOT` | the 49 bots, with their ratings |
| `SNAP#<snapshot id>` | `SNAP` | a replay (gsi1 `GAMESNAPS#<game>`); it outlives its game, as `ON DELETE SET NULL` did |
| `SNAP#<snapshot id>` | `PLAYER#<user>` | one copy per player, gsi2 `PLAYER#<user>`: "my replays" is one query |
| `CHAT#<game id>` | `<created>#<id>` | a chat message |
| `CONN#<connection id>` | `CONN` | a realtime connection: user, vsn, topics, expires_at (TTL, seconds) and expires_ms |
| `TOPIC#<topic>` | `CONN#<connection id>` | the reverse index fan-out queries: a whole copy of the connection row with the same TTL, in the base table so the read is strongly consistent and a push right after a join is not missed |
| `SCAN` | `GAME#<id>` | present while a game needs bots (sparse; the heartbeat's scan reads one partition) |

`game_commit` is one `TransactWriteItems`: the game update conditioned on the version, the membership puts and deletes, the rating updates, the SCAN marker.

The 49 bots: `sql/schema.sql` seeds the nicknames and strategy keys of `seed.sql` with FIXED ids (UUIDv5 of the nickname), because `seed.sql` leaves bot ids to `uuid_generate_v4()` and so they differ per database.
A migrated store replaces them with the hosted rows, since the hosted rosters name the hosted ids.

As built (store builder, then integration):

- `store_sqlite.c` implements every operation over one connection with cached prepared statements; one process mutex spans each operation, and list callbacks run after the lock is released so a callback may call the store again.
- `game_commit` mirrors `commit_table`: the version-fenced `UPDATE`, membership and bot seats rewritten, and ratings added to the rows as they stand (clamped at 0, `games_played + 1`, `previous_elo` the old value, a missing human rating row created when the account exists).
- Membership is rewritten on every commit (the header's contract), where `commit_table` touched it only when the roster changed; so a seat whose account is gone (delete-account keeps the id in the roster) is skipped in both backends instead of failing every later commit on the foreign key.
  The app relies on exactly this, since a redacted roster still names the deleted id; the store tests hold both backends to it (`cas_second_writer_loses` commits with a deleted seat on SQLite, `flow_commit_drops_missing_account` on DynamoDB).
- `last_commit_at` is stamped by the store from `row->updated_at` on every commit; `row->last_commit_at` is read only by the import.
- The lease: acquire and renew move `updated_at` (never `last_commit_at`); release clears `lease_until` and does not move `updated_at`.
- `game_commit`, `game_delete` and `lease_acquire` on a game that is gone return `CB_E_NOT_FOUND` (DynamoDB tells it apart from a lost CAS with `ReturnValuesOnConditionCheckFailure: ALL_OLD`).
- `dynamo_wire.c` spells every DynamoDB string and builds every request as a pure function over buffers; `store_dynamo.c` decides only which requests to send in which order.
  Its transport is `aws/aws_call.h` (one signed-request step for every AWS call in c_base), and it honours `AWS_ENDPOINT_URL_DYNAMODB`, the SDKs' own override.
- Ratings on DynamoDB are read, clamped in C and written fenced on the value read, because an update expression cannot clamp; a rating another table moved cancels the transaction and the commit re-reads and resends (up to 5 times).
- Refresh tokens carry `revoked_at`: the first `refresh_take` stamps it (later takes keep it), and `refresh_revoke_session` and `refresh_revoke_user` set it to 0, so auth can grant GoTrue's 10 s reuse grace to a rotation and never to a logout (see Auth).
- The snapshot is not inside `game_commit`: it stays a separate `snapshot_put` after the winning commit, as `finalize.ts` did.
  A snapshot failure is logged and the game stays finished without a replay, as today.
- `CB_ADMIN_GAMES` ("games") names the same collection on both backends for the console: SQLite's `games` table, DynamoDB's `GAME` family.
- Four import-only operations (`user_import`, `rating_import`, `bots_replace`, `game_import`) write rows as they are; `store_import.c` is the import as a library so a unit test runs it, and `migrate/import.c` is a thin main.
- The bot ladder order is the kernel's `bot_roster` tier, then the nickname, so it holds for the hosted ids too.

The rehearsal against the real export (2026-10-13): 41 users, 49 bots, 35 ratings, 60 games, 47 player seats, 44 bot seats, 144 replays and 42 chat messages, every count equal to the export's; 41 of 41 hashes bcrypt-shaped; 60 of 60 games load through `table_load`; every blob compared equal to the CSV; a rerun duplicated nothing.
The export and the scratch database were deleted afterwards.

Tests: `test/unit_store.c` (SQLite), `test/unit_dynamo.c` (the wire byte for byte, scripted flows, and the template check), `test/unit_import.c`, and the DynamoDB transport on the wire in `test/unit_aws_calls.c`.

Untested: the DynamoDB backend has never talked to DynamoDB; the scripted answers follow the API reference's documented shapes, and the first thing to run once a table exists is a create, join, rated game end and lease cycle against it.
GSI eventual consistency (a dashboard or replay list read right after a write) is not exercised.
The DynamoDB console paths (Scan, PartiQL) are checked only as request shapes and the write guard.

Open defects: DynamoDB `user_delete` and `game_delete` cascade in steps after the main delete, so a crash between them leaves unreachable orphans; `users_list`, `bots_list` and `ratings_list` read a whole index partition and sort in C (fine at 41 users, not at thousands); `game_commit` reads the seats on every commit (two Queries on DynamoDB) where a `membership_changed` flag would skip them; `store_import.c` is linked into `cbase` and `bootstrap` too, unused there.

## Module: auth

Owner: `src/auth/`.
Headers: `auth.h` (users, sessions, verify a bearer), `jwt.h` (HS256 in GoTrue's claim shape), `bcrypt.h` (to embed: a public-domain OpenBSD-derived bcrypt with its test vectors), `auth_http.h` (the six GoTrue routes).

Email and password only; the clients derive the email from the username.
Access tokens are HS256 JWTs signed with `CBASE_JWT_SECRET`, 3600 s; refresh tokens are 32 random bytes, stored hashed, rotated on use.
GoTrue's bcrypt hashes import unchanged, so existing players keep their passwords.
A username with '%' is refused.
Error bodies follow GoTrue's shape only as far as supabase-js and supabase-swift read them.

As built:

- `bcrypt.c` is Solar Designer's public-domain crypt_blowfish (musl's copy), notice kept, with four marked changes: `$2a$` runs the correct algorithm (Go's `x/crypto/bcrypt`, which wrote GoTrue's hashes, has no anti-collision tweak), `$2x$` is refused, costs run to 31, and sign-compare casts for `-Werror`.
  It hashes `$2b$` at cost 10 (59 ms a verify on this Mac) and verifies `$2a$`, `$2b$` and `$2y$` in constant time.
- `jwt.c` mints and verifies HS256 with libcrypto: the signature first, in constant time, then alg exactly HS256, aud and role "authenticated", sub and session_id UUIDs, exp required, 30 s of skew; `iss` is written but not checked, so a change of `CBASE_PUBLIC_URL` does not sign everyone out.
- `auth.c`: refresh tokens are 32 CSPRNG bytes in base64url, stored as their SHA-256; a refresh consumes its token and issues a new pair in the same session.
  A consumed token presented again within `CB_AUTH_REUSE_GRACE_MS` (10 s, GoTrue's reuse interval) of its first use is a retry of a lost response and gets a sibling pair in the same session; the window is anchored at the first use.
  Later it is reuse, and the whole session is revoked; a token a logout revoked never gets the grace.
  An unknown email still spends one bcrypt verify against a dummy hash.
  Passwords are 6 to 72 bytes (longer is refused, not cut); usernames 1 to 64 bytes with no '%' and no control bytes.
- `auth_http.c` serves the six GoTrue routes.
  Error bodies follow the request's API version as GoTrue does: with `X-Supabase-Api-Version: 2024-01-01` (both SDKs send it) `{"code":"<error_code>","msg":..}`, else `{"code":<status>,"error_code":..,"msg":..}`; supabase-swift decodes `code` as a String, so the brief's single integer shape would have broken every auth error on iOS.
  Dates are `YYYY-MM-DDTHH:MM:SS.ffffffZ`, which supabase-swift's `ISO8601FormatStyle` reads.
- Logout: no scope or `global` (supabase-js's default) revokes every session of the user (`refresh_revoke_user`), `local` only the caller's, `others` nothing (no client sends it).
- PUT /user changes only the password; GET and PUT /user answer from the stored row, so a deleted account answers 403 `user_not_found` while its token lives.

Tests: `test/unit_bcrypt.c`, `test/unit_jwt.c`, `test/unit_auth.c` (the accounts over a fake store, the routes through `cb_handle`), and the game smoke, where every request carries a real JWT.

Untested: a real GoTrue hash from the export was checked for shape by the import rehearsal but not verified against its password (no password is known); the import must sign in one migrated account before cutover.
Open: access tokens outlive logout and account deletion until they expire (up to an hour), as GoTrue's did.

## Module: app

Owner: `src/app/`.
Headers: `table_ops.h` (one operation per request: load, kernel section, CAS commit with retries, finalize, views on demand), `bots.h` (drive, scan, the two kick functions), `fanout.h` (after a winning commit, who hears), `functions_http.h` (create, action, meta, delete-account, byte for byte).

The kernel section uses per-thread scratch (a `Game` of about 37 KB and `TableSnaps` of about 46 KB).
`table_commit_products` takes a const table, so when its products ask for ratings the C host reads them, calls `table_rate` and commits again without re-running the operation.
Views (player_views, spectator_views) are computed from the stored row on demand, never stored.

As built:

- `table_ops.c`: `cb_table_run` (load from the row cache or the store, 32 fresh deal-seed bytes, one kernel section on heap scratch, the CAS commit with five attempts), `cb_table_create` (five id draws), and the views computed from the stored row (`cb_table_view`, `cb_table_views_of_user`).
  `commit_table`'s plpgsql arithmetic moved into C, because the store writes whole rows: the log is appended (reset on a deal, emptied in a lobby), `round_epoch` is 0 on a deal or in a lobby and the new version when the operation closed a round, the seed is the fresh one when the operation dealt.
- The per-op `noCommit`/`freshOnly` flags became one rule: MOOT, STALE_ROUND and REJECTED commit nothing; STALE_ROUND, REJECTED and any refusal read from a cached row re-run fresh.
- Ratings: when the products want them, the host reads `ratings_for`, calls `table_rate` and `table_commit_products` again in the same section; the operation is not re-run.
- The end of a game: the replay code and extras go into a separate `snapshot_put` after the winning commit, as `finalize.ts` did.
  finalize.ts then cleared `logs_packed` with a second, unfenced UPDATE; the store has no such write, so the finished game's log stays until the next commit that leaves the finished table (a lobby commit empties it, a deal replaces it).
  This is accepted: the log is read only by the next deal's append and by the replay gate, both of which handle it.
  The snapshot's `player_ids` include the bots, as finalize.ts wrote them.
- `fanout.c`: a pv- INSERT/UPDATE/DELETE per human row on every commit, the gu- casts per human seat and the game- cast only when `n_events > 0 || goods_changed` (table_io.ts's rule).
- `functions_http.c` follows wrap400: auth is checked before the content type; 400 for every refusal with a reason (AUTH, MALFORMED, REFUSED, NOT_FOUND, NO_BOT), 415 for a body that is not octets, 500 INTERNAL for the server's own failure (write contention included), 503 when the store is not up; create 400 AUTH or 500; delete-account 405, 401, 500.
  These match inv_supabase.md section 3 line by line.
  `cb_app_delete_account` is the one implementation behind delete-account and the console's Users panel.
  After the live tables, it anonymizes the user's stored replays: each snapshot whose `player_ids` hold the user gets its extras rewritten by the kernel's `replay_extras_redact` (the seat's name becomes `REDACTED_PLAYER_NAME`, every other byte stays) through the store's `snapshot_set_extras`, which rewrites every DynamoDB copy in one transaction.
  An extras blob the kernel cannot read is cleared rather than kept; the moves code names nobody.
  Production's delete-account (Supabase) never did this, so its replays still name deleted players.
- `bots.c`: `cb_bots_drive` (lease, cycles with the kernel's wait, renew per cycle, release on every exit, re-kick when the budget ends with work left), `cb_bots_scan`, and the two kicks.
  The edge runtime's 1700 ms CPU budget is gone: a Lambda drive stops when the predicted next cycle would pass the invocation deadline minus 5 s; the 120 s wall ceiling stays.
  `cbase serve` runs the scan every 10 s on a loop timer, on the request pool, as the heartbeat; `main_lambda.c` answers `{}` for a drive whose game is gone, so Lambda does not retry it.

Tests: `test/unit_app.c` (16 tests through `cb_handle` over an in-RAM store with a CAS-stealing hook), and the game smoke (`test/smoke.c`), which plays whole games over HTTP and WebSocket, crashes the server mid-game and proves the heartbeat resumes the bots.

Untested: the Lambda deadline budget and the Lambda re-kick (no Lambda yet); create's id redraw on a collision.

Found here:

- Kernel, present in production's TypeScript host too: `bot_drive_pre_action_hook` (bot_drive.c) is a process global, so serve's bot pool runs drives one at a time (see Kernel thread safety).
- c_base's own build, fixed at integration: the kernel was compiled with its native arena's compact capacities (`MAX_LOG_PAIRS=16` and the rest), so a pickup of more than 16 cards was logged short, `table_replay_code`'s gate refused the finished game, and it got no snapshot (about 1 in 15 random three-seat games).
  The app builder reported it as a production kernel defect; it is not, because the shipped wasm and iOS builds use the wide human-game capacities.
  `c_base/Makefile` now reads them from `foolish/c`'s own `WASM_FLAGS` (its `print-human-caps` target, so no second copy exists) for the kernel and for c_base's objects, and `table_ops.c` asserts `MAX_LOG_PAIRS >= MAX_DECK` at compile time.
- Kernel, the same in production: a session over `MAX_LOGS` (1024) records is truncated and its replay refused the same way; it is sized on p99 by design and was not observed.

## Module: realtime and PostgREST

Owner: `src/realtime/`.
Headers: `realtime.h` (the publish calls and the transport interface), `phoenix.h` (the protocol, shared), `rt_local.h` (serve's in-process WebSocket), `rt_apigw.h` (API Gateway's WebSocket API and CONN rows), `postgrest.h` (the reads the clients make, and the chat insert).

Both Phoenix serializers are needed: realtime-js 2.x sends `vsn=2.0.0` arrays, supabase-swift `vsn=1.0.0` objects.
Topic policy in C mirrors the `realtime.messages` policies: `pv-<uid>` only that user, `gu-<game>-<uid>` that user if a member, `game-<game>` any authenticated user, `chat:<game>` members.
There is no catch-up, as today: a client that missed a push refetches its view row when it rejoins.
The web client reaches API Gateway through CloudFront at the same domain, `/realtime/v1/websocket` rewritten to the stage by a CloudFront Function; if that proves fragile, the fallback is a client-side realtime URL override, documented, not built.

As built:

- `phoenix.c`: both serializers (vsn 2.0.0 arrays, 1.0.0 objects; no vsn means 1.0.0), heartbeat, `phx_join`, `phx_leave` (ok, then `phx_close` with ref = join_ref, as Phoenix sends it), `access_token`, and `cb_phx_may_join`.
  A gu- topic is rebuilt from the caller's uid and compared whole before membership is checked.
  `access_token` is handled per channel with no reply, as Realtime does; a token that does not verify closes that channel.
  A join token equal to `CBASE_ANON_KEY` is "nobody" and is refused by policy, since supabase-js hands Realtime the anon key when signed out.
  Topics must carry "realtime:"; unknown families (`user-`, `room:`) are refused.
- Binding ids are a hash (`cb_phx_binding_id`, FNV-1a over event, schema, table and filter), so neither transport stores anything per binding; a pv- change is tagged with the ids of its event and "*" crossed with its filter and none.
- `realtime.c` builds each frame once per serializer in Supabase Realtime's own shapes (keys sorted as Jason writes small maps; `commit_timestamp` with milliseconds and "Z"); `rt_rows.c` prints every timestamp as Postgres prints a TIMESTAMPTZ in UTC, with "+00:00", including the columns production keeps as plain TIMESTAMP (chat, snapshots, ratings): a bare timestamp is read as local time by the browser, which put the chat clock off by the viewer's UTC offset.
- `rt_local.c` (serve): `GET /realtime/v1/websocket` on the loop thread, refusing an unknown vsn with 400 and echoing `Sec-WebSocket-Protocol: phoenix` (Chrome fails the handshake otherwise); each client frame runs on the request pool, one at a time per connection, so frames are answered in order.
  The server's idle sweep closes a WebSocket with no client traffic for 120 s, which a client's 25 s heartbeat prevents.
- `rt_apigw.c` (Lambda): `$connect` stores a CONN row with a 2 h TTL, `$disconnect` deletes it, `$default` runs the frame and writes the row back only when it changed or a third of the TTL has gone (heartbeats cost no write); replies and pushes go through the management API, and 410 deletes the row.
- `postgrest.c`: GET for player_views, spectator_views, chat_messages, bots, user_elo_ratings and game_snapshots, and POST for chat_messages, through one filter, order, page and select pipeline after a per-table fetch that applies the RLS.
  `gt` is supported (the leaderboard sends `games_played=gt.0`); spectator_views and chat_messages require `game_id=eq.`; a bearer equal to the anon key, or to the request's own apikey, is anon.
- `rt_deps.h` is a table of the four calls into auth, table_ops and API Gateway that the unit tests swap for fakes.
  Integration kept it: the API Gateway entries cannot be real without AWS, and the fakes let the unit tests pin edge cases (410, expired rows, a lost CONN row) deterministically.
  The production table is exercised end to end by the game smoke, where real JWTs, the real table_ops and `rt_local` carry every push.

Tests: `test/unit_phoenix.c`, `test/unit_postgrest.c`, `test/unit_rt_apigw.c`, `test/unit_rt_local.c` (a real loopback server), and the game smoke, where A listens as realtime-js (vsn 2.0.0, gu-) and B as supabase-swift (vsn 1.0.0, pv- and game-).
The browser acceptance ran realtime-js against it (see Acceptance).

Untested: supabase-swift against a running cbase; `rt_apigw` against real API Gateway.
Carried over on purpose: no catch-up after a missed push; chat `is_system` settable by a client; chat length bounded only by the 1 MB body limit.

## Module: aws

Owner: `src/aws/`.
Headers: `runtime_api.h` (complete: the Lambda Runtime API client), `event.h` (the event envelope in, the response out), `sigv4.h` (signing and credentials), `https.h` (a TLS client on OpenSSL), `apigw_mgmt.h` (post to a connection), `lambda_invoke.h` (the async self-invoke).
The stubs live together in `aws_stubs.c`; each builder file replaces its part and deletes it there.

As built:

- `runtime_api.c`: the Runtime API client, exercised by `test/smoke_lambda.c` against the real `bootstrap` through a fake Runtime API.
- `sigv4.c`: Signature Version 4 with libcrypto, credentials from the Lambda environment (`AWS_REGION`, falling back to `AWS_DEFAULT_REGION`); the path is signed double-encoded (every service but S3); `X-Amz-Content-Sha256` is not sent (only S3 needs it).
  Checked against seven cases of AWS's signing test suite.
- `https.c`: a blocking HTTPS/1.1 client on OpenSSL, TLS 1.2+, peer and hostname verified, one deadline per exchange, Content-Length, chunked and read-to-close bodies, a small idle pool per host with one retry for a pooled connection that died before any byte.
  The CA bundle is found on provided.al2023 without configuration (`CBASE_CA_FILE` overrides).
- `aws_call.c`: the one signed-request step (credentials, SigV4, HTTPS) under `apigw_mgmt.c`, `lambda_invoke.c` and the DynamoDB store.
- `event.c`: Lambda events in, responses out; binary bodies are base64 both ways.
  The self-invoke and schedule payloads are the JSON string literals `"bot_drive:<id>"` and `"scan"` (the decision record), read by stripping the quotes; EventBridge's `"source":"aws.events"` is also a scan.
  `cb_event_bot_drive_payload` is the one writer of the literal.
- `lambda_invoke.c` (`AWS_ENDPOINT_URL_LAMBDA` overrides the endpoint) and `apigw_mgmt.c` (410 reaches `rt_apigw` as `CB_APIGW_GONE`).
- `main_lambda.c` clears the deadline after each invocation and answers `{}` for a drive whose game is gone.

Tests: `test/unit_sigv4.c`, `test/unit_https.c` and `test/unit_aws_calls.c` against a local TLS server, where each request's signature is recomputed from the bytes that arrived; `test/unit_event.c`; `test/smoke_lambda.c` (health, scan, a garbage event, a drive for a missing game).
`make bootstrap` links a static aarch64 ELF; the unit tests pass in an Ubuntu 24.04 arm64 container.

Untested: nothing here has run against real AWS; the double-encoded `@connections/<id>` path follows botocore's rule and the first real push confirms it.
Open: `getaddrinfo` has no timeout, so a hung resolver can outlive `timeout_ms`.

## Module: admin

Owner: `src/admin/` and `admin/console.html`.
Header: `src/admin/admin.h`.
One HTML file, vanilla JS, light and dark, under about 1500 lines, embedded at build time like the schema.
Panels: Overview, Tables (DynamoDB by key prefix, SQLite by table), Query (PartiQL or SQL, SELECT only unless confirmed), Users, Games, Bots, Logs (this process's ring, plus a CloudWatch Logs Insights link under Lambda), Config (secrets redacted), Backups (point-in-time recovery explained; SQLite `VACUUM INTO`).

As built:

- `src/admin/admin.c` serves the page (embedded by `tools/embed.c`) and one `/admin/api/*` route dispatching 13 calls: overview, collections, browse, query, users, users/delete, games, game, bots, logs, config, backups (GET and POST).
  With `CBASE_ADMIN_TOKEN` unset every admin route answers 404; an API call without the exact Bearer answers 401 (constant-time compare).
- Only three paths write, each a deliberate act: a query confirmed by typing `WRITE` (the store refuses a write otherwise, with `CB_E_FORBIDDEN`), users/delete (`cb_app_delete_account`, delete-account's own function), and the SQLite backup (`VACUUM INTO` a timestamped file through the same guarded query).
- The Games panel browses `CB_ADMIN_GAMES` and reads each game; seats come from the kernel's `roster_decode`, and the envelope is the spectator's `cb_table_view`.
- `admin/console.html`: about 740 lines, vanilla JS, no CDN, a strict CSP, light and dark, usable at phone width; the token lives in `sessionStorage` only.
- Under DynamoDB the Backups panel explains point-in-time recovery (on in the template) rather than offering an export.

Tests: `test/unit_admin.c` drives every route through `cb_handle`; the user delete runs on the real SQLite store with real auth.
The page was checked by eye in headless Chrome, light and dark, at 1280 and 390 px, against a mock API, by its builder.

Untested: the console against a live server with data was not looked at by eye after integration.
Open: under Lambda the Logs panel and the Overview counters see one warm container, not the function (the panel says so and links to CloudWatch Logs Insights); the admin API answers with the router's CORS header like every other route (harmless: it needs a Bearer, not a cookie).

## Module: ops and migrate

Owner: `ops/` (`cbase.yaml`, `package.sh`, the GitHub workflow) and `migrate/` (`export.sh`, `import.c`, `out/` gitignored).
`make bootstrap` already produces `dist/bootstrap` and `dist/bootstrap.zip`: a stripped static Linux arm64 binary built in an arm64 Alpine container pinned by digest (`tools/build_bootstrap.sh`), byte-for-byte reproducible across runs on this Mac.
Alpine (musl) rather than Ubuntu because glibc's static `getaddrinfo` still loads NSS modules at run time.
`ops/package.sh` should call it rather than repeat it.
The export holds password hashes: it lives under `migrate/out/`, is gitignored, and is deleted after the rehearsal.

As built:

- `ops/cbase.yaml`: the table, the function (arm64, provided.al2023, 256 MB, 60 s), its role, the Function URL, the WebSocket API with its three routes and stage, the one-minute scan schedule (input `"scan"`), CloudFront with a CloudFront Function that rewrites `/realtime/v1/websocket` onto the stage, and an optional GitHub Actions deploy role.
  It passes `cfn-lint`; its index names, key types and TTL are held to the store's by `test/unit_dynamo.c`.
  The zip comes from an S3 bucket parameter, because CloudFormation's inline code works only for Node.js and Python.
- `ops/package.sh` calls `make bootstrap` and checks the zip holds exactly one executable `bootstrap`.
- `ops/deploy.sh` is the whole deploy from this Mac once `aws configure` has credentials: secrets generated once into a gitignored `ops/.env`, the code bucket, the zip under a sha-named key, `cloudformation deploy`, the outputs, and the import with `--import`.
  Checked with `bash -n` and shellcheck only.
- `.github/workflows/c_base.yml` builds and tests on Ubuntu and macOS, under ASan, lints the template, builds the zip, and deploys on `main` once the deploy secrets exist; it has not run yet.
- `migrate/export.sh` (read-only, through the Management API with the Supabase CLI's keychain token) and `cbase-import` (see Store).

Unproven until the first deploy: the CloudFront WebSocket path, the Function URL permission, the CI lanes and the deploy job.

## Tests

`make test` builds and runs every `test/unit_*.c` as its own program; `make smoke` runs both smoke lanes; `make check` is both; `make check-asan` rebuilds everything, the kernel included, under ASan and UBSan.
The game scenario in `test/smoke.c` runs by default (`CBASE_SMOKE_GAME=0` turns it off): two humans and a bot play whole games against `cbase serve` over real HTTP and WebSocket, the server is SIGKILLed mid-game when only the bots owe a move and restarted with nobody bumping (the heartbeat must resume them once the dead process's lease expires, about 30 s), every push stream is checked strictly increasing, ratings change, one snapshot is stored, and a clean restart reloads the finished game unchanged.
Each module's mutation checks are listed in `docs/mutation_checks.md`.
Test files are found by wildcard, and every `test/lib_*.c` is linked into every test program (`lib_client.c` is an HTTP and WebSocket client, `lib_proc.c` starts a binary as a child).
`test/smoke.c` starts `cbase serve`; `test/smoke_lambda.c` is a fake Runtime API that drives the real `bootstrap`.
The smoke builders extend `smoke.c` to the brief's full scenario: two humans, a bot, a whole game over HTTP and WebSocket, ratings changed, a snapshot stored, pushes with monotone versions, a restart, the game reloaded.
Every test is mutation-checked: break the code, see the right assertion fail, restore by re-editing (never `git checkout`), and remember that make compares whole seconds, so delete the objects after restoring.

## Kernel capacities

The kernel's array capacities are build parameters, and its compact defaults belong to the native bot arena.
c_base builds it with the shipped human-game capacities (`KERNEL_CAPS` in the Makefile, read from `foolish/c`'s `WASM_FLAGS` through `print-human-caps`), the same as the wasm module the hosted backend runs and the iOS build, and passes them to its own objects too, because they change the sizes of structs both halves use.

## Kernel thread safety

`cbase serve` runs kernel calls on several threads, so the kernel is compiled with its `_Thread_local` qualifiers intact (foolish/c's own Makefile strips them for its single-threaded programs).
One kernel global is not per-thread: `bot_drive_pre_action_hook` (bot_drive.c), which `table_bot_drive` sets and restores around each drive.
Two concurrent drives race on the restore, and one of them can lose its per-decision seeding.
So serve's bot pool defaults to one thread (`CBASE_BOT_WORKERS=1`) until the kernel makes that hook `_Thread_local`, a one-word kernel change outside c_base.
Lambda runs one invocation per container at a time and is not affected.
Human moves and lobby edits do not touch the hook.

## Binary alternatives and the latency bench

Alex chose DynamoDB for the exact $0 after being shown three fully binary alternatives (2026-10-13), and asked for them to be written down and measured later, so the choice can be revisited from numbers rather than a hunch:

1. **Front door: AWS Lambda Web Adapter** (the official extension) instead of our Runtime API loop.
   Our process would be only `cbase serve`, and the JSON envelope would be consumed outside our code.
   Expected: +10 to 20 ms of cold start and about +1 ms warm per request for the extra localhost hop; removes `event.c` and `main_lambda.c`.
2. **Store: S3 with conditional writes** (`PutObject If-Match` on the ETag as the CAS, `If-None-Match` to create; strongly consistent; raw bodies) instead of DynamoDB.
   Expected: slower per call (S3 GET and PUT p50 roughly 15 to 40 ms against DynamoDB's 3 to 10 ms), about $0.10 to $0.20 a month, no JSON in the data path, XML only for admin listing; the indexes (a user's games, a topic's connections, the scan) would need a second mechanism.
3. **Push: client polling of a binary endpoint** instead of API Gateway WebSocket and Phoenix.
   Expected: removes the CONN rows, the management API and the CloudFront path rewrite; adds up to one poll interval to a push; needs web and iOS client changes.

The honest expectation, stated before any measurement: the binary options are not expected to be faster end to end, because the dominant cost is the network hop to the store, and S3 is slower than DynamoDB; what they buy is the removal of JSON from our code.

The bench is owed and NOT built: `c_base/bench/` does not exist yet.
It is to be a C program driven from this Mac against the deployed stack and against `cbase serve`, measuring per variant the cold start (the first request after a forced new container), warm p50, p95 and p99 of `POST /functions/v1/action` end to end, store GET and conditional PUT latency, bytes on the wire per request, and the CPU time in JSON encode and decode (measurable locally today with a microbenchmark over recorded envelopes).
`store.h` and the handler split already let an S3 backend and the adapter mode be added as variants without touching the app layer.

## What stays TypeScript, and why

The migration deletes the server TypeScript once Supabase is switched off (measured in REPORT.md, section 5): the edge functions and their adapter under `foolish/server/impls/supabase`, `foolish/server/api`, and `foolish/sdk/ts/table/server_table.ts`.
It does not delete, and does not try to:

- the web client (`foolish/src`): it is the product, and it reaches the kernel through generated bindings already;
- the tests and harnesses (`foolish/e2e`), which drive the web client and today's server; once the server TypeScript is gone, the harnesses that exist only to run it (`fake_supabase.mts`, the edge helpers) go with it, and the rest are the next target;
- the tools, visualisers and scripts, each its own later decision;
- generated code, which is a build artifact.
Phase 2 (above) is what removes the client-side Supabase SDKs.

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

## Acceptance in a real browser (2026-10-13)

The web app from this branch (`npx next dev`, `.env.local` pointing `NEXT_PUBLIC_SUPABASE_URL` at `http://127.0.0.1:8080`) was driven in a headless Chrome over the DevTools protocol, two isolated browser contexts as two players, against `cbase serve` on a fresh SQLite file.
Verified: sign-up of both players through supabase-js (GoTrue facade); the dashboard (PostgREST `player_views`); create; the second player joining by URL; adding a bot; both players marking ready and the game starting; moves dragged from both tabs, each tab's table updated by the realtime push without a reload, the two tabs agreeing on the table after every round; a pickup; and the leaderboard and match history pages loading.
The detailed record and screenshots are in REPORT.md.

Found on the way, NOT a c_base defect: when the first player joins a one-player lobby, the host's page fails to plan the join's animation (`anim_build_plan error -2`, `n_players < 2` in anim_plan.c, because the page plans against its held one-player board) and its animation queue then stalls until a reload.
The same failure reproduces against `e2e/fake_supabase.mts`, which mirrors the hosted adapter, so production has it today; it belongs to the web client and kernel (`src/state/animPlan.ts`, `useAnimationRun.ts`, `c/src/anim_plan.c`).

## Open defects carried over from Supabase

Kept on purpose, so behaviour does not change under the clients; each is a later decision, not an accident: no rate limiting (create is unlimited, BUMP is open to non-members), ratings are world-readable, `game_snapshots.player_ids` survives account deletion, nothing cleans up stale lobbies or old snapshots, the game id space is 24 bits with five redraws, a snapshot failure is logged and swallowed, and there is no realtime catch-up.

## How it was built, and what integration decided

Six builders (store, auth, app, realtime, aws, admin and ops) worked in parallel on disjoint directories against the skeleton's headers, then one integrator merged the six branches (no textual conflicts) and aligned them.
The integration decisions and fixes:

1. Refresh tokens: `refresh_revoke_user` and `revoked_at` added to `store.h` and every backend; GoTrue's 10 s reuse grace in `auth.c`; logout `global` revokes every session.
2. The snapshot stays a separate write after the commit, and a finished game's log is cleared by the next lobby commit, not at once (see App).
3. Member sync skips a deleted account's seat, in both backends (see Store).
4. The payloads are the literals `"scan"` and `"bot_drive:<id>"` everywhere: `event.c`, `lambda_invoke.c`, the template's schedule input, and the comments in `ctx.h` and `app/bots.h`.
5. The template's indexes were wrong (`GSI1`/`GSI2`, `gsi1sk` a Number); they are now the store's `gsi1`/`gsi2` with String keys, and a test holds the two together.
   The TTL attribute `expires_at` is in seconds on DynamoDB and ms in C.
6. The console: the user delete calls `cb_app_delete_account` directly (it used to mint a token and replay the HTTP route), and `CB_ADMIN_GAMES` gives the Games panel one collection name on both stores (DynamoDB has no "games" collection, so the panel would have failed in production).
7. `store_dynamo.c` signs and sends through `aws_call.c`, removing a second copy of that step.
8. `cbase serve` runs the bot scan every 10 s; `smoke_lambda` lost its stub branch and gained a drive for a missing game.
9. `rt_deps.h` stays (see Realtime).
10. The kernel is built with the shipped human-game capacities, not its arena's compact defaults (see Kernel capacities); this, not a kernel defect, was why one game in about fifteen had no replay.
11. Every status and body of `functions_http.c` was compared with inv_supabase.md section 3 (wrap400) and agrees; auth follows GoTrue as above.
