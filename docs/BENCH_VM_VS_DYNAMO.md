# c_base per-move latency and server cost: Lambda + DynamoDB vs a single VM (SQLite)

Measured 2026-10-18 from Alex's M1 in New York on home Wi-Fi (CloudFront POP BOS50), against origin/main 78f03f792 (includes #365 trims and #370 speedup knobs).
Investigation only: nothing committed, worktree removed, no AWS resource changed.
The raw outputs (runs, CloudWatch pulls, the SQL profile) were kept outside the repository.

## Summary

| per move (one human's play) | 2026-10-18, before the wins: CloudFront -> Lambda -> DynamoDB | projected: CloudFront -> t4g VM, cbase serve on SQLite |
|---|--:|--:|
| action, send to response, p50 | 57.0 ms (57.0 / 57.2 over two runs, n=300) | about 23 ms |
| action, send to response, p95 | 85 ms (86.9 / 84.1) | about 30 to 45 ms (see assumptions) |
| opponent sees the move (action + push after response), p50 | about 65 ms (push lands 6.3 to 9.3 ms after the response) | about 23 ms (push leaves before the response) |
| server time inside the move, p50 | 18.6 ms response phase + 39 ms after-phase (billed about 65 ms) | about 0.1 ms on M1 loopback (p50 97 us, p95 155 us); est. 0.3 to 0.5 ms on Graviton2 |
| server CPU per move, same harness, 40 games, full speed | 410 to 447 us (DynamoDB store, DynamoDB Local) | 104 to 106 us (SQLite store) |
| DynamoDB requests per move | 1.38 average (1 TransactWriteItems always; GetItem on a row-cache miss) | 0 |

Headline: the VM would cut a move from about 57 ms to about 23 ms at the median, and send-to-opponent from about 65 ms to about 23 ms.
Of the 34 ms saved, about 17.5 ms is DynamoDB (almost all of it the commit's TransactWriteItems) and about 15 ms is the Lambda Function URL invoke overhead.
Server CPU per move drops about 4x (about 430 us to about 105 us), and in the VM about 40% of what is left is SQLite's WAL commit.

## 1. Server-side cost per move (same harness, full speed, 40 games)

Two p3client processes play 40 two-human games against one `cbase serve`, both with P3FIX=1 (retry a play refused at once).
Client binaries built from origin/main; the harness was ported to the MQTT stream (main moved the stream to `/mqtt` with binary MQTT frames, so `o->text` became `o->bytes` sent as BINARY with `Sec-WebSocket-Protocol: mqtt`) and the nc_debug_dump calls were dropped.
I did not use PR #371's branch: the pump-full stall is a client issue, and P3FIX works around it without touching the server being measured.
CPU is `ps` time of the server process between the go file and both done files; runs without the sampler.

| store | run | moves | refused | server CPU s | us / move | us / committed move | wall ms / move |
|---|---|--:|--:|--:|--:|--:|--:|
| SQLite | sql40_1 | 4629 | 526 | 0.49 | 106 | 119 | 1.47 |
| SQLite | sql40_2 | 4402 | 488 | 0.46 | 104 | 118 | 1.55 |
| SQLite | sql40_3 | 4725 | 557 | 0.49 | 104 | 118 | 1.43 |
| DynamoDB Local | ddb40_1 | 4538 | 658 | 1.86 | 410 | 479 | 3.62 |
| DynamoDB Local | ddb40_2 | 4409 | 667 | 1.92 | 435 | 513 | 3.64 |
| DynamoDB Local | ddb40_3 | 4364 | 638 | 1.95 | 447 | 523 | 3.62 |

DynamoDB Local's own JVM burned 1.85 to 1.92 ms of CPU per move on top (4.5x the server), so its wall times are not production's.
The DynamoDB-store CPU here (about 430 us) is above the earlier ~300 us post-#365 figure; the sampled run of the same config read 281 us, and run-to-run refusal counts differ, so treat it as a 300 to 450 us range.

### Where a VM server spends its time (SQLite, /usr/bin/sample at 1 ms, five 40-game runs merged, 2949 non-wait samples)

Self time, waits (cvwait, poll, mutexwait) removed:

| # | frame | share |
|--:|---|--:|
| 1 | `__sendto` (responses and pushes on the loop thread) | 13.5% |
| 2 | `guarded_pwrite_np` (SQLite WAL frame writes) | 13.1% |
| 3 | `__fcntl` (SQLite locks and syncs) | 12.2% |
| 4 | `_kernelrpc_mach_vm_purgable_control_trap` (malloc) | 4.0% |
| 5 | `__recvfrom` | 3.8% |
| 6 | `write` | 2.7% |
| 7 | `madvise` | 2.0% |
| 8 | `sqlite3VdbeExec` | 2.0% |
| 9 | `_platform_memmove` | 1.5% |
| 10 | `state_blob_put_rated_at` | 1.4% |

Inclusive: `h_action` 68%, `cb_section_commit` 53%, `sq_game_commit` 40% (of which WAL checkpoints 7.8%, `unixSync` 8.2%), loop-thread WebSocket sends 11%, JWT verify 8%, fanout publish 5%, kernel `table_act` 0.3%.
So on a VM the cost is the durable write and the socket syscalls, not game logic and not encoding.
The sampler inflates CPU (sampled SQLite runs read 148 to 209 us/move), so the CPU table above uses unsampled runs only.

## 2. Real DynamoDB per-operation latency (CloudWatch, table cbase, 7 days to 2026-10-18 16:2x UTC)

`SuccessfulRequestLatency`, server-side inside DynamoDB, one 7-day period:

| operation | requests | avg ms | p50 | p90 | p99 | max |
|---|--:|--:|--:|--:|--:|--:|
| GetItem | 13,669 | 1.30 | 1.03 | 1.53 | 11.86 | 33.8 |
| Query | 22,607 | 4.66 | 1.62 | 14.02 | 18.95 | 235.3 |
| TransactWriteItems | 4,302 | 17.94 | 16.93 | 21.07 | 38.91 | 243.7 |
| UpdateItem | 3,088 | 3.67 | 2.37 | 4.26 | 17.89 | 244.0 |
| PutItem | 403 | 2.59 | 2.33 | 2.80 | 13.52 | 17.8 |
| BatchGetItem | 367 | 1.64 | 1.49 | 1.98 | 4.31 | 16.6 |
| BatchWriteItem | 823 | 3.67 | 3.17 | 4.38 | 16.40 | 35.7 |
| DeleteItem | 18 | 12.20 | 13.11 | 15.56 | 16.87 | 17.1 |

During my bench window (16:19 to 16:21 UTC): TransactWriteItems n=339 p50 15.4 ms p99 27.1; GetItem n=433 p50 0.87 p99 11.6.
Lambda-side, from the function's own `inv:` timing lines for my 300 moves: DynamoDB time per call p50 15.5 ms, p95 21.6 ms; so the Lambda-to-DynamoDB network adds little, and the commit's transaction is the cost.

Lambda `inv:` lines for `POST /v1/action` (my 300 moves, current deploy):

| field | avg | p50 | p95 | max |
|---|--:|--:|--:|--:|
| response phase ms | 20.8 | 18.6 | 38.9 | 59.9 |
| of which DynamoDB ms | 19.7 | 17.5 | 37.5 | 58.1 |
| response ms minus DynamoDB ms (Lambda compute) | | 1.0 | 1.3 | |
| DynamoDB calls | 1.38 | 1 | 3 | 5 |
| after-phase ms (after the answer is sent) | 44.4 | 39.0 | 64.8 | 223.8 |
| IoT publishes in the after-phase | 5 | 5 | 5 | 5 |
| ms per IoT publish | | 7.8 | 12.9 | |

`GET /v1/games/<id>` (the view): 3.3 ms avg, 2.6 p50, one GetItem at 1.8 ms p50.
Call mix: 212 moves made 1 call, 65 made 2, 21 made 3, 2 made 4 or 5.
The older 7-day rows (148 moves on the evening of 2026-10-17, older inv format) averaged 5.5 DynamoDB calls and 28 ms of DynamoDB per move, so the current code already cut the calls by 4x.
REPORT Duration for those older moves: avg 58 ms, p50 51, p95 117, 1 cold start in 148 (init 28 ms, duration 443 ms).

## 3. Production end to end, decomposed

`cbase-bench roundtrip --url https://api.foolish.cards --n 150 --concurrency 1`, twice (rt1 16:19, rt2 16:19 UTC): 2 games each, 0 rejected, 0 lost pushes, 0 errors.

| metric | run | n | p50 ms | p95 | p99 | max |
|---|---|--:|--:|--:|--:|--:|
| action | rt1 | 150 | 56.96 | 86.86 | 101.47 | 106.33 |
| action | rt2 | 150 | 57.19 | 84.09 | 95.50 | 103.06 |
| view | rt1 | 153 | 39.99 | 67.39 | 116.12 | 123.44 |
| view | rt2 | 154 | 40.40 | 65.98 | 85.70 | 173.42 |
| push_view (after the action response) | rt1 | 150 | 9.32 | 23.04 | 102.84 | 176.68 |
| push_view | rt2 | 150 | 6.28 | 16.92 | 33.17 | 188.90 |

Network legs (kept-alive, same Mac, same hour):
- CloudFront cache hit (`foolish.cards/favicon.ico`, BOS50): p50 14.9 ms, p95 16.7 (cbase-bench spaced, n=100); curl 13.8 p50.
- ICMP: api.foolish.cards 13.7 ms, the Function URL host 13.8, dynamodb.us-east-1 15.4.
- Function URL directly (`/health`, answered 403 by c_base's origin check, so the handler runs and returns at once): p50 29.1 ms, p95 36.0 (curl, n=59).
- CloudFront to Lambda `/health`: bimodal, 65% of samples 30 to 40 ms, about 30% at 60 to 85 ms (p50 37 to 60 ms depending on the minute).

Decomposition of the action's 57 ms p50:

| leg | ms | how it was obtained |
|---|--:|---|
| client to CloudFront POP and back (TLS kept-alive, edge processing) | 14.9 | CloudFront hit p50 |
| POP (Boston) to us-east-1 and back | about 7.5 | the remainder: view 40.0 = 14.9 + this + 15 + 2.6 |
| Function URL front end + Lambda invoke | about 15 | Function URL /health 29.1 minus ICMP 13.8 minus a ~0.5 ms handler |
| Lambda compute (decode, JWT, kernel, encode) | 1.0 | inv ms minus ddb ms |
| DynamoDB: 1.38 calls, the commit's TransactWriteItems about 15.5 of it | 17.5 | inv ddb ms p50; CloudWatch agrees |
| total | about 56 | measured 57.0 |

The same legs predict the view at 14.9 + 7.5 + 15 + 2.6 = 40.0 ms; measured 40.0 and 40.4.
After the response, the Lambda publishes 5 IoT messages one after another (39 ms p50); the opponent's push lands 6 to 9 ms after the mover's response, so send-to-opponent is about 65 ms p50 (the earlier IoT figure was 68).

## 4. Projected VM latency

Projection: action p50 = 14.9 (client to POP) + 7.5 (POP to us-east-1) + 0.3 to 0.5 (server) = about 23 ms.
The opponent's push: `cbase serve` hands the pushes to its loop before the answer is written (local push_view p50 -7 us), so the opponent sees the move about one client-to-VM round trip after it was sent, about 23 ms p50, if the WebSocket also runs through CloudFront or straight to the VM from a similar distance.
p95: client-to-POP p95 is 16.7, so a clean origin leg gives about 30 ms; if the slow mode seen on CloudFront-to-Lambda (30% of samples near 60+ ms) is CloudFront's origin-connection churn rather than Lambda, a VM origin inherits it and p95 is closer to 45 ms.
Local evidence for the server term: `cbase-bench roundtrip` against `cbase serve` on SQLite over loopback, n=1000: action p50 97 us, p95 155, p99 279; view p50 56 us.

Assumptions:
- The VM is in us-east-1 behind the same CloudFront distribution, with CloudFront keeping origin connections alive.
- The POP-to-region leg (about 7.5 ms) is derived, not measured: it is what is left after the edge hit, the Function URL overhead and the handler; BOS to Ashburn fiber RTT is consistent with it.
- Graviton2 (t4g) runs the server 3 to 5x slower than the M1's 0.1 ms; still under 1 ms.
- SQLite runs WAL with synchronous=NORMAL (the store's setting), so a commit does not fsync; EBS fsync only lands on checkpoints. A t4g's burst credits are not exhausted (CPU use is under 0.1% of a core at today's load, see vm_cost.md).
- Home Wi-Fi to the POP is the same in both designs; phones on LTE add the same amount to both.

A real check would remove the two derived terms (POP-to-VM leg and Graviton server time): one t4g.nano for 1 hour plus a public IPv4 is about $0.0042 + $0.005 = about $0.01, plus pennies of EBS, if a CloudFront behaviour or a second test distribution points at it.
I did not launch anything; I think the projection is good to about +-5 ms at the median without it, and the hour is only worth it if the decision rests on the p95.

## Exact commands

The helper scripts named `bench-vm/...` below (the ported p3client harness, the run and Logs Insights wrappers) were throwaway and are not in the repository; `cbase-bench` is.

Build (throwaway worktree off origin/main 78f03f792, patches saved as bench-vm/worktree.diff and p3client_mqtt.c):

```
git worktree add --detach .claude/worktrees/bench-vm origin/main
cp bench-vm/harness/p3client_fixretry.c c_base/tools/p3client.c   # then the MQTT port, see worktree.diff
git apply bench-vm/harness/Makefile.diff
make -C c_base -j10 build/cbase build/p3client build/cbase-bench
```

Local runs (bench-vm/run.sh STORE LABEL GAMES SAMPLE [LOGLEVEL], P3FIX=1, port 8858):

```
docker run -d --name benchvm-ddb -p 127.0.0.1:8857:8000 amazon/dynamodb-local:3.3.1 -jar DynamoDBLocal.jar -inMemory -sharedDb
CBASE_DYNAMO_PORT=8857 c_base/ops/dynamo_local.sh
bench-vm/series2.sh          # sql40_1..3, ddb40_1..3, sql40_s, ddb40_s
for i in 1 2 3 4; do bench-vm/run.sh sqlite sqls_$i 40 1; done
python3 bench-vm/selfprof.py runs/sqls_1/sample_server.txt,...,runs/sql40_s/sample_server.txt 12
bench-vm/run.sh dynamo ddb_dbg 5 0 debug    # per-request DynamoDB call counts in the server log
bench-vm/local_rt.sh                        # cbase-bench roundtrip vs cbase serve on SQLite, n=1000
```

CloudWatch and logs (read-only):

```
aws cloudwatch get-metric-statistics --namespace AWS/DynamoDB --metric-name SuccessfulRequestLatency \
  --dimensions Name=TableName,Value=cbase Name=Operation,Value=<op> --start-time <now-7d> --end-time <now> \
  --period 604800 --statistics Average SampleCount Minimum Maximum --extended-statistics p50 p90 p99
bench-vm/li.sh 168 'fields @message | filter @message like /inv: http POST \/v1\/action 200/ | parse ... | stats ...'
python3 bench-vm/summ.py prod/rt_inv.txt
```

Production:

```
c_base/build/cbase-bench roundtrip --url https://api.foolish.cards --n 150 --concurrency 1   # x2 (a third hit the signup limit)
c_base/build/cbase-bench spaced --url https://foolish.cards --path /favicon.ico --n 100 --gap 100 --games 0
bench-vm/curlt.sh https://api.foolish.cards/health 60
bench-vm/curlt.sh https://<function-url-host>/health 60
ping -c 20 api.foolish.cards; ping -c 20 dynamodb.us-east-1.amazonaws.com
```

The roundtrip binary was patched (worktree only) to `POST /v1/account/delete` both of its accounts at the end; rt1 and rt2 deleted all four (204).

## Surprising

- The commit's TransactWriteItems costs 15 to 17 ms server-side at p50, 15x a GetItem and 7x a conditional UpdateItem (2.4 ms); it is about 30% of a move's latency and nearly all of its DynamoDB time.
  If c_base stays on DynamoDB, a mid-game commit as one conditional UpdateItem (when the transaction's other items, the SCAN marker and seat puts, are not needed) would likely save about 13 ms per move; I did not verify which items a mid-game commit carries.
- The Lambda Function URL invoke overhead is about 15 ms per request, 15x the Lambda's own compute (1.0 ms).
- The after-phase publishes 5 IoT messages sequentially at 7.8 ms each (39 ms p50, up to 224 ms); every move is billed about 65 ms for about 1 ms of CPU, and the opponent's push trails the response by 6 to 9 ms.
- Per-move DynamoDB calls are already down to 1.38 in production from 5.5 on the evening of 2026-10-17.
- Harness: main's stream is now MQTT, so the saved p3client no longer compiled; the 40-game sqlite runs without P3FIX stalled at game 6 (the pump-full refusal PR #371 fixes); and one account can only create about 40 games (429 on the 41st), which caps a single run.
- One throwaway account leaked: a third roundtrip hit the sign-up limit (429) after its first player signed up, and the account stayed in production until it was deleted through the admin API on 2026-10-18.
  `cbase-bench roundtrip` now names each account as it signs it up and deletes both at the end.

## After the three wins (2026-10-18, PR #372, deployed 17:51 UTC)

Alex decided on 2026-10-18 to stay on Lambda + DynamoDB for now; the VM option above is parked by that decision.
PR #372 made three changes: a commit that writes the row alone is one conditional `UpdateItem` instead of a `TransactWriteItems` (DESIGN.md "Store"), a commit's IoT publishes go out at once on one lane per user (DESIGN.md "Pushes, after the answer"), and the API's answers carry CloudFront's `Server-Timing`.
Measured with the same commands at 17:56 to 17:58 UTC: `cbase-bench roundtrip --url https://api.foolish.cards --n 150 --concurrency 1`, twice, 2 games each, 0 rejected, 0 lost pushes, 0 errors, every bench account deleted at the end.

### The network moved, so compare the parts it does not touch

The Mac was on a different network for the after runs, so a raw end-to-end delta mixes the wins with the network.
The network legs, re-measured right before the runs with the same tools:

| leg | before (16:2x UTC) | after (17:4x UTC) |
|---|--:|--:|
| CloudFront cache hit (`foolish.cards/favicon.ico`, `cbase-bench spaced`, n=100), p50 / p95 | 14.9 / 16.7 ms (BOS50) | 12.9 / 20.6 ms (JFK50) |
| ping api.foolish.cards, p50 | 13.7 ms | 11.8 ms |
| ping dynamodb.us-east-1, p50 | 15.4 ms | 16.4 ms |
| Function URL `/health` direct (curl, kept alive), p50 | 29.1 ms | 30.5 ms |

The parts no client network reaches, from the function's own `inv:` and `REPORT` lines for the 300 moves of each pair of runs and from CloudWatch:

| per move (`POST /v1/action`) | before | after |
|---|--:|--:|
| response phase (request in to answer out), p50 / p95 | 18.6 / 38.9 ms | 4.9 / 7.9 ms |
| of which DynamoDB, p50 / p95 | 17.5 / 37.5 ms | 4.0 / 6.8 ms |
| DynamoDB calls, average | 1.38 | 1.15 |
| after-phase (pushes once the answer is out), p50 / p95 | 39.0 / 64.8 ms | 17.6 / 35.8 ms |
| IoT publishes per move | 5 | 5 (on 3 lanes at once; 8.2 ms each at p50, no new connection dialed) |
| billed duration, p50 / avg / p95 | 61 / 66.6 / 94 ms | 24 / 29.1 / 42 ms |
| cold starts | 0 of 300 | 0 of 300 |

CloudWatch `SuccessfulRequestLatency` for the after window: 219 `UpdateItem` at p50 2.81 ms (p99 3.75) and 22 `TransactWriteItems` at p50 16.8 ms (the creates, joins and ends of games, which stay transactions).
So nine moves in ten no longer pay a transaction, and a move's DynamoDB time fell from 17.5 to 4.0 ms at p50.
The after-phase is now about two publishes long, not five: each user's view still goes before its cast on that user's lane (a host showing boards spends a GET on a cast that beats its view), and the users' lanes and the spectators' run at once.

The five pushes per move between two humans are each needed: two views on `u/<user>/v` (the dashboard, and how iOS's boards mode takes every move of the open game), two seat casts on `u/<user>/c/<game>` (each carries that seat's hand), and the spectators' cast on `s/<game>`.
The server cannot see who is subscribed (IoT names no subscriber), so none is dropped.

### End to end, adjusted for the network

| metric | before | after, raw | after, adjusted to the before network |
|---|--:|--:|--:|
| action p50 | 57.1 ms | 34.6 ms | 36.6 to 43.2 ms |
| action p95 | 85.5 ms | 49.8 ms | 45.9 to 69.3 ms |
| opponent sees the move (action + push after the response), p50 | 64.9 ms | 39.1 ms | 41.1 to 47.7 ms |
| push lands after the mover's response, p50 | 6.3 to 9.3 ms | 2.5 to 6.5 ms | (relative: no network term) |

Each row is the mean of the two runs.
How the adjustment was made, two ways, each with its limit:
- the cached-hit adjustment adds back what the client-to-edge leg gained (14.9 - 12.9 = +2.0 ms at p50; 16.7 - 20.6 = -3.9 ms at p95); it covers only the client-to-POP leg, not the POP change (BOS50 to JFK50) or the edge-to-origin leg, so it is the low end;
- the view adjustment uses `GET /v1/games/<id>` as a control: its work inside the function did not change (2.6 vs 3.1 ms p50 by its `inv:` lines), yet its client time fell from 40.2 to 31.6 ms p50 (66.7 to 47.2 ms p95), and that difference (+8.6 ms p50, +19.5 ms p95) is added back; it covers the whole path, but the view also gained from the shorter after-phase (a view no longer lands while the warm container is still publishing the last move), so it over-corrects, and it is the high end.
The figure that needs no adjustment is the function's own: the answer leaves the Lambda 13.7 ms sooner at p50 (31 ms at p95), which on the before network makes the action about 43 ms p50, against the 57 it was.

### What the edge's Server-Timing found (the third win)

About 30% of requests through CloudFront took 25 to 30 ms more than the same request to the Function URL, and on a request-by-request join with the function's logs the function's own time stayed 1.2 to 1.5 ms (`/health`, no init) in the slow ones.
CloudFront's `Server-Timing` (now on every API answer) names the cause: a new connection from the edge to the Function URL.
From one viewer connection to JFK50, one request after another and with 2 s between requests:

| CloudFront to origin | share (back to back / 2 s apart) | cdn-upstream-connect p50 | cdn-upstream-fbl p50 / p95 | client time p50 |
|---|--:|--:|--:|--:|
| new connection | 61% / 31% | 12 ms | 32 / 40 ms | 45.7 / 48.2 ms |
| reused connection | 39% / 69% | 0 | 19 / 25 ms | 30.8 / 33.8 ms |

Back-to-back requests on one viewer connection (100 ms apart) were the likelier to get a new origin connection, so this is not the keep-alive timeout (already 60 s, the most without a quota request): the edge spreads requests over hosts that each keep their own origin connections, and at foolish's traffic most of them hold none.
What can be done at $0, and why not:
- `OriginKeepaliveTimeout` is at its 60 s ceiling, and the gaps here were 0.1 to 2 s.
- The origin request policy forwards viewer headers, which CloudFront does not key connections on; nothing in it opens a connection.
- There is no Origin Access Control and no SigV4 signing on the origin (the Function URL is `AuthType NONE`, guarded by `X-Cbase-Origin`; DESIGN.md says why), so nothing there is per request.
- The connect itself is 12 ms, about two round trips from JFK to us-east-1, which is TCP plus a one-round-trip TLS handshake already; CloudFront speaks HTTP/1.1 to custom origins, so there is no multiplexed connection to choose.
- Cold starts are not it: 162 inits in 21,824 invocations over 7 days (0.7%), init p50 28 ms, and none in any of the runs above.
- Origin Shield (a regional layer in us-east-1 whose connections to the origin are shared by every edge host) is the one setting aimed at this, but it bills per request, so it is not $0 (at about 25,000 API requests a month it would be cents) and whether its added hop costs more than the handshakes it saves is unmeasured; it is Alex's call.
- Provisioned concurrency is ruled out (it costs money), and it would not touch this either: the time is spent before the request reaches Lambda.
The Function URL's own share, from the reused-connection rows: the origin's first byte comes 19 ms after CloudFront sends at p50, of which about 1 ms is the function, so about 18 ms is the JFK to us-east-1 round trip plus the Function URL front end and invoke; the direct call from the Mac (30.5 ms against a 16.4 ms ping to us-east-1) puts the front end and invoke at about 14 ms.
That part has no $0 lever: API Gateway or an ALB in front would cost money, and the function's memory does not change the invoke path.
