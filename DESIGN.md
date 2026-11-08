# cbase design

cbase is a backend platform in C: a product links it into its own server binary and runs on one of its providers, `local` (`cbase serve`, one process over SQLite) or `aws` (Lambda, DynamoDB, IoT Core and CloudFront).
This is the design record: the decisions that hold for the platform, the reasoning behind them, and what each part is as built.
What a product hands the platform is [docs/PLATFORM.md](docs/PLATFORM.md); running one is [RUNBOOK.md](RUNBOOK.md).
The section names are stable, because comments in the code cite them.

cbase was written as the backend of one product and split out of its monorepo on 2026-11-08 ("History", at the end).
Where a decision was measured on that product's traffic, the number is kept as the evidence it was, and said to be so.

## Decisions

1. **Language: C.**
   Toolchain: make and clang or gcc through `tools/c_rules.mk`.
   Dependencies: SQLite, OpenSSL (libcrypto, and libssl for the outbound HTTPS client), pthreads.
   Nothing else, and the build fetches nothing.
2. **Shape: one handler, a main per provider.**
   `bootstrap` is the Lambda custom runtime (provided.al2023, arm64, statically linked); `cbase serve` is a plain HTTP and WebSocket server.
   Both reach the same core entries, and the provider is chosen at link time ("Two mains, one handler").
3. **Store: one interface, a backend per provider.**
   SQLite under `local`, DynamoDB under `aws` ("Store").
   Every write a request makes is done before its response is sent.
4. **Consistency: a compare-and-swap on a version.**
   A product's document is loaded, changed and committed conditioned on the version it was loaded at, and a lost race reloads and runs again; the store offers the conditional write, and the product owns the loop.
5. **Work without timers in the handler.**
   Work a request leaves behind is a job, started by name (a pool thread under serve, an async self-invoke under Lambda); periodic work is a schedule ("Jobs and schedules, and the kick").
6. **Client surface: binary.**
   Clients speak a binary wire (`client/include/cbase/auth_wire.h` for the platform's part, the product's own header for the rest), and JSON exists only where a foreign format demands it ("Where JSON is tolerated, and why").
7. **Admin console:** one static HTML file at `/admin`, its JSON API at `/admin/api/*`, behind a Bearer token.
8. **Ops:** a CloudFormation template and its scripts per provider (`providers/aws/ops/cbase.yaml`, `providers/local/ops/vm.yaml`).
9. **Tests in C**, every one mutation-checked ([docs/mutation_checks.md](docs/mutation_checks.md)), and gates that hold the rules (the README's "Rules the code keeps").
10. **Conventions:** no em dashes; no JSON outside `core/src/core_json.c` and `providers/aws/src/aws_json.c`; one random source per kind; one integer parser ("Numbers from text"); every time from the host's clock, passed in.

## Why C, and not Rust

Rust was weighed for the server on its strongest ground: memory safety at compile time for exactly the code a server exposes to the internet (HTTP parsing, WebSocket framing, token decoding), a maintained ecosystem for it, concurrency without data races, and an official Lambda runtime and AWS SDK.
C was chosen, with those arguments answered rather than dismissed:

- A product's own code is C, and the platform's hot path is "parse the wire, call the product, commit, publish": in Rust every product call would cross an `unsafe extern` seam, so the guarantee would stop at the boundary that carries the interesting data.
- The parsers came from code that was already fuzzed, and every parser and codec has a libFuzzer target in CI, replayed under ASan and UBSan.
- One toolchain: make and clang build everything, the sanitizer and fuzz lanes included, with no second build system and no crate supply chain to audit.
- The honest extra price of C on Lambda is cbase's own SigV4 signer, HTTPS client, Runtime API loop and DynamoDB wire, each with unit tests and the readers among them with fuzz targets; it is paid once.

If the AWS surface grows a great deal, the Rust case gets stronger and this section should be reread.

## Why Lambda, and what it costs

The aws provider exists because it is free at a small product's scale: Lambda's 1M requests and 400,000 GB-seconds a month, DynamoDB's always-free 25 GB with 25 provisioned write and 25 read units ("The free tier, provisioned"), a free Function URL, CloudFront's free plan, free ACM certificates and a free EventBridge Scheduler.
AWS IoT Core (realtime) is the one line that bills after its first year: $0.08 per million connection-minutes and $1.00 per million messages in us-east-1, cents a month at that scale.

What it costs is what one long-lived process gives for free.
A Lambda has no process between invocations, so the "one consistent server" is rebuilt on AWS primitives, deliberately:

- **The compare-and-swap.**
  A commit is an `UpdateItem` conditioned on `version = :expected` (or a `TransactWriteItems` when it writes more than the document), and a lost race reloads and runs again.
- **The lease.**
  Work that must not run twice at once (one drive of one document) takes a conditional lease on a small item, renews it every cycle and releases it on every exit.
- **The schedule.**
  EventBridge Scheduler invokes the function with a schedule's literal name, and a scan finds what a lost kick missed.
- **The kick.**
  An async self-invoke per job ("Jobs and schedules, and the kick").

The other cost is JSON.
The Lambda event envelope, the DynamoDB API, IoT Core's custom authorizer and the Lambda Invoke API are JSON by AWS's design.
That tax is confined to one file, `providers/aws/src/aws_json.c`, and nothing above `store.h` and the event header knows it exists.

S3 with conditional writes (`If-Match` on an ETag) was the binary-bodied alternative for the store.
It was not chosen because DynamoDB costs nothing at this scale and gives the query indexes that S3 would need a second store for.

## Why one VM, too

The local provider runs the same core as one process over one SQLite file, on a laptop or on one small VM (`providers/local/ops/vm.yaml`, a t4g.nano at about $3 a month).
There, what the process holds in memory is the authority: one instance by design (boot takes an exclusive `flock` on `<db>.serve-lock`, and a second `cbase serve` on the same file refuses to start), sessions found live are held in memory, and the store's writes are group-committed.
Measured on the first product's moves (2026-10-18), the VM was projected at about 23 ms a request against Lambda and DynamoDB's 57, mostly the network hops to the store it saves; the measurement is [docs/history/BENCH_VM_VS_DYNAMO.md](docs/history/BENCH_VM_VS_DYNAMO.md).
So both providers stay first-class, and a product picks at link time.

## Two mains, one handler

```
                 +-- bootstrap (main_lambda.c) --+     +-- cbase serve (main_serve.c) --+
 event / socket  | runtime API loop, event.h     |     | ingress_process.c over net/    |
                 +---------------+---------------+     +----------------+---------------+
                                 |   cb_core_http / job / schedule      |
                                 +------------> core (core.h) <---------+
                                       handler.c: Host, CORS, 404/405, the routes
                                                      |
                        the platform's routes (auth, console, health)  +  the app's routes
                                                      |
                  store.h: store_sqlite | store_dynamo        realtime.h: rt_local | rt_iot
```

`ctx.h` holds the seams: `ctx->store`, `ctx->rt` (with its transport) and `ctx->job_start` (`core/src/jobs.h`).
Each main names its provider (`core/include/cbase/provider.h`: `cb_provider_local` or `cb_provider_aws`, which picks the store, the transport, how a job starts, the edge's purge, the viewer header and the console's links) and serves the app the product links (`cb_main_app`); nothing else asks which main is running.
The provider owns `main` and calls only core's entries (`core/src/core.h`: `cb_core_http`, `cb_core_connect` for IoT's authorizer, `cb_core_job`, `cb_core_schedule`, `cb_core_after`), so core never sees a Lambda event.
`cbase serve`'s body is the process ingress (`core/src/ingress_process.c`, `cb_process_main`), which another provider that runs a process would share.
The fake Runtime API that drives the real `bootstrap` in its smoke is one library, `providers/aws/test/lib_fake_runtime.{h,c}`.
`boot.c` brings a context up the same way for every main and runs the app's self-test first.

### Serve's threads and its loop

Under serve, `cb_handle` runs on a request pool for every route but those flagged `CB_ROUTE_LOOP` (a WebSocket upgrade, `/health`), so SQLite and bcrypt never block the network loop.
A route flagged `CB_ROUTE_INLINE` may run on the loop thread while the pass has inline time left; a step there that would wait answers `CB_E_WOULD_BLOCK`, and the request runs again on the pool, and a run over its budget three times in a second turns inline off for a while (`CBASE_INLINE_BUDGET_US`).
Serve's threads are the loop, one request worker (what would block the loop), one job worker (the app's jobs) and SQLite's checkpointer, which sleeps until there is work (`CBASE_REQ_WORKERS`, `CBASE_JOB_WORKERS`, both 1 by default, `core/src/config.c` "THE THREADS"; nothing is sized from the CPUs the process sees).
The saturation knob pins them (`core/src/util/affinity.h`, Linux only): `CBASE_LOOP_CPU` puts the loop on one CPU and `CBASE_BG_CPU` every other thread on another, and both unset is auto.

- The loop waits on epoll (Linux) or kqueue (macOS, BSD) behind `poller.h`, or on io_uring with `CBASE_IO_URING=1`, where a pass's sends go to the kernel in the same call as its wait.
- An idle connection owns no buffers: the loop reads into one loop-owned scratch, and only an unfinished request is copied into a connection buffer; `CBASE_MAX_CONNS` caps them and reserves nothing.
- A closed connection is freed on the next loop pass, so a callback further up the stack never touches freed memory.
- Sends queue, and the loop flushes each marked connection once per iteration; an answer and the pushes its pass queued for the same socket leave in one send.
- A session a token check found live is held in memory (`cb_auth_hold_sessions`, under `CB_PROV_ONE_PROCESS`), and every write that ends a session takes it out first-hand.
- SIGTERM and SIGINT drain: requests in flight finish, WebSockets get 1001, then the app's `serve_close` runs.

## Module: util and net

`core/src/util/`: `err.h` (the process-wide codes), `str.h` (buffers, spans, URL decoding), `uuid.h`, `clock.h`, `log.h` (stderr plus a ring the console tails), `json.h` (a small writer and a validating reader, included only by the two JSON files), `pool.h`, `affinity.h` and `random.c` (`cb_random_bytes`, the one source of secret bytes).
`core/src/net/`: the loop and its pollers, HTTP/1.1 and WebSocket codecs as pure functions over buffers, the router, the server, files (`files.h`, docs/PLATFORM.md 2.11), the outbound TLS client (`https.h`), and HTTP/2 and gRPC ([docs/H2.md](docs/H2.md)).
The HTTP reader refuses a second differing Content-Length, any Transfer-Encoding on a request, and a bare CR or LF in a header line; the WebSocket reader refuses the 8-byte length's top bit and the reserved bits.
The outbound client resends only a safe method whose pooled connection died before any byte; a POST is never resent.

## Store

Owner: `core/src/store/` (the interface, `store.h`, and the item layer, `items.h`), `providers/local/src/store/store_sqlite.c` with `providers/local/sql/schema.sql`, and `providers/aws/src/store/store_dynamo.c`.
The interface is one table of functions (`cb_store_ops`) called through `CB_STORE_CALL`; the platform's rows are the accounts and what authentication keeps, and a product adds its own through `cb_app.store` (docs/PLATFORM.md 2.2).

SQLite: one file, WAL with `synchronous=NORMAL`, the schema (embedded into the binary at build time) applied in one transaction at every open, `PRAGMA user_version` set to `CB_SCHEMA_VERSION`, and a database newer than the binary refused.
Plain tables, every statement prepared at open, UUIDs as 16-byte BLOBs.
Writes are group-committed (`core/src/store/store_sql.h`): the first writer to find no group running runs every queued write in one transaction, a SAVEPOINT each, and every caller returns only after that COMMIT.
Checkpoints never run inside a commit: the writer's automatic checkpoint is off, a thread with its own connection runs a PASSIVE pass every 1000 frames, and at 8192 frames the next group waits for a pass that lets the WAL rewind.

DynamoDB: one table, PROVISIONED (below), point-in-time recovery on, TTL on `expires_at` (epoch seconds).
Two overloaded indexes, `gsi1` (`gsi1pk`, `gsi1sk`) and `gsi2` (`gsi2pk`, `gsi2sk`), all four keys String, both projecting ALL.
`providers/aws/src/aws_json.h` owns these names and "THE ITEM LAYOUT", the item table of the platform's families (`USER#`, `EMAIL#`, `AUTHFAIL#`, `REFRESH#`, `SESSION#`), after which a product's own families follow; `providers/aws/test/unit_dynamo.c` reads `providers/aws/ops/cbase.yaml` and fails when the template disagrees.
The request bodies are pure functions in `aws_json.c`; `store_dynamo.c` decides only which requests to send in which order, through `aws/aws_call.h` (the one signed-request step), and honours `AWS_ENDPOINT_URL_DYNAMODB`.

`make check-dynamo` runs the store conformance suite (`core/test/conformance/`) and the product's own runs against Amazon's DynamoDB Local, with the table made from the template itself (`providers/aws/ops/dynamo_local.sh` and `dynamo_table.py`, which refuses any property it does not carry over, so the two cannot drift).
A product that keeps a golden file of request shapes (`APP_DYNAMO_GOLDEN`) gets the key-capture proof there: a change that moves a key or renames an attribute fails.
DynamoDB Local differs from AWS where it matters: it serializes transactions (so `TransactionConflict` never occurs), its indexes are effectively synchronous, it never expires an item, and it never throttles.

### The free tier, provisioned

On-demand capacity has no free tier: every request unit is billed from the first.
DynamoDB's always-free allowance is 25 GB of storage and 25 write and 25 read units of PROVISIONED capacity, and the template provisions exactly that: **the table 15 / 15, `gsi1` 5 / 5, `gsi2` 5 / 5, 25 a side, no auto scaling** (it would scale past 25 and bill).

What AWS says (read on 2026-10-13):

- The free tier is "25 WCUs, 25 RCUs" a month "on a per Region, per-payer account basis", and "uses provisioned capacity and the DynamoDB Standard table class" ([DynamoDB pricing, provisioned capacity](https://aws.amazon.com/dynamodb/pricing/provisioned/)).
- A global secondary index on a provisioned table has its own read and write capacity, "separate from those of its base table"; index updates "consume write capacity units from the index, not from the base table"; and when an index lacks write capacity, "the write activity on the table will be throttled" ([Developer Guide, Global Secondary Indexes](https://docs.aws.amazon.com/amazondynamodb/latest/developerguide/GSI.html)).
- Every table and every index needs at least 1 read and 1 write unit ([API reference, ProvisionedThroughput](https://docs.aws.amazon.com/amazondynamodb/latest/APIReference/API_ProvisionedThroughput.html)).
- The account's throughput quotas "apply to the sum of the provisioned throughput capacity for all your account's tables and global secondary indexes in a given Region" ([Developer Guide, quotas](https://docs.aws.amazon.com/amazondynamodb/latest/developerguide/ServiceQuotas.html)).
- Unused capacity banks for up to 300 seconds as burst capacity, which "might change in the future" ([Developer Guide, burst and adaptive capacity](https://docs.aws.amazon.com/amazondynamodb/latest/developerguide/burst-adaptive-capacity.html)).
- A transaction writes every item twice, prepare and commit: 2 WCU per KB, consumed even when the transaction is cancelled ([Developer Guide, transactions](https://docs.aws.amazon.com/amazondynamodb/latest/developerguide/transaction-apis.html)).

What AWS does not say in so many words is whether an index's provisioned units draw on the same 25.
The design assumes they do, the reading under which "$0" cannot be wrong; the Billing console's Free Tier page shows it for an account.
The table gets most because it takes almost every write (a document's CAS); the indexes take what moves an index key.
An index throttled for writes throttles the table write behind it, which is why neither index is at the minimum of 1.
Point-in-time recovery is the one DynamoDB line outside the free set ($0.20 per GB-month), and it stays on: it is the only backup.
A stack whose table was created on-demand switches to PROVISIONED on an update in place.

### A throttle is busy, not broken

Past its free units DynamoDB throttles instead of billing.
The store retries a throttled request with jittered exponential backoff (25 ms doubling to 1 s, at most 10 sends, inside the Lambda deadline less a second, `store_dynamo.c`), counts each throttle for the console, and only past that gives up.
Then the backend marks the give-up on the calling thread (`cb_store_mark_busy`), and `CB_STORE_CALL` turns that call's `CB_E_DB` into `CB_E_BUSY`: one owner, so no store function maps it itself.
Routes answer `CB_E_BUSY` with 503, `Retry-After: 1` and the `CBW_WHY_BUSY` refusal (`core/src/net/refuse.h`), never a 500, and the client sends a read again and never a write, which may have committed before the throttle.

An offline bulk tool (a product's import) puts the store in bulk mode instead (`cb_store_dynamo_bulk`): a throttle is never given up on, anything else retried still stops after 10 sends of its own, and every send is paced to the provisioned rate read from `DescribeTable`.

### A known gap: a resent write that was applied (2026-11-08, not yet fixed)

The store's send loop (`store_dynamo.c`) resends every request that got no answer, writes included, and it is the one layer that does: the HTTP client never resends a POST (`net/https.h`).
A lost connection does not say whether DynamoDB applied the write before it went.
When it did, the resend of a conditional write (a `ConditionExpression`, or a transaction whose items carry one) meets the state its own first send made, and comes back `ConditionalCheckFailedException` or `TransactionCanceledException` with a `ConditionalCheckFailed` reason.
The caller then sees a conflict for a write that succeeded: a false CB_E_CONFLICT, or a version mismatch, where the truth is "done".
Nothing is written twice, since the condition stops the second, but the answer is wrong.
The fix wants the outcome made knowable: a `ClientRequestToken` on `TransactWriteItems` (idempotent for 10 minutes, so a resend answers the first send's result), or, after a resent write's condition fails, a consistent read that tells "mine already" from "someone else's".

## Module: auth

Owner: `core/src/auth/`.
`auth.h` (accounts and sessions), `jwt.h` (the access token), `bcrypt.h`, `names.h` (the one rule two usernames compare by), `oidc.h` (Sign in with Apple, [docs/AUTH_APPLE.md](docs/AUTH_APPLE.md)), and the routes over them, `auth_http.h` (`/v1/auth/*`, on when the app sets `auth_routes`).

- Accounts are a username and a password; the account email is derived from the name with the app's `account_domain` (`hex(SHA-256(name))[:16]@<domain>`), so a client never carries the derivation.
- `bcrypt.c` is Solar Designer's public-domain crypt_blowfish, notice kept: it hashes `$2b$` at cost 10 and verifies `$2a$`, `$2b$` and `$2y$` in constant time, and `$2x$` is refused.
  Passwords are at least 8 characters and at most 72 bytes (longer is refused, not cut), and not one of the most common passwords nor the username.
  An unknown name still spends one bcrypt verify against a dummy hash.
- The access token is a fixed binary layout with an HMAC-SHA256, base64url (`jwt.h`; no client reads it, since the session states its expiry), valid `CBASE_JWT_TTL` seconds (3600), with 30 s of skew.
- A refresh token is 32 random bytes, stored only as its SHA-256 and rotated on every use.
  A consumed token presented again within 10 s of its first use is a retry of a lost answer and gets a sibling pair in the same session; later it is reuse, and the whole session is revoked.
- A session is one row: a sign-in writes it, a refresh reads it, ending it is one write, and it ends by itself 180 days after it began.
  A token of an ended session or a deleted account is refused at once, not at its expiry.
- Failed sign-ins are counted per account (`AUTHFAIL#`, a TTL'd row): after 5, each further one locks the account for 30 s, doubling to 15 minutes, and while locked a sign-in costs one read and no bcrypt; a name with no account is never counted.

## Module: realtime

Owner: `core/src/realtime/` (`realtime.h`: the targets, the publish calls and the transport interface; `stream_mqtt.h`: the policy), `providers/local/src/realtime/rt_local.c` (serve's hub at `/ws`) and `providers/aws/src/realtime/rt_iot.c` (IoT Core).
The model is docs/PLATFORM.md 2.3: topic families the app declares, and one static policy, a connection may subscribe under `u/<its user>/` and `s/`, and no client may publish.
So privacy is decided once, at connect, and never by a callback.

Why IoT Core under Lambda: a function holds no connections, so a broker must.
The first transport was API Gateway's WebSocket API, with a connection item per socket and a subscription index in the table, a compare-and-set on every subscription and one Query per pushed topic per commit; IoT Core owns connections, subscriptions and fan-out, so all of that went, and the function answers its custom authorizer with the policy above.
Under serve the hub holds every socket in memory and a push is a copy posted to the loop.

A publish under IoT is one HTTPS POST per topic, which waits several milliseconds for well under one of CPU, so a commit's publishes go out as a batch (`cb_rt_batch_begin` and `cb_rt_batch_end`): one lane per user and one for the rooms, the lanes concurrently, each in the order published, over the HTTPS client's pool of kept-alive connections.
IoT's data plane has no batch publish, and the lanes need nothing new.
Under Lambda the batch runs after the answer is sent (`core/src/after.h`), so an answer never waits for the broker; a client must gate pushes on a version, since an answer can arrive before its own pushes.

The MQTT 3.1.1 subset both sides speak is `lib/mqtt`, one owner of every packet's bytes, each reader refusing what the other side never sends, an overlong remaining length before its body arrives, and a length written in more bytes than it needs (a defect the fuzzer found first).

## Jobs and schedules, and the kick

A job is a name and a key (`core/src/jobs.h`); the app declares what runs for each name (`cb_app.jobs`), and the provider decides how one starts (`ctx->job_start`), so no provider knows what a job does and no app knows how one is started.

### The kick

Under serve a job is a task on the job pool, and a delayed one a loop timer that holds no thread.
Under Lambda it is an async self-invoke (`X-Amz-Invocation-Type: Event`, `providers/aws/src/aws/lambda_invoke.c`) carrying the JSON string literal `"<job>:<key>"`, and a schedule is EventBridge invoking the function with the schedule's literal name.
Lambda retries a failed async invocation twice, so a job with nothing to do (`CB_E_NOT_FOUND`: its key names nothing any more) is answered `{}` rather than an error; a job runs against the invocation's deadline (`ctx->deadline_ms`), and one that needs longer starts itself again.
The function's right to invoke itself is its own statement in the template.

## Edge cache

A product's public reads, the same for every caller, can be cached at the edge and invalidated by event (`core/src/edge.h`).
The app declares them (`cb_app.cached_paths`, at most `CB_EDGE_MAX`), and its handlers answer them with `Cache-Control: public, max-age=0, s-maxage=<safety net>` (`max-age=0` keeps browsers asking the edge, so there is no browser cache to invalidate) and `Access-Control-Allow-Origin: *`, so the answer cannot vary by caller.
Whatever changes one of those answers marks its path stale, a bit in a process-wide mask that never blocks or fails what marked it; an admin write query marks every path.
`cb_edge_flush` sends one purge for every path marked since the last flush through the provider's `edge_purge` (CloudFront's CreateInvalidation under AWS, none under serve), after the invocation's answer is out, so no caller waits for the edge; a failed purge is logged and dropped, and the `s-maxage` bounds how stale the edge can get.
Each flush's CallerReference is unique, since CloudFront answers a repeated one with the first batch.
The function names its distribution through `CBASE_CF_DISTRIBUTION`, a stack parameter the deploy reads back and passes, because a reference would be a cycle (distribution, Function URL, function); on a first deploy it is empty and nothing is invalidated until the next.

The template's cache key for a cached path is the path alone: no header, cookie or query string, so `?x=<random>` cannot walk past the cache, and `Authorization` never reaches the function there.
CloudFront's free 1,000 invalidation paths a month cover a few dozen marks a day with no coalescing; each path past them costs $0.005.
The measurement that led here, a CloudFront hit at about 14 ms against 30 to 45 ms through Lambda from the same client, is [docs/history/DIRECT_DYNAMO.md](docs/history/DIRECT_DYNAMO.md).

## Module: admin

Owner: `core/src/admin/` (`admin.h`, `admin.c`, `console.html`).
One HTML file, vanilla JS, no CDN, a strict CSP, light and dark, usable at phone width, embedded at build time (`tools/embed.c`); the token lives in `sessionStorage` only.
With `CBASE_ADMIN_TOKEN` unset every console route answers 404; an API call without the exact Bearer answers 401 (constant-time compare).
Only three paths write, each a deliberate act: a query confirmed by typing `WRITE` (the store refuses a write otherwise), `users/delete` (the app's `delete_account`), and the SQLite backup (`VACUUM INTO` a timestamped file).
Under DynamoDB the Backups panel explains point-in-time recovery rather than offering an export; under Lambda the Logs panel and the counters see one warm container, and the overview links to CloudWatch.
A product adds its panels as data (docs/PLATFORM.md 2.5).

## Module: aws

Owner: `providers/aws/src/aws/`.
`runtime_api.h` (the Lambda Runtime API client), `event.h` (the event envelope in, the response out), `sigv4.h` (Signature Version 4 with libcrypto, checked against AWS's signing test suite), `aws_call.h` (the one signed-request step under every AWS call), `iot_data.h`, `lambda_invoke.h`, `cloudfront.h`, `ssm.h` (the secrets, read at container start) and `s3_blob.h`.
An endpoint URL that says `http://` (DynamoDB Local) is reached over plain TCP, signed the same way, and refused for any `amazonaws.com` host.
The runtime client honours both Content-Length and chunked answers from `/next`: the Runtime API answers chunked once an event is large, which CloudFront's added headers make every POST through it.
`make bootstrap` links a static aarch64 ELF in a pinned Alpine container (`ops/build_static.sh`): musl and not glibc, because glibc's static `getaddrinfo` still loads NSS modules at run time.
Open: `getaddrinfo` has no timeout, so a hung resolver can outlive a request's deadline.

## Where JSON is tolerated, and why

No client speaks JSON to cbase: the client surface is binary.
JSON appears only where something outside cbase sets the format and cannot be changed:

1. AWS's APIs: the Lambda event and response envelope, the Runtime API's error body, DynamoDB, the self-invoke payload Lambda Invoke carries, IoT Core's custom authorizer request and its policy answer, and SSM.
2. OpenID Connect: a provider's ID token header and claims, its published keys (JWKS), its token endpoint's answer, and the client secret Sign in with Apple wants as a signed claim set.
3. A product's own foreign API, in the one file it names (`APP_JSON_FILES`), under the same rule.
4. The admin console's API, because it is a browser page we own and JSON is what `fetch` reads.

### One file holds all of it

Every JSON shape lives in one file per side: `core/src/core_json.c` (the console, OpenID Connect) and `providers/aws/src/aws_json.c` (everything AWS's), over the low-level writer and reader `core/src/util/json.c`, which nothing else includes.
Everything else hands them structs and spans and gets bytes or structs back, so a reader who wants to know what cbase says in JSON reads two files.
The functions are pure over buffers (no socket, no clock, no randomness), so each shape is checked byte for byte against a recorded body in the unit tests.
CloudFront's API has no JSON form, so its CreateInvalidation body is the one XML shape, and it is contained the same way, in `aws_json.c`.

`core/test/unit_json_gate.c` holds the rule.
It reads every C source under `core/`, `providers/` and `bench/`, and the product's trees, but the tests and fuzz targets, and fails, naming the file and line, when any other file includes `util/json.h`, names the writer or reader, holds a string literal with a JSON key in it, or holds a string literal that is XML.
It lexes rather than greps: comments may describe a shape, and adjacent literals are joined first, as the compiler joins them.

## Numbers from text

Every decimal integer cbase reads from text goes through one parser, `num_parse_i64` (`lib/num_parse.h`, header only): the `CBASE_*` settings, the console's query numbers, a DynamoDB `N` attribute, an endpoint's port, the Lambda runtime's headers.
It takes exactly `-?[0-9]+`, the `-` only when the range has negatives, and the value in the caller's range; overflow is seen digit by digit, and leading zeros are taken (base 10 is fixed).
The C library's parsers took a leading blank and `+`, `strtoull` wrapped `-1` to 2^64-1, and call sites that ignored the end pointer took anything after the digits.
`core/test/unit_num_gate.c` fails on `strtol`, `strtoll`, `strtoul`, `strtoull`, `strtoimax`, `strtoumax`, `atoi`, `atol`, `atoll` or any `scanf` in the sources, and allows `strtod` only in `aws_json.c`, for the one fractional number cbase reads (DynamoDB's `CapacityUnits`).
`core/fuzz/fz_num.c` holds the parser against a model that accumulates in `__int128`.

## Tests

`make test` builds and runs every `<side>/test/unit_*.c` as its own program; `make smoke` runs the smokes; `make check` is both, with the client's tests and the repository's gates; `make check-asan` rebuilds everything under ASan and UBSan.
Every program runs at once (`RUN_ALL`): each has its own server, port, directory and database or table, so nothing is shared, and each one's output is printed whole when it ends.
Test files are found by wildcard, and every `<side>/test/lib_*.c` is linked into every test program.
A product's tests run through the same rules (`APP_TEST_ROOTS` in `cbase.mk`).
Every test is mutation-checked: break the code, see the right assertion fail, restore by re-editing, and delete the objects after restoring, since make compares whole seconds.

## History

cbase was written from 2026-10-13 as the backend of foolish, a card game, replacing Supabase, and served it in production on AWS Lambda and DynamoDB from 2026-10-14 and on one VM under `cbase serve` from 2026-10-19.
This document was then foolish's design record too: its game module, its bots, its link-preview route, its Supabase migration and import, its acceptance runs in a browser and on iOS, its latency benches and its client wire.
Those records are foolish's, in its repository (https://github.com/alexsave/foolish), and in this repository's git history before 2026-11-08.
How the platform was carved out of the product is [docs/history/PLATFORM_SPLIT.md](docs/history/PLATFORM_SPLIT.md).
