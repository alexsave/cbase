# c_base design

c_base is the foolish backend written in C, replacing Supabase (Postgres, PostgREST, GoTrue, Realtime, the Deno edge functions, pg_cron, pg_net and Vault).
It runs on AWS Lambda as a custom runtime in production, and as a local server for development and tests.
The web and iOS clients speak its binary surface ("Binary surface", below), the kernel's bytes end to end.
It first shipped a Supabase-compatible facade, so the clients of the day could switch by changing a URL and an anon key; that facade and the Supabase code it stood in for were deleted once every client had moved (stage 4 of "Binary surface").

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
6. **Client surface: a Supabase-compatible facade, then the binary surface.**
   The four edge functions byte for byte, the GoTrue auth routes, the PostgREST reads the clients make, and Realtime's Phoenix protocol.
   iOS installs cannot be updated in place, so old installs keep calling Supabase until they update; the runbook says so.
   Superseded (2026-10-14) and done: every client moved to the binary surface and the facade was deleted ("Binary surface", below).
7. **Admin console:** one static HTML file served at /admin, its JSON API at /admin/api/*, behind a Bearer token.
8. **Ops:** one CloudFormation template (`ops/cbase.yaml`) Alex uploads in the console; no AWS CLI needed locally.
9. **Migration:** an export through the Supabase Management API, and `cbase-import` writing through the store interface.
10. **Tests in C**, every one mutation-checked.
11. **Conventions:** no em dashes; no JSON outside the list below, and all of it in `src/json_surface.c`; no hand-rolled kernel byte layouts (the kernel's own encoders and decoders only); every kernel call takes now_ms from the host; the kernel never sees a socket, a JWT, SQLite or DynamoDB.

## Why Lambda, and what it costs us

Alex chose Lambda because it is free at this scale: 1M requests and 400k GB-seconds a month, DynamoDB's always-free 25 GB with 25 provisioned write and 25 read units (see "The free tier, provisioned" under Store), a free Function URL, CloudFront's free plan, free ACM certificates and a free EventBridge Scheduler.
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

The semantics preserved lived in the Supabase adapter's `table_io.ts`, `bot_actions.ts`, `game_cache.ts`, `finalize.ts` and `bot-heartbeat/index.ts` (deleted in stage 4; `git log --diff-filter=D -- foolish/server/impls` finds them).
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
                                                  (handler.c: CORS, 404/405, /health)
                                                      | routes
             v1_http  functions_http  admin  unfurl  (rt_local's upgrade, serve only)
                                                      |
              app/table_ops (load, kernel, CAS)  app/bots  app/fanout -> realtime (stream)
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

DynamoDB: one table `cbase`, PROVISIONED at 15 RCU and 15 WCU with 5 and 5 on each index (the always-free 25 a side; see "The free tier, provisioned" below), point-in-time recovery on, TTL on `expires_at` (epoch seconds).
Two indexes, `gsi1` (`gsi1pk`, `gsi1sk`) and `gsi2` (`gsi2pk`, `gsi2sk`), all four keys String, both projecting ALL.
`src/json_surface.h` owns these names (`DW_GSI1`, `DW_GSI2`, `DW_TTL_ATTR`), and `test/unit_dynamo.c` reads `ops/cbase.yaml` and fails when the template disagrees.
Items, by partition key and sort key:

| pk | sk | what |
|---|---|---|
| `USER#<id>` | `PROFILE` | email, bcrypt hash, username, created, last sign-in |
| `USER#<id>` | `RATING` | elo_rating, previous_elo, games_played, username (the leaderboard row) |
| `EMAIL#<lowercase email>` | `USER` | the user id (signup's uniqueness is a conditional put on this) |
| `REFRESH#<sha256 hex>` | `TOKEN` | user, session, created, revoked, revoked_at; gsi1 `USERREFRESH#<user>`, gsi2 `SESSION#<session>` |
| `GAME#<id>` | `ROW` | state, roster, logs_packed, seed (Binary); version, round_epoch, status, needs_bots, created/updated/last_commit |
| `GAME#<id>` | `MEMBER#<user>` | a seated human; gsi1 `USERGAMES#<user>` / `<updated_at, 15 digits>#<game>` gives the dashboard |
| `GAME#<id>` | `BOT#<bot id>` | a seated bot |
| `BOT#<id>` | `BOT` | the 49 bots, with their ratings |
| `SNAP#<snapshot id>` | `SNAP` | a replay (gsi1 `GAMESNAPS#<game>`); it outlives its game, as `ON DELETE SET NULL` did |
| `SNAP#<snapshot id>` | `PLAYER#<user>` | one copy per player, gsi2 `PLAYER#<user>`: "my replays" is one query |
| `CHAT#<game id>` | `<created>#<id>` | a chat message |
| `CONN#<connection id>` | `CONN` | a realtime connection: user, vsn, topics, expires_at (TTL, seconds) and expires_ms |
| `TOPIC#<topic>` | `CONN#<connection id>` | the reverse index fan-out queries: a whole copy of the connection row with the same TTL, in the base table so the read is strongly consistent and a push right after a join is not missed |
| `SCAN` | `GAME#<id>` | present while a game needs bots (sparse; the heartbeat's scan reads one partition); carries the bot lease (lease_token, lease_until) |

`game_commit` is one `TransactWriteItems`: the game update conditioned on the version, the membership puts and deletes, the rating updates, the SCAN marker.

The 49 bots: `sql/schema.sql` seeds the nicknames and strategy keys of `seed.sql` with FIXED ids (UUIDv5 of the nickname), because `seed.sql` leaves bot ids to `uuid_generate_v4()` and so they differ per database.
A migrated store replaces them with the hosted rows, since the hosted rosters name the hosted ids.

As built (store builder, then integration):

- `store_sqlite.c` implements every operation over one connection with cached prepared statements; one process mutex spans each operation, and list callbacks run after the lock is released so a callback may call the store again.
- `game_commit` mirrors `commit_table`: the version-fenced `UPDATE`, membership and bot seats rewritten, and ratings added to the rows as they stand (clamped at 0, `games_played + 1`, `previous_elo` the old value, a missing human rating row created when the account exists).
- Membership follows every commit (the header's contract), where `commit_table` touched it only when the roster changed; a seat whose account is gone (delete-account keeps the id in the roster) is skipped in both backends instead of failing every later commit on the foreign key.
  DynamoDB writes only the seats that change (a human's MEMBER item when the seat is taken, never again on later commits), so its dashboard index orders by when the seat was taken; PostgREST sorts by the rows it loads, so nothing reads that order.
  The app relies on exactly this, since a redacted roster still names the deleted id; the store tests hold both backends to it (`cas_second_writer_loses` commits with a deleted seat on SQLite, `flow_commit_drops_missing_account` on DynamoDB).
- `last_commit_at` is stamped by the store from `row->updated_at` on every commit; `row->last_commit_at` is read only by the import.
- The lease: acquire and renew move the scan's staleness clock (never `last_commit_at`); release clears `lease_until` and does not move it.
  SQLite keeps lease and clock on the game row; DynamoDB keeps both on the game's SCAN marker (see "The free tier, provisioned"), so there the row's `updated_at` is its last commit's.
  A game with no bot work has no lease to take: acquire answers `CB_E_NOT_FOUND`, on both backends.
- `game_commit`, `game_delete` and `lease_acquire` on a game that is gone return `CB_E_NOT_FOUND` (DynamoDB tells it apart from a lost CAS with `ReturnValuesOnConditionCheckFailure: ALL_OLD`).
- The DynamoDB section of `json_surface.c` (formerly `dynamo_wire.c`) spells every DynamoDB string and builds every request as a pure function over buffers; `store_dynamo.c` decides only which requests to send in which order.
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

Tests: `test/unit_store.c` (SQLite, and DynamoDB Local under `make check-dynamo`), `test/unit_dynamo.c` (the wire byte for byte, scripted flows, and the template check), `test/unit_import.c`, and the DynamoDB transport on the wire in `test/unit_aws_calls.c`.

### Against DynamoDB Local (2026-10-13)

`make check-dynamo` runs `unit_store`, `unit_admin`, `unit_import` and the whole game smoke (two humans, a bot, a SIGKILL mid-game, ratings, a replay, a restart) with `CBASE_STORE=dynamo` against Amazon's DynamoDB Local, and CI runs it on every pull request.
`ops/dynamo_local.sh` starts the container (`amazon/dynamodb-local:3.3.1`, pinned by digest, in memory, `-sharedDb` so a dummy key sees the CLI's table) and creates the table from `ops/cbase.yaml` itself: `ops/dynamo_table.py` reads the template's table resource and refuses any property it does not carry over, so the two cannot drift.
Each unit test gets its own fresh table (`test/lib_store_backend.c`), which is only ever made at an `http://` endpoint, so a test cannot create a table in AWS.
The AWS client speaks plain HTTP for exactly that case: an endpoint URL that says `http://`, refused for any `amazonaws.com` host.

What the first contact found, all fixed and each held by a test:

- A fresh DynamoDB table had no bots: SQLite seeds them at open, nothing seeded DynamoDB, so a deploy without an import could not add a bot.
  `cb_import_seed_bots` writes the schema's 49 into a store that has none (read through an in-memory SQLite store, so the list keeps one owner), `cbase-import --seed-bots` runs it, and `ops/deploy.sh` runs that after every deploy.
- The import never checked what DynamoDB stored (only SQLite's counts were read), so a lost seat passed. A keys-only Scan of the table (`dw_scan_keys`, each item classed by `dw_export_kind`) now holds DynamoDB to the same check, counted against the export's own ids (see "The import against the provisioned table").
- Serve's bot pool queued one drive per kick, and a long drive let thousands pile up (every bump kicks); each then took the lease, read the row and let go, and the SIGTERM drain outlived its 15 s. At most one drive per game now waits in the queue.
- The smoke read its server's stderr only while waiting for a line, so a debug-level server filled the pipe and stopped answering; `test/lib_proc.c` now reads it on a thread all the time.

What held without a change: every attribute shape, condition and update expression, the CAS and its cancellation reasons, the lease, `TransactWriteItems` at 98 items (the import's `bots_replace` of 49 seeded and 49 hosted bots), Query paging at 1 MB (1.5 MB of replays through GSI2, 1.2 MB of chat), a 250 KB item with Binary attributes byte for byte, numbers (negative, 13-digit ms), TTL in seconds, both GSIs, PartiQL in the console, and `BatchWriteItem` cascades.

The import rehearsal into DynamoDB Local (the real export, 2026-10-13, seeded first exactly as a deploy is): 41 users, 49 bots, 35 ratings, 67 games, 47 player seats, 47 bot seats, 144 replays (685 per-player copies) and 42 chat messages, every count equal to the CSV's both in the import's own check and in an independent `aws dynamodb scan --select COUNT` per item kind; 67 of 67 games load through the kernel; all 41 hashes parse as `$2a$` bcrypt (40 at cost 10, one at another cost) and `cb_bcrypt_verify` answers 0, not an error, for a wrong password on each; a rerun counts the replays and messages as present.
The export and the rehearsal table were then deleted.

Latency against DynamoDB Local on this Mac (`make bench-dynamo`, median of 200 after 10 warm-ups, one kept-alive connection, a 4 KB state and a 1 KB log, 2 humans and a bot seated), the baseline the AWS numbers are compared with:

| operation | median | p90 | requests | capacity |
|---|---|---|---|---|
| `game_get` | 0.64 ms | 0.84 ms | 1 GetItem | 2 RCU |
| `game_commit` | 4.5 ms | 5.4 ms | 2 Query + 1 TransactWriteItems | 2 RCU, 20 WCU |
| `games_of_user` (20 rows) | 0.79 ms | 0.99 ms | 1 Query on GSI1 | 1 RCU |

What an action costs, measured over a whole smoke game (every request asks for `ReturnConsumedCapacity`, and the handler and the bot drive log the per-thread tally at debug level, `cb_store_tally_take`):

| what | requests | RCU | WCU (min to max) |
|---|---|---|---|
| a human move (`/functions/v1/action`) | 2 Query + 1 TransactWriteItems | 2 | 12.7 (8 to 32) |
| a bot cycle that moves | 1 GetItem + 2 Query + 2 UpdateItem + 1 TransactWriteItems | 3 | 17.9 (12 to 24) |
| a bot cycle that waits out the bot's think time | 1 GetItem + 2 UpdateItem (lease renew, SCAN touch) | 1.2 | 4.6 (3 to 6) |
| a view read (`/rest/v1/player_views`) | 1 GetItem | 1 | 0 |
| sign-up | 1 PutItem + 1 TransactWriteItems | 0 | 9 |

One 3-seat game of 171 commits cost 922 requests, 552 RCU and 2,756 WCU.
Under Lambda add the fan-out: `rt_apigw` reads each pushed topic's connections, one Query per topic, 2 per seated human plus 1 per commit.
The write units go mostly to the game row: a transaction bills 2 WCU per KB of every item it writes, and the row (state, roster and the growing log, 4 to 8 KB) is rewritten on every commit along with every seat; the lease renew and the SCAN touch are an `UpdateItem` on that same row, billed by its whole size, every bot think cycle.
Cheaper, if it matters: the lease on its own small item, one partition Query for the seats instead of two, and seats written only when they change.
All three are built; the numbers after them are in "The free tier, provisioned" below.

Unverified until AWS: real IAM (the role's policy against each call), throttling (the retry path is unit-tested against the documented bodies, see below), `TransactWriteItems` conflict behaviour under real concurrency (DynamoDB Local serializes, so `TransactionConflict` never occurs and `cas_under_threads` only proves the fence), GSI eventual consistency (Local's indexes are effectively synchronous: on AWS a dashboard, replay list or session revoke read through a GSI right after a write may miss it), TTL deletion (Local never deletes), point-in-time recovery (Local has none), and latency over the network.

Open defects: DynamoDB `user_delete` and `game_delete` cascade in steps after the main delete, so a crash between them leaves unreachable orphans; `users_list`, `bots_list` and `ratings_list` read a whole index partition and sort in C (fine at 41 users, not at thousands); `game_commit` reads the seats on every commit (one keys-only Query on DynamoDB) where a `membership_changed` flag would skip it; `store_import.c` is linked into `cbase` and `bootstrap` too, unused there.

### The free tier, provisioned (2026-10-13)

The table was `PAY_PER_REQUEST`, and on-demand capacity has no free tier: every request unit is billed from the first, about $0.17 a month for 100 games at the measured cost.
DynamoDB's always-free allowance is 25 GB of storage and 25 write and 25 read capacity units of PROVISIONED capacity, and the template now provisions exactly that.

What AWS says, read on 2026-10-13:

- The free tier is "25 WCUs, 25 RCUs" a month "on a per Region, per-payer account basis", and "uses provisioned capacity and the DynamoDB Standard table class" ([DynamoDB pricing, provisioned capacity](https://aws.amazon.com/dynamodb/pricing/provisioned/)).
- A global secondary index on a provisioned table has its own read and write capacity, "separate from those of its base table"; index updates "consume write capacity units from the index, not from the base table"; and when an index lacks write capacity, "the write activity on the table will be throttled" ([Developer Guide, Global Secondary Indexes, "Provisioned throughput considerations"](https://docs.aws.amazon.com/amazondynamodb/latest/developerguide/GSI.html)).
- Every table and every index needs at least 1 read and 1 write unit ([API reference, ProvisionedThroughput](https://docs.aws.amazon.com/amazondynamodb/latest/APIReference/API_ProvisionedThroughput.html): "Valid Range: Minimum value of 1").
- The account's throughput quotas "apply to the sum of the provisioned throughput capacity for all your account's tables and global secondary indexes in a given Region"; a table can lower its capacity at most 27 times a day ([Developer Guide, quotas](https://docs.aws.amazon.com/amazondynamodb/latest/developerguide/ServiceQuotas.html)).
- Unused capacity banks for up to 300 seconds as burst capacity, which "might change in the future" ([Developer Guide, burst and adaptive capacity](https://docs.aws.amazon.com/amazondynamodb/latest/developerguide/burst-adaptive-capacity.html)).
- A transaction writes every item twice, prepare and commit: 2 WCU per KB, consumed even when the transaction is cancelled ([Developer Guide, transactions, "Capacity management"](https://docs.aws.amazon.com/amazondynamodb/latest/developerguide/transaction-apis.html)).
- The 2025-07-15 free tier change (credits and a six-month Free plan for new accounts) did not touch the always-free offers: "30+ AWS services are always free within monthly usage limits on both the Free and Paid plans" ([AWS Free Tier](https://aws.amazon.com/free/)), and "All customers, new and existing, can access over 30 services with always free offers" ([Free Tier FAQs](https://aws.amazon.com/free/free-tier-faqs/)). An account opened before that date keeps them too.

What AWS does not say in so many words is whether an index's provisioned units draw on the same 25.
The design assumes they do: the allowance is "provisioned capacity" for the account and Region, an index's capacity is provisioned capacity billed in the same unit hours, and the quotas already add tables and indexes together.
It is the reading under which "$0" cannot be wrong, and the Billing console's Free Tier page settles it after the first deploy (RUNBOOK step 1).

**The split: table 15 RCU / 15 WCU, `gsi1` 5 / 5, `gsi2` 5 / 5, 25 a side.**
No auto scaling (it would scale past 25 and bill); `test/unit_dynamo.c` (`the_template_stays_free`) fails when the template's six numbers add up to more than 25 a side, when the billing mode is not PROVISIONED, or when an auto scaling resource appears.
The table gets most because the table takes almost every write: the CAS on the game row is the bulk of every action.
The indexes take only seat changes, ratings, replays, chat and refresh tokens, and their reads are the dashboard, replay lists and the leaderboard; 5 each is their floor with room.
An index throttled for writes throttles the table write behind it, which is why neither index is at the minimum of 1.

**What the cuts bought.**
Measured with `make check-dynamo` and `CBASE_LOG_LEVEL=DEBUG` (the handler's and the drive's per-request tally of `ReturnConsumedCapacity`), each run summed per kind of action, on the same smoke game mix (115 human moves, 26 bot moves, 45 bot cycles that wait out think time, sign-ups, a create, 62 view reads).
DynamoDB Local reports the table's units and not the indexes', so these are table units; the index units are worked out below.

| step | human move | bot move | bot wait | one smoke game |
|---|---|---|---|---|
| before | 3.0 req, 2.1 RCU, 11.6 WCU | 6 req, 3 RCU, 15.0 WCU | 3 req, 1 RCU, 4.0 WCU | 729 req, 439 RCU, 1,984 WCU |
| (a) the lease on the SCAN marker | 3.0, 2.1, 10.8 | 5, 3, 12.5 | 2, 1, 1.0 | 659, 441, 1,683 |
| (b) seats written when taken | 3.0, 2.0, 8.6 | 5, 3.4, 10.8 | 2, 1.2, 1.0 | 657, 451, 1,356 |
| (c) one Query for the seats | 2.0, 1.1, 8.2 | 4, 2, 8.3 | 2, 1, 1.0 | 515, 296, 1,248 |

(a) The lease lives on the game's SCAN marker, not a new `LEASE#<game>` item: the marker is the small item that exists exactly while the game needs bots, and its `updated_at` is already the heartbeat's staleness clock, so one write of well under 1 KB acquires or renews the lease and tells the scan a drive is in flight (it was two writes, the first billed by the row's whole 4 to 8 KB).
A separate item would still need the marker touch, would outlive its game, and could not tell a missing game from a free lease without reading the row; a refused acquire on the marker with no item back is `CB_E_NOT_FOUND` for free.
The interplay with commits: the commit's CAS on the row's version is the only thing that keeps two writers apart, and it never depended on the lease, which only keeps two drives off one game.
A commit now updates the marker in place (an `Update`, where it was a whole `Put`), so a drive's lease survives the drive's own commits; the commit that ends the bot work deletes the marker and the lease with it, and the drive's next renew is refused and it stops, which is right.
If a later commit brings bot work back while that drive is still finishing a cycle, a second drive can take the new marker's lease for at most that one cycle; the CAS keeps them from both committing.
A lease renew no longer contends with a human's commit transaction on the row, which on AWS could cancel the commit with `TransactionConflict`.
(b) A human's MEMBER item was rewritten on every commit (2 WCU in the transaction plus 2 on GSI1 as its sort key moved) to keep the dashboard index in commit order, which nothing reads; it is now written when the seat is taken, as bot seats already were, and the deleted-account skip still holds because the comparison is made after `drop_missing_accounts`.
(c) Both seat kinds sort below `ROW` in the game partition, so one keys-only range Query reads them; the sort key says which is which.

**Index units, worked out** (Local does not report them; 1 WCU per KB per index item written, 2 when a key moves): a seat taken, 1 on GSI1; a game's end, its rated rows on GSI2 (about 3) and its replay, the SNAP item and one copy per player on GSI1 and the copies again on GSI2 (about 8 and 6 for three players at 2 KB); a chat message, 1 on GSI1; a refresh-token rotation, about 4 across both.
Before (b), every human move also cost 2 GSI1 units a seated human.
So a three-seat game now costs about 15 units on GSI1 and 9 on GSI2, against 18,000 an hour each index can sustain at 5 a second.

**What fits.**
The table sustains 15 WCU a second: 54,000 an hour, about 38.9 million in a 30-day month.
One smoke game is 1,248 table WCU, so an even month holds about 31,000 such games, and an even hour about 43; foolish plays a few games a day.
The real limit is a burst, not a month: a game in play writes about 8 WCU a move, so 15 a second is one move a second sustained from every table together, plus the burst bank.
A 13-WCU commit fits inside one second's 15.
A commit larger than the second's capacity (the 23-WCU game end with its ratings and seats, or two moves landing in the same second) draws on the burst bank: up to 300 seconds of unused capacity, 4,500 WCU on the table and 1,500 on each index, refilled at the provisioned rate whenever the load is under it.
When the bank is empty, DynamoDB throttles instead of billing: the store retries with jittered exponential backoff (25 ms doubling to 1 s, at most 10 sends, inside the Lambda deadline less a second or 5 s of pauses with no deadline), counts each throttle in the admin overview (`store_throttles`), and fails the request only past that, as a 503 the clients send a read again on ("Edge cache", "A throttle is busy, not broken"); a move then takes longer, it does not cost money.
Reads: 15 RCU a second on the table against about 1 RCU a move and 1 per view read.

**Not free, still:** point-in-time recovery bills $0.20 per GB-month of table and is not in the always-free set: at our few MB that is about a tenth of a cent a month, under the zero-spend budget's one-cent threshold but not zero.
It stays on (it is the only backup); turning it off in the template is the one line that makes DynamoDB exactly $0.

**Switching an existing table:** a stack created while the template said `PAY_PER_REQUEST` switches to PROVISIONED on a stack update in place; DynamoDB allows on-demand to provisioned "at any time", and the way back up to four times in a 24-hour window ([Developer Guide, switching capacity modes](https://docs.aws.amazon.com/amazondynamodb/latest/developerguide/bp-switching-capacity-modes.html)).

Unverified until AWS: that the indexes' units come out of the same 25 (the Free Tier page will show it), the throttle bodies (the unit test's are the API reference's; Local never throttles), and the burst bank's behaviour, which AWS documents as subject to change.
A further cut not built: the commit could leave the transaction when it writes only the row and the marker (a plain `UpdateItem` at 1 WCU a KB instead of 2), with the marker written after it; that halves the dominant cost but splits an atomic write in two, and at a few games a day it is not needed.

### The import against the provisioned table (2026-10-14)

The first live import (`ops/deploy.sh --import`) failed: the store's request-path retry gave up after 10 sends and about 5 s of pauses, which is right when someone waits on a move and wrong for an offline bulk tool.
It lost 1 game, 1 bot seat, 4 replays and 2 chat messages to `ProvisionedThroughputExceededException`, and the counting Scans were themselves throttled.
`cbase-import` now puts the store in bulk mode (`store_priv.h cb_store_dynamo_bulk`), which the Lambda never does:

- A throttle is never given up on: jittered pauses (25 ms doubling, capped at 2 s) for as long as it lasts, with no deadline.
  Anything else retried (a 5xx, a lost connection, a transaction conflict) still stops after 10 sends of its own, so a dead endpoint cannot hang the import.
- Every send is paced: `DescribeTable` gives the provisioned units, the smallest of the table's and each index's is the rate (5 RCU and 5 WCU on our table, since every write also lands in an index), and a token bucket per direction holding at most one second's worth is debited each response's `ConsumedCapacity`; a send waits until its bucket is back to zero.
  On-demand reads as 0 and is unpaced.
- The check is one keys-only, consistent Scan in pages of 100 instead of eight counting Scans, each item counted against the ids the export holds.
  An item of an id the export lacks (a game played on the new stack, a bench leftover) is listed as an extra and does not fail the import.

The rerun into the live table: 3 min 43 s, not one throttle, every table at its export count (games 67, replays 144, chat 42, the other five as before), 41 of 41 hashes, 67 of 67 games through `table_load`, exit 0, with 15 extras named (5 games, 2 seats, 2 replays, 2 accounts with their ratings).
An independent per-family `aws dynamodb scan --select COUNT` agreed with export plus extras for all eight, the admin users list held 43 (41 + 2), and a wrong password for an imported email answered 400 `invalid_credentials` through CloudFront.

## Module: auth

Owner: `src/auth/`.
Headers: `auth.h` (users, sessions, verify a bearer), `jwt.h` (HS256 in GoTrue's claim shape), `bcrypt.h` (to embed: a public-domain OpenBSD-derived bcrypt with its test vectors).
The routes over them are `app/v1_http.c`'s (`/v1/auth/*`, "Binary surface").

Email and password only; the clients derive the email from the username.
Access tokens are HS256 JWTs signed with `CBASE_JWT_SECRET`, 3600 s; refresh tokens are 32 random bytes, stored hashed, rotated on use.
GoTrue's bcrypt hashes import unchanged, so existing players keep their passwords.
A username with '%' is refused.
A refusal is the kernel's AskRefusal with an `NW_ACCOUNT_E_*` code (netwire.h).

As built:

- `bcrypt.c` is Solar Designer's public-domain crypt_blowfish (musl's copy), notice kept, with four marked changes: `$2a$` runs the correct algorithm (Go's `x/crypto/bcrypt`, which wrote GoTrue's hashes, has no anti-collision tweak), `$2x$` is refused, costs run to 31, and sign-compare casts for `-Werror`.
  It hashes `$2b$` at cost 10 (59 ms a verify on this Mac) and verifies `$2a$`, `$2b$` and `$2y$` in constant time.
- `jwt.c` mints and verifies HS256 with libcrypto: the signature first, in constant time, then alg exactly HS256, aud and role "authenticated", sub and session_id UUIDs, exp required, 30 s of skew; `iss` is written but not checked, so a change of `CBASE_PUBLIC_URL` does not sign everyone out.
- `auth.c`: refresh tokens are 32 CSPRNG bytes in base64url, stored as their SHA-256; a refresh consumes its token and issues a new pair in the same session.
  A consumed token presented again within `CB_AUTH_REUSE_GRACE_MS` (10 s, GoTrue's reuse interval) of its first use is a retry of a lost response and gets a sibling pair in the same session; the window is anchored at the first use.
  Later it is reuse, and the whole session is revoked; a token a logout revoked never gets the grace.
  An unknown email still spends one bcrypt verify against a dummy hash.
  Passwords are 6 to 72 bytes (longer is refused, not cut); usernames 1 to 64 bytes with no '%' and no control bytes.
- `auth_http.c`, the six GoTrue routes with GoTrue's error bodies and dates, served the supabase clients until stage 4 deleted it.
- Sign-out: global revokes every session of the user (`refresh_revoke_user`), local only the caller's.
- Change password changes only the password; a deleted account's token is refused by the routes that read its row while the token lives.

Tests: `test/unit_bcrypt.c`, `test/unit_jwt.c`, `test/unit_auth.c` (the accounts over a fake store), `test/unit_v1.c` (the routes through `cb_handle`), and the game smoke, where every request carries a real JWT.

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
- `fanout.c`: a pv- INSERT/UPDATE/DELETE per human row on every commit, the gu- casts per human seat and the game- cast only when `n_events > 0 || goods_changed || roster_changed`.
  table_io.ts's rule had no `roster_changed`, so a delete-account redaction on a dealt table (no card, no good) cast nothing, and an open game page, which ignores the pv- row of the game on screen, kept the deleted name until a reload; the cast carries the roster trailer and the page applies an empty cast as a board.
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

## Unfurl route

`GET /m/<payload>` (`src/app/unfurl_http.{h,c}`, built 2026-10-14) is the link preview a shared iMessage game gets in Messages, Slack and WhatsApp, which run no JavaScript.
It replaces the Next app's only server code, `foolish/src/app/m/[payload]/layout.tsx` generateMetadata (docs/VERCEL.md 4(a)), so the web app can become a static export.

As built:

- The payload is percent-decoded as `decodeURIComponent` does (a bad escape, or escapes that are not UTF-8, give the generic tags) and trimmed with JavaScript's whitespace set.
  A `1` payload goes through the same kernel C the wasm wraps: `replay_b32_decode` (case-blind, stops at a share link's `-`), `msg_decode`, then `msg_replay` into a `Game`, because a header that lies about its chain decodes and only the replay refuses it.
  Names are decoded as TextDecoder decodes them (WHATWG: each bad subpart one U+FFFD, a leading BOM dropped), empty names are dropped before the count, and the title is "A vs B - turn N", "- a finished Durak game" (phase 3) or "- a rematch, waiting to start" (phase 0 with a carry key), or "N players" when fewer than two seats are named.
- The page: title, description, robots `noindex, follow`, og:title, og:description, og:site_name Foolish, og:type website, twitter:card summary, twitter:title, twitter:description, escaped as React escapes (`& < > " '`, the apostrophe as `&#x27;`), then `<meta http-equiv="refresh" content="0;url=https://foolish.cards/">` and a plain link to the same place.
  No script: page.tsx's `router.replace('/')` becomes the refresh.
  200 `text/html; charset=utf-8`, `Cache-Control: public, max-age=300`, no API key.
  404 for `/m/` and for a path with another slash in it, as Next answered.
- HEAD, for every route: `cb_router_match` falls back from HEAD to the GET route when a path has no HEAD route of its own, and `cb_handle` marks the response `head`, so `cbase serve` writes GET's headers and Content-Length with no body (RFC 9110 9.3.2).
  HEAD /health answered 405 before.
  The Lambda encoder still sends the body, and the function URL front end drops it but answers `Content-Length: 0` whatever the body was (seen on the live stack, direct and through CloudFront); that is AWS's, not ours to set.
- One decode at a time, behind a mutex, with one static `Game`: see the defect below.

Tests: `test/unit_unfurl.c` holds the page to 20 goldens in `test/fixture_unfurl.h`, tag for tag and byte for byte: a live game at turn 10 (3 seats) and at turn 12 (2 seats), a finished 4-seat game, its rematch lobby, a plain lobby, a lobby with one named seat, an unnamed seat, names with all five escaped characters, non-ASCII names, names that are not UTF-8, a share-link suffix, lower case, percent-encoded blanks, a truncated payload, a header the replay refuses, a `2` payload, garbage, a bad escape, and an escape that is not UTF-8.
The payloads are kernel-made (`tools/unfurl_payloads.c`, `make build/unfurl-payloads`: kernel-sealed fixtures decoded, renamed, encoded again, base32'd).
The tags are the TS route's generateMetadata output under tsx with bots.wasm loaded, each rendered by react-dom/server so the escaping is React's own; the mapping from Metadata to tags was checked tag for tag against a real `next start` page.
The smoke (`test/smoke.c`) does GET and HEAD `/m/<payload>` and HEAD `/health` through `cbase serve`.
Mutation-checked: no replay, the apostrophe as `&#39;`, no carry-key test, no trim, no U+FFFD for a cut sequence, no empty-name filter, no UTF-8 check on escapes, no HEAD fallback, a body on HEAD, no 404, the cache age.

Found here:

- Production foolish.cards has unfurled every link with the generic tags.
  Under `next build` the route's server bundle hands node:fs a URL object from another realm when it loads bots.wasm.gz ("The "path" argument must be of type string or an instance of Buffer or URL. Received an instance of URL"), the catch swallows it, and the title is always "A Durak game in iMessage" (seen with `next start` and on https://foolish.cards on 2026-10-14).
  e2e/msg_og.test.ts calls generateMetadata under tsx, where the load works, so it never saw this.
  The goldens are what the route was written to say, and the C route says it.
- Kernel, and a c_base defect in `cbase serve`: the replay codec keeps its scratch in plain process globals (`replay.c`: `g_bn`, `g_rec`, `g_model`, `g_opts`, `g_weights`, `g_deal_slot`, `g_atom_sink`, `g_err`), not `_Thread_local` ones, so two replay encodes or decodes on two pool threads race.
  The unfurl's mutex covers unfurl against unfurl only; `table_replay_code` (a game finishing) runs the same codec on another pool thread.
  Lambda runs one invocation per container and is not affected.
  The fix is the kernel's: mark those statics `_Thread_local` (every single-threaded build defines the qualifier away already), then drop the mutex.
- What differs from the TS page on purpose: the root layout's tags (theme-color, apple-itunes-app, the web-app tags, fonts, analytics) are not on this page, because nothing renders it but a crawler and the instant redirect; the payload is percent-decoded once (Next may decode a param before the route's own `decodeURIComponent`, which differs only for a payload holding `%25`, which base32 never does).
- Open: crawlers that follow a meta refresh would unfurl the homepage instead.
  The TS page redirected in JavaScript for exactly that reason; docs/VERCEL.md 4(a) proposed an inline `location.replace` with the refresh only under `<noscript>`, and this build follows the later instruction to use no script.
  Check a real Messages, Slack and WhatsApp unfurl once the web distribution routes `/m/*` here.

## Module: realtime

Owner: `src/realtime/`.
Headers: `realtime.h` (the publish calls and the transport interface), `stream.h` (the protocol and the topic policy), `rt_local.h` (serve's in-process WebSocket), `rt_apigw.h` (API Gateway's WebSocket API and CONN rows).

The protocol is "The push stream" under "Binary surface".
Topics inside the server are strings with the policy of the `realtime.messages` policies they replaced: `pv-<uid>` only that user, `gu-<game>-<uid>` that user if a member, `game-<game>` any authenticated user, `chat:<game>` members.
Under Lambda the client reaches API Gateway through CloudFront at the same domain, `/v1/stream` rewritten to the stage by a CloudFront Function.

As built:

- `stream.c`: `cb_nw_on_frame` runs one client frame against the connection's row (its user and topics) and answers through a send callback, so both transports share it; `cb_rt_may_hear` is the policy.
  A game subscription picks the caller's own seat's topic when they are a member and the spectators' otherwise, and is answered after its ACK with the caller's current view when it is newer than `since` (the catch-up).
- `realtime.c` builds each push once, as the stream's message, and hands it to the transport.
- `rt_local.c` (serve): `GET /v1/stream?proto=nw1` on the loop thread; each client frame runs on the request pool, one at a time per connection, so frames are answered in order; client frames may be binary or base64 text, and pushes go out binary.
  The server's idle sweep closes a WebSocket with no client traffic for 120 s, which a client's 25 s ping prevents.
- `rt_apigw.c` (Lambda): `$connect` requires `proto=nw1` and stores a CONN row with a 2 h TTL, `$disconnect` deletes it, `$default` runs the frame (base64 text) and writes the row back only when it changed or a third of the TTL has gone (pings cost no write); replies and pushes go through the management API as base64 text, and 410 deletes the row.
  API Gateway does not serialize a connection's frames: each is its own invocation, and a page subscribes to its games, a game and its chat within a few milliseconds.
  So the row is written by compare-and-set on its version (`conn_put`, one `TransactWriteItems` with the TOPIC copies), a frame that loses re-reads and runs again (up to 12 times), and its replies are held until the run that stored.
  Found by the live browser acceptance of 2026-10-14 (then on Phoenix), where three joins each wrote back only their own topic and the page heard no pushes until it reloaded.
- Answer first (`src/app/after.{h,c}`): under Lambda an HTTP invocation POSTs its response, then publishes its commits' pushes and kicks the bots before it asks for the next event, so a move's answer no longer waits for the stream's connection Queries, the @connections POSTs and the async invoke.
  The work queues on the invocation's thread in call order and is drained in that order, each fan-out under the game's order lock; under Lambda that thread is the only committer, so a game's pushes still go out in commit order.
  A response can now arrive before its own pushes (the clients gate every push on its version, as they always had to across containers).
  A queue copy that cannot be made publishes at once; a push that fails after the answer is logged at WARN with its game and version (`after: game <id> v<n>: <k> push(es) failed after the answer was sent`), since it can no longer fail the invocation.
  `cbase serve`, bot drives and the scan publish at once, as before: a drive's pushes are paced and must not wait for its end.
- `rt_deps.h` is a table of the calls into auth, table_ops and API Gateway that the unit tests swap for fakes: the API Gateway entries cannot be real without AWS, and the fakes pin edge cases (410, expired rows, a lost CONN row) deterministically.
  The production table is exercised end to end by the game smoke, where real JWTs, the real table_ops and `rt_local` carry every push.
- Until stage 4 the module also held the facade: `phoenix.c` (both Phoenix serializers), the Supabase Realtime payloads (`rt_rows.c`), and `postgrest.c` (the PostgREST reads and the chat insert); "Binary surface" says what replaced each.

Tests: `test/unit_stream.c` (the protocol on the fake world, the frames a publish builds, and a loopback `cbase serve` socket driven by the kernel's client library), `test/unit_rt_apigw.c` (the three events, base64 both ways, the CAS race, 410 and expired rows), `test/unit_rt_local.c` (a real loopback server: the upgrade route, isolation between sockets, frames answered in order, garbage closing), and the game smoke, where three sockets hear a seat's casts, the spectators' casts and a user's views through a whole game and a crash.

Untested: `rt_apigw` against real API Gateway before the stage 3 web deploy.
Carried over on purpose: chat `is_system` settable by a client (the server ignores it now: `NW_CHAT_SEND` has no such field).

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
  An endpoint URL that says `http://` (DynamoDB Local) is reached over plain TCP (`cb_http_plain_request`), signed the same way; plain HTTP is decided by the URL alone and refused for any `amazonaws.com` host.
- The event section of `json_surface.c` (formerly `event.c`, which keeps only freeing and logging an event): Lambda events in, responses out; binary bodies are base64 both ways.
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

- `ops/cbase.yaml`: the table, the function (arm64, provided.al2023, 256 MB, 60 s), its role, the Function URL, the WebSocket API with its three routes and stage, the one-minute scan schedule (input `"scan"`), CloudFront with a CloudFront Function that rewrites `/realtime/v1/websocket` onto the stage. The GitHub Actions deploy role and OIDC provider are `ops/github_oidc.yaml`'s, a stack of their own (RUNBOOK.md step 13).
  It passes `cfn-lint`; its index names, key types and TTL are held to the store's by `test/unit_dynamo.c`.
  The zip comes from an S3 bucket parameter, because CloudFormation's inline code works only for Node.js and Python.
- `ops/package.sh` calls `make bootstrap` and checks the zip holds exactly one executable `bootstrap`.
- `ops/deploy.sh` is the whole deploy from this Mac once `aws configure` has credentials: secrets generated once, for a new stack only, into the ops settings file outside the repo (`ops/ops_env.sh`, RUNBOOK "The ops settings file"), the code bucket, the zip under a sha-named key, `cloudformation deploy`, the outputs, and the import with `--import`.
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
The replay codec is not per-thread either (replay.c's scratch is plain process globals): see "Unfurl route", Found here.

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

`store.h` and the handler split already let an S3 backend and the adapter mode be added as variants without touching the app layer.

### The bench: `c_base/bench/` (built 2026-10-13)

`make -C c_base bench` builds `build/cbase-bench` (`make all` builds it too, so CI keeps it compiling); `bench/bench.h` lists the sub-commands and `bench/bench_stats.h` the run file.
It is one C program, and the same client measures `cbase serve` and the deployed stack: `http://` is a kept-alive socket read by `https.c`'s own response parser, `https://` goes through `cb_https_request` itself, and the WebSocket (ws or wss) uses the frame codec of `net/ws.c`.

- `roundtrip --url <base> [--n 500] [--concurrency 1|4]`: each worker signs up two players, opens a stream socket per player (`/v1/stream`, frames as base64 text, which both transports take) subscribed to its games list, and plays two-human games (create, join, both ready, moves until the end, then a new game).
  Per move it times `view`, `GET /v1/games/<id>` for the mover (the move is then chosen on that envelope with the kernel's client table and legal moves, as the browser does), `action`, `POST /v1/action` from request to response, and `push_view`: the opponent's view push for the new version, its arrival on the opponent's socket minus the action response's arrival (a reader thread per socket stamps each frame, so a push that beats the response is negative, not lost).
  Bodies go through the kernel's pure codecs (`netwire.h`), because the client library keeps one session in statics and the workers are threads.
  Until stage 4 it measured the facade (`player_views` over PostgREST, `push_pv` over Phoenix); the baselines below are those runs.
  Setup is not timed; bytes are the mean HTTP head and body per request, each way.
- `coldstart --url <base> [--idle S] [--repeat K] [--path /health] [--force-cmd CMD]`: per repeat it runs the forcing command, sleeps, then times the first GET on a fresh connection and the next one on the same connection.
- `cpu`: no network; fixtures come from a game the kernel plays in-process (two humans, dealt, eight moves in), and each operation's median nanoseconds per call over batches of at least 2 ms is reported with its share of a 5 ms request: the Function URL event decode and response encode (`event.c` then, `json_surface.c` now), a GetItem answer read into a game row and the commit's TransactWriteItems built (`dynamo_wire.c` then, `json_surface.c` now), one player's view push (`view_push`: the stream's frame and its base64 text; it was the `pv-` Realtime JSON, `pv_push_json`, 2141 ns against 114 ns on the same Mac), base64 of a 2 KB envelope each way, a JWT verify, a bcrypt verify (cost 10, sign-in only, for scale), and the kernel section itself (`table_load`, `table_set_deal_seed`, `table_act`, `table_commit_products`).
- `report [FILE...]`: the Markdown tables of saved runs (default: every `bench/results/*.txt`).
- `direct --url <base> [--n 200] [--edge URL] [--burst K]`: the web's /v1 list reads (my games, leaderboard, match history, bots) and one dealt game through the stack, the same DynamoDB calls signed from this Mac with the environment's credentials (reads only), the unsigned GetItem DynamoDB's front door refuses (the network floor), a cached file on the CDN, and a burst of fresh connections for a cold start; docs/DIRECT_DYNAMO.md has the run and what it decides.

Every run prints itself and is saved as `bench/results/<kind>-<UTC stamp>.txt` (gitignored); its note carries the concurrency, games, rejected moves, lost pushes, errors and the machine's 1-minute load average.
The baseline below is committed as `bench/results/baseline-*.txt`.
Not built: a `store` sub-command timing a conditional PUT directly from this Mac; the deployed `roundtrip` already includes it.

### The local baseline (2026-10-13, Apple M1, macOS 27.0.1)

`cbase serve` on SQLite (WAL), the default build (`-O2`, no LTO), client and server on the same Mac over loopback.
The Mac was shared with other builders' jobs: the 1-minute load average was 30 to 35 during these runs (an hour earlier it was 290, and the same runs gave p50s about twice these), so the tails are the machine's as much as the server's.

| metric | concurrency | n | p50 ms | p95 ms | p99 ms | max ms | bytes out / in |
|---|--:|--:|--:|--:|--:|--:|--:|
| action | 1 | 500 | 0.16 | 0.34 | 0.46 | 6.60 | 768 / 211 |
| player_views | 1 | 508 | 0.11 | 0.21 | 0.28 | 0.39 | 781 / 681 |
| push_pv | 1 | 500 | -0.02 | 0.00 | 0.01 | 0.03 | |
| action | 4 | 500 | 0.24 | 0.56 | 1.78 | 6.48 | 767 / 211 |
| player_views | 4 | 511 | 0.18 | 0.40 | 0.82 | 1.03 | 781 / 682 |
| push_pv | 4 | 500 | -0.02 | 0.02 | 0.04 | 0.08 | |
| coldstart first (fresh connection, 2 s idle) | 1 | 5 | 0.33 | 0.41 | 0.41 | 0.41 | 47 / 270 |
| coldstart warm | 1 | 5 | 0.09 | 0.11 | 0.11 | 0.11 | 47 / 270 |

Locally the push arrives before the action's response: `cbase serve` hands the pushes to its loop before the worker's answer is written.
Against `serve` the cold start has no container to start, so both of its rows are warm numbers.

| operation (cpu) | ns per call | share of 5 ms |
|---|--:|--:|
| event_decode (Function URL event, 2.2 KB) | 24,500 | 0.49% |
| event_encode (the 7-byte answer) | 912 | 0.02% |
| dynamo_get_parse (GetItem, 2.4 KB) | 22,960 | 0.46% |
| dynamo_commit_build (TransactWriteItems, 4.6 KB) | 18,632 | 0.37% |
| pv_push_json (one player) | 2,349 | 0.05% |
| b64_encode_2k | 822 | 0.02% |
| b64_decode_2k | 4,724 | 0.09% |
| jwt_verify | 5,925 | 0.12% |
| bcrypt_verify (sign-in only) | 60,921,000 | 1218% |
| kernel_action (the whole section) | 1,403 | 0.03% |

The JSON tax of one action on the Lambda path is about 70 µs of CPU on this chip (the event in and out, the GetItem read, the commit's body, two `pv-` payloads), about 1.4% of a 5 ms request; the kernel's own work for the move is 1.4 µs.

Where `cbase serve`'s time goes: `/usr/bin/sample` on the server for 10 s during an 80,000-move `roundtrip` (concurrency 1) took 20,232 samples over six threads, 1,756 of them not waiting.
The top three frames, by time on the CPU outside waits:

1. `guarded_pwrite_np` (208 samples): SQLite's WAL write inside `sq_game_commit`, the commit of the CAS, which is 532 samples (40% of the request threads' busy time) with its locks (`__fcntl`, 99) and checkpoints.
2. `__sendto` (192): the loop thread writing responses and pushes.
3. `__fcntl` (99): SQLite's file locks and syncs.

By whole calls on the request threads: the commit 40%, the `player_views` read (`h_get`) 22%, the fanout's pushes (`cb_fanout_commit`) 17%, the JWT check about 15%; JSON's own frames (`cb_json_*`, `cb_jw_*`, `skip_value`, `skip_string`) are about 5% of the busy samples by self time.

What it says so far: the JSON tax is real and small.
The Lambda path pays about 0.07 ms of CPU per action for JSON, while each DynamoDB call it would replace costs milliseconds over the network, two to three per action.
So the fully binary stack can win at most that 0.07 ms per action on CPU, and S3's slower calls would cost more than that back; this supports the expectation above, and the deployed run will test it with the network included.

### The first AWS run (2026-10-14, from an Apple M1 in New York to us-east-1)

The stack `cbase` (Lambda arm64 256 MB, DynamoDB at 25 provisioned units, API Gateway WebSocket), reached through CloudFront's own domain `d3rcf2zypd78ll.cloudfront.net` because `api.foolish.cards` had no DNS record yet.
`roundtrip --n 150 --concurrency 1` over home Wi-Fi: 2 games, 0 rejected moves, 0 lost pushes, 0 errors, 24 s of wall time.

| metric | concurrency | n | p50 ms | p95 ms | p99 ms | max ms | bytes out / in |
|---|--:|--:|--:|--:|--:|--:|--:|
| action | 1 | 150 | 92.90 | 132.20 | 166.10 | 221.44 | 840 / 614 |
| player_views | 1 | 151 | 40.93 | 69.54 | 76.39 | 83.59 | 853 / 1086 |
| push_pv | 1 | 150 | -15.27 | -7.60 | -1.67 | -1.39 | |

Every push arrived before its action's response, by 15 ms at the median: the function posts the `pv-` frames to API Gateway's management API before it answers, and the response then still travels Lambda, the Function URL and CloudFront.
`player_views` (one GetItem behind the edge) is about 41 ms, which is mostly the round trip from New York to Virginia through CloudFront; `action` adds the conditional commit and the two management-API posts to that, about 52 ms more at the median.
The run is saved outside the repository; these rows are its numbers as printed.

This run came after the first real-AWS defect was fixed: every POST through CloudFront answered 502, because Lambda's Runtime API answers `/next` with `Transfer-Encoding: chunked` once an event is large, CloudFront's added headers take an event past 2 KB, and the runtime client honoured only `Content-Length` (`unit_runtime_api.c`).

### Running it against the deployed stack, and the variants

The deployed baseline is one command from this Mac once the stack answers (RUNBOOK.md "What you must give me afterwards"):

```
make -C c_base bench
c_base/build/cbase-bench roundtrip --url https://api.foolish.cards --n 500 --concurrency 1
c_base/build/cbase-bench roundtrip --url https://api.foolish.cards --n 500 --concurrency 4
c_base/build/cbase-bench coldstart --url https://api.foolish.cards --repeat 6 \
  --force-cmd 'm=$(aws lambda get-function-configuration --function-name cbase --query MemorySize --output text); aws lambda update-function-configuration --function-name cbase --memory-size $((m == 256 ? 257 : 256)) >/dev/null && aws lambda wait function-updated --function-name cbase'
c_base/build/cbase-bench report
```

Each `roundtrip` worker signs up two real accounts (`BENCH<HEX><n>`) and plays real games; delete them in the console's Users panel afterwards if they matter.
A Lambda container lives for minutes after its last request, so `--idle` alone rarely gives a cold start; the `--force-cmd` above toggles the memory size between 256 and 257 MB, which makes Lambda start new environments on the next request without touching the environment variables (an `--environment` update would replace them all).
It needs the CLI's credentials, and `--idle 0` is right with it; an even `--repeat` leaves the memory at the template's 256 MB, since CloudFormation does not put back a value changed outside it.
Pass `--note variant=...` on every run so the report tells variants apart.

The three binary variants do not exist yet; when one does, the same commands measure it:

- **Web Adapter:** deploy a second function from the same zip with the adapter layer and `cbase serve` as the process, behind its own Function URL, and run `roundtrip` and `coldstart` against that URL with `--note variant=web-adapter`.
  Its saving is exactly the `event_decode` and `event_encode` rows of `cpu`; its cost is the localhost hop and the adapter's start, which `roundtrip` and `coldstart` see.
- **S3 store:** once a `store_s3.c` backend exists (`store.h` admits it), a second stack with `CBASE_STORE=s3`, measured with `--note variant=s3`.
  `cpu` would gain the S3 rows (a raw GET body, a PUT with `If-Match`) beside `dynamo_get_parse` and `dynamo_commit_build`, which are what it removes.
- **Polling:** it needs a binary poll endpoint on the server and a polling client; `roundtrip` would then gain a `--poll-ms` mode in which `push_view` is the poll's arrival instead of the socket's.
  Neither the endpoint nor the mode exists.

## What stays TypeScript, and why

Stage 4 deleted the server TypeScript (measured in REPORT.md, section 5): the edge functions and their adapter under `foolish/server/impls/supabase`, `foolish/sdk/ts/table/server_table.ts`, the schema and its migrations, and the harnesses that existed only to run them (`fake_supabase.mts`, the Postgres e2e adapter).
It does not delete, and does not try to:

- the web client (`foolish/src`): it is the product, and it reaches the kernel and the client library through generated bindings;
- `foolish/server/api`, the types and constants the web still imports;
- the tests and harnesses (`foolish/e2e`) that drive the web client;
- the tools, visualisers and scripts, each its own later decision;
- generated code, which is a build artifact.

## Where JSON is tolerated, and why

The kernel's bytes never become JSON, and no client speaks JSON to c_base: the client surface is binary ("Binary surface").
JSON appears only where something outside c_base sets the format and cannot be changed:

1. AWS's APIs: the Lambda event and response envelope, the Runtime API's error body, DynamoDB, and the self-invoke payload Lambda Invoke carries.
   API Gateway Management posts a stream message (base64 text) as its body and needs no JSON of its own.
2. The access token's header and claims, because a JWT is JSON by RFC 7519 (the clients never read it: the session states its expiry).
3. The admin console's API, because it is a browser page we own and JSON is what `fetch` reads.

Until stage 4 the list also held the Supabase facade: GoTrue's bodies, PostgREST's rows and error object, Phoenix frames and the Realtime payloads inside them, and the gateway's `{"message"}` error.

### One file holds all of it

Every JSON shape above lives in one file, `src/json_surface.c` (declared in `src/json_surface.h`): every key name, every shape, and every call into the low-level writer and reader `src/util/json.c`, which nothing else includes.
Everything else in c_base hands it structs and spans and gets bytes or structs back, so a reader who wants to know what c_base says in JSON reads one file; deleting the facade in stage 4 was deleting its sections.
The functions are pure over buffers (no socket, no clock, no randomness), so each shape is checked byte for byte against a recorded body in the unit tests.

The sections of `json_surface.c`, with their callers:

- DynamoDB (`dw_*`, for `store_dynamo.c` and `store_import.c`): every request the store sends (the point reads, the 14 query shapes, the user, refresh-token, game, commit, rating, bot, snapshot, chat, connection, lease, batch, scan, describe and PartiQL requests) and every reader of an answer (the error classes and cancellation reasons, Item, Attributes, Items, pages, unprocessed work, the item-to-row readers, provisioned and consumed capacity), with the item layout and the expressions.
- The Lambda event envelope (`cb_event_*`, for `main_lambda.c` and `lambda_invoke.c`): the Function URL and WebSocket events in, the HTTP, WS and empty answers out, the `"bot_drive:<id>"` payload, and the redaction a raw event gets before it is logged.
- The Lambda Runtime API's error body (`cb_runtime_error_body`).
- The JWT header and claims (`cb_js_jwt_*`, for `jwt.c`, which keeps the base64url, the HMAC and the time checks).
- The admin console (`cb_js_admin_*`, for `admin.c`): one-shot answers take a struct, and the lists the store streams row by row are written through an opaque `cb_js_stream`, so no writer leaves the file.
- The client's side (`cb_js_client_function_url_event`, for `bench/cpu.c`): a Function URL event as AWS delivers a browser's request, the mirror of the event envelope.
- CloudFront (`cf_invalidation_xml`, for `aws/cloudfront.c`): the CreateInvalidation body, the one XML shape c_base speaks ("Edge cache").
  CloudFront's API has no JSON form, so XML is the second tolerated foreign text format, and it is contained the same way: in this file, with the same gate.

The one JSON outside it is `migrate/export.sh`, the one-off shell exporter that read production through Supabase's Management API (JSON by Supabase's design) with `python3`; it is not C and it never runs in c_base.

`test/unit_json_gate.c` holds the rule.
It reads every C source under `src/`, `bench/` and `migrate/` and fails, naming the file and line, when any file other than `json_surface.c` and `util/json.{c,h}` includes `util/json.h`, names the writer or reader (`cb_jw`, `cb_jw_*`, `cb_json_*`, `CB_JW_*`, `CB_JSON_*`), or holds a string literal with a JSON key in it (a quoted word and a colon).
It also fails a string literal that is XML: an `<?xml` prolog or an `xmlns` namespace (the unfurl route's HTML has neither, and is not XML to the gate).
It lexes rather than greps: comments may describe a shape, two literals around a ternary's colon are not a key, and adjacent literals are joined first, as the compiler joins them.
Tests (`test/`) are not scanned: their recorded bodies are the oracle the shapes are checked against.

## Edge cache (2026-10-17)

Alex, approving it: "Bot list almost never changes and leaderboard requires a game to change. Go do it."
The measurement behind it is docs/DIRECT_DYNAMO.md: a CloudFront hit is about 14 ms p50 from New York, against 30 to 45 ms for the same read through Lambda.

### What is cached

Exactly two routes, the two whose answer is the same for every caller:

| route | Cache-Control | why that long |
|---|---|---|
| `GET /v1/leaderboard` | `public, max-age=0, s-maxage=3600` | changes only when a rated game ends; invalidated then, so the hour is a safety net |
| `GET /v1/bots` | `public, max-age=0, s-maxage=86400` | changes only when the bots are seeded; invalidated then |
| everything else, refusals included | `no-store` | per caller, or an error |

Both were checked to carry nothing of the caller: `h_board` never read one, and `h_bots` read the bearer only to refuse an anonymous caller, then answered the same list to everyone; it no longer reads it, and the client's `nc_req_bots` no longer sends a token (its `now_ms` argument stays so no host changes).
`max-age=0` keeps browsers asking the edge every time (no browser cache to invalidate); `s-maxage` is for CloudFront alone.
A public answer says `Access-Control-Allow-Origin: *` whatever `CBASE_ALLOWED_ORIGINS` lists, so it cannot vary by `Origin` either.

The template (ops/cbase.yaml) gives each path its own cache behavior with `PublicReadsCachePolicy`: the key is the path alone, with no header, cookie or query string in it, and gzip and brotli normalised.
The brief said path and query; the query is left out on purpose, since no client sends one and a key with it would let `?x=<random>` walk past the cache to the origin on every request.
`Authorization` is never in the key and never reaches the function on these paths, so a client that still sends a token (an old app build) gets the shared copy.
`Managed-CORS-CustomOrigin` forwards only the CORS request headers, so an old page that still preflights is answered, and the Host stays the Function URL's.
TTL: the origin's `s-maxage`, minimum 0, default 0 (an answer without one is not kept), maximum a day.
Every other path keeps `CachingDisabled` and the stream keeps its own behavior.

The web's `fetch` and iOS's `URLRequest` add nothing that splits the key: no query string, no cache-busting header, and the C client builds every path (netclient.c).

### Freshness by event: invalidations

`src/app/edge.{h,c}` owns it.
Whatever changes one of the two answers marks its path stale, a bit in a process-wide mask (never blocking, never failing what marked it):

- the leaderboard: the commit that applied ratings (`cb_section_commit`, when `gc.n_rated > 0`, after the store accepted it; a human's game or a game the bots ended alike), an account deleted (`cb_app_delete_account`, the user's own or the admin console's), an admin write query, an import, and a ratings reindex that rewrote anything;
- the bots: a seed that wrote bots, an import, an admin write query.

`cb_edge_flush` sends one CreateInvalidation for every path marked since the last flush.
The Lambda main calls it after it has POSTed the invocation's response (`cb_after_run`, last, after the pushes), so no caller waits for CloudFront (an HTTP request, a bot drive or the scan alike); `cbase-import` calls it before it exits.
A failed flush is logged and dropped: the `s-maxage` bounds how stale the edge can get.
The request (`aws/cloudfront.c`) is signed for `cloudfront` in us-east-1 from any region (`cb_aws_call_region`), and only 201 is success; its XML body is `json_surface.c`'s, checked byte for byte against the API reference's request in `test/unit_aws_calls.c`.
Each flush's CallerReference is unique (`cbase-<ms>-<uuid>`): CloudFront answers a repeated one with the first batch, which would invalidate nothing new.

The function names its own distribution through `CBASE_CF_DISTRIBUTION`, a stack parameter `ops/deploy.sh` reads from the stack and passes back: a reference would be a cycle (Distribution -> FunctionUrl -> Function).
On a first deploy it is empty and nothing is invalidated until the next deploy.
The right to invalidate is its own `AWS::IAM::Policy` (`cloudfront:CreateInvalidation` on that one distribution), for the same cycle.

**The free 1,000 paths a month, counted.**
foolish plays a few games a day ("The free tier, provisioned" above; the import of 2026-10-14 held 67 games and 144 replays from the whole Supabase era).
At five rated games a day that is about 150 leaderboard paths a month, plus a handful for deploys and deletions: well under the ~800 at which coalescing would be needed, so there is none.
A coalescing window would also need a trailing send that Lambda has no timer for, and to stay under 1,000 a month it would have to be about 45 minutes, about the safety TTL itself.
If traffic grows past about 25 rated games a day, the billing page will show it first; each path past the 1,000 costs $0.005.

### The origin is cheap regardless

A request that misses (the first after an invalidation, at each POP, or a cold POP) still reaches DynamoDB, and before this change two routes read every rating row there.
A bench run throttled `gsi2` (73 throttled reads, 4 user-facing 500s, PR #311): `/v1/leaderboard` and `/v1/history` each read the whole `RATINGS` partition, one row per account ever made.

- The leaderboard reads only what it shows: store `ratings_top(limit)`, one Query of at most 100 items.
  A rating row's GSI2 key is now sparse and rating-ordered (json_surface.h "THE ITEM LAYOUT"): `gsi2sk` = `E#` + (2147483647 - rating, ten digits) + `#<user id>`, written only once the player has played; a sign-up's row is not in the index at all.
  Every rated commit moves the key with the rating, in the same transaction write (2 index units instead of 1 for that seat).
  `cbase-import --reindex-ratings` (run by every deploy) moves rows written before the layout, by a conditional UpdateItem that touches only the keys, only while the values are the ones it read (so it cannot lose a concurrent commit) and only when the key is not already right (so a second run writes and invalidates nothing). It ran once against production before this merged, so the new leaderboard read had every played row from its first request: 36 rows.
- The history reads only the caller's own rating, `ratings_for` by key (two keys, consistent, on the table), instead of the whole partition.

RCU per request, measured on DynamoDB Local with `ReturnConsumedCapacity` (rating rows the size of production's; the bots' Query, unchanged, is not counted):

| accounts / played | leaderboard before | leaderboard after | history's rating before | after |
|---|--:|--:|--:|--:|
| 43 / 12 (today) | 1.5 gsi2 | 0.5 gsi2 | 1.5 gsi2 | 1.0 table |
| 1,000 / 12 | 27 gsi2 | 0.5 gsi2 | 27 gsi2 | 1.0 table |
| 1,000 / 300 | 27 gsi2 | 3.0 gsi2 | 27 gsi2 | 1.0 table |

gsi2 is provisioned at 5 RCU a second, so at a thousand accounts one leaderboard or history read a second would have throttled it; now the cost follows the 100 rows shown, not the sign-ups.

### A throttle is busy, not broken

A request DynamoDB still throttles after the store's backoff used to fail as `CB_E_DB` and answer 500.
Now the backend marks the give-up on the calling thread (`cb_store_mark_busy`, store_dynamo.c), and `CB_STORE_CALL` turns that call's `CB_E_DB` into `CB_E_BUSY`: one owner, so no store function maps it itself.
The routes answer `CB_E_BUSY` with 503, `Retry-After: 1` and the kernel's `ASK_WHY_BUSY` refusal (`cb_v1_busy`), never a 500.
The client decides what to do with it, in C: `nc_retry_ms(method, attempt, status, body)` says wait 250, 500, then 1000 ms and send a GET again, and 0 for anything else (a POST may have committed before the throttle, so a write is never sent twice).
The client's pump applies it to every read it sends; the two transports (web `backend.ts`, iOS `Backend.swift`) only send what the pump names, so they decide nothing.

## Binary surface (designed 2026-10-14)

Phase 2, decided: the binary surface replaces the Supabase-compatible facade for every client, the iOS app and the web alike, and the facade is then deleted.
Alex: "if we're gonna do this, let's do it right".
It lands as four pull requests, each green and landed before the next: (1) this surface on the server and the C client library; (2) iOS on it, supabase-swift removed; (3) the web on it, supabase-js removed, the env names renamed; (4) the facade deleted from c_base (GoTrue, PostgREST and Phoenix routes and their sections of `json_surface.c`) with the dead Supabase server code and harnesses.

### What the clients use (the inventory this is cut to)

iOS (`foolish/ios/FoolishNet`): sign-up, sign-in, the restored session, sign-out, delete-account; create, action, meta (join, start, add bot, exit, continue, bump); one game's player view (the watchdog's resync); the `pv-` feed of the user's views; the `game-` spectator feed (declared, never fired).
The web (`foolish/src`): all of that, plus change password (Settings), the dashboard list (every view of the user), one game's view with the spectator fallback, the `gu-` cast stream of the open game and the spectators' `game-` stream, chat (history, send, the `chat:` stream), the bot roster (the lobby picker), the leaderboard (humans and bots), match history (the user's replays and their rating), and one game's replay (the share screen).
Nothing else: no client reads `bots` beyond id, nickname and strategy key, or ratings beyond the leaderboard's four columns.

### One owner for the bytes: `foolish/c/src/netwire.{h,c}` and `netclient.{h,c}`

The wire is the kernel's, like every other wire in this repo (awire, ask, the table request and response, the roster trailer, the envelope).
`netwire.c` is the one codec: it writes and reads every request body, response body and stream frame below, both directions, so the server and every client run the same encoder against the same decoder and there is no second statement of a layout anywhere.
`netclient.c` is the client: the session (the token store as plain data), the request builders, the response readers, the stream's subscriptions, retries and version gates.
Both are in the kernel's source list (`CORE_SRC` and `IOS_CORE_SRC`), so c_base links them through the list it already reads (`print-core`), `web.wasm` exports the client's entry points, and the iOS xcframework carries them; structgen reads `netclient.h` for the Swift and TypeScript readers of the answer structs, so no host writes or reads a byte of any of it.
They sit in the kernel's tree rather than under `c_base/` because three builds compile one list and the kernel already owns the neighbouring formats; they import nothing, never see a socket, parse no JWT (a token is opaque bytes with an expiry the server states), and take `now_ms` from the host like every kernel call.
Nothing under `shared/` changes, so the rule that nothing there names a product is untouched.

All integers are little-endian.
Every body and frame starts with two bytes, `[NW_V1][kind]`; a reader refuses another version or a kind it does not expect.
A string is a length byte and UTF-8 (ids, names, tokens up to 255 bytes) or, where noted, a u16 length; a blob (an envelope, a cast, a replay) is a u32 length and bytes; a time is u48 epoch ms.
The byte-level contract, every layout written out, is the comment at the top of `netwire.h`.

### HTTP endpoints

All under `/v1/`, `application/octet-stream` both ways, `Authorization: Bearer <access token>` where marked (B).
No `apikey` header: the anon key was a Supabase concept, and the binary surface has no anonymous key.
A refusal is a 4xx or 5xx whose body is the kernel's `AskRefusal` (ask.h), the one the writes answer with already: `why` (AUTH, MALFORMED, REFUSED, NOT_FOUND, NO_BOT, INTERNAL, METHOD, and two new ones, ACCOUNT with `code` an `NW_ACCOUNT_E_*` (bad login, name taken, weak, long or same password, bad name, bad or reused refresh token), and FORBIDDEN for a read the caller may not make), `code` and `detail`.
The client decodes it into an enum the host localizes, so no host string-matches a server message, and every route under `/v1/` refuses in one format.

| route | request body | response |
|---|---|---|
| `POST /v1/auth/signup` | `NW_SIGNUP`: name (the username upper-cased by the host), password | 200 `NW_SESSION` |
| `POST /v1/auth/signin` | `NW_SIGNIN`: name, password | 200 `NW_SESSION` |
| `POST /v1/auth/refresh` | `NW_REFRESH`: refresh token | 200 `NW_SESSION` |
| `POST /v1/auth/signout` (B) | `NW_SIGNOUT`: scope (local, global) | 204 |
| `POST /v1/auth/password` (B) | `NW_PASSWORD`: new password | 204 |
| `POST /v1/account/delete` (B) | none | 204 (delete-account's function) |
| `POST /v1/create` (B) | none | the caller's envelope (create's function, unchanged) |
| `POST /v1/action` (B) | the kernel's table request | the kernel's 7-byte table response (unchanged) |
| `POST /v1/meta` (B) | the kernel's ask | the caller's envelope (unchanged) |
| `GET /v1/games` (B) | | `NW_GAMES`: every view of the caller, newest first: version, envelope |
| `GET /v1/games/<id>` (B) | | `NW_VIEW`: the caller's view if seated, else the spectator view; 404 when neither exists |
| `GET /v1/chat/<id>` (B) | | `NW_CHATS`: the newest 100 messages, oldest first (the web asked PostgREST for the oldest 100, against its own comment); empty for a non-member, as the row policy answered |
| `POST /v1/chat/<id>` (B, member) | `NW_CHAT_SEND`: message (u16, 1 to 1000 bytes) | 204 |
| `GET /v1/bots` (B) | | `NW_BOTS`: id, nickname, strategy key |
| `GET /v1/leaderboard` | | `NW_BOARD`: the top 100 humans and top 100 bots with games played, each kind, id, name, rating, games |
| `GET /v1/history` (B) | | `NW_HISTORY`: the caller's rating (or none) and their newest 50 replays: id, created, player ids, moves, extras |
| `GET /v1/replay/<game id>` (B) | | `NW_REPLAY`: the newest replay of that game, if the caller played in it (game_snapshots' policy) |

The writes keep their bodies byte for byte (they were binary already) and their functions in `functions_http.c`; until stage 4 the same handlers were also mounted at `/functions/v1/` behind the `apikey` gate.
Sign-up and sign-in take the upper-cased username rather than an email: the server derives the account email exactly as both clients did (`hex(SHA-256(name))[:16]@foolish.cards`), so existing accounts sign in unchanged and neither client carries the derivation any more.
Upper-casing stays the host's one text call (`String.uppercased()`, `toUpperCase()`), because it is Unicode case mapping that both platforms already agree on and the kernel has no table for; a username with `%` is refused by the server, as before.
Change password and sign-out are the GoTrue semantics they replace (auth.h), from the same functions.

`NW_SESSION`: access token (u16 string), refresh token, `expires_at` (i64 ms), user id, username.
The client keeps it as a struct and hands the host an opaque blob to persist (`nc_session_save`/`nc_session_load`: the Keychain on iOS, localStorage on the web); a host never reads a token's contents, only asks for the `Authorization` value.
Refresh is the client's decision: every authenticated request builder first checks `now_ms` against `expires_at` less 60 s and, when due, answers with the refresh request instead, flagged so the host sends it, hands the answer back and asks again.
Rotation, reuse grace and revocation are auth.c's, unchanged.

### The push stream

One WebSocket per client at `/v1/stream`, carrying the kernel's bytes.
Under `cbase serve` it is `rt_local`'s upgrade; under Lambda it is the API Gateway stage, reached through CloudFront (a CloudFront Function rewrites the path onto the stage, and `$connect` requires `proto=nw1`).
The connection row (`cb_conn_row`) keeps its user, topics and version; its `vsn` is the protocol, `CB_VSN_NW` (3), the only one since stage 4 (1 and 2 were Phoenix's serializers).

Client to server:

- `NW_WS_AUTH`: the access token (u16); it names the connection's user. A bare `NW_WS_SUB` before it, or a token that does not verify, is refused with AUTH. Today's clients send no AUTH frame: every subscription is a `NW_WS_SUB_AS`.
- `NW_WS_SUB`: ref (u16), what (`NW_SUB_GAMES` the caller's views, `NW_SUB_GAME` one game's casts, `NW_SUB_CHAT` one game's chat), game id, and `since` (u32, the newest version the client holds, 0 for none).
- `NW_WS_SUB_AS`: the access token (u16), then a `NW_WS_SUB`'s fields.
  The subscription says who asks, so it never depends on an AUTH frame having been stored before it.
  API Gateway invokes the function once per frame with no order per connection, and the connection row is written by compare-and-set, so an AUTH sent first can still be stored after the SUB that followed it was read; a bare SUB then finds nobody and is refused with AUTH until its retry.
  A token that does not verify is refused with AUTH and changes nothing; another user's token ends the subscriptions made as the first one, as AUTH does.
- `NW_WS_UNSUB`: what, game id.
- `NW_WS_PING`.

Server to client:

- `NW_WS_ACK`: ref, why (0, or the `ASK_WHY_*` of the refusal: AUTH, FORBIDDEN), and for a game subscription whether the caller is seated.
- `NW_WS_VIEW`: game id, version (u32), envelope; an empty envelope means the seat is gone. The `pv-` feed.
- `NW_WS_CAST`: game id, version, the kernel's cast. A member hears its own seat's casts (`gu-<game>-<user>`), anyone else the spectators' (`game-<game>`); the server picks by membership when the subscription is made, and the ACK says which.
- `NW_WS_CHAT`: game id and the message (id, user, created, is_system, text).
- `NW_WS_PONG`.

Topics inside the server stay the strings they were (`pv-`, `gu-`, `game-`, `chat:`), with the policy of the old `cb_phx_may_join` in one function, `cb_rt_may_hear`.
The catch-up the Phoenix facade never had: a `NW_SUB_GAME` with `since` below the game's version is answered, after its ACK, with the caller's current `NW_WS_VIEW` of that game, so a client that reconnects asks for what it missed by version instead of refetching on a guess.
Version gating is the client's, in C, over one book of games that the stream (`nc_stream_in`) and the answers (`nc_on_response`) both feed (netclient.h "what changed"): a stale push or answer is dropped, a cast that skips a version is dropped and the game refetched, a refused move refetches the game when no push catches up within `NC_RECONCILE_MS`, and a game subscription accepted again (a reconnect, a refusal before it) refetches the game.
Subscriptions, their retries after a refusal (a member's join can race the membership write: 0.5 s doubling to 10 s), the 25 s ping and the reconnect delay are the client's too, and so are the subscription changes a create, a join or a leave makes: the host reports `open`, `frame` and `closed`, holds and lets go of what its screens show, and sends whatever `nc_stream_next` has.

**The pump.**
A host states intents (`NcIntent`: sign in, create, play, ask, open a game, the lists) with `nc_do` and gets a ticket; it sends every request `nc_next_request` names, hands each answer back with `nc_on_response(id, status, body)` (status 0: it never arrived), settles the ticket the answer finished, reads what the feed changed with `nc_event(i)`, and calls again when `nc_due_ms` says.
Endpoints, tokens, refreshes, retries and the meaning of every answer are the client's; the web's loop is `src/common/backend.ts` and iOS's `ios/FoolishNet/Backend.swift`, each a transport and nothing else.
A host that shows boards and never decodes a cast (iOS) says so with `nc_set_boards`, and every cast becomes a refetch of the game.

**Frames on the two transports.**
API Gateway does not accept binary frames from a client: "API Gateway WebSocket APIs don't currently support binary frames in incoming message payloads. If a client app sends a binary frame, API Gateway rejects it and disconnects the client with code 1003" ([Binary media types for WebSocket APIs](https://docs.aws.amazon.com/apigateway/latest/developerguide/websocket-api-develop-binary-media-types.html)).
Its workaround is text that carries base64 (the same page: `contentHandlingStrategy` `CONVERT_TO_BINARY` on the integration), and AWS documents no opcode for what an `@connections` post is delivered as; a question on AWS re:Post, "API Gateway Websocket Binary Frames" (https://repost.aws/questions/QUtbrnTNl6RJeseAE6ZCzx9Q, which refused a direct read and is known here only through a search summary), reports those posts arriving as text frames, which a browser refuses to decode when the bytes are not UTF-8.
Not verified against the live API: creating a throwaway WebSocket API to watch the opcode was refused by this session's permissions, so the design does not depend on the answer.
So the stream is defined over messages, and each transport carries them in the frame it can: the client always sends a text frame holding the message in base64; `rt_local` answers with binary frames; `rt_apigw` posts the message in base64, which arrives as text whatever opcode API Gateway picks.
The client accepts both (`nc_stream_in` takes the frame and whether it was text), so neither host branches on the transport, and a deployment that is shown to deliver binary from `@connections` changes one line in `rt_apigw.c`.
The cost is a third more bytes on the Lambda path for frames of a few hundred bytes to a few KB; the JSON gate is untouched, because none of this is JSON.

### The client's surface (what structgen reads)

`netclient.h` declares the answer structs the hosts read through generated readers: `NcRequest` (method, path, whether to attach the bearer and the full `Authorization` value, the body), `NcSessionInfo` (signed in, user id, username), `NcError` (the refusal's why, code and detail), `NcEvent` (one thing for the host to do: apply a view, apply a cast, apply a chat message, resync a game, a subscription refused, nothing), and one row struct per list (`NcGameRow`, `NcChatRow`, `NcBoardRow`, `NcBotRow`, `NcHistoryRow`), read one row at a time by index after the list is parsed, so a 100-row answer is not one 100 KB struct.
Blobs (an envelope, a cast, a replay's moves and extras) cross as a count and a fixed array in the answer, copied out by the generated snapshot readers; the host hands an envelope straight to `client_adopt_envelope` as it does today.
The host's whole job is: ask for a request, send it with URLSession or fetch, hand back the status and bytes; open the socket the client names, pass frames in, send the frames it asks for, call tick on a timer; persist the session blob.

### What it deletes, and what stays

Stage 4 deletes `auth_http.c`, `postgrest.c`, `phoenix.c`, `rt_rows.c`, the facade's routes and the anon key, the Phoenix half of `realtime.c` and the transports, and their sections of `json_surface.c` (GoTrue, PostgREST, Phoenix, the Realtime payloads, the gateway error, and the bench's client mirror of them), so the JSON gate's allowed surface shrinks to what AWS forces, the JWT's claims and the admin console's API ("Where JSON is tolerated").
The hosts lose supabase-js, supabase-swift, `Connector.ts`, `privateChannel.ts`, the hex decoding of view rows, `PackedAction`'s hand-written envelope, `VersionGate.swift` and the client-side email derivation.

### As built (stage 1, 2026-10-14)

- `foolish/c/src/netwire.{h,c}`: the codec, both directions, as above. The action request and response moved out of `table.c` into `table_wire.c` (same functions, same bytes), because `table.c` reaches every bot and the iOS core library and the client link neither.
  `ask.h` gained `ASK_WHY_ACCOUNT` (code an `NW_ACCOUNT_E_*`) and `ASK_WHY_FORBIDDEN`.
- `foolish/c/src/netclient.{h,c}`: the client, in the kernel's source list for c_base, the iOS core library and (stage 3) `web.wasm`; nothing exports it yet.
  One request builder per route; an authenticated builder answers the refresh first when the token is within 60 s of its expiry (`NcRequest.is_refresh`), so a host's whole transport is one loop of "build, send, if it was the refresh hand it back and build again".
  Answers are read into fixed structs, with blobs as offsets into the body the host passed (no copy); a stream event's payload is in the client's own 32 KB buffer, because a text frame has to be decoded somewhere.
- `c_base/src/app/v1_http.c`: the routes, reading the store through the same calls `postgrest.c` makes, without its query language.
  `functions_http.c` mounts its four handlers under `/v1/` too (no anon key), so the writes keep one implementation.
  The leaderboard sorts each population in C and sends each one's top 100; the web merges them and cuts the whole again, as it did.
- `c_base/src/realtime/stream.c`: the stream's protocol (`cb_nw_on_frame`), with the topic policy of `cb_phx_may_join`.
  `realtime.c` builds each push's binary frame beside the two Phoenix ones (`CB_RT_PROTOS` frames per publish, indexed by `cb_rt_proto(vsn)`), `rt_local` serves `GET /v1/stream?proto=nw1` and sends binary frames, and `rt_apigw` takes `proto=nw1` at `$connect` (no anon key) and posts base64 text.
  A CONN row whose `$connect` was lost is rebuilt as the stream when its frame does not start like JSON.
- `ops/cbase.yaml`: CloudFront sends `/v1/stream*` to the WebSocket API through the same rewrite as `/realtime/v1/websocket*`.

Tests: `unit_netwire` (every layout against hand-written bytes; every prefix and every body one byte long refused, from exactly-sized heap copies so an over-read is an ASan failure), `unit_netclient` (the client with no server: the blob, every request, refresh timing, answers, the stream's order, retries, reconnects, pings and gates), `unit_v1` (every route driven by the client library through `cb_handle` on a real store and real auth; also in `make check-dynamo`), `unit_stream` (the protocol on the fake world, the frames a publish builds byte for byte, API Gateway's base64 posts, and a real loopback `cbase serve` socket driven end to end by the client library).
Mutation-checked: `docs/mutation_checks.md`, "Binary surface".
The JSON gate is unchanged: none of this is JSON.

### As built (stage 4, the facade deleted)

- c_base: `auth_http`, `postgrest`, `phoenix` and `rt_rows` are gone with their tests, as are the anon key (`CBASE_ANON_KEY`, the router's `CB_ROUTE_APIKEY`, the template's `AnonKey`), the `/functions/v1/` mounts, the `/realtime/v1/websocket` CloudFront behaviour and the router's JSON gateway error (its own refusals are plain text now).
  `realtime.c` builds one message per push; `rt_local` and `rt_apigw` speak only the stream.
  `json_surface.c` lost its GoTrue, Phoenix, Realtime payload, PostgREST, gateway error and bench client sections (3472 lines to about 2940).
- The game smoke (`test/smoke.c`) plays its whole game, crash included, over `/v1` and `/v1/stream` with the kernel's client library building every request and reading every answer; three sockets hear a seat's casts, the spectators' casts and a user's views.
  `cbase-bench roundtrip` and `cpu` measure the binary surface.
- The TypeScript server, its schema and its harnesses are deleted ("What stays TypeScript").
- Still in the row format on purpose: `cb_conn_row.vsn` (always `CB_VSN_NW`) and `join_refs` (unused by the stream); dropping them is a store schema change in both backends for no behaviour.

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

## Acceptance on iOS (2026-10-13)

The iOS app (`foolish/ios`, scheme Foolish, Debug, supabase-swift 2.55.4) was built with an untracked `Config/Local.xcconfig` pointing at `cbase serve` and driven on one simulator, every byte recorded by a TCP tap in front of the server.
Verified: sign-up and sign-in through GoTrue (a wrong password renders GoTrue's "Invalid login credentials"); create; a second player (curl) joining and leaving the lobby, each seen on the iOS pv- feed; add bot; start; moves (pickup, covers, a transfer answered by the bot, attacks, Good) played to game over and the win screen, with the replay snapshot stored; the 9 s resync watchdog refetching `player_views` and healing two pushes the tap dropped on purpose; reconnect and rejoin after the socket was cut; token refresh every 30 s under a 120 s token TTL with supabase-swift's `access_token` channel frames accepted and the feed still live; sign-out (`logout?scope=global`, 204) and delete-account (204, then sign-in refused).
No request was refused or misread by the server: every response the app received was the shape it decodes.
supabase-swift 2.55.4 connects with `vsn=2.0.0`, offers `permessage-deflate` (not negotiated, so frames stay plain), and sends its join payload as a Swift dictionary, whose key order changes between runs; `unit_phoenix.c` supabase_swift_frames pins both recorded orders.

Found on the way, NOT c_base defects (the hosted backend answers the same, because the kernel decides): the app's `RootView` does not observe the `OnlineGame`, so after Ready it stays on the lobby with a spinner while the started board sits in the model; joining one's own started game by code is refused 400 `TABLE_E_NOT_WAITING`; Home after game over sends an exit the kernel refuses the same way; stored bot nicknames are cities (`%Vienna 1`), which `BotNames.displayNickname` does not know, so the lobby and table print the raw `%`; after delete-account the app stays on a Dashboard whose Create game does nothing; the win screen's buttons sit over the fool's hand.

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
