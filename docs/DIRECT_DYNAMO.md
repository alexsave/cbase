# Should the web's list reads go straight to DynamoDB?

> **Since then (2026-10-17):** the recommendation landed.
> The list reads stayed on c_base, and `/v1/leaderboard` and `/v1/bots` are cached at CloudFront and invalidated by event (DESIGN.md, "Edge cache").

## Recommendation

No: keep the list reads (my games, leaderboard, match history, bots) on c_base, CloudFront -> Lambda -> DynamoDB, even though a direct read is faster.
Measured signed from New York, a direct read beats c_base by 11 to 27 ms at the median: bots 16.5 against 42.9 ms, my games 27.6 against 42.6, history 29.5 against 43.7, leaderboard 33.3 against 44.8.
My games still cannot go direct: the items behind it hold every player's hand and the deal seed, and the safe alternative (a public list item the server writes at each commit) costs about 4 write units per human per move against a 5-unit index and still could not open a game.
Match history could go direct only after re-keying the table to Cognito identity ids, or any browser could read anyone's history.
Leaderboard and bots are public, but a direct read needs a Cognito identity pool, JSON and SigV4 in the page, and lets any client spend the table's read capacity: this bench, one client on one connection, throttled `gsi2` 73 times while measuring.
A CloudFront cache hit measured 14.5 ms, as fast as the fastest direct read, so cache `/v1/leaderboard` (and `/v1/bots` once it stops requiring a caller) at the edge instead.
Separately, `/v1/leaderboard` and `/v1/history` each read every RATINGS row on every request (about 4.5 read units); that is what throttled, and `h_history` needs only the caller's own rating row.

## The Supabase precedent (July 2026)

On Supabase the client read lists straight from the database through PostgREST, and moves went through edge functions.
The measurements behind it are in four commits of 2026-07-08 on the `get_my_games` edge function.
`4d9e5874` (#41) logged in production: JWT verify 88 ms warm / 565 ms cold, then the PostgREST fetch 370 ms warm / 753 ms cold, in sequence; the query itself ran in 0.072 ms locally.
`10658d3b` (#42) tried a direct Postgres connection from the edge function and reverted it the same day: 1781 ms cold against PostgREST's 753 ms, because each cold isolate (recycled about every 15 s) paid a TLS and auth handshake to Supavisor.
`b747303d` then made the server write each player's already masked view (`player_views`) at commit time, and the client read its own rows straight from PostgREST under RLS, which removed the edge function's ~760 ms cold floor (foolish/docs/PLAYER_VIEWS.md); `c3077229` (#51) did the same for the single game screen with `spectator_views`.
Two things made that right, and neither holds on c_base: the skipped hop cost hundreds of milliseconds, not tens, and the rows read were already masked per viewer, paid for by Postgres writes that cost nothing extra.

## How it was measured

`cbase-bench direct` (c_base/bench/direct.c), from an Apple M1 in New York on home Wi-Fi (RCN, AS6079; not a phone hotspot); ICMP is 13 ms to both dynamodb.us-east-1.amazonaws.com and api.foolish.cards, whose CloudFront POP is BOS50 (Boston).
It signs up a throwaway account through /v1 with the kernel's own client (netclient.h), creates a game, seats a bot and starts it, then times each read once per round, interleaved, 200 rounds after 5 warm-up rounds, over kept-alive TLS (https.c, the client the Lambda itself uses); it deletes the account at the end.
The direct side sends the server's own DynamoDB calls, built by json_surface.c's `dw_*` writers, in store_dynamo.c's order, signed with `aws configure export-credentials`; all are reads.
It also sends the GetItem unsigned; DynamoDB refuses it, and that refusal (16.4 ms) turned out slower than the signed read (13.5 ms), so it is no floor, and the estimates an earlier version of this page drew from it were too high.
DynamoDB's own time, from CloudWatch `SuccessfulRequestLatency` for table `cbase` over the runs: GetItem 1.0 to 1.2 ms, Query 1.8 ms on average.

## The numbers (ms, n = 200 per row)

The signed run, 2026-10-17 15:38 UTC, load average 4: bench/results/baseline-direct-20261017T153830Z.txt.

| read | through c_base p50 / p95 / p99 | direct p50 / p95 / p99 | direct calls, bytes received |
|---|--:|--:|---|
| my games (one dealt game) | 42.6 / 64.7 / 72.4 | 27.6 / 32.6 / 37.0 | Query GSI1 + GetItem per game, 2.7 KB |
| leaderboard | 44.8 / 65.4 / 83.0 | 33.3 / 41.2 / 47.9 | Query RATINGS + Query BOTS, 34.3 KB (c_base answers 1.8 KB) |
| match history (no finished games) | 43.7 / 56.6 / 76.6 | 29.5 / 34.7 / 36.7 | Query RATINGS + Query PLAYER#, 14.2 KB |
| bots | 42.9 / 65.5 / 1381.7 | 16.5 / 20.2 / 24.5 | Query BOTS, 20.2 KB (c_base answers 3.4 KB) |
| one game, for scale | 40.6 / 51.1 / 67.4 | 13.5 / 16.7 / 18.4 | GetItem, 2.3 KB |
| CloudFront hit (`foolish.cards/favicon.ico`, 200 of 200 hits) | 14.5 / 17.2 / 19.8 | | |

Through c_base every read costs about the same whatever the number of DynamoDB calls, because inside us-east-1 a call is a few milliseconds; direct, each call is a full 13 to 17 ms trip from New York, so the two-call lists save the least.
The bots p99 of 1.4 s and four 500s from c_base (one leaderboard, three history) were the throttling below: c_base backed off and retried, and gave up on four.
Two earlier unsigned runs (14:52 and 15:25 UTC, also committed) put c_base at 41 to 45 ms and 28 to 32 ms p50 for the same reads while the edge stayed at 14 ms, so the c_base side moves by about 13 ms with the CloudFront-to-Lambda leg from run to run.

### The bench throttled the live index

`gsi2` is provisioned at 5 read units; every round read every RATINGS row four times (c_base's leaderboard and history, and their direct twins), about 38 units a second.
CloudWatch counted 6 + 3 throttled reads at 15:24 to 15:25 UTC (the unsigned run, c_base's reads only) and 64 at 15:38 (the signed run), after DynamoDB's burst credit ran out; the table itself never throttled.
So one client on one connection was enough to make the leaderboard and history fail for everyone for about a minute: through c_base the fix is ours to make (read the caller's own rating with one GetItem, cache the board), direct it would be anyone's to cause.
Do not run `direct` at n=200 again without that fix or a pause between rounds.

## Each list, read directly

### My games

The dashboard's list answers each game as the caller's own masked envelope: the web reads every row into a full table view (`readEnvelope` in contexts/ServerContext.tsx), with the caller's hand, and uses it as the game cache.
The `GAME#<id> / MEMBER#<user>` items the list is found by hold only `game_id`, `user_id`, `updated_at` and `joined_at`, so a direct client must then read `GAME#<id> / ROW`.
That item is the whole game: `state` (every hand and the deck), `logs_packed`, and `game_seed`, the secret every later draw and bot decision is seeded from.
Masking happens only in the kernel at read time (`view_of_row` in src/app/table_ops.c), and IAM cannot hide bytes inside an attribute, so no policy makes that read safe.

A public list item would: one item per human seat, written by the server inside the commit, carrying only public fields (status, players, whose turn, card counts, version).
Its price is written down in `dw_game_commit` (src/json_surface.c), which stopped rewriting the seat item on every commit for exactly this cost: a transactional write of a small item is 2 WCU, and moving its GSI1 sort key (to keep the list newest first) is 2 more on the index.
So a two-human game would pay about 4 table WCU and 4 GSI1 WCU per move on top of the commit, and GSI1 is provisioned at 5 WCU (ops/cbase.yaml): about one move a second across every game before index throttling pushes back on the commits themselves.
A single public item per game (`GAME#<id> / PUBLIC`, no index) is 2 WCU per commit, but the list then needs the member Query and a BatchGetItem, two trips, about the 27.6 ms measured above.
And either way the rows carry no hand, so opening a game from the dashboard would cost one more read through c_base, which today the list already answered.

### Leaderboard

The RATINGS items (`user_id`, `username`, `elo_rating`, `previous_elo`, `games_played`, `updated_at`) and BOT items are public; /v1/leaderboard already sends the ids, names and ratings.
But the server reads every row, drops players with no games, sorts and keeps the top 100 of each kind, and answers 1.8 KB; a direct client downloads all of it, 34 KB today, growing with every sign-up and every unit of it billed to our capacity.

### Match history

The `SNAP#<id> / PLAYER#<user>` items are the player's own copies of finished games: what /v1/history returns, with no hidden state left.
The danger is access, not content: limiting a browser to its own partition needs `dynamodb:LeadingKeys` to equal `PLAYER#<id>`, where `<id>` is c_base's user id, and a Cognito identity pool's policy variable is Cognito's identity id; without re-keying the table, any credentialed browser could read anyone's history.

### Bots

`GSI2 BOTS` is one Query of public rows, the cleanest direct read: 16.5 ms against 42.9 through c_base, the same as the 14.5 ms edge hit, but at 20 KB a call against c_base's 3.4 KB.

## What decides it beyond latency

Browser credentials need a Cognito identity pool (developer-authenticated, so c_base vends its tokens) and an IAM role whose `dynamodb:LeadingKeys` and `dynamodb:Attributes` conditions admit exactly the public partitions; the pool is free, but it is a second identity system, two extra calls at session start, and a policy whose mistakes leak data without failing a test.
The table is provisioned at 15 read units and each index at 5, 25 in all; the throttling above is what direct reads would let any script do on purpose, with nothing of ours in front of the table to rate-limit or cache.
The DynamoDB API is JSON in its typed-attribute shape and every request is SigV4-signed, so the page would carry a JSON encoder, a JSON reader and a signer, against the no-JSON rule and outside json_surface.c, the one place c_base allows JSON; today the web speaks only netwire.h bytes, built and read by the kernel's client.

## What edge caching of the public lists would take

Today nothing on api.foolish.cards is cacheable: every path uses the managed CachingDisabled policy (ops/cbase.yaml) and every /v1 answer says `Cache-Control: no-store` (`octets` in src/app/v1_http.c).

1. A CloudFront cache behavior for `/v1/leaderboard` whose cache policy keys on nothing (no headers, cookies or query strings) and honours the origin's TTL; `nc_req_board` already sends no Authorization header.
2. `h_board` answering `Cache-Control: public, max-age=0, s-maxage=30`, so the POP keeps it and browsers always ask the POP.
3. The same for `/v1/bots`, after `h_bots` stops calling `caller()`.

A hit measured 14 to 14.5 ms p50 and about 20 ms p99 in all three runs, as fast as the fastest direct read, and with a 30 s TTL each POP reads the board from Lambda at most twice a minute, so its read cost stops growing with readers.
A newly rated game shows up to 30 s late.
My games and match history are per caller and stay uncached; their 15 ms gap to a direct read is the price of masking and access control living in one place.

## Cold start, for reference

No cold start was caught: `--burst 6` after 20 minutes with no traffic from here found six environments already up for 18 to 34 minutes (an earlier eight-request test burst had started most of them).
A fresh TLS connection to a warm environment, what a returning player pays, was 86 ms p50 for `/health`.
Forcing new environments was out of bounds: `coldstart --force-cmd` changes the function's memory size, a change to the live stack.
A cold start costs a read through c_base and never a direct one, but at this traffic it is rare, and it does not change the answer for my games, which cannot go direct at any latency.
