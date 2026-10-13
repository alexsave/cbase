# As built: admin and ops

Builder: admin + ops, branch `claude/c_base-admin`.

## What was built

- `src/admin/admin.c`: GET /admin serves `admin/console.html`, embedded at build time by `tools/embed.c` into `build/gen/console_html.c` (one Makefile rule, and the file added to `LIB_SRC`).
  `/admin/api/*` is one route (method `*`) dispatching to 13 calls: overview, collections, browse, query, users, users/delete, games, game, bots, logs, config, backups (GET and POST).
  `src/admin/admin.h` lists them.
- The gate: `console_on()` answers 404 for every admin route when `CBASE_ADMIN_TOKEN` is unset; `admin_gate()` answers 401 for an API call without the exact Bearer (constant-time compare).
  The page needs only `console_on()`, since a browser navigation cannot carry a Bearer and the page holds no data.
- `admin/console.html`: 740 lines, vanilla JS, no CDN, a strict CSP (`default-src 'none'`, same-origin fetch only), light and dark by `prefers-color-scheme`, a sidebar that becomes a scrolling tab bar under 760 px, tables that scroll inside their own box.
  Token in `sessionStorage` only; a 401 drops it and returns to the token prompt.
- `ops/cbase.yaml`, `ops/package.sh`, `.github/workflows/c_base.yml`, `RUNBOOK.md`.

## Differences from the design, and why

- Unset token: 404, not the 403 the old `admin.h` comment said; the task asked for 404 and `admin.h` now says so.
- Tables panel: the key prefix and the cursor are server side (store `admin_browse`); the eq filter and the column sort apply to the rows loaded so far, in the page.
  A server-side filter would need a store operation that does not exist.
- Users > delete runs `POST /functions/v1/delete-account` through `cb_handle` with a 60 s access token minted for that user (`cb_jwt_mint`), so the redaction from every game, the leaderboard name and the account delete stay the app's one implementation.
  It answers 503 if the app's route is not registered (the router's 404) or the mint fails.
- Games panel: there is no "list games" store operation, so it browses the `games` collection and calls `game_get` per id.
  Seats come from the kernel's `roster_decode`; the envelope is `cb_table_view(ctx, id, NULL)`, the spectator's.
- Backups: `config.h` has no backup bucket, so there is no "export the DynamoDB table to S3" button.
  Under DynamoDB the panel explains point-in-time recovery (on in the template) and links to the table's Backups tab; under SQLite it runs `VACUUM INTO <db dir>/backups/cbase-<UTC stamp>.sqlite` through `admin_query(allow_write = true)` and lists those files.
- Lambda code: CloudFormation's inline code works only for Node.js and Python, so the template takes the zip from an S3 bucket parameter; there is no placeholder path.
- `CBASE_WS_ENDPOINT` is computed in the template from the WebSocket API, not a parameter.
- The template also sets `CBASE_STORE=dynamo` and `CBASE_LOG_LEVEL=info` explicitly.

## What the other builders must match (the integrator aligns these)

1. **DynamoDB key attributes** (store builder): `pk` S, `sk` S; index `GSI1` on `gsi1pk` S (`USERGAMES#<user>`) and `gsi1sk` **N** (updated_at, ms); index `GSI2` on `gsi2pk` S (`PLAYER#<user>`) and `gsi2sk` S (`<created>#<id>`); both project ALL.
   DESIGN.md left these names open; I picked them.
   There is no topic GSI: DESIGN.md stores a topic's connections as base-table items under `TOPIC#<topic>`, which needs no index.
2. **TTL**: the attribute is `expires_at`, and DynamoDB reads it as epoch **seconds**.
   `store.h` says times are ms; if `store_dynamo.c` writes ms there, the items never expire (a ms value reads as the year ~57000).
   The `TOPIC#` reverse-index items should carry the same `expires_at` as their `CONN#` item, or they outlive it.
3. **The schedule payload** is the JSON string literal `"scan"` (c_base_brief_lambda.md, decision record).
   `aws/event.h`'s comment still says `{"cbase":"scan"}` or `source: aws.events`; the event decoder must accept `"scan"`.
   The same record says the self-invoke is `"bot_drive:<id>"`, while `ctx.h` and `lambda_invoke.h` say `{"cbase":"bot_drive","game":id}`: one of them should change.
4. **admin_query** must return `CB_E_FORBIDDEN` for a write when `allow_write` is false (the console maps it to 403 and offers the typed confirmation), and must run `VACUUM INTO '<path>'` when `allow_write` is true (SQLite).
5. **admin_collections** must list `games`, and `admin_browse("games", ...)` rows must carry `id` (SQLite) or `pk` `GAME#<id>` with `sk` `ROW` (DynamoDB; other items under the same pk are skipped).
6. **WebSocket replies** travel over the management API: the template configures no route responses, so a body returned from a `$default` invocation is not delivered.
7. **IAM**: the function role has the DynamoDB item actions plus `PartiQLSelect/Insert/Update/Delete` (for the console's PartiQL), `execute-api:ManageConnections` on the API, `lambda:InvokeFunction` on itself, and log writes.
   `TransactWriteItems` is authorized per item by `PutItem`/`UpdateItem`/`DeleteItem`/`ConditionCheckItem` and is not an IAM action of its own (cfn-lint flags it).

## Tests

`test/unit_admin.c` (10 tests, 122 checks) drives every route through `cb_handle` against `test/lib_admin_fakestore.c`: unset token 404 everywhere; missing, wrong, prefix-of, one-longer and anon-key Bearers all 401; the page served without a Bearer; browse paging by cursor, prefix and the 500-row cap; query refuses a write without the exact word `WRITE` (lowercase and `true` refused) and passes `allow_write` only with it; config never contains a secret value; the log tail by seq and level; the games list skipping a vanished row; SQLite backup statement with a quoted path; user delete (see untested).

Mutation checks (each broke the code, the named assertion failed, the source was restored by rewriting the saved copy and the object deleted):

- admin.c `console_on` always true: `unset_token_hides_the_console` GET /admin == 404.
- admin.c gate ignores `const_eq`: `the_token_gates_every_call` "wrong" == 401.
- admin.c `const_eq` drops the length check: the prefix token == 401.
- admin.c any non-empty confirm allows a write: confirm "write" == 403 and `!last_allow_write`.
- admin.c browse passes NULL for the cursor: `last_cursor == "3"`.
- admin.c browse limit uncapped: `last_limit == 500`.
- admin.c logs ignore the level: `entries` length == 2.
- admin.c backup path quote not doubled: `strstr(..., "/q''d/backups/cbase-")`.
- admin.c needing-bots count not incremented: overview `n == 3`.
- admin.c games list keeps a row `game_get` did not find: `games` length == 1.
- admin.c API method not checked: GET query == 405, POST backups under dynamo == 409.

Ops: `cfn-lint 1.57.2` passes on `ops/cbase.yaml` (and fails as expected on a copy with a bad runtime and a misspelled `IndexName`); `bash -n` and `shellcheck` pass on `ops/package.sh`; `actionlint` passes on the workflow; `ops/package.sh` ran end to end on this Mac (2,202,382-byte zip holding one executable `bootstrap`, static aarch64 ELF).
The console was checked by eye in headless Chrome against a mock API, light and dark, at 1280 px and at 390 px (inside an iframe, since headless Chrome will not size a window under 500 px); the header badge now truncates instead of pushing Sign out off a phone screen.

## Untested

- User delete beyond the token mint: `cb_jwt_mint` is a stub in this tree, so the test checks the 503 and that nothing was called; once auth lands the same test checks the delete-account route is reached as the user (`sub == "u0"`).
- The envelope hex (`cb_table_view` is a stub) and every call against a real store backend.
- The template has never been deployed; in particular the WebSocket path through CloudFront (managed `AllViewerExceptHostHeader`, the `/prod` rewrite) and the dual Function URL permission (`InvokedViaFunctionUrl`) are unproven until step 9 of the runbook.
- The CI workflow has not run; the `bootstrap` job builds under QEMU on an x86 runner and may be slow (90-minute timeout); `ubuntu-24.04-arm` is the faster alternative if the repository's plan has it.
- The deploy job has never run.
- The console against a live server with data.

## Header changes I would make

- `config.h`: none needed now. A `CBASE_BACKUP_BUCKET` would only matter for a DynamoDB export-to-S3 button, which PITR makes unnecessary.
- `store.h`: a `games_list(cursor, limit)` for the console would replace parsing browse rows for ids (item 5 above).

## Open defects

- The router adds `Access-Control-Allow-Origin` (default `*`) to admin API responses like every other route; harmless because the API needs a Bearer, not a cookie.
- Under Lambda the Logs panel and the Overview counters see one warm container, not the function; the panel says so and links to CloudWatch Logs Insights.
- The Function URL is public and reachable without CloudFront, as the Supabase functions' URL was.
