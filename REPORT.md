# c_base - report to Alex (2026-10-13)

## 1. The one-line answer on cost

Yes, at $0, but only by giving up the always-on process.
Nothing on AWS that keeps a process running all day is free forever; the cheapest VM is $5 a month.
Alex chose the serverless shape: Lambda (a C custom runtime) plus DynamoDB, both inside AWS's always-free allowances, with the consistent-server illusion rebuilt on DynamoDB conditional writes exactly as it is built on Postgres today.
The only line item that is not always free after the first year is the API Gateway WebSocket that carries realtime pushes, which at our volume is about a cent to a dime a month.
Supabase today is $0 and would stay $0 until the project is paused for a week of inactivity or the 500k invocations a month run out.

## 2. What we actually use today (measured on the live project)

| Fact | Value |
|---|---|
| Plan | Supabase Free, project ref wngpfwmwkltonwosqflx, us-west-1, Postgres 17 |
| Users | 41 accounts, 3 signed in within the last 30 days |
| Database | 35 MB, of which 19 MB is cron.job_run_details (the 10 second heartbeat's own log) |
| Public rows | 60 games, 144 snapshots, 47 memberships, 49 bots, 35 ratings |
| Edge functions | 5 (action, meta, create, delete-account, bot-heartbeat), all redeployed 2026-10-13 by CI |
| Background | pg_cron every 10 s calling bot-heartbeat through pg_net, plus a VACUUM job every 15 min |
| Traffic sample | 448 REST calls in 13 minutes, almost all of them the cron talking to itself |

The product's real load is tens of requests an hour.
The heartbeat machinery that fakes a consistent server out of stateless functions is the largest consumer of the free tier (258,660 posts in one 30 day window in September against the 500,000 invocation cap).
In c_base that machinery does not exist: bots are a timer inside the one process.

## 3. AWS prices, current as of 2026-10-13

AWS changed its free tier on 2025-07-15.
New accounts get $100 of credit at sign-up plus up to $100 more for using services, and the free plan ends after six months or when the credit runs out.
Accounts created before that date keep the legacy twelve months of t2/t3.micro hours, which for an account older than a year have already expired.
The "always free" list has no always-on compute in it: Lambda (1M requests), DynamoDB (25 GB), 100 GB a month of egress, CloudFront's free plan.
Every public IPv4 address costs $3.65 a month since February 2024, in use or not.

| Option | Monthly after any free period | Free period | Keeps an always-on process |
|---|---|---|---|
| Supabase Free (today) | $0 | forever, with the 7-day pause and 500k invocation limits | no (stateless functions) |
| Supabase Pro | $25 | none | no |
| A. EC2 t4g.micro (1 GiB) + public IPv4 + 10 GB gp3 | about $11 (about $8 on t4g.nano, which is tight at 0.5 GiB) | credits cover roughly six months on a new account | yes |
| B. Lightsail $5 bundle (0.5 GB, IPv4 and 1 TB transfer included) | $5 ($7 for the 1 GB bundle) | first three months free | yes |
| B'. Lightsail $3.50 IPv6-only bundle behind CloudFront's free plan or Cloudflare free | $3.50 | first three months free | yes |
| C. Lambda custom runtime (a C binary) + DynamoDB + Lambda function URL | $0 | always free at our volume | no, and it would re-fake the consistent server |

DNS costs nothing extra: foolish.cards is already on Google Cloud DNS, so c_base is one A record (api.foolish.cards), not a Route 53 zone.
TLS costs nothing: Caddy on the box fetches Let's Encrypt certificates.
Egress at our volume is inside the 100 GB free allowance.
Backups to S3 cost cents, or nothing if they stay on the box's disk.

The decision (Alex, 2026-10-13): option C, Lambda plus DynamoDB, because it is $0.
What that costs in design: no resident state, so the CAS on version, the 25 second bot lease, the once-a-minute heartbeat scan and the async self-invoke for bot drives are rebuilt in C on DynamoDB.
What it costs in JSON: the Lambda event envelope and the DynamoDB API are JSON by AWS's design; both are confined to src/aws/ and store_dynamo.c and the rest of c_base never sees them.
Alex was shown the fully binary alternatives (AWS Lambda Web Adapter as the front door, S3 with If-Match conditional writes as the store at about $0.15 a month, client polling instead of API Gateway WebSocket) and kept DynamoDB for the exact zero; those alternatives are written into c_base/DESIGN.md, and c_base/bench/ now measures them: the local baseline is in DESIGN.md, the run against the deployed stack is owed.

The line items of the chosen shape:

| Service | Allowance | Our use | Monthly |
|---|---|---|---|
| Lambda | 1M requests and 400k GB-seconds always free | well under 100k requests; bot drives sleep inside invocations at 256 MB | $0 |
| DynamoDB | 25 GB storage, 25 read and 25 write units always free | about 35 MB and a few requests a second at peak | $0 |
| Lambda Function URL | free | the HTTP front door | $0 |
| API Gateway WebSocket | 1M messages and 750k connection-minutes free for 12 months, then $1 per million messages and $0.25 per million connection-minutes | a few thousand messages a month | $0 then about $0.01 to $0.10 |
| CloudFront free plan | 1M requests and 100 GB a month | the custom domain and the path split between HTTP and WebSocket | $0 |
| ACM certificate | free | api.foolish.cards | $0 |
| EventBridge Scheduler | 14M invocations a month free | one scan a minute, 43,200 a month | $0 |
| CloudWatch Logs | 5 GB ingest free | function logs | $0 |
| DNS | foolish.cards is already on Google Cloud DNS | one CNAME | $0 extra |

The VM option stays in this report as the measured alternative.
Had Alex chosen it, the recommendation was option B, the $5 Lightsail bundle, Ubuntu 24.04.
It is a flat price with the IPv4 address and transfer included, the first three months are free, and the native server was measured at well under 100 MB resident, so 0.5 GB is comfortable for a C process plus SQLite plus Caddy.
If $5 matters, B' is the same box without an IPv4 address behind a free CDN front, and it is a one-line change later.
Option C was first rejected by the orchestrator for rebuilding the CAS, lease and heartbeat machinery; Alex overruled that on cost ("If you need to rebuild the fake consistency, so be it"), which is the right call for a product with 3 active users and a kernel that already makes the rebuild cheap.

Outside AWS, for completeness: Oracle Cloud's always-free tier includes ARM VMs that cost $0 forever, and Hetzner's smallest VM is about 4 euro.
The runbook targets AWS as asked; the code does not care which Linux box it runs on.

So the answer to "can we do the same for free by going direct" is: yes for a serverless C backend, no for an always-on server.
The migration costs about $0 to $1 a year and buys ownership of the data, no Deno, no Postgres, and the deletion of about 2,000 lines of TypeScript plus 1,300 lines of plpgsql.

## 4. C or Rust for the server (the languages only)

The repo already argued this once.
RUST_VS_C_KERNEL.md recommends Rust for the server first, on correctness grounds: six of ten shipped memory-safety bugs were in the server's hand-rolled plumbing, and all six were compile-time-preventable.
The owner declined the rewrite for the kernel because a second engine forks the one source of truth across wasm, iOS and the server.
c_base is not the kernel, so the question is genuinely open for the shell, and here is the honest version of both sides.

Where Rust wins:

- Memory safety at compile time for exactly the code a server exposes to the internet: HTTP parsing, WebSocket framing, token decoding, JSON for the compat facade.
  Those are where the repo's server bugs actually were.
- The ecosystem: tokio, hyper, tungstenite, rustls, rusqlite, argon2 and jsonwebtoken are maintained, fuzzed crates that replace about 900 lines of our own parsers.
  The native server's TLS mode falls back to thread per connection because doing TLS inside a hand-rolled epoll loop in C was too much work; tokio plus rustls deletes that compromise.
- Concurrency without data races, which matters once bots run on worker threads next to a network loop.

Where C wins:

- The kernel is C and the server's whole hot path is "parse wire, lock the game, call the kernel, append, broadcast".
  In Rust every one of those kernel calls is an `unsafe extern` call through bindgen, so the memory-safety guarantee stops at the boundary that carries all the interesting data.
  You pay for Rust's safety on the plumbing and still own the unsafe seam.
- The plumbing already exists in this repo, in C, and it has been fuzzed: the native server's HTTP parser and RFC 6455 framing ran 6.67 million semantic fuzz probes and an ASan/UBSan fuzz lane with zero findings in today's assessment, and its two known residuals were fixed in September.
  Rewriting working, fuzzed code in another language to remove bugs it no longer has is a weak trade.
- One toolchain.
  make plus clang already builds the kernel for wasm, iOS and the native server through shared/tools/c_rules.mk, and CI already has the sanitizer lanes.
  A Rust server adds rustup to every lane, a Cargo build next to make, and a 250 to 450 crate supply chain that needs cargo-audit and cargo-deny to be honest about.
- The two biggest C weaknesses the architecture doc names, TLS and compression, leave the C process entirely: Caddy terminates TLS and compresses.
  What remains in C is a plain HTTP/1.1 loopback listener, SQLite, HMAC, bcrypt and the kernel.
- The house rule: CLAUDE.md says prefer C whenever it is a realistic option, and here it is more than realistic, it is the incumbent.

The decision is C, with the Rust arguments answered structurally rather than dismissed:

1. TLS is not in C (Caddy).
2. No thread per connection, and a tiny portable event loop (poll or an epoll/kqueue shim) instead of the native server's epoll shards, because 41 users do not need shards.
3. The parsers are the already-fuzzed ones, lifted, and the fuzz and sanitizer lanes come with them into CI.
4. The one truly new C parsing surface, the small JSON the compat facade needs, is a few hundred lines with a fuzz target, and it is scheduled for deletion in Phase 2 when the clients move to binary.
5. Every kernel call stays a plain C call, no FFI, no bindgen, no second declaration of any struct.

Lambda changes one weight in this debate and it should be said: Rust has an official Lambda runtime crate and an official AWS SDK, while C has neither (the AWS Common Runtime exists in C but is a heavy dependency set).
c_base therefore carries its own small SigV4 signer, HTTPS client, runtime-API loop and DynamoDB wire, about a thousand lines with their own unit tests and fuzz targets.
That is the honest extra price of C on Lambda, and it is paid once.
If the facade grows instead of shrinking, or the AWS surface grows past DynamoDB and API Gateway, the Rust case gets stronger and this section should be reread.

## 5. What was built

c_base is one C program with two mains: `bootstrap`, the Lambda custom runtime, and `cbase serve`, a local HTTP and WebSocket server on SQLite for development, the tests and the browser.
Both call the same handler, and the kernel is linked from `foolish/c` with its own source list.
It speaks the Supabase surface the shipped clients use, so the web app moves to it by changing two environment values.
Six builders wrote the modules in parallel against the skeleton's headers; one integrator merged them (no textual conflicts), aligned them, and ran the end-to-end checks.
DESIGN.md has each module's as-built record and the integration's decisions and fixes; `docs/mutation_checks.md` has every mutation check.

| Module | Non-blank lines of C | What it is |
|---|---|---|
| util | 1,288 | errors, strings, base64, hex, UUIDs, clocks, the log ring, a small JSON writer and reader, a thread pool |
| net | 1,659 | poll(2) loop, HTTP/1.1 and RFC 6455, lifted from the native server's fuzzed code |
| store | 5,451 | `store.h`, the SQLite backend, the DynamoDB backend and its wire, the import |
| auth | 1,664 | bcrypt (crypt_blowfish, 880 lines, public domain, embedded), HS256 JWTs, accounts, the GoTrue routes |
| app | 1,540 | the kernel section with the CAS commit, the four edge functions, bots (lease, drive, scan), fanout |
| realtime | 1,989 | Phoenix in both serializers, the local and API Gateway transports, the PostgREST reads |
| aws | 1,643 | SigV4, an HTTPS client on OpenSSL, the Runtime API, Lambda events, API Gateway pushes, the self-invoke |
| admin | 860 | the console's JSON API (the page is a further 686 lines of HTML) |
| mains and boot | 680 | config, boot, the handler, `main_serve.c`, `main_lambda.c` |

That is about 16,800 lines of C in `src/`, plus 9,400 lines of C tests and a 469-line CloudFormation template.

Tests: 20 unit programs (182 test cases, about 4,900 checks; the app's games are random, so the count varies a little run to run) and two smoke lanes.
The game smoke plays whole games against `cbase serve` over real HTTP and WebSocket: two humans and a bot, A listening as realtime-js and B as supabase-swift; once mid-game the server is SIGKILLed while only the bot owes a move and restarted with nobody bumping, and the bots resume about 30 s later (10 s scans against the dead process's 25 s lease); then game over, every push stream strictly increasing in version, ratings changed, one snapshot stored, and a clean restart reloads the game unchanged.
The Lambda smoke drives the real `bootstrap` through a fake Runtime API (health, the scan, a garbage event, a drive for a missing game).
194 mutation checks are recorded, each red at the named assertion and then restored; the few equivalent mutants are named and, where the code was redundant, the code was deleted.

Results on 2026-10-13, after the last change: `make check` green on this Mac (clang); `make check-asan` green on this Mac (ASan and UBSan over everything, the kernel included); `make check` green on Ubuntu 24.04 arm64 in Docker (gcc 13.3, after two gcc-only warnings in the auth module were fixed); `make bootstrap` produces the static arm64 Lambda zip (3.4 MB, holding a 6.5 MB statically linked, stripped aarch64 ELF).

What integration found and fixed, beyond aligning interfaces: c_base compiled the kernel with its bot arena's compact capacities instead of the shipped human-game ones, so about one game in fifteen ended without a replay (reported by the app builder as a production kernel defect; production is not affected, and the build now reads the capacities from the kernel's own wasm flags, enforced by a compile-time assert); the template's DynamoDB indexes did not match the store's (names and a key type), so every dashboard, session and replay query would have failed on the first deploy, and a test now reads the template; the console's Games panel would have failed on DynamoDB; refresh had no reuse grace, so a phone that lost a refresh response would have been signed out; logout did not sign out other devices; serve had no heartbeat; DynamoDB carried its own copy of the request-signing step.

The browser acceptance (2026-10-13): the web app from this branch, served by `npx next dev` with `.env.local` pointing at `cbase serve` on a fresh SQLite file, driven in a headless Chrome launched for the purpose (fresh profile, no extensions) over the DevTools protocol, two isolated browser contexts as two players.
Verified: both players signed up through supabase-js; the dashboard loaded; a table was created, the second player joined by its URL, a bot was added, both marked ready and the game started; 58 human moves (47 cards dragged onto the table and 11 pickups) went through the UI from both tabs while the bot played, the server reaching version 170; each tab redrew the other's moves from the realtime push without a reload, and the two tabs' tables agreed after every one of the 33 rounds checked; a third context signed in with a password, and the dashboard, leaderboard and match history pages loaded (empty states, since no game in the browser reached its end; the smoke covers game over, ratings and replays); the admin console showed the live game.
A persistent DevTools monitor recorded every WebSocket frame, console error and failed HTTP response: none but the one below.
Screenshots: `/private/tmp/claude-501/-Users-alex-Dev-foolish/8f0c400b-6797-4ece-ae66-85b191c32756/scratchpad/browser/shots/acc_*.png` (27 files).
Not verified in the browser: a game played to its end (my scripted player cannot cover, so it picked up for ever), chat, delete-account, and supabase-swift.

Found in the browser, and NOT c_base's: when the first player joins a one-player lobby, the host's page fails to plan the join's animation (`anim_build_plan error -2`: the page plans against its held one-player board, and anim_plan.c refuses fewer than two players), and its animation queue then stalls, so the host does not see the joiner, or anything after, until a reload.
The same error reproduces against `e2e/fake_supabase.mts`, which mirrors the hosted adapter, so production has it today; it belongs to `src/state/animPlan.ts`, `useAnimationRun.ts` and `c/src/anim_plan.c`.

Not verified, and why:

- Real AWS: nothing has run against DynamoDB, Lambda, API Gateway or CloudFront, because there are no AWS credentials yet.
  The DynamoDB wire is checked against the API reference's documented shapes and its signing against AWS's own test vectors and a local TLS server; the first deploy is the first contact.
- The CloudFront WebSocket path (the `/realtime/v1/websocket` rewrite onto the API Gateway stage) and the API Gateway management API's `@connections/<id>` encoding.
- `ops/deploy.sh` and the CI workflow have never run (`bash -n`, shellcheck and actionlint only).
- iOS: supabase-swift has not talked to c_base; the smoke speaks its Phoenix serializer, and the auth error shape was chosen from its decoder's source.
- Under Lambda: the bot drive's deadline budget and re-kick, and the per-container cache under real concurrency.
- The latency bench (c_base/bench/, DESIGN.md) is built with a local baseline; its run against the deployed stack is owed.
- A migrated GoTrue password has been shape-checked but not verified against a known password.

The TypeScript this migration deletes once Supabase is switched off, measured on this branch without deleting anything: 1,687 non-blank lines in `foolish/server/impls/supabase` (the five edge functions and their adapter, 14 files, none of them tests) and 417 in `foolish/sdk/ts/table/server_table.ts`, 2,104 lines in all, plus 1,683 lines of SQL (`seed.sql` and five migrations).
`foolish/server/api` (192 lines) does not go: the web client's replay screen imports its replay codec.
The deletion is a follow-up PR after cutover.

## 6. What Alex does next

1. Give the AWS CLI credentials (`aws configure`), and request the ACM certificate for api.foolish.cards in us-east-1 (RUNBOOK.md, "The CLI path").
2. Run `c_base/ops/deploy.sh`; it builds, uploads and creates the stack, and prints the outputs.
3. Point the `api` CNAME at the CloudFront domain it prints, and check `/health` and `/admin`.
4. Export from Supabase, rehearse into SQLite, then `deploy.sh --import` into DynamoDB, and delete the export.
5. Switch the web build's two environment values in Vercel and re-run the web workflow; the old values are the rollback.
6. Ship an iOS build with the new URL and key; keep Supabase running 30 days for old installs, then pause it.
7. Send back the stack outputs and the health check, so the first real-AWS verification (a game against DynamoDB, a push through CloudFront) can be run.
8. The two kernel follow-ups the builders reported, one confirmed and one corrected:
   - `bot_drive_pre_action_hook` (bot_drive.c) is a process global, so serve runs bot drives one at a time; making it `_Thread_local` in `foolish/c` lets serve's bot pool grow (Lambda is unaffected: one invocation per container).
   - "A pickup of more than `MAX_LOG_PAIRS` (16) cards truncates the session log, so the replay check refuses the game and no snapshot is written, about 1 in 15 random three-seat games, present in production": true of c_base as first built, not of production, whose wasm and iOS builds use 64; fixed here in c_base's build (section 5), nothing to do in the kernel.
9. One web-client defect found in the browser run, also in production today (section 5): the host's lobby does not draw the first player who joins until a reload.

## 7. North star: TypeScript left in the repo after the migration

Today, outside node_modules and generated code, the repo holds about 81,900 non-blank lines of TS/JS: 20,600 web client, 37,600 tests and harnesses, 15,300 generated, and about 8,500 of everything else.
The migration deletes the 1,900 lines of server TypeScript (five edge functions and their adapter) and the 420 line Deno-only server_table.ts, and retires the 1,343 line seed.sql and 5 migrations with it.
It does not touch the 2,600 lines of tools and visualisers, the 1,300 lines of scripts, or the 37,600 lines of tests; those are the next targets and are listed in c_base/DESIGN.md under "What stays TypeScript, and why".
