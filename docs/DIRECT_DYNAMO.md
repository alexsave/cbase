# Should the web's list reads go straight to DynamoDB?

## Recommendation

No: keep the list reads (my games, leaderboard, match history, bots) on c_base, CloudFront -> Lambda -> DynamoDB.
On Supabase the direct read won because the edge function it skipped cost hundreds of milliseconds; on c_base the hop it would skip costs about 10 to 20 ms warm, and two of the four lists need two DynamoDB calls, so from a browser they would come out slower, not faster.
My games cannot go direct at all: the items behind it hold every player's hand and the deal seed, and the safe alternative (a public list item the server writes at each commit) costs about 4 write units per human per move against a 5-unit index and still could not open a game.
Leaderboard and bots are public and could go direct, but only with a Cognito identity pool and IAM conditions, JSON and SigV4 in the page, and anyone able to spend the table's read capacity; caching them at CloudFront is faster (14 ms measured) and needs none of that.
If list latency matters, do that: a cache behavior and an `s-maxage` for `/v1/leaderboard` (and `/v1/bots` once it stops requiring a caller).

## The Supabase precedent (July 2026)

On Supabase the client read lists straight from the database through PostgREST, and moves went through edge functions.
The measurements behind it are in four commits of 2026-07-08 on the `get_my_games` edge function.
`4d9e5874` (#41) logged in production: JWT verify 88 ms warm / 565 ms cold, then the PostgREST fetch 370 ms warm / 753 ms cold, in sequence; the query itself ran in 0.072 ms locally.
`10658d3b` (#42) tried a direct Postgres connection from the edge function and reverted it the same day: 1781 ms cold against PostgREST's 753 ms, because each cold isolate (recycled about every 15 s) paid a TLS and auth handshake to Supavisor.
`b747303d` then made the server write each player's already masked view (`player_views`) at commit time, and the client read its own rows straight from PostgREST under RLS, which removed the edge function's ~760 ms cold floor (foolish/docs/PLAYER_VIEWS.md); `c3077229` (#51) did the same for the single game screen with `spectator_views`.
Two things made that right, and neither holds on c_base: the skipped hop cost hundreds of milliseconds, and the rows read were already masked per viewer, paid for by Postgres writes that cost nothing extra.

## How it was measured

`cbase-bench direct` (c_base/bench/direct.c), from an Apple M1 in New York on home Wi-Fi (RCN, AS6079; not a phone hotspot); ICMP is 13 ms to both dynamodb.us-east-1.amazonaws.com and api.foolish.cards, whose CloudFront POP is BOS50 (Boston).
It signs up a throwaway account through /v1 with the kernel's own client (netclient.h), creates a game, seats a bot and starts it, then times each read once per round, interleaved, 200 rounds after 5 warm-up rounds, over kept-alive TLS (https.c, the client the Lambda itself uses); it deletes the account at the end.
The direct side sends the server's own DynamoDB calls, built by json_surface.c's `dw_*` writers, in store_dynamo.c's order; all are reads.
The AWS CLI's login session had expired (`aws login` needs the account owner at a browser), so the signed rows are missing; the bench measured instead the same GetItem sent unsigned, which DynamoDB's front end refuses before touching storage: the network and TLS cost of any direct call.
DynamoDB's own service time for these reads is single-digit milliseconds, so a direct call from here is the floor plus about 2 to 5 ms.
To fill in the signed rows: `eval "$(aws configure export-credentials --format env)"`, then `c_base/build/cbase-bench direct --url https://api.foolish.cards --n 200 --edge https://foolish.cards/favicon.ico`.

## The numbers (ms, n = 200 per row)

Two runs, 2026-10-17, saved as bench/results/baseline-direct-*.txt; load average 3 for both.
The second run was faster through c_base by about 13 ms on every row while the floor and the edge did not move, so the difference lies on the CloudFront-to-Lambda leg and is the network's, not ours.

| read | calls behind it | run 1 14:52 UTC p50 / p95 / p99 | run 2 15:25 UTC p50 / p95 / p99 | direct, estimated p50 |
|---|---|--:|--:|--:|
| my games `GET /v1/games` (one dealt game) | Query GSI1 + GetItem per game | 43.0 / 55.7 / 67.1 | 29.7 / 38.3 / 43.6 | 40 to 45 (two trips) |
| leaderboard `GET /v1/leaderboard` | Query RATINGS + Query BOTS | 44.6 / 62.4 / 71.6 | 31.7 / 40.7 / 56.4 | 40 to 45, or 20 to 23 if the two run in parallel |
| match history `GET /v1/history` | Query RATINGS + Query PLAYER# | | 30.7 / 41.9 / 50.3 | 40 to 45, or 20 to 23 in parallel |
| bots `GET /v1/bots` | Query BOTS | | 30.2 / 38.2 / 48.2 | 20 to 23 |
| one game `GET /v1/games/<id>`, for scale | GetItem | 40.6 / 51.9 / 70.9 | 27.6 / 39.7 / 49.0 | 20 to 23 |
| DynamoDB floor (unsigned GetItem) | none | 18.7 / 21.2 / 23.6 | 17.4 / 21.2 / 23.3 | |
| CloudFront hit (`foolish.cards/favicon.ico`, 400 of 400 hits) | none | 14.0 / 17.0 / 20.7 | 13.9 / 17.0 / 19.2 | |

The bench account had no finished games, so match history read an empty PLAYER# partition; a real history pages through every replay the player has, each carrying its moves.
Through c_base every list costs about the same whatever the number of DynamoDB calls, because inside us-east-1 a call is a few milliseconds; from a browser in New York each call is a full ~20 ms trip.
So the best a direct list read could save is about 10 ms (bots, or a two-call list run in parallel), and my games, which must read the member index before it knows which games to read, gets slower.
A CloudFront hit beats every one of them by 15 to 30 ms.

## Each list, read directly

### My games

The dashboard's list answers each game as the caller's own masked envelope: the web reads every row into a full table view (`readEnvelope` in contexts/ServerContext.tsx), with the caller's hand, and uses it as the game cache.
The `GAME#<id> / MEMBER#<user>` items the list is found by hold only `game_id`, `user_id`, `updated_at` and `joined_at`, so a direct client must then read `GAME#<id> / ROW`.
That item is the whole game: `state` (every hand and the deck), `logs_packed`, and `game_seed`, the secret every later draw and bot decision is seeded from.
Masking happens only in the kernel at read time (`view_of_row` in src/app/table_ops.c), and IAM cannot hide bytes inside an attribute, so no policy makes that read safe.

A public list item would: one item per human seat, written by the server inside the commit, carrying only public fields (status, players, whose turn, card counts, version).
Its price is written down in `dw_game_commit` (src/json_surface.c), which stopped rewriting the seat item on every commit for exactly this cost: a transactional write of a small item is 2 WCU, and moving its GSI1 sort key (to keep the list newest first) is 2 more on the index.
So a two-human game would pay about 4 table WCU and 4 GSI1 WCU per move on top of the commit, and GSI1 is provisioned at 5 WCU (ops/cbase.yaml): about one move a second across every game before index throttling pushes back on the commits themselves.
A single public item per game (`GAME#<id> / PUBLIC`, no index) is 2 WCU per commit, but the list then needs the member Query and a BatchGetItem, two trips from the browser, slower than c_base.
And either way the rows carry no hand, so opening a game from the dashboard would cost one more read through c_base, which today the list already answered.

### Leaderboard

The RATINGS items (`user_id`, `username`, `elo_rating`, `previous_elo`, `games_played`, `updated_at`) and BOT items are public; /v1/leaderboard already sends the ids, names and ratings.
But the server reads every rating row, drops players with no games, sorts and keeps the top 100 of each kind; a direct client must page through every row itself, a download and a read cost that grow with every sign-up.

### Match history

The `SNAP#<id> / PLAYER#<user>` items are the player's own copies of finished games: what /v1/history returns, with no hidden state left.
The danger is access, not content: limiting a browser to its own partition needs `dynamodb:LeadingKeys` to equal `PLAYER#<id>`, where `<id>` is c_base's user id, and a Cognito identity pool's policy variable is Cognito's identity id; without re-keying the table, any credentialed browser could read anyone's history.
The history header also needs the caller's own rating, which the server finds by reading every RATINGS row, as above.

### Bots

`GSI2 BOTS` is one Query of public rows, the one list that would be a clean direct read: about 20 to 23 ms against 30 ms through c_base, or 14 ms from the edge.

## What decides it beyond latency

Browser credentials need a Cognito identity pool (developer-authenticated, so c_base vends its tokens) and an IAM role whose `dynamodb:LeadingKeys` and `dynamodb:Attributes` conditions admit exactly the public partitions; the pool is free, but it is a second identity system, two extra calls at session start, and a policy whose mistakes leak data without failing a test.
The table is provisioned at 15 read units and each index at 5, 25 in all; once DynamoDB's burst credit (up to 300 s of unused capacity) is spent, ten leaderboard Queries a second from one script exhaust `gsi2`, and with direct reads nothing of ours sits in front of the table to rate-limit or cache.
The DynamoDB API is JSON in its typed-attribute shape and every request is SigV4-signed, so the page would carry a JSON encoder, a JSON reader and a signer, against the no-JSON rule and outside json_surface.c, the one place c_base allows JSON; today the web speaks only netwire.h bytes, built and read by the kernel's client.

## What edge caching of the public lists would take

Today nothing on api.foolish.cards is cacheable: every path uses the managed CachingDisabled policy (ops/cbase.yaml) and every /v1 answer says `Cache-Control: no-store` (`octets` in src/app/v1_http.c).

1. A CloudFront cache behavior for `/v1/leaderboard` whose cache policy keys on nothing (no headers, cookies or query strings) and honours the origin's TTL; `nc_req_board` already sends no Authorization header.
2. `h_board` answering `Cache-Control: public, max-age=0, s-maxage=30`, so the POP keeps it and browsers always ask the POP.
3. The same for `/v1/bots`, after `h_bots` stops calling `caller()`.

A hit measured 14 ms p50 and 19 to 21 ms p99, against 32 to 45 ms p50 for the leaderboard through Lambda, and with a 30 s TTL each POP reads the board from Lambda at most twice a minute, so its read cost stops growing with readers.
A newly rated game shows up to 30 s late.
My games and match history are per caller and stay uncached.

## Cold start, for reference

No cold start was caught: `--burst 6` after 20 minutes with no traffic from here found six environments already up for 18 to 34 minutes (an earlier eight-request test burst had started most of them).
A fresh TLS connection to a warm environment, what a returning player pays, was 86 ms p50 for `/health`.
Forcing new environments was out of bounds: `coldstart --force-cmd` changes the function's memory size (a live-stack change), and the Lambda's `Init Duration` log lines need the expired credentials.
A cold start costs a read through c_base and never a direct one, but at this traffic it is rare, and it does not change the answer for my games, which cannot go direct at any latency.
