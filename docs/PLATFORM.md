# c_base as a platform: splitting the general part from foolish

Designed on origin/main at b98c630b and a66076da (2026-10-17); refreshed on 56a9fe87 (2026-10-18) for what changed in between (section 0.1).
The goal is Alex's: a C backend platform (database, client SDK, AWS plumbing, and a packager that turns C into a bare Lambda) that foolish is one product of.
Alex's decisions are recorded at the end of sections 7 and 8; section 8.6 is the plan that is executed, one pull request per stage.

## 0. The headline, in five lines

About two thirds of c_base's C source is already product-free in substance, but only a third of it is free by file: the foolish half is threaded through the mixed files rather than fenced off.
The single most valuable and most unusual piece is the packager, the minimal C AWS client and the fake Runtime API harness together: "one C file with a handler table, one command, a tested Lambda with a URL" exists nowhere else in this shape.
Nothing outside foolish uses c_base today (uttt, werewolf, pickemup, chuiniu and tallybones have no server), so an example product inside c_base is the only honest second user until one of them needs a backend.
The live data constraint is narrower than it looks: SQLite has no production data, so the local backend can be rebuilt freely, and the DynamoDB key and attribute names are the only thing that must not move.
Every existing literal can stay as-is by making it an instance of the general form (`bot_drive:<id>` is `<job>:<key>`, `scan` is a schedule name, `u/<user>/v`, `u/<user>/c/<game>`, `s/<game>` and `u/<user>/m/<game>` are topic families, netwire kind numbers are frozen in place).

### 0.1 What changed since the design was written (2026-10-17 to 2026-10-18)

- Realtime is AWS IoT Core over MQTT 3.1.1 (pull requests #353, #361, #362), and the API Gateway WebSocket API is deleted.
  `rt_apigw.c`, `apigw_mgmt.c`, the CONN and TOPIC items, `stream.c` and the nw1 stream frames are gone.
  The stream is `realtime/realtime.c` (targets and their MQTT topics, the publish batch), `realtime/stream_mqtt.c` (the policy: who may connect and subscribe, and the hub's side of a connection), `realtime/rt_iot.c` (Lambda: one IoT Data publish per target over the HTTPS pool, `AWS_ENDPOINT_URL_IOT_DATA_PLANE` for test stand-ins, and the custom authorizer's answer) and `realtime/rt_local.c` (`cbase serve`: the in-process MQTT hub at `/mqtt`).
  The fuzz targets are `fz_mqtt`, `fz_mqtt_hub` and `fz_iot_auth`.
- `cb_rt_transport` is `deliver(to)`, `deliver_lanes(to[], frames, lanes)` and `forget(user)`; `cb_rt.also` is gone.
  Pull request #372 made a commit's pushes parallel lanes, one per user plus one for the spectators, each user's view before their cast.
- SQLite schema 4 adds `game_members`; the platform rows now include `sessions` and `signin_fails` (DynamoDB `SESSION#`, `AUTHFAIL#`, `USERREFRESH#`).
- Pull request #372 made a mid-game commit one conditional UpdateItem when the bot-scan marker does not change; transactions remain for create, join, finish and a marker change.
  DESIGN.md's Store section holds the CAS argument, and the item layer (stages 5 and 6) must keep that single-item path, so it offers a conditional update of one item beside the transaction.
- Pull request #370 added `CBASE_BOT_SPEEDUP` and the board's time (`cb_bots_ms`, `cb_bots_board_now`), and the test lanes run every program in parallel (`RUN_ALL`).
- Pull request #373 added `shared/c/num_parse.h`, and `test/unit_num_gate.c` forbids `strto*`, `atoi` and `scanf` in `c_base/src` except `strtod` in `json_surface.c`.
- Pull request #371 gave the kernel netclient its own-work pool (`NC_OWN_MAX`) and `ASK_WHY_BUSY` for a full pump.
- Pull request #368 added `ops/role_covers_template.sh`: the GitHub OIDC deploy role must cover every resource type the template GetAtts, and the generated template of stage 8 must keep that gate passing.
- Pull request #364 queues deploys in a workflow-level concurrency group, and `require_main.sh` refuses re-runs and trees behind main.
- Secrets come from SSM Parameter Store at container start (`aws/ssm.c`, `CBASE_SECRETS_SSM`), and the template now has an SNS alarm topic and ten CloudWatch alarms, the origin secret header, and reserved concurrency.
- THE SHARED RULE (Alex, 2026-10-18): c_base is a consumer of `shared/` like the apps.
  A helper used by more than one of {apps, c_base} lives in `shared/c`, never in `c_base/src/util`; pull requests #376 and #377 moved base32, base64 and hex there.
  c_base-only platform code (HTTP, the store, AWS, the contained JSON) stays in c_base, and `core/` must not grow general helpers that belong in `shared/c`.
- JSON CONTAINMENT (Alex): every JSON shape and every JSON reader or writer call lives in one file, `json_surface.c` over `util/json.c`, enforced by `unit_json_gate.c`.
  Stage 7 splits it into `core_json.c` and `aws_json.c`, one file per side, still gated, still no JSON anywhere else.

## 1. Inventory

Counts are physical lines of every tracked file under `c_base/` (44,771 in all), by file, then the mixed source split by section.

### 1.1 Totals by class

| class | files | lines | of which src | test | bench | ops | other (docs, sql, html, tools, migrate) |
|---|---|---|---|---|---|---|---|
| platform | 89 | 11,082 | 6,377 | 3,392 | 622 | 660 | 31 |
| foolish | 46 | 11,516 | 2,853 | 5,715 | 1,574 | 948 | 426 |
| mixed | 68 | 22,173 | 10,975 | 5,877 | 700 | 957 | 3,664 |

The mixed source (10,975 lines), split by section as read: about 7,270 platform and 3,680 foolish.
So `c_base/src` as a whole is about 13,650 lines of platform and 6,530 lines of foolish: two thirds general.
The tests follow the source: about 3,400 platform, 5,700 foolish and 5,900 mixed (the store, DynamoDB, stream and admin tests exercise both halves).

### 1.2 Platform (product-free in substance)

- `src/util/*`: err, str, clock, log, json, pool, uuid (base64 and hex moved to `shared/c` in #377), about 1,300 lines.
- `src/net/*` (loop, http, ws, router, server), 1,825 lines; only comments name `/v1/games/*` as an example.
- `src/aws/{https,sigv4,aws_call,iot_data,lambda_invoke,cloudfront,runtime_api,ssm}.[ch]`, about 1,850 lines: the minimal C AWS client.
- `src/auth/{bcrypt,jwt,passwords}.[ch]` and most of `auth.c` and `names.c`.
- `src/realtime/stream_mqtt.[ch]` (the policy is two prefixes, `u/<user>/` and `s/`, which are product-free), `rt_local.[ch]` (the MQTT hub), `rt_iot.[ch]` (IoT publish and the authorizer); all carry opaque bytes to a topic string.
- `src/app/limits.[ch]` (token buckets per viewer address or account) and `src/app/after.[ch]`'s window, though both live under `app/` today.
- `tools/embed.c`, `tools/cov_gate.c`; `ops/{package,lambda_deploy,dynamo_local,ops_env,require_main,role_covers_template}.sh`, `ops/dynamo_table.py`, `ops/changeset_replacements.jq`, `ops/test/{changeset_gate_test,ops_env_test,role_covers_template_test}.sh`.
- `bench/{bench_stats,bench.h,coldstart,spaced,mqtt,unit_bench}`.
- Tests: `t.h`, `oom.h`, `lib_proc`, `lib_aws_tls`, `lib_client`, `lib_auth_fakestore`, `lib_faultstore`, `lib_store_backend`, `lib_rt_token`, `fixture_cloudfront_event.h`, and `unit_{util,http,net,ws,https,sigv4,jwt,bcrypt,runtime_api,proc,json_gate,num,num_gate,mqtt,stream_mqtt,rt_iot,rt_local,names,limits,oom,server_edges,config}.c`.
- Fuzz: `fz_{http,ws,json,jwt,bcrypt,num,https_response,event,iot_auth,mqtt,mqtt_hub,query}.c`.

### 1.3 Foolish (product in substance)

- `src/app/{table_ops,bots,fanout,functions_http,unfurl_http}.[ch]`, `src/app/app_internal.h` (the kernel section, CAS runner, row cache, bot lease/drive/scan, fan-out, create/action/meta/account-delete routes, the `/m` unfurl).
- `src/store/store_import.[ch]` (the Supabase CSV import), `migrate/*`, `tools/unfurl_payloads.c`.
- `ops/web.yaml`, `ops/web_deploy.sh`, `ops/test/web_*` (the foolish.cards and uttt.live static sites; general in mechanism, but every value is a product's).
- `bench/{cpu,direct,roundtrip,subscribe}.c` and `bench/results/`.
- Tests: `unit_{app,v1,netclient,netwire,unfurl,bots_kick,kernel_threads,import,after,edges,leftovers}.c`, `lib_app_store_mem.[ch]`, `fixture_unfurl.h`, `smoke.c` (the game smoke), `smoke_matrix.c`.
- Fuzz: `fz_{table,table_wire,client_table,codec,netwire,netclient,sequence,unfurl,import}.c`.

### 1.4 Mixed files, and the foolish identifiers inside each

- `src/store/store.h` (526): the row structs `cb_game_row`, `cb_game_commit` (seats, `rated_ids`, `rating_delta`, `rated_names`), `cb_member_row`, `cb_bot_row` (`nickname`, `strategy_key`), `cb_rating_row` (`elo_rating`, `previous_elo`, `games_played`), `cb_snapshot_row` (`moves`, `extras`), `cb_chat_row`; the ops `game_*`, `games_of_user`, `games_get_views`, `is_member`, `game_members`, `games_needing_bots`, `lease_*`, `bots_idle`, `bots_list`, `ratings_*`, `rating_set_username`, `snapshot_*`, `chat_*`, `bots_replace`, `game_import`, `rating_import`, `rating_reindex`; constants `CB_GAME_ID_MAX`, `CB_SEED_LEN` (== kernel `FOOLISH_SEED_LEN`), `CB_MAX_SEATS` (== `MAX_PLAYERS`), `CB_USERNAME_MAX` (== `ROSTER_NAME_MAX`), `CB_ADMIN_GAMES`; `user_create` also creates the rating row.
  Platform: users, sessions, refresh tokens, sign-in failures, admin, tally, throttling, deadline, the side thread.
- `src/store/store_dynamo.c` (1,641) and `store_sqlite.c` (1,820): the platform rows (users, sessions, refresh, sign-in failures, admin, open, paging, backoff) beside the foolish ones (games, seats, the SCAN marker, the lease, bots, ratings, snapshots, chat, the import), about half each.
- `src/store/store.c`, `store_priv.h`: the bot ladder order through `bot_roster_find` (kernel), the leaderboard sort; user search is platform.
- `sql/schema.sql` (schema 4): platform `users`, `sessions`, `refresh_tokens`, `signin_fails`; foolish `bots` (and the 49 seeded with UUIDv5 ids), `games`, `game_members`, `game_bots`, `ratings`, `game_snapshots`, `chat_messages`.
- `src/json_surface.c` (3,561) and `.h` (625): the DynamoDB item layout lists `USER#`, `EMAIL#`, `REFRESH#`, `USERREFRESH#`, `SESSION#`, `AUTHFAIL#` (platform) beside `GAME#`/`ROW`, `MEMBER#`, `BOT#`, `SNAP#`/`PLAYER#`, `CHAT#`, `SCAN` and GSI keys `USERGAMES#`, `GAMESNAPS#`, `USERCHAT#`, `RATINGS`/`E#<rank>`, `BOTS` (foolish); the per-row encoders; the admin `games`/`game`/`bots` JSON; the bench's sample event (origin `https://foolish.cards`). The Lambda envelope, the IoT authorizer request and answer, the runtime error, JWT, CloudFront XML, SSM and the generic DynamoDB expression layer are platform.
- `src/realtime/realtime.[ch]`: the transport vtable, `cb_rt_topic`'s level check and the batch are platform; the four families (`CB_RT_VIEWS` `u/<user>/v`, `CB_RT_SEAT` `u/<user>/c/<game>`, `CB_RT_WATCH` `s/<game>`, `CB_RT_CHAT` `u/<user>/m/<game>`), `rt_publish_view`/`_cast`/`_chat` and `cb_chat_to_nw` are foolish, and the header includes `app/table_ops.h` and `netwire.h`.
- `src/realtime/rt_deps.h`: the test seam is platform; it includes `app/table_ops.h`.
- `src/app/v1_http.[ch]` (654): `/v1/auth/{signup,signin,refresh,signout,password}` are platform; `/v1/games`, `/v1/games/*`, `/v1/chat/*`, `/v1/bots`, `/v1/leaderboard`, `/v1/history`, `/v1/replay/*` are foolish, and so is `ACCOUNT_DOMAIN "foolish.cards"` (the username-to-email derivation).
- `src/app/after.[ch]`: the answer-first window is platform; its queue holds a `cb_fanout` (seats, envelopes, casts) and a bot kick, which are foolish.
- `src/app/edge.[ch]`: the mark-then-flush invalidation is platform; `CB_EDGE_BOARD` = `/v1/leaderboard` and `CB_EDGE_BOTS` = `/v1/bots` are foolish.
- `src/admin/admin.[ch]` (817) and `admin/console.html`: overview, collections, browse, query, users, logs, config and backups are platform; `games`, `game` (roster decode, spectator envelope), `bots`, the overview's `games_needing_bots`, and the delete path through `cb_app_delete_account` are foolish.
- `src/auth/auth.[ch]`, `names.[ch]`: the `%` refusal in usernames (bot nicknames) and the `@foolish.cards` comment.
- `src/boot.[ch]`: `cb_kernel_selftest` (`table_create`, `table_commit_products`, the 7-byte action response); the choice of `cb_bots_kick_lambda`/`_local`; the mode switch that picks the store, the transport and the kick.
- `src/ctx.h`: `cb_kick_fn` with `game_id`, `bot_pool`, `CB_MODE_LAMBDA`.
- `src/config.[ch]`: `CBASE_BOT_WORKERS`, `CBASE_BOT_SPEEDUP`, `CBASE_RATED_MIN_AGE`, `api.foolish.cards` in a comment; `table`, `iot_endpoint`, `cf_distribution`, `secrets_ssm` are AWS's.
- `src/handler.[ch]`: the fixed module list (`cb_functions_routes`, `cb_v1_routes`, `cb_admin_routes`, `cb_unfurl_routes`, under serve `cb_rt_local_routes`).
- `src/main_lambda.c`: `CB_EVENT_SCHEDULE` runs `cb_bots_scan`, `CB_EVENT_BOT_DRIVE` runs `cb_bots_drive`, `CB_EVENT_IOT_AUTH` the authorizer.
- `src/main_serve.c`: the scan timer and the bot pool.
- `src/aws/event.[ch]`, `lambda_invoke.[ch]`: `CB_EVENT_BOT_DRIVE`, `game_id[32]`, the `"bot_drive:<id>"` and `"scan"` literals.
- `Makefile`: the kernel's `print-kernel` and `print-human-caps`, `KERNEL_CFLAGS`, kernel objects linked into every program; `tools/build_bootstrap.sh` copies `foolish/c`.
- `ops/cbase.yaml`: `DomainName` default `api.foolish.cards`, the two cached paths, the `${FunctionName}-bot-scan` schedule, the IoT authorizer's name; the rest (table, function, URL, IoT authorizer, CloudFront, alarms) is general.
- `ops/deploy.sh`: `cbase-import --seed-bots` and `--reindex-ratings` after every deploy, `api.foolish.cards`.
- `ops/github_oidc.yaml`: the repo `alexsave/foolish`, and the web stack's rights beside the Lambda's.
- `bench/main.c`, `bench/net.[ch]`: the generic harness dispatches to foolish scenarios.
- Tests: `unit_store`, `unit_store_faults`, `unit_dynamo`, `unit_dynamo_ops` (also the free-tier template check, which is platform), `unit_admin`, `unit_admin_edges`, `unit_auth`, `unit_event`, `unit_aws_calls`, `unit_stream`, `lib_admin_fakestore`, `smoke_lambda`, `smoke_lambda_after`, `smoke_authz`, `smoke_boot`, `bench_dynamo`.
- Docs: `DESIGN.md`, `RUNBOOK.md`, `REPORT.md`, `README.md`, `docs/*`.

### 1.5 Kernel and shared files c_base touches

- Mixed: `foolish/c/src/netwire.[ch]` (814 lines) and `netclient.[ch]` (1,819 lines); see section 2.6.
- Foolish: `table.h`, `game.h`, `ask.h` (the 10-byte `AskRefusal`, which c_base uses as its error body everywhere), `roster.h`, `bot_roster.h`, `replay.h`, `replay_extras.h`, `msg_wire.h`, `awire.h`, `legal.h`, `client_table.h`, `view.h`, `evwire.h`, `cordite_sim.h`, `thread_heap.h`; `foolish/c/Makefile`'s `print-core` and `print-human-caps`; `foolish/c/wasm/wasm_net_api.c` (the web's netclient bridge).
- Shared: `shared/c/sha256.h` (the username email), `shared/tools/c_rules.mk`.

## 2. The platform API a product implements

The rule, from ARCHITECTURE_AS_A_PATTERN.md: the shell knows column names, an RPC name and a topic format, never a seat.
Here the platform is the shell and the product is the Core plus a thin shell of its own.
A product is ONE const struct, `cb_app`, mostly data tables; function pointers appear only where the product's kernel has to run.

```c
// cbase/app.h (platform)
typedef struct {
    const char *name;                      // "foolish": the stack, the log tag, the key namespace
    const cb_route        *routes;   int n_routes;      // method, pattern, flags, handler
    const cb_collection   *collections; int n_collections;
    const cb_topic_family *topics;   int n_topics;
    const cb_sub_kind     *subs;     int n_subs;
    const cb_job          *jobs;     int n_jobs;        // async self-invokes and schedules
    const cb_cached_path  *cached;   int n_cached;      // edge cache, and the CloudFront behaviours
    const cb_config_var   *config;   int n_config;
    const cb_admin_panel  *panels;   int n_panels;
    cb_account_rules       account;                     // username alphabet, email domain
    // the unavoidable callbacks
    int  (*selftest)(char *err, size_t cap);
    int  (*on_account_delete)(cb_ctx *, const char *user_id);   // before the platform's cascade
    int  (*catch_up)(cb_ctx *, const char *topic, const char *viewer, uint32_t since, cb_buf *frame);
} cb_app;
```

### 2.1 Routes

A table `{method, pattern, flags, handler}`, exactly today's `cb_route`, so `handler.c`'s fixed module list becomes a loop over the platform's own routes (`/health`, `/v1/auth/*`, `/v1/stream`, `/admin*`) and then the app's.
The platform refuses an app route that shadows a platform one (router `CB_E_CONFLICT`, already there).
Handlers keep running on the request pool under serve and once per invocation under Lambda.

### 2.2 Collections instead of hard-coded rows

The store becomes two layers.
The platform owns an item layer that is DynamoDB's model on both backends: a string pk and sk, two overloaded GSIs, typed attributes (string, number, binary), a condition (attribute equals, attribute absent, version equals), transactions of up to 100 writes, Query on the table or an index with a limit and paging, a prefix scan for admin, and TTL in seconds.
SQLite implements that model as one `items` table plus two index tables, so both backends behave alike by construction; SQLite holds no production data, so this costs nothing to migrate.
A product declares its collections as data:

```c
static const cb_collection FOOLISH[] = {
  { "games",   .pk = "GAME#{id}",  .sk = "ROW",          .cas = "version" },
  { "seats",   .pk = "GAME#{id}",  .sk = "MEMBER#{user}", .gsi1 = { "USERGAMES#{user}", "{updated:15}#{id}" },
                .owner = "user" },
  { "scan",    .pk = "SCAN",       .sk = "GAME#{id}" },
  { "chat",    .pk = "CHAT#{game}", .sk = "{created:15}#{id}", .gsi1 = { "USERCHAT#{user}", "{created:15}#{id}" },
                .owner = "user" },
  ...
};
```

The templates spell today's keys exactly, which is how live data stays where it is.
Keys, index keys, the CAS attribute, TTL and the `owner` field (what the account cascade deletes) are data; the typed row to attribute mapping is a small per-collection codec the product writes against the platform's attribute API (never JSON: the item becomes JSON inside `json_surface.c` only).
The foolish operations (`game_commit` as one transaction with seats, ratings and the SCAN marker, the lease on the marker, the ladder order, the leaderboard's sparse `E#<rank>` key) are written once above the item layer in the product, instead of twice today (once per backend).
The platform offers the one pattern every kernel product needs as a helper: `cb_doc_run(collection, key, step)`, load, run a step, commit on the version, retry five times, with the striped order lock and the warm row cache; `step` is a callback because it runs the kernel.

### 2.3 Topics, subscriptions and authorization, as data

The platform owns the MQTT stream: CONNECT with a token (the password or the upgrade URL's `token=`), the client id rule `<user>-...`, the connection's life bounded by the token, SUBSCRIBE, PINGREQ, the refusals that end a connection, and delivery over both transports (the hub under serve, IoT Core under Lambda).
The policy is already product-free and stays one fixed rule: a connection may subscribe under `u/<its user>/` and under `s/`, nothing else, and no client may publish.
So who may hear is decided once, at CONNECT, and never by a callback; a product makes a topic private by putting it under the user it is for, and publishes a member's copy to each member's own topic.
A product declares topic families as templates over that rule:

| family | template | scope | foolish today |
|---|---|---|---|
| views | `u/{user}/v` | the user's own | the dashboard views |
| seat | `u/{user}/c/{doc}` | the user's own | one seat's casts |
| watch | `s/{doc}` | anyone signed in | the spectators' casts |
| chat | `u/{user}/m/{doc}` | the user's own | a game's chat, once per member |

A family's template must start with `u/{user}/` or `s/` (checked when the app is booted, and by a test over every declared family), and every level the platform fills passes `cb_rt_topic`'s level check, so no id can add a level or a wildcard.
There is no subscribe-time membership read and no catch-up callback: a client takes a game whole again over HTTP when its subscription goes live (netclient.h), and per-viewer privacy is the per-user topic.
The publish batch (one lane per user, one for the spectators, in publish order within a lane) is platform; a lane is keyed by the target's user, which every family names or does not.

### 2.4 Per-viewer projection

The platform never projects; it delivers opaque message bytes to a topic, which is what `deliver(topic, frame, len)` already does.
A product computes each viewer's bytes in its kernel and publishes `(topic, bytes)` pairs; per-viewer privacy is the per-viewer topic plus the family policy.
The answer-first window (`after.c`) queues generic `(order key, topic, bytes)` entries and generic job kicks, instead of a copied `cb_fanout`; the striped order lock keeps a doc's pushes in commit order as today.
This deletes `after.c`'s knowledge of seats, envelopes and casts.

### 2.5 Delete-account and the admin console

Delete-account is the platform's route (`/v1/account/delete` moves from `functions_http.c`).
The order: the app's `on_account_delete` hook (foolish: redact the user from every game and every stored replay, clear the leaderboard name), then the platform's cascade over every collection with an `owner` field, then the user, email and refresh items.
The hook must be idempotent, since a retried delete runs it again; foolish's already is (fresh CAS commits, a redaction of a redacted seat is `TABLE_E_NOT_SEATED`).
The admin console stays one platform page; a product adds panels as data `{title, api name, handler}`, where the handler emits rows of name/value text pairs through the existing `cb_admin_row_fn`, and the platform writes the JSON, so the containment gate holds for products too.
Foolish's Games, Game and Bots panels become three such panels; the overview's "games needing bots" becomes a product counter line.

### 2.6 The netwire contract and the C client

The live clients pin the numbers, so the split is by range, not by renumbering:

| range | owner | kinds |
|---|---|---|
| request 1-5, response 20 | platform | SIGNUP, SIGNIN, REFRESH, SIGNOUT, PASSWORD; SESSION |
| request 6, responses 21-27 | foolish | CHAT_SEND; VIEW, REPLAY, GAMES, CHATS, BOTS, BOARD, HISTORY |
| stream 51-53 | foolish | WS_VIEW, WS_CAST, WS_CHAT (MQTT payloads) |
| 64-255 | any new product | declared in the product's header |

The stream's own frames (40-44, 50, 54) are gone with the nw1 stream: the MQTT packets carry the stream's protocol, and its payloads are the product's frames.
The platform header (`cbase/wire.h`) holds `[NW_V1][kind]`, str8/str16/blob/u48, the platform kinds, the session layout and the 10-byte refusal layout, which moves out of the kernel's `ask.h` into the platform with its bytes unchanged (`ask.h` includes it and keeps the `ASK_WHY_*` values a product adds, `ASK_WHY_BUSY` among them).
The product's header holds its kinds and is read by structgen exactly as `netwire.h` is today.
A static-assert test pins every existing number and layout, and the existing golden byte tests in `unit_netwire.c` stay as the proof.
The client splits the same way: the platform's `cb_client` owns the session blob, token refresh before expiry, the pump (`do`, `next_request`, `on_response`, `next_done`, `due_ms`), the own-work pool (`NC_OWN_MAX`) and the busy answer, read retries and write no-retry, the MQTT connection's lifecycle, counted subscriptions, keepalive, backoff and per-key version gates.
A product's intents become a data table `{kind, method, path template, needs auth, is a read, answer kind}`, so `NC_DO_GAMES` is a row `{GET, "/v1/games", auth, read, NW_GAMES}` and the pump has no product switch.
The foolish part that stays foolish is the book of games (cast-skip refetch, reconcile after a refused move, `nc_set_boards`).
The `nc_*` names the web and iOS hosts call stay as they are (foolish's file forwards), so the hosts gain no lines.

## 3. The C-to-Lambda packager

### 3.1 What a product provides

One C file is enough: `#include "cbase/app.h"`, handlers, and `const cb_app CB_APP = {...}`.
A directory with an `app.mk` is the general form:

```make
APP_NAME   := foolish
APP_SRC    := $(wildcard *.c)
APP_DIRS   := foolish/c shared/c            # what the static build copies into its container
APP_SRC    += $(KERNEL_SRC)                 # foolish: the kernel's print-core list
APP_CFLAGS := -I../../c/src $(KERNEL_CAPS)
```

### 3.2 The build

`make -C c_base APP=<dir>` builds `build/<name>/cbase` (serve), `build/<name>/bootstrap` (native, for the fake-runtime lane) and `build/<name>/test/*`.
`make -C c_base APP=<dir> zip` builds `dist/<name>/bootstrap.zip` through today's `tools/build_bootstrap.sh`, generalized to copy `APP_DIRS` instead of the hard-coded `foolish/c`: static, musl, arm64, the same bytes every run, for the `provided.al2023` runtime (Amazon Linux 2023 with no language runtime: the bare Lambda).
`package.sh` already checks the zip holds exactly one executable `bootstrap`.
The platform library is compiled WITHOUT any product include path, so a platform file that includes a product header fails to compile: the compiler enforces the split, and the name gate (section 4) catches comments and literals.

### 3.3 The template

`cbase describe` (the app's own `cbase` binary) prints the variable part of the stack from the descriptor: the cached paths become CloudFront cache behaviours, the schedules become Scheduler entries with their literal inputs, the IoT authorizer (and its CloudFront path) exists only when the app declares topic families.
Text out of a C table, YAML because CloudFormation reads it, never in a shipped path.
The generated template is a build output and not committed, generated twice and compared as `gen.sh --check` does.
Two stacks:

- **base** (one per account and region): the code bucket, the GitHub OIDC role (trusting `refs/heads/main`, allowed to update code on any `cbase-*` app stack), and later the shared table.
- **app** (one per product): the function, its log group, its role, the Function URL, the IoT authorizer, the schedules, the CloudFront distribution and its MQTT path, the cached paths and the alarms.

The free-tier gate (`the_template_stays_free` in `unit_dynamo.c`) moves to the platform and sums every declared table and index across the base and every app template, refusing more than 25 a side.

### 3.4 Deploy, OIDC, serve

`cbase deploy <dir>` is today's `deploy.sh` with the product's values from the descriptor and the settings file (`~/.config/cbase/<name>.env`): build, upload by sha256, change set, the replacement guard, the secrets guard (never regenerate a JWT secret against a live stack), wait, `/health`.
CI's code-only path is today's `lambda_deploy.sh` with the stack name as an argument; a template change stays a hand deploy, as now.
`cbase serve` is the same binary on SQLite for any product, with DynamoDB Local (`make check-dynamo APP=<dir>`) as the second lane.
Product steps that today run inside `deploy.sh` (`--seed-bots`, `--reindex-ratings`) become the app's declared "after deploy" jobs, run through the same self-invoke as any job.

### 3.5 Cost model

- DynamoDB's always-free 25 WCU / 25 RCU is per payer account per region, summed over every table and every index.
  Every table and index needs at least 1 a side, so a second table with two indexes takes 3 a side off foolish whatever its load.
  Recommended: one shared table in the base stack, keeps the 15/15 + 5/5 + 5/5 split, products separated by key namespace; foolish stays unprefixed (its keys are live), every new product gets `<name>|` in front of each pk and GSI pk; the app role is scoped with `dynamodb:LeadingKeys` (`StringLike <name>|*`), whose behaviour on GSI queries must be verified before relying on it.
  The cost of sharing: one product's burst throttles another's, which at this volume is a non-issue and is visible in the admin's throttle counter.
- Lambda's 1M requests and 400k GB-seconds are per account, summed over functions: a function per product costs nothing extra.
- EventBridge Scheduler's free invocations are per account: a once-a-minute scan is about 43k a month per product, far inside.
- CloudFront: the account's three free flat-rate plans are already used (the API, the foolish web, the uttt web); a new app's distribution goes on pay-as-you-go, which is $0 inside the always-free 1 TB and 10M requests.
- IoT Core is billed per message and connection-minute after its first year: cents per product at this volume; an app with no topic families has no authorizer and publishes nothing.
- PITR at $0.20 per GB-month is per table, which is one more reason for one table.

## 4. Where things live

```
c_base/                       the platform; NO PRODUCT NAMES (gate)
  src/{util,net,aws,auth,store,realtime,admin}/   the items layer replaces the row ops
  client/                     cbase/wire.{h,c}, cbase/client.{h,c}: compiled into each product's
                              wasm and iOS builds by relative path, like shared/c
  include/cbase/app.h         the descriptor
  ops/                        base.yaml, app.yaml.in, deploy, package, the guards
  examples/notes/             a tiny product: two routes, one CAS collection, one topic
                              family, one schedule, an account-delete hook; CI runs it on
                              SQLite, DynamoDB Local and the fake Runtime API
  test/, bench/               platform tests and the latency harness
foolish/server/impls/cbase/   foolish's app: table_ops, bots, fanout, the routes, unfurl,
                              collections, the import, its admin panels, its smoke,
                              app.mk; beside impls/native, as the A10 layout has it
foolish/c/src/netwire.*, netclient.*   foolish's kinds and the book of games only
```

The gate: a C test in `c_base/test` that reads the product-name patterns from one list both gates read (moved out of `shared_is_shared_validation.test.ts` into a plain text file under `shared/tools/`), plus a c_base-only list of foolish domain words (`game`, `seat`, `bot`, `roster`, `rating`, `elo`, `replay`, `snapshot`, `leaderboard`, `deal`, `card`, `u/`-topic family names such as `/c/` and `/m/` are checked by the family test instead), refusing any hit under `c_base/` except `examples/`.
It starts with an explicit allowlist of today's offenders whose count may only go down, and is strict after stage 6.

How uttt could adopt it: uttt has no server; an online mode would declare a `matches` collection with CAS on version, `m-{doc}-{user}` and `m-{doc}` families, a few product kinds from 64 up, link `uttt/c` through its `app.mk`, and deploy its own app stack at `api.uttt.live`.
Werewolf is the pattern's strongest case, hidden roles being exactly a per-viewer projection on a per-viewer topic, but it is paused on having no group to play with, which a server does not change.

## 5. Migration, in stages

This is the product axis alone, as first designed; section 8.6 merges it with the provider axis and is the plan that is executed.
Its stage 3 predates the move to MQTT: there is no `cb_rt_may_hear` and no catch-up hook any more (section 2.3).

Each stage leaves `make check`, `check-asan` and `check-dynamo` green and the deployed stack unchanged unless it says otherwise.
Sizes are touched lines, net change in brackets.

1. **The gate and the descriptor seam** (about 700, net +300).
   Add the name gate in allowlist mode, add `cb_app`, and make `handler.c`, `boot.c`, `main_lambda.c` and `main_serve.c` read it; foolish's registrations become one `foolish_app` still inside `src/app/`.
   Deployable as a code-only update, wire and keys byte for byte the same.
2. **Jobs and schedules** (about 300, net 0).
   `"<job>:<key>"` and named schedules replace `BOT_DRIVE` and `SCHEDULE`; foolish's job is named `bot_drive` and its schedule `scan`, so the payloads in flight and the Scheduler input already deployed are instances of the new form and nothing on AWS changes.
   `cb_kick_fn` takes a job; the bot pool becomes a job pool.
3. **Topics and subscriptions as data** (about 450, net -100).
   `cb_rt_may_hear` becomes the family table and the policy set; the after-window queues `(topic, bytes)`; catch-up becomes the hook.
   Stream bytes unchanged, held by `unit_stream`, `unit_rt_*` and the smoke.
4. **The item layer, platform rows first** (about 2,500, net -400).
   Generic items on DynamoDB (the existing transport, backoff and expression builders generalized) and on SQLite (one items table: no live data, so free); users, email, refresh, CONN and TOPIC move onto it with the same keys and attributes.
5. **Foolish's rows onto items, written once** (about 3,500, net -1,200).
   Collections declared, the foolish ops rewritten once above items, both per-backend copies deleted.
   The live-data proof: a test lists every pk, sk, GSI key and attribute name each operation writes, captured before the stage and required equal after it, plus the existing DynamoDB golden requests (expression spelling may change, names may not).
   Deployed as a code-only update; this is the riskiest code change against live data, but it changes no key, so no data migration exists.
6. **The physical move** (about 9,000 moved, net 0).
   Foolish's half to `foolish/server/impls/cbase/`, `make -C c_base APP=...`, CI paths, the gate goes strict.
   No AWS change.
7. **The packager and the generated template: THE LIVE STAGE** (about 1,200, net +400).
   `app.mk`, the generalized static build, `cbase describe`, `base.yaml`, the generated app template.
   Foolish's existing stack `cbase` is updated to the generated template with the same logical ids and the same physical names (function `cbase`, table `cbase`, the WS API, the distribution), and the existing change-set guard must show zero replacements, or the deploy stops.
   The table stays in the `cbase` stack (`DeletionPolicy: Retain` already) and the base stack holds only the code bucket and the OIDC role, whose stack is re-pointed by hand with `CBASE_AWS_ROLE_ARN` unchanged if its logical role is kept.
   Done by hand from the Mac with the RUNBOOK's rollback (the same update with the previous template and CodeKey).
8. **The example product** (about 500, net +500).
   `c_base/examples/notes` in CI on all three lanes; optionally deployed as a second app stack to prove multi-product end to end.
9. **The wire and client split** (about 2,000 moved, net +200).
   `cbase/wire.h` and `cbase/client.c` out of the kernel; foolish's Makefile, `web.wasm` and the iOS xcframework add them by relative path; numbers frozen by static assert; hosts unchanged.
   Ships with the next web deploy and the next iOS build; old iOS installs see identical bytes.
10. **Only when a second product goes live: the second live stage** (about 200).
    The table moves into the base stack by CloudFormation resource import (remove from `cbase` with Retain, import into base), name `cbase` unchanged, no data copied; app roles gain `LeadingKeys` scoping.

## 6. What else is general, ranked, and where it goes

### 6.1 The headline product: packager + C AWS client + fake Runtime API harness

The minimal experience it should reach:

```
$ cat hello.c
#include "cbase/app.h"
static void hi(cb_ctx *c, const cb_http_req *q, cb_http_res *r) { cb_res_text(r, 200, "hi\n"); }
const cb_app CB_APP = { .name = "hello", .routes = (cb_route[]){ {"GET", "/hi", 0, hi} }, .n_routes = 1 };

$ cbase run hello.c          # serve on 127.0.0.1:8080 with SQLite; curl /hi
$ cbase test hello.c         # its unit tests, the fake Runtime API lane, ASan
$ cbase deploy hello.c       # static arm64 zip, base stack if absent, app stack, /health
  https://<id>.lambda-url.us-east-1.on.aws/   (and a CloudFront domain when asked for)
```

What exists today: the runtime loop and event decoding (`main_lambda.c`, `aws/runtime_api`, `aws/event`), the answer-first window, the reproducible static build, the zip check, the code-by-sha256 deploy with the change-set and secrets guards, the fake Runtime API lane (`smoke_lambda.c` serves `/next` and records `/response` and `/error`), SigV4, HTTPS with keep-alive and per-call timing, and six AWS calls (DynamoDB, IoT Data publish, Lambda invoke, CloudFront invalidation, SSM GetParameters, and the IoT authorizer answer).
What is missing to reach it:

- The descriptor and the app-free main (stages 1 to 3): today `main_lambda.c` and `boot.c` call foolish directly.
- The kernel taken out of the default link: today every program links `foolish/c`; `hello.c` must link only the platform.
- A `cbase` driver (one shell script or a tiny C front) for `run`, `test`, `deploy`, `describe`, and `app.mk` support in the Makefile and `build_bootstrap.sh`.
- The generated app template with the optional parts (no table, no WebSocket, no CloudFront) switched off by the descriptor, and a base stack bootstrapped on first deploy.
- The fake Runtime API as a library (`cbase test` runs any app's bootstrap against scripted events, HTTP and job and schedule and WebSocket), not a foolish smoke program.
- First-run ergonomics: credentials check, region default, the cert step only when a custom domain is asked for (a Function URL alone needs none).
- Docs for a stranger: README, the descriptor reference, and the example.

### 6.2 Ranked by uniqueness times value

| rank | piece | today | home | why |
|---|---|---|---|---|
| 1 | C-to-Lambda packager (static musl arm64, reproducible, zip check, sha256 code keys, template from a C descriptor) | `tools/build_bootstrap.sh`, `ops/package.sh`, `ops/*deploy.sh` | c_base platform | nothing else ships C to `provided.al2023` this simply |
| 2 | Minimal C AWS client: SigV4, HTTPS keep-alive, DynamoDB, IoT Data, Lambda invoke, CloudFront, SSM | `src/aws/*` | c_base platform | no C SDK this small exists; JSON contained in one file |
| 3 | Lambda runtime loop + fake Runtime API harness | `main_lambda.c`, `aws/runtime_api`, `test/smoke_lambda.c` | c_base platform | tests the real bootstrap with no AWS and no Docker |
| 4 | Binary realtime over IoT Core MQTT (a custom authorizer, one static policy, per-user topics, lane-parallel publish) and its local hub twin | `realtime/*` | c_base platform | push with no connection table to keep |
| 5 | DynamoDB free-tier gate + throttle backoff to the invocation deadline + paced bulk importer + DynamoDB Local lane built from the template | `unit_dynamo.c`, `store_dynamo.c`, `store_import.c` (pacing), `ops/dynamo_local.sh`, `dynamo_table.py` | c_base platform | "$0 by construction" is the platform's promise |
| 6 | The JSON/XML containment gate | `test/unit_json_gate.c` | shared/tools as a lint taking (allowed file, forbidden calls, forbidden literal patterns) | useful to any C tree that tolerates a format |
| 7 | The request-pump client pattern (intent, ticket, next_request, on_response, due_ms; hosts are transports) | `netclient.[ch]` | the generic pump to c_base/client; the pattern written up in ARCHITECTURE_AS_A_PATTERN | the client half of the platform |
| 8 | Auth: bcrypt, HS256 JWT, refresh rotation with the reuse grace | `src/auth/*` | c_base platform | needed by every product with accounts |
| 9 | Deploy safety: `require_main.sh`, the change-set replacement guard, the secrets guard, OIDC role limited to code | `ops/*` | c_base platform; `require_main.sh` also to shared/scripts | cheap, catches the expensive mistakes |
| 10 | Edge cache with event invalidation | `app/edge.*`, cache behaviours | c_base platform, paths declared | small, real latency win |
| 11 | Admin console (overview, browse, query with a WRITE confirm, logs ring, config, backups) | `admin/*` | c_base platform, product panels as data | a Supabase-studio stand-in |
| 12 | The latency bench (spaced, cold start, per-call ddb/push tallies, the `inv:` log line) | `bench/*`, `main_lambda.c` timing | harness to c_base platform; stats to shared/c/stats | measured rather than guessed |
| 13 | `tp_child` test-process helper (always-drained stderr, own temp dir, owned PIDs only) | `test/lib_proc.[ch]` | shared/c/test | every product smoke that spawns a binary wants it |
| 14 | `embed.c` (file into a C array) | `tools/embed.c` | shared/tools | trivial, used by any binary that ships a page |
| 15 | net (poll loop, HTTP/1.1 parser hardened, WebSocket codec, router) | `src/net/*` | c_base platform | general but not unique |
| 16 | util (pool, log ring, clock, uuid, b64, hex) | `src/util/*` | c_base platform; nothing here is worth shared/ until a second non-server user appears | commodity |

`rng.h` is already on its way to shared/ (`foolish/foolyard/include/rng.h`), and the store's `backoff` RNG could use it when it lands.

## 7. Decisions for Alex

1. **The platform's name and home.**
   Options: keep `c_base/` at the top level; rename it; move it under `shared/`.
   Recommend keeping `c_base/` and the name: the binaries, `CBASE_*` variables, the stack, function and table are all `cbase`, renaming any of the live ones costs a migration for nothing, and `shared/` is for code products compile in, not a deployable service.
2. **Where foolish's server half goes.**
   Options: `foolish/server/impls/cbase/` (beside `impls/native`, the A10 layout); `foolish/c_base_app/`; `foolish/server/` flat.
   Recommend `foolish/server/impls/cbase/`.
3. **Stacks and tables.**
   Options: one stack and one function hosting every product by Host; a base stack plus one app stack per product sharing one table by key namespace; fully separate stacks each with its own table.
   Recommend base plus per-app stacks with one shared table (the 25/25 is per account and every table and index costs at least 1), foolish's keys grandfathered unprefixed, and the table moved into the base stack only when a second product goes live.
4. **The store: schema-declared or code.**
   Options: keep typed per-op functions per backend (every product op written twice); a generic item layer with collections declared as data and product ops written once over it; fully declarative collections with structgen-generated row codecs and no product store code.
   Recommend the item layer with declared collections now (it also deletes foolish's second copy of every op), and revisit generated codecs after the example product.
5. **The wire and client split, and when.**
   Options: split `netwire`/`netclient` into a platform `cbase/wire` + `cbase/client` with frozen kind ranges (platform 1-5, 20, 40-44, 50, 54; foolish 6, 21-27, 51-53; new products 64+); leave them whole in the kernel and split only the server.
   Recommend splitting, as the last code stage, because it touches `web.wasm` and the iOS build while the server stages do not.

One more, smaller: a product's account rules (foolish's username-to-email `@foolish.cards` and the `%` ban) become descriptor data, and the rating row `user_create` makes "as the auth.users trigger did" should be checked for redundancy, since `game_commit` already creates a missing human rating; if `/v1/history` reads fine without it, the side effect goes and the platform's signup knows nothing about ratings.

## Decisions (Alex, 2026-10-17)
- Name and home: keep c_base/ (platform, product-free).
- Foolish's server half: foolish/server/ directly (not server/impls/cbase/).
- Stacks: one stack AND one DynamoDB table per product (not a shared table). Consequence to design for: the free 25 RCU/25 WCU are per account and foolish's stack provisions all 25 today (15/15 table + 5/5 per GSI), so the free-tier gate must become account-wide and the units get split across products (or a product goes on-demand), decided per product at creation.
- Store: declared collections now, generated codecs later.
- Wire and client split: last (touches web.wasm and iOS).

## 8. The second axis: providers (AWS today, local today, Google Cloud possible)

Read on origin/main at a66076da (2026-10-17), in a detached worktree, with nothing in the repo changed.
Alex (2026-10-17): "allow for various cloud server impls. So Google Cloud should be possible. Don't actually add a Google Cloud implementation but split up the code so that it is organized accordingly."
So c_base gets two axes: product (foolish vs platform, sections 1 to 7) and provider (aws, local, and a gcp slot that is named but not created).
The rule for the second axis is the same as the first: core names no provider, a provider names no product, and the product names no provider.

### 8.1 The headline, in five lines

About 12,500 lines of c_base are AWS-specific (about 6,200 of source, 1,450 of ops, 4,000 of tests, 900 of bench), over a quarter of the tree.
Three seams already exist and are the right shape: the store vtable (`cb_store_ops`: SQLite, DynamoDB), the push transport vtable (`cb_rt_transport`: rt_local, rt_iot) and the bot kick (`cb_kick_fn`: local pool, Lambda self-invoke).
What is NOT behind a seam is the ingress (`main_lambda.c` switches on four AWS event shapes), the edge purge (`edge.c` calls CloudFront by name), the admin's AWS console links, the `ctx->mode == CB_MODE_LAMBDA` checks that pick every backend at once, and `json_surface.c`, which is 85% AWS shapes.
The surprise is how little a GCP provider would need that local does not already have: Cloud Run runs a long-lived HTTP and WebSocket server process, which is `cbase serve`'s main, not the Lambda one.
The one real gap on GCP is push: there is no managed MQTT broker like IoT Core, so sockets are held by the process (local's hub) and a relay carries publishes between instances.

### 8.2 Inventory: every provider-specific piece on main

Spans are `file:lines` on a66076da; counts are physical lines.

#### 8.2.1 Source (about 6,000)

Spans are approximate on 56a9fe87; counts are physical lines.

| piece | where | lines |
|---|---|---|
| Lambda runtime loop, dispatch, timing line | `src/main_lambda.c` | 182 |
| Runtime API client (`/next`, `/response`, `/error`, `/init/error`) | `src/aws/runtime_api.[ch]` | 227 |
| Event kinds and struct (HTTP, IOT_AUTH, SCHEDULE, BOT_DRIVE) | `src/aws/event.[ch]` | 75 |
| SigV4 | `src/aws/sigv4.[ch]` | 410 |
| Signed call, endpoint parse, `AWS_ENDPOINT_URL_*` overrides | `src/aws/aws_call.[ch]` | 150 |
| IoT Data publish (`/topics/<topic>?qos=0`) | `src/aws/iot_data.[ch]` | 67 |
| Lambda self-invoke (`X-Amz-Invocation-Type: Event`) | `src/aws/lambda_invoke.[ch]` | 74 |
| CloudFront CreateInvalidation | `src/aws/cloudfront.[ch]` | 84 |
| SSM GetParameters for the secrets | `src/aws/ssm.[ch]` | 120 |
| IoT Core transport (publish per target, lanes) and the custom authorizer | `src/realtime/rt_iot.[ch]` | 161 |
| DynamoDB backend (transport, backoff, paging, every op) | `src/store/store_dynamo.c` | 1,641 |
| DynamoDB-only seams: `cb_store_open_dynamo_with`, bulk pacing, export scan | `src/store/store_priv.h` | 30 |
| DynamoDB JSON: targets, item layout, expressions, readers, per-row items | `src/json_surface.c:24-2445` | 2,420 |
| Lambda event envelope in and out (Function URL, IoT authorizer request and answer, schedule literal, `bot_drive:` payload, redaction) | `src/json_surface.c:2446-2998` | 553 |
| Function URL event the bench sends, CloudFront XML, SSM | `src/json_surface.c:3466-3561` | 95 |
| The same shapes declared | `src/json_surface.h` | about 420 |
| Scattered seams | `boot.c` (mode picks store, transport, kick; refuses the bot speedup under Lambda), `ctx.h` (`CB_MODE_LAMBDA`, `deadline_ms`), `config.h` (`table`, `iot_endpoint`, `cf_distribution`, `secrets_ssm`), `admin.c` (AWS console links, `AWS_REGION`, function name), `bots.c` (`cb_bots_kick_lambda`), `edge.c` (calls CloudFront), `realtime.c` and `rt_deps.h` (`iot_publish` in the generic deps table), `handler.c` (mode label, the hub's `/mqtt` route), `main_serve.c` (the timer stands in for EventBridge) | about 130 |
| Mixed: the Supabase import's DynamoDB pacing and export check | `src/store/store_import.c` | 622 (foolish and AWS) |

Not provider-specific, and staying in core: `src/aws/https.[ch]` (671, a TLS client over OpenSSL with a pool and timings; only its tally names `ddb` and `push` are AWS words), the whole of `net/`, `util/`, `auth/`, `stream_mqtt.c`, `rt_local.c`'s hub, the JWT and admin JSON.

The origin proof (`CBASE_ORIGIN_SECRET`, `X-Cbase-Origin`) landed and is generic: one config check, applied to HTTP and to the MQTT CONNECT alike.

#### 8.2.2 Ops (about 1,900, plus 690 of static-site hosting)

| piece | lines |
|---|---|
| `ops/cbase.yaml`: parameters, table, log group, role and edge policy, function, Function URL and its permissions, IoT authorizer and its permission, scheduler role and schedule, the CloudFront function, cache and response-header policies and distribution, the SNS alarm topic and ten alarms, outputs | 845 |
| `ops/github_oidc.yaml` (OIDC provider and deploy role) | about 190 |
| `ops/deploy.sh` (change set, replacement guard, secrets guard, `/health`) | about 250 |
| `ops/lambda_deploy.sh` (CI code-only update), `ops/require_main.sh` (main ancestry, no re-runs) | about 170 |
| `ops/role_covers_template.sh` and its test (the deploy role covers every GetAtt'd type) | about 120 |
| `ops/package.sh` and `tools/build_bootstrap.sh` (static musl arm64 zip) | about 100 |
| `ops/dynamo_local.sh`, `ops/dynamo_table.py` (DynamoDB Local from the template) | about 280 |
| `ops/changeset_replacements.jq`, `ops/test/changeset_gate_test.sh`, `ops/test/changesets/*` | about 20 + test |
| `ops/web.yaml`, `ops/web_deploy.sh` (S3 plus CloudFront static sites; AWS mechanism, product values) | about 690 |
| `.github/workflows/c_base.yml`: the DynamoDB Local lane, cfn-lint, the ops gates, package and deploy jobs, the deploy queue | part |

Provider-free in ops: `ops_env.sh` (the settings file; its secret helpers are generic).

#### 8.2.3 Tests and bench (about 5,000)

| piece | lines |
|---|---|
| `test/unit_dynamo.c`, `unit_dynamo_ops.c` (recorded-answer transport; also the free-tier template check) | about 2,000 |
| `test/unit_event.c`, `unit_runtime_api.c`, `unit_sigv4.c`, `unit_aws_calls.c` | about 1,300 |
| `test/unit_rt_iot.c` (the IoT publish and the authorizer over a stand-in endpoint) | part |
| `test/lib_aws_tls.[ch]` (local TLS endpoint for the signed calls), `fixture_cloudfront_event.h` | about 280 |
| `test/smoke_lambda.c`, `smoke_lambda_after.c`: the fake Runtime API (a `cb_server` serving `/next`, recording `/response` and `/error`) | about 400 |
| `test/bench_dynamo.c`; the DynamoDB Local lane through `lib_store_backend.c` (tables from `cbase.yaml`) | 120 + parts |
| `bench/coldstart.c`, `bench/direct.c` (DynamoDB direct vs through Lambda), `bench/cpu.c` (Lambda envelope cost), `bench/mqtt.c` | about 1,100 |
| fuzz: `fz_dynamo`, `fz_dynamo_store`, `fz_event`, `fz_iot_auth`, `fz_https_response` | part |

### 8.3 The provider interfaces

The shape follows `cb_app`: one const struct per provider, mostly data, with plain functions where a provider must act, and the two vtables that already exist (store, push transport) kept as the only runtime-swappable parts.
A binary links core, one product and exactly one provider, which defines `const cb_provider CB_PROVIDER` and owns `main`.
The store and push stay vtables because the test lanes mix them (the DynamoDB Local lane runs `cbase serve`'s ingress over the DynamoDB store); everything else is chosen at link time, so no registry and no plugin loading exist.

```c
// core/include/cbase/provider.h
enum {
    CB_PROV_AFTER_WINDOW = 1u << 0,   // work may run after the answer (Lambda, local; Cloud Run only with CPU always allocated)
    CB_PROV_HELD_SOCKETS = 1u << 1,   // the process holds the MQTT connections (local, Cloud Run); else a broker does (IoT Core)
    CB_PROV_EDGE_PURGE   = 1u << 2,   // the edge can purge a path; else s-maxage alone bounds staleness
};

typedef struct cb_provider {
    const char *name;                                         // "aws", "local"
    const char *mode;                                         // the /health label: "lambda", "serve" (kept byte for byte)
    unsigned flags;
    int  (*store_open)(const cb_config *, cb_store **);       // the provider's default backend
    int  (*rt_open)(cb_ctx *, cb_rt **);                      // the push transport (realtime.h cb_rt_transport)
    int  (*job_start)(cb_ctx *, const char *job, const char *key);   // fire and forget; the scan catches a lost one
    int  (*edge_purge)(const cb_config *, const char *const *paths, int n);
    void (*console_links)(const cb_ctx *, cb_admin_links *out);      // the admin's "open in the cloud console" links
} cb_provider;
```

#### 8.3.1 Runtime and ingress

The provider owns `main` and calls only core entries, so core never sees an event envelope:

```c
int  cb_core_boot(cb_ctx *, const cb_provider *, char *err, size_t cap);
void cb_core_http(cb_ctx *, const cb_http_req *, cb_http_res *);              // today's cb_handle
int  cb_core_connect(cb_ctx *, const char *token, size_t, const char *client_id, size_t,
                     const char *origin, int64_t now_ms, cb_mqtt_grant *out); // an MQTT CONNECT's verdict
int  cb_core_job(cb_ctx *, const char *job, const char *key);                 // "<job>:<key>"
int  cb_core_schedule(cb_ctx *, const char *name);
void cb_core_after(cb_ctx *);                                                 // the answer-first window
```

- aws: `main_lambda.c` plus `runtime_api.c` and `event.c` decode Lambda's envelope and call these; IoT Core's custom authorizer request becomes `cb_core_connect`, and aws encodes the grant as an IoT policy (`aws_json.c`).
  `ctx->deadline_ms` comes from `Lambda-Runtime-Deadline-Ms`.
- local: the process ingress (today's `main_serve.c` over `net/server`), the MQTT hub at `/mqtt` (its CONNECT answered by the same `cb_core_connect`), a loop timer per schedule the app declares (today's hard-coded scan), and a job pool.
- gcp (slot): the same process ingress as local, because Cloud Run runs a long-lived server with many concurrent requests per instance and native WebSockets (request timeout up to 60 minutes, after which the client's existing reconnect and catch-up take over).
  Jobs and schedules arrive as HTTP POSTs from Cloud Tasks and Cloud Scheduler to reserved paths (`/_cbase/job/<job>/<key>`, `/_cbase/schedule/<name>`) that the ingress authenticates (a shared header secret, as `CBASE_ORIGIN_SECRET` does, or Google's OIDC token checked with OpenSSL's RS256 against pinned keys) and maps onto `cb_core_job` and `cb_core_schedule`.
  The deadline is the request start plus the service's configured timeout.
  `CB_PROV_AFTER_WINDOW` is the sharp edge: with request-based billing Cloud Run throttles CPU once the response is sent, so either the service runs with CPU always allocated or core runs the after-work before answering when the flag is clear.
- So the process ingress (`main_serve.c`'s body) moves into core as `core/src/ingress/process.c`, and local and gcp each have a short `main` that picks backends; only aws has a different ingress.

#### 8.3.2 Store

The item layer of section 2.2 is the interface: string pk and sk, two overloaded indexes, typed attributes, conditions, transactions, Query with limit and an opaque cursor, batch get, a prefix scan for admin, TTL.
Three choices keep it portable: the cursor is opaque bytes, a read returns an opaque version tag that a conditional write hands back (DynamoDB: `version = :v`; Firestore: the document's `updateTime` precondition), and the limits are the tightest of every provider, enforced by local.

| semantics | DynamoDB (aws) | SQLite (local) | Firestore native (gcp fit) | Spanner | Bigtable |
|---|---|---|---|---|---|
| CAS on version | condition expression, 1 round trip | `UPDATE ... WHERE version=?` | `currentDocument.updateTime` precondition, 1 round trip given the read's tag | read-write transaction | `CheckAndMutateRow`, single row only |
| other conditions (lease free or expired, attribute absent) | condition expression | `WHERE` | `exists` precondition natively; anything else is a read-check-write transaction (3 round trips) | transaction | single row only |
| multi-item transaction | up to 100 items | one SQLite transaction | up to 500 documents | yes | none: `game_commit` with seats and ratings cannot be written |
| index on another key (GSI) | 2 overloaded GSIs, eventually consistent | index tables | composite index on fields `g1pk`,`g1sk`; sparse by omission; strongly consistent | secondary indexes | none |
| TTL | number attribute in seconds, deleted within about 48 h | the layer deletes | TTL policy on a Timestamp field, deleted within about 24 h | row deletion policy | GC policy per column family |
| batch get | BatchGetItem, 100 keys, unprocessed keys retried | `IN (...)` | `batchGet` | read in one transaction | `ReadRows` by keys |
| key limits | pk 2048 B, sk 1024 B, item 400 KB | none | doc id 1500 B and no `/`, so id = escaped pk and sk; doc 1 MiB; indexed value 1500 B | generous | row key 4 KB |
| rate limits | provisioned units, throttling (backoff exists) | none | about 1 sustained write a second per document (soft); contention aborts and retries | provisioned | provisioned |
| free tier | 25 RCU and 25 WCU per account | free | 1 GiB, 50k reads, 20k writes, 20k deletes a day, one database per project | none past a trial; tens of dollars a month at the smallest size | none; a node is hundreds a month |

Firestore in native mode is the GCP fit, and the only one with a free tier that matches DynamoDB's.
The hard parts: general attribute conditions cost a transaction (fine for the lease, which is rare; the hot path is CAS on version, which maps to one round trip), the per-document write rate (a bot drive playing several moves a second on one game document is the case to measure), and the doc-id encoding of pk and sk.
The conformance tests assert only the weaker side of each row: indexes may lag, an expired item may still be stored and the layer must filter it, a conditional write may fail spuriously under contention and is retried.

#### 8.3.3 Push

Core owns the protocol and the policy (`stream_mqtt.c`: CONNECT, the token, the client id, the two subscribe prefixes, the refusals), the topic families (section 2.3) and the publish batch with its lanes (`realtime.c`).
The transport vtable stays as it is: `deliver(to)`, `deliver_lanes(to[], frames, lanes)` and `forget(user)`.
There are two shapes of transport, and each provider picks one:

- broker (aws): IoT Core holds the connections and subscriptions; the function publishes one HTTPS request per target (`rt_iot.c` over `aws/iot_data.c`, lanes run concurrently over the HTTPS pool) and answers the broker's CONNECT through `cb_core_connect`.
  `forget` is NULL: the broker names no connection, and a deleted user's connections end at their token's expiry while nothing more is published to their topics.
  `rt_deps.h` loses `iot_publish`: aws's transport carries its own seam.
- hub (local): `core/src/realtime/hub.c`, the in-memory half of `rt_local.c` (connections, filters, binary MQTT frames on a held socket), with an optional relay; local runs it with no relay.
- gcp (slot), the realistic design: Cloud Run holds the sockets in `hub.c`; with one instance (`max-instances=1`, enough for foolish's measured peak) no relay is needed at all.
  Past one instance, a publish goes to one Pub/Sub topic and every instance has its own subscription (created at boot with an expiration policy, so dead instances' subscriptions expire), pulls with a streaming pull on a relay thread and delivers to its own sockets; every instance sees every publish, which at this volume is nothing.
  An open WebSocket keeps a Cloud Run instance billed while it is open, so the cost of held sockets must be measured before the gcp provider is built.

#### 8.3.4 Async jobs and schedules

`job_start(job, key)` is the whole runtime interface; schedules are declared by the app (`cb_app.jobs`) and wired at deploy, then arrive through the ingress.

| | aws | local | gcp (slot) |
|---|---|---|---|
| job | Lambda Invoke, `Event`, payload `"<job>:<key>"` | submit to the job pool | Cloud Tasks task, HTTP target = the service's job path |
| schedule | EventBridge Scheduler, literal input `"<name>"` | loop timer from the descriptor | Cloud Scheduler HTTP job (3 free jobs per account) |
| retries | async invoke retried twice | none | queue retry policy |

#### 8.3.5 Edge cache

Core keeps the marks (`edge.c`'s bit mask becomes the app's declared cached paths) and calls `edge_purge` from the after-window.
aws: CloudFront CreateInvalidation (today's `cloudfront.c`).
local: none.
gcp: Cloud CDN's `invalidateCache` on the URL map, which only exists behind a global external Application Load Balancer, a fixed monthly cost; without one, the flag is clear and the s-maxage alone bounds staleness, which `edge.h` already accepts for a failed flush.
The origin proof from `claude/sec-h2` is generic: CloudFront adds `X-Cbase-Origin`, and a Cloud CDN backend can add a custom request header the same way.

#### 8.3.6 Secrets

No runtime interface: the contract is "every secret is an environment variable at boot", which `config.c` already reads.
aws delivers them from NoEcho parameters today (or from SSM by deploy-time resolution later); gcp would mount Secret Manager versions as environment variables (Cloud Run's `secretKeyRef`), with no C code.
The deploy-side guards (never regenerate a JWT secret against a live stack, compare with the live value) are a provider's deploy script obeying a shared contract.

#### 8.3.7 Signing and credentials

Internal to each provider, never a core interface.
aws: `sigv4.c` and `aws_call.c` over core's `https.c`, credentials from Lambda's environment.
gcp (slot): an OAuth2 access token from the metadata server (`metadata.google.internal`, header `Metadata-Flavor: Google`), cached until expiry; for a developer's machine, a service-account key signed RS256 with the OpenSSL already linked, or the Firestore emulator, which needs no token (the twin of DynamoDB Local).
`https.c`'s tally becomes `store`, `push` and `other` by role, not by AWS service.

#### 8.3.8 Foreign formats

The containment gate stays "every foreign text format in one file per owner": core's surface (JWT, admin JSON), `providers/aws/aws_json.c` (DynamoDB, the Lambda envelope, the Runtime API body, the CloudFront XML), and later `providers/gcp/gcp_json.c` (Firestore REST, Cloud Tasks, the metadata token).
`unit_json_gate.c` takes the list of allowed files from one table.

#### 8.3.9 Deploy, logs and metrics

Deploy is the packager's (section 8.5).
Logs are stdout lines on every provider (CloudWatch Logs, Cloud Logging, the terminal); the `inv:` line stays text, never JSON, with provider-free keys.
Alarms belong to the provider's template (sec-h2's ten CloudWatch alarms; Cloud Monitoring alert policies in a gcp template).

### 8.4 Layout, both axes

```
c_base/
  core/                         names no product and no provider (gates)
    include/cbase/              app.h, provider.h, items.h, wire.h, client.h
    src/util, net (loop, http, ws, router, server, https), auth, items, realtime/{stream,gateway,hub}.c,
        ingress/process.c, after.c, edge.c (marks), handler.c, boot.c, config.c, admin, core_json.c
    client/                     the wire and client split (last stage)
    test/                       core units; conformance/{store,push,ingress,job}.c, each a library a provider's lane runs
  providers/
    local/                      main_serve.c, store_sqlite.c, the job pool, schedule timers, provider.c, provider.mk
    aws/                        main_lambda.c, runtime_api, event, sigv4, aws_call, iot_data and rt_iot (the broker transport),
                                lambda_invoke, cloudfront, store_dynamo.c, aws_json.c, describe.c (the template),
                                provider.c, provider.mk; ops/ (cbase.yaml as app.yaml.in, github_oidc.yaml, deploy,
                                lambda_deploy, package, build_bootstrap, dynamo_local, dynamo_table.py, the change-set
                                guard, site/ for the static-site stack); test/ (sigv4, event, runtime_api, aws_calls,
                                dynamo transport, rt gateway, the fake Runtime API as lib_fake_runtime.c, lib_aws_tls);
                                bench/ (coldstart, direct, bench_dynamo)
    (gcp/)                      the slot, named in docs and the gate's word list, NOT created
  examples/notes/               the second product, run on every provider lane
  ops/                          provider-free: require_main.sh, ops_env.sh, the deploy contract test
  tools/                        embed.c, the cbase driver
  bench/                        the harness and stats
foolish/server/                 the product (Alex: directly here); names no provider
```

The gates:

- Compiler: core is compiled with only `core/include` and `core/src` on its include path; each provider with core's headers and its own directory only, so `providers/local` cannot include `providers/aws`; the product with `core/include` only.
  A lane that mixes (serve's ingress over the DynamoDB store) is assembled by the Makefile's link line, never by an include.
- Names: one word list file under `shared/tools/` with three lists, products (section 4's), providers (`aws`, `amazon`, `lambda`, `dynamo`, `apigw`, `api gateway`, `execute-api`, `cloudfront`, `sigv4`, `x-amz`, `iot core`, `mqtt authorizer`, `eventbridge`, `cloudformation`, `gcp`, `google`, `firestore`, `cloud run`, `pub/sub`, `sqlite`), and foolish's domain words.
  Core may hit none of them; a provider may hit no product word; `foolish/server/` may hit no provider word.
  It starts with a shrinking allowlist and goes strict at the physical move.

### 8.5 The packager, generalized

`cbase package --provider <p> <app>` writes `dist/<app>/<p>/`: the artifact, the infrastructure file and a `manifest` (fixed `name=value` lines: provider, artifact sha256, entry, built-from commit).
`cbase deploy --provider <p> <app>` packages, then runs `providers/<p>/ops/deploy.sh dist/<app>/<p> <settings file>`, which every provider writes to one contract: refuse a replacement of any stateful resource, refuse to regenerate a secret against a live deployment, wait, probe `/health`, exit nonzero on any of them.
A provider contributes three things: `provider.mk` (its sources, its `main`, its artifact rule), `describe.c` (the infrastructure text from `cb_app`, emitted by the app's own binary as `cbase describe --provider <p>`, generated twice and compared like `gen.sh --check`), and `ops/deploy.sh`.

| provider | artifact | infrastructure | deploy |
|---|---|---|---|
| local | the `cbase` binary | none (SQLite file) | `cbase run` |
| aws | `bootstrap.zip`: static musl arm64 for `provided.al2023`, byte-reproducible, one executable checked | CloudFormation: table and GSIs from the collections, function, Function URL, IoT authorizer when topic families exist, Scheduler entries from schedules, CloudFront behaviours from cached paths, alarms | upload by sha256, change set, replacement guard, secrets guard, `/health`; CI code-only update |
| gcp (slot) | an OCI image: one layer holding the static musl binary (linux/amd64, Cloud Run's platform; arm64 if supported by then) and a CA bundle, `FROM scratch` with fixed mtimes, so its digest is the artifact's sha256 | Terraform: Cloud Run service (concurrency, timeout, CPU allocation from `CB_PROV_AFTER_WINDOW`), Firestore database with composite indexes and TTL policy from the collections, Cloud Tasks queue from jobs, Cloud Scheduler jobs, Secret Manager references, service account and IAM, optionally the load balancer and Cloud CDN from cached paths | push to Artifact Registry by digest, `terraform plan` gated for replacements, apply, `/health` |

The descriptor never names a provider: collections, topic families, jobs, schedules and cached paths are each translated by the provider's `describe.c`.

### 8.6 Revised migration, both axes in one sequence

Each stage is one pull request, merged (rebased onto main) before the next starts, and leaves `make -C c_base check check-asan check-dynamo coverage` green.
Each is deployable as a code-only Lambda update unless marked LIVE.
Through stage 7 the client-visible bytes (netwire, MQTT topics and payloads, HTTP routes and bodies), the DynamoDB table, key and attribute names, and every AWS resource name stay unchanged, held by the existing tests plus the key-capture proof of stage 6.
Every new or changed function gets unit tests, mutation-checked and recorded in `docs/mutation_checks.md`; untrusted-input code is fuzzed; coverage floors only go up.
Section 5's stage 10 is gone (Alex: one stack and one table per product).

0. **This document** as `c_base/docs/PLATFORM.md`, refreshed for section 0.1.
1. **Gates and both descriptors** (about 900, net +400).
   Name gate in allowlist mode with all three word lists; `cb_app`; `cb_provider` defined by each main (local: SQLite, hub, pool, no purge; aws: DynamoDB, IoT, invoke, CloudFront); `boot.c`'s mode switch, `ctx->mode`, the admin's links and labels read the provider instead.
   The `inv:` line's `ddb=` becomes `store=`, with the RUNBOOK's Logs Insights queries in the same commit.
   Wire, keys and AWS resources unchanged.
2. **Jobs and schedules** (about 350, net 0).
   `"<job>:<key>"` and named schedules (section 5 stage 2) through `job_start`; local's timer reads the declared schedules.
   The deployed Scheduler input `"scan"` and in-flight `bot_drive:<id>` payloads are instances of the new form.
3. **Push and edge behind the provider** (about 600, net -100).
   Topic families as data (section 2.3): `cb_rt_to` names a declared family and its `{user}` and `{doc}`, `cb_rt_topic` fills the template, and a boot check refuses a family outside `u/{user}/` and `s/`.
   `rt_publish_view`, `_cast` and `_chat` and `cb_chat_to_nw` move to the product beside `fanout.c`, and `realtime.h` loses `app/table_ops.h` and `netwire.h`.
   The provider's `rt_open` replaces boot's transport choice; `rt_deps.h` keeps only the token check, and the IoT publish seam moves into `rt_iot.c`.
   The after-window queues generic `(lane key, family, ids, bytes)` pushes and job kicks instead of a copied `cb_fanout`; the striped order lock keeps a game's pushes in commit order.
   `edge.c` keeps the marks over the app's declared cached paths and calls the provider's `edge_purge`.
   MQTT topics and payloads unchanged, held by `unit_stream_mqtt`, `unit_rt_iot`, `unit_rt_local`, `unit_after` and both smokes.
4. **Ingress behind core entries** (about 600, net -50).
   `main_serve.c`'s body becomes the process ingress; `main_lambda.c` calls the `cb_core_*` entries (the IoT authorizer through `cb_core_connect`); `smoke_lambda.c`'s fake Runtime API becomes `lib_fake_runtime.c`, and the ingress conformance scenario (HTTP, connect, job, schedule, deadline, after-window) runs through both mains.
5. **Store conformance first, then the item layer for platform rows** (about 3,000, net -300).
   The store conformance suite is written against the item API before either backend implements it, runs on SQLite and DynamoDB Local, and local enforces the tightest limits of section 8.3.2; then section 5 stage 4 for users, email, sessions, refresh and sign-in failures.
   The item API has a conditional update of one item beside the transaction, because a mid-game commit is one conditional UpdateItem (section 0.1).
6. **Foolish's rows onto items** (section 5 stage 5, unchanged, including the key-capture proof).
   The single-UpdateItem mid-game commit and the transactions for create, join, finish and a marker change are kept exactly, and the captured requests show it.
7. **The physical move, both axes at once** (about 12,000 moved, net 0).
   `core/`, `providers/local/`, `providers/aws/` (with `ops/site/` for the static-site stacks) and `foolish/server/` in one `git mv` so no file moves twice; `json_surface.c` splits into `core_json.c` and `aws_json.c`, each the only JSON file on its side; the include paths and all three name gates go strict; CI paths add `foolish/server/**`.
   No AWS change.
8. **LIVE: the packager with `--provider aws` and the generated template** (about 1,400, net +500).
   `provider.mk`, `describe.c`, the manifest and the deploy contract; the existing `cbase` stack is updated to the generated template with the same logical ids and physical names (function, table, IoT authorizer and its permission, the scan schedule and its role, the CloudFront function, policies and distribution, the alarm topic and alarms, the SSM secrets prefix), and the change-set guard must show zero replacements or the deploy stops.
   The change set is shown to Alex before it is executed.
   The free-tier gate becomes account-wide, summing every product stack's provisioned units (Alex's per-product tables; foolish's 15/15 table and 5/5 per GSI is all 25/25 today).
   `role_covers_template.sh` runs on the generated template, so the OIDC role still covers every type it GetAtts, and the deploy queue and `require_main.sh` are unchanged; the OIDC stack keeps its logical role and `CBASE_AWS_ROLE_ARN`.
9. **The example product** on the local and aws lanes (section 5 stage 8).
10. **The wire and client split** (section 5 stage 9; last, as decided: it touches `web.wasm` and the iOS build).
11. **Only when wanted: `providers/gcp/`**, built against the conformance suites, with the Firestore emulator as its CI lane; a named slot only, no code now.

### 8.7 Decisions for Alex (provider axis)

1. **Is local the reference implementation?**
   Options: local is the reference, and every interface's semantics are defined by the conformance suites it passes first; AWS is the reference and local imitates it; a written spec with no reference.
   Recommend local as the reference, enforcing the tightest limit of every provider (item size, transaction size, key length, an expired item filtered on read, an index allowed to lag), so a product that passes on local passes on every provider.
2. **A provider conformance suite now?**
   Options: now, before the item layer; after the physical move; when a second provider is started.
   Recommend now: store, push, ingress and job suites as libraries in `core/test/conformance/`, run against local and against aws (DynamoDB Local, the fake Runtime API, a fake gateway), and against real AWS by hand before each LIVE stage.
3. **Provider selection: link time or run time?**
   Options: one provider per binary, chosen at link time, with only the store and push vtables swappable (for the mixed test lanes); a runtime registry of providers.
   Recommend link time.
4. **Where the process ingress lives.**
   Options: core (shared by local and a future Cloud Run provider); inside `providers/local`, with gcp copying it.
   Recommend core: Cloud Run's main is serve's main with different backends.
5. **Infrastructure as code for a future gcp provider.**
   Options: Terraform; gcloud command scripts; Infrastructure Manager (Google's managed Terraform).
   Recommend Terraform when the slot is filled, because `terraform plan` gives the replacement gate the deploy contract requires and a script does not; nothing to build now beyond the packager's `describe` and `deploy.sh` contract.
6. **The `inv:` log line's keys.**
   Options: keep `ddb=` and `push=`; rename to `store=`, `push=`, `other=`.
   Recommend renaming in stage 1 and updating the RUNBOOK's Logs Insights queries in the same commit.
7. **The static-site stacks (`web.yaml`, `web_deploy.sh`).**
   Options: `providers/aws/ops/site/` as a generic static-site target with the values in each product; leave them as foolish ops.
   Recommend `providers/aws/ops/site/`, since uttt.live already uses the same mechanism with different values.

## Provider decisions (Alex, 2026-10-17)
All seven section-8 recommendations accepted: local is the reference provider enforcing the tightest limits; conformance suites (store, push, ingress, job) now, run against local and aws (DynamoDB Local + fake Runtime API); provider chosen at link time; process ingress moves to core; Terraform for a future gcp; `ddb=` renamed `store=` in the inv: line; static-site stacks move under providers/aws/ops/site.

## 9. Tests owed (Alex, 2026-10-18: new test work is deferred to stage 8)

For speed, stages 1 to 7 write no new unit tests, mutation checks, conformance suites or fuzz targets, and raise no coverage floor.
The existing tests stay green (every merge to main deploys), are changed only where a move or rename requires it, and no coverage floor goes down; where a stage would drop below a floor, it writes just enough test to hold it and says so here.
The stage 6 key-capture proof is not deferred, since it guards production data.
Stage 8 pays this list down, mutation-checked and recorded in `docs/mutation_checks.md`, before its LIVE deploy.

### Stage 1

- The name gate (section 8.4): `unit_name_gate.c` over one word list file under `shared/tools/` (products, providers, foolish's domain words) with a shrinking allowlist, and `shared_is_shared_validation.test.ts` reading its product list from the same file.
