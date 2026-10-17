# Direct DynamoDB reads from the web: measured, and not worth it

## Recommendation

Keep every web read on c_base (CloudFront -> Lambda -> DynamoDB); do not let the browser read DynamoDB directly.
The only reads that could ever go direct are public ones (the leaderboard, the bot list), and for those caching at CloudFront is about three times faster than a direct read would be and costs no DynamoDB capacity at all.
A direct game read would save roughly 17 to 20 ms at the median on one warm GetItem, but it would hand every player the whole game: every hand, the deck order and the deal seed.
If the leaderboard's 45 ms matters, give `/v1/leaderboard` a CloudFront cache behavior and an `s-maxage` (below); that measured 14 ms from here.

## The earlier experiment (Supabase, July 2026)

We asked the same question on the Supabase stack on 2026-07-08, in four commits on the `get_my_games` edge function.
`4d9e5874` (#41) logged the phases in production: JWT verify 88 ms warm / 565 ms cold, then the PostgREST fetch 370 ms warm / 753 ms cold, run one after the other.
Locally the query itself ran in 0.072 ms and a warm Postgres round trip in 0.63 ms, so the fetch was almost entirely the Kong + PostgREST HTTP layer.
That commit read over a direct Postgres connection from the edge function instead, and `10658d3b` (#42) reverted it the same day: live, `auth+fetch(7,direct)` took 1781 ms cold, worse than PostgREST's 753 ms, because every cold isolate paid a full TLS and auth handshake to Supavisor and isolates recycled about every 15 s.
The fix that stuck was `b747303d` (player_views): the server writes each player's already masked view at commit time, and the client reads its own row straight from PostgREST under RLS, which removed the edge function's ~760 ms cold floor from every read (foolish/docs/PLAYER_VIEWS.md).
So "go direct" won then for two reasons that do not hold now: the hop being skipped cost hundreds of milliseconds, and the rows read were already masked per viewer.
On c_base the hop costs about 20 ms warm, and the game row is not masked.

## How it was measured

`cbase-bench direct` (c_base/bench/direct.c), from an Apple M1 in New York on home Wi-Fi (RCN, AS6079; not a phone hotspot), on 2026-10-17 between 14:49 and 15:13 UTC.
ICMP round trip from here: 13 ms to dynamodb.us-east-1.amazonaws.com and 13 ms to api.foolish.cards, whose CloudFront POP is BOS50 (Boston).
The bench signs up an account through /v1 with the kernel's own client (netclient.h), creates a game, seats a bot and starts it, so the game read is a dealt game; it deletes the account at the end.
Each round makes one request of every kind, interleaved, after 5 warm-up rounds, all over kept-alive TLS connections (https.c's pool, the same client the Lambda uses for AWS).
The signed direct requests are the server's own calls, built by json_surface.c's `dw_*` writers and sent by `cb_aws_call`, in the order store_dynamo.c sends them; every one is a read.
The committed run is bench/results/baseline-direct-20261017T145210Z.txt; the 1-minute load average was 3.

## The numbers (ms, n = 200 each)

| read | path | p50 | p95 | p99 |
|---|---|--:|--:|--:|
| leaderboard | c_base `GET /v1/leaderboard` | 44.56 | 62.39 | 71.63 |
| my games (one dealt game) | c_base `GET /v1/games` | 43.02 | 55.66 | 67.06 |
| one game | c_base `GET /v1/games/<id>` | 40.58 | 51.87 | 70.93 |
| network floor to DynamoDB | unsigned GetItem, refused at the door | 18.70 | 21.20 | 23.58 |
| CloudFront cache hit | `GET foolish.cards/favicon.ico`, 200 of 200 hits | 13.95 | 16.95 | 20.68 |
| `GET /health` on a fresh connection (TLS setup included) | 6 at once, after 20 min idle | 86.11 | 91.34 | 91.34 |

The signed direct rows are missing from this run.
The AWS CLI's login session had expired (`aws login` needs the account owner at a browser), so the bench found no credentials and measured the unsigned floor instead.
The floor is the same request to the same endpoint over the same warm connection: DynamoDB's front end refuses it before any storage is read, so it is the network and TLS time of a direct read, without DynamoDB's own work.
DynamoDB documents single-digit millisecond service time for a GetItem; CloudWatch `SuccessfulRequestLatency` for table `cbase` would give the exact figure and also needs those credentials.
So a direct GetItem from here is about 19 + 2 to 5 ms, call it 21 to 24 ms, against 40.6 ms through c_base.
To fill the rows in: `eval "$(aws configure export-credentials --format env)"` and then `c_base/build/cbase-bench direct --url https://api.foolish.cards --n 200 --edge https://foolish.cards/favicon.ico`.

What the rows say:

- Through c_base all three reads cost about the same, 41 to 45 ms, although they make one, two and three DynamoDB calls: inside us-east-1 a call is a few milliseconds.
- One game direct would be about 17 to 20 ms faster at the median.
- The leaderboard direct is two Queries (RATINGS, then BOTS) and my games is a Query and then a GetItem per game; from a browser each one pays the full ~21 ms trip, so both come out at about 45 ms for one game, no faster than c_base, and my games gets 21 ms slower for every further game unless the client batches.
- A CloudFront hit (14 ms) is the fastest read of all, three times faster than any of them, because it never leaves the POP in Boston.
- The first AWS run (DESIGN.md, 2026-10-14) measured one GetItem behind the edge at 40.93 ms p50; today's 40.58 ms agrees.

## What decides it regardless of latency

### A game read direct from DynamoDB leaks the game

The `GAME#<id> / ROW` item holds the whole game: `state` (every hand and the deck), `logs_packed`, and `game_seed`, the secret every mid-game draw and every bot decision is seeded from.
Masking happens on the server at read time: `cb_table_view` loads the row into the kernel and `cb_section_envelope` writes the envelope for the caller's seat (src/app/table_ops.c `view_of_row`).
There is no per-viewer copy in the table to read instead; Supabase's `player_views` table was such a copy, and c_base answers the same question by masking on read.
IAM can limit which items and attributes a caller reads, but not which bytes inside the `state` blob, so no policy makes a direct game read safe.
Only public items could ever go direct: the leaderboard's RATINGS rows and the BOTS rows.

### Browser credentials need a Cognito identity pool and fine-grained IAM

A browser cannot hold our keys, so it would need a Cognito identity pool that trades a login for short-lived AWS credentials, plus an IAM role per kind of caller.
The pool itself has no charge, but it is a second identity system beside c_base's own accounts (a developer-authenticated identity, so c_base must vend tokens), and two extra calls (GetId, GetCredentialsForIdentity) at every session start.
The role must use `dynamodb:LeadingKeys` and `dynamodb:Attributes` conditions to allow only Query on `gsi2` with partition keys `RATINGS` and `BOTS`; a mistake there is a data leak, and nothing in our tests can see it.
Even a correct policy lets anyone with credentials read every rating row: the server cuts the board to the top 100 with games played, a direct client must page through every user's row, which grows with sign-ups.

### Anyone could spend the table's capacity

The table is provisioned at 15 read units, and each index at 5 (ops/cbase.yaml), 25 in all.
A strongly consistent GetItem of a game row is 1 unit, so once DynamoDB's burst credit (up to 300 s of unused capacity) is spent, 15 direct reads a second from one script throttle the reads every move makes, for every player, and 10 leaderboard Queries a second exhaust `gsi2`.
Through c_base the capacity is spent only by our code, and a rate limit or a cache can sit in front of it; with direct reads the only defence is more provisioned capacity, paid for by whoever runs the script.

### The DynamoDB API is JSON, signed with SigV4

Every request and answer is JSON in DynamoDB's typed-attribute shape, and every request needs a SigV4 signature computed in the page.
That puts a JSON encoder, a JSON reader and a signer in the web client, against the no-JSON rule and outside json_surface.c, the one place c_base's JSON is allowed to live.
Today the web speaks only netwire.h bytes, built and read by the kernel's client.

## What edge caching of public reads would give instead

The CloudFront hit above is 14 ms p50 and 21 ms p99, against 45 ms p50 and 72 ms p99 for the leaderboard through Lambda.
The leaderboard is a good fit: it is the same for everyone, `nc_req_board` sends no Authorization header, and a rating changes only when a rated game ends.
Today it cannot be cached: every path on api.foolish.cards uses the managed CachingDisabled policy (ops/cbase.yaml), and `h_board` answers `Cache-Control: no-store` (src/app/v1_http.c `octets`).
What it would take:

1. A CloudFront cache behavior for `/v1/leaderboard` with a cache policy that keys on nothing (no headers, cookies or query strings) and honours the origin's TTL.
2. `h_board` answering `Cache-Control: public, max-age=0, s-maxage=30` (or 60), so the edge holds it and the browser always asks the edge.
3. The same for `/v1/bots`, which today requires a caller (`caller()` in `h_bots`); it would first have to stop requiring one.

With a 30 s TTL a POP reads the board from Lambda at most twice a minute, so the leaderboard's DynamoDB cost stops growing with readers, the opposite of what direct reads do.
A newly rated game would show up to 30 s late; nothing else changes.

## Cold start, for reference

No cold start was caught.
`--burst 6` after 20 minutes with no traffic from here found six environments already up, with uptimes of 18 to 34 minutes: the eight-request burst of an earlier test run at 14:50 UTC had started most of them, and Lambda kept them warm far longer than the idle.
What a returning player pays, a fresh TLS connection to a warm environment, is 86 ms p50 for `/health`, about 45 ms more than a warm read on a kept connection.
The two ways to force a new environment are out of bounds here: the documented `coldstart --force-cmd` changes the function's memory size (a change to the live stack), and a burst wide enough to outrun the warm pool could meet the account's concurrency limit, which needs credentials to read.
The Lambda's `REPORT` log lines carry `Init Duration` for every cold start that happened anyway; reading them also needs the expired credentials.
A cold start costs a read through c_base and never a direct read, but it is rare at this traffic and does not change the recommendation: the game read cannot go direct at any latency.
