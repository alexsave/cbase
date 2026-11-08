# The cbase app API

What a product hands cbase, and what cbase does with it: routes, CORS and the Host allow-list, the store and the product's rows, realtime topics, accounts and the console, the wire, storeless apps, child processes, settings and files.
The contract itself is `core/include/cbase/app.h`; this document explains it.

The sections are numbered 2.x because comments in the code cite them by number.
The design of the split that produced this API (the inventory of the monorepo cbase came from, the packager, the provider axis and the staged migration) was the rest of this document, and is now [docs/history/PLATFORM_SPLIT.md](history/PLATFORM_SPLIT.md); the appendix below says in a few lines how cbase came to be.

## 2. The platform API a product implements

The rule: the platform knows column names, an RPC name and a topic format, never what a product's document means.
A product is ONE const struct, `cb_app`, mostly data tables; function pointers appear only where the product's own code has to run.
The product defines it and points `cb_main_app` at it, and each provider's main serves that app (`ctx->app`).
Abridged from `core/include/cbase/app.h`, where every field is documented:

```c
struct cb_app {
    const char *name;                                   // "example": the log tag
    const cb_route_list *const *routes; int n_route_lists;
    bool health_route, ready_route, admin_routes, auth_routes;   // the platform's routes it wants (2.9)
    int  (*selftest)(char *err, size_t cap);            // run once at boot, before anything is served
    int  (*config)(struct cb_config *cfg, char *err, size_t cap);  // the app's own settings
    const cb_job *jobs;               int n_jobs;       // started by name with a key
    const cb_schedule *schedules;     int n_schedules;  // run every every_ms
    const cb_topic_family *const *topics; int n_topics; // what it publishes to (2.3)
    const char *const *cached_paths;  int n_cached_paths;   // public reads an edge may cache
    const struct cb_store_app *store;                   // its rows and its part of an account (2.2)
    bool storeless;                                     // it keeps its own data (2.7)
    int  (*delete_account)(cb_ctx *, const char *user_id, int *code, int *detail);   // (2.5)
    const cb_admin_call *admin_calls; int n_admin_calls;     // the console (2.5)
    const cb_admin_panel *admin_panels; int n_admin_panels;
    int  (*admin_count)(cb_ctx *, int64_t now_ms); const char *admin_count_label;
    int  (*serve_open)(cb_ctx *, char *err, size_t cap); void (*serve_close)(cb_ctx *);
    const struct cb_metric_def_s *metrics; int n_metrics;    // GET /metrics
    const char *account_domain;                         // the domain a username's account email takes
    const char *const *cors_origins;  int n_cors_origins; bool cors_credentials;   // (2.1)
    int req_workers; const char *log_level;             // serve's defaults it ships (2.10)
    const struct cb_mime_s *mime_types; int n_mime_types;    // (2.11)
    const char *const *host_allow;    int n_host_allow;      // (2.1)
};
extern const cb_app *const cb_main_app;
```

[example/src/example_app.c](../example/src/example_app.c) is a whole app: two routes, a job and a schedule.

### 2.1 Routes

A table `{method, pattern, flags, handler}` (`cb_route`, `core/src/net/router.h`), registered after the platform routes the app declares (2.9, items 5 and 6) and the provider's own (the local hub's `/ws`).
The platform refuses an app route that shadows one already registered (router `CB_E_CONFLICT`).
Handlers run on the request pool under serve (`CB_ROUTE_POOL`, the default), on the loop thread for a route that must not wait (`CB_ROUTE_LOOP`), and once per invocation under Lambda.

A pattern is exact (`/health`), a prefix (`/v1/items/*`), or has path parameters (`/decks/:id/notes/:nid`), which the handler reads decoded with `cb_route_param(req, "id", out, cap)`.
An exact pattern always wins, then a parameter pattern (the one whose first differing segment is literal), then the longest prefix, whatever order they were added in (`net/router.h`).
A route may take a body larger or smaller than `CBASE_MAX_BODY` (1 MiB) with its own `max_body` (at most 1 GiB, for a media upload); serve answers a longer Content-Length 413 from the head alone, before reading the body.
Chunked request bodies stay refused (411): every client sends a Content-Length.

CORS is the app's to declare, and the default is deny (`net/router.h`).
`cb_app.cors_origins` lists the web origins whose pages may call the app, each exactly as a browser sends `Origin`; the deployment may add its own through `CBASE_ALLOWED_ORIGINS` (default empty).
A listed origin is echoed in `Access-Control-Allow-Origin` with `Vary: Origin`; any other gets no `Access-Control-*` header, and its preflight is answered 403.
`"*"` in either list is any origin, answered `*`; `cors_credentials` adds `Access-Control-Allow-Credentials: true` to an echoed origin, and boot refuses it beside `"*"`.
A WebSocket upgrade, on every socket path (`/ws`, `/ws/public`, an app's own), is held to the same lists: an `Origin` neither names is answered 403 before the handler runs, so it never gets a 101.
An upgrade without `Origin` (a native client, the C SDK's POSIX host, a test) goes ahead, since only a browser sends one and the check exists to stop a browser driven by another site.

The Host allow-list is the app's to declare too, against DNS rebinding (`net/router.h`).
A page on `evil.example` whose name its owner points at 127.0.0.1 is same-origin with a server on the user's machine as far as the browser knows, so CORS cannot stop it; the `Host` header it sends still says `evil.example`.
`cb_app.host_allow` lists the hosts the app answers to, each as `host:port`, `[v6]:port`, or a bare `host` / `[v6]` that means that host at the port serve bound (so an app that binds port 0 writes `{ "127.0.0.1", "localhost", "[::1]" }`).
Once it is declared, every request is held to it before anything else runs, on every route: the platform's, the app's, a WebSocket upgrade (refused with no 101) and a file.
A `Host` that is listed goes ahead (the host compared without case, a missing port read as 80); a well-formed one that is not is 403 "host not allowed".
A request with no `Host`, two of them, or one that is not `host[:port]` is 400, as RFC 9112 3.2 says: a browser always sends exactly one, so only a hand-made request can lack it, and there is nothing to hold it to.
Boot refuses an entry that is not shaped so (a scheme, a path, a wildcard, port 0).
Not declared (NULL / 0), nothing is checked: a server behind an edge (CloudFront, which sends its own Host) is as it was.

### 2.2 The store, and the product's rows

The store is one interface (`core/src/store/store.h`, a table of operations called through `CB_STORE_CALL`) with a backend per provider: SQLite under `local` (`providers/local/src/store/store_sqlite.c`), DynamoDB under `aws` (`providers/aws/src/store/store_dynamo.c`).
The platform's own rows are the accounts and what authentication keeps: users, sessions, refresh tokens, failed sign-ins, linked identities and one-time claims.

SQLite keeps them in plain tables (`providers/local/sql/schema.sql`, applied at every open in one transaction and stamped with `CB_SCHEMA_VERSION`; a newer database is refused).
DynamoDB keeps them as items on one table through the item layer (`core/src/store/items.h`, which only that backend implements): a string pk and sk, typed attributes, two overloaded indexes, conditional writes, transactions of up to 100 writes, Query with paging, and TTL; the layout is in `providers/aws/src/aws_json.h` ("THE ITEM LAYOUT"), and `core/src/store/rows_auth.c` writes the session, refresh-token and sign-in-failure rows over it.
The item layer's limits are the tightest of every provider, and every backend refuses a write past one the same way (`CB_E_INVALID`, nothing sent), so a product that works on one works on the other.

A product adds its rows through `cb_app.store`, a `cb_store_app` (`store.h`, "WHAT A PRODUCT ADDS TO EVERY STORE"), which every backend installs at open:

- its operations table (`rows`), read by the product's own call macro, so the platform never names a product row;
- on SQLite, its tables (`sql_schema`, applied after the platform's in the same transaction), its prepared statements (`sql_stmts`) and its schema steps (`sql_before`, `sql_upgrade`), written against `core/src/store/store_sql.h`;
- on DynamoDB, its key families (`families`, `family_aliases`) and the shapes of its keys (`key_templates`, which the key-capture check in `make check-dynamo` reads);
- its part of an account: `signup_txn` and `sql_signup` (its rows of a new account, in the account's own transaction), `delete_keys`, `delete_txn` and `sql_delete` (every row of its that hangs off the user, deleted with the account);
- `seed`, the rows it writes into a store that has none.

### 2.3 Topics, subscriptions and authorization, as data

A push names a target (`cb_rt_to`, `core/src/realtime/realtime.h`): one of the app's topic families with its `{user}` and `{doc}` filled in by `cb_rt_topic`, which refuses any id that could add a level or a wildcard.
A family is declared as data (`cb_topic_family`: a name, a pattern, and whether it is `anonymous`), for example:

| family | pattern | who may hear it |
|---|---|---|
| views | `u/{user}/v` | that user |
| member | `u/{user}/c/{doc}` | that user: one member's own copy of a document's pushes |
| watch | `s/{doc}` | anyone signed in, and anyone at all when the family is `anonymous` |
| chat | `u/{user}/m/{doc}` | that user: a document's messages as one member hears them |

The policy is one fixed rule: a connection may subscribe under `u/<its user>/` and under `s/`, nothing else, and no client may publish.
So who may hear is decided once, at connect, and never by a callback; a product makes a topic private by putting it under the user it is for, and publishes a member's copy to each member's own topic.
Boot refuses a family whose pattern does not start with `u/{user}/` or `s/`, or names anything but `{user}` and `{doc}` (`cb_rt_families_check`).

The transport is the provider's:

- `local`: serve's in-process hub at `GET /ws` (`providers/local/src/realtime/rt_local.c`, frames of `lib/stream_frame.h`: HELLO with a token, SUB a room, REQ a route over the socket, PING), and `GET /ws/public` for a client that never signed in, served only when the app has an anonymous family.
- `aws`: AWS IoT Core over MQTT 3.1.1 (`providers/aws/src/realtime/rt_iot.c`), reached at `/mqtt` through the same CloudFront distribution; its custom authorizer is the same function, answering with the policy above (`core/src/realtime/stream_mqtt.h`), so a broker that authorizes every connection by its token hears an anonymous family only from the signed in.

There is no subscribe-time membership read and no catch-up callback: a client takes its state whole again over HTTP when its subscription goes live.

### 2.4 Per-viewer projection

The platform never projects; it delivers opaque message bytes to a topic (`cb_rt_publish`).
A product computes each viewer's bytes itself and publishes `(target, bytes)` pairs; per-viewer privacy is the per-viewer topic plus the family policy.
A batch (`cb_rt_batch_begin` and `cb_rt_batch_end`) hands several publishes to the transport at once, one lane per user and one for the rooms, each lane in the order published.
Under Lambda an HTTP invocation answers first and then runs what the app deferred (`core/src/after.h`: its pushes, the jobs it wakes, then the edge cache's invalidation), so an answer never waits for the broker; a step that must keep an order of its own (a document's pushes in commit order) takes its own lock.
Under serve a publish is a copy posted to the loop.

### 2.5 Delete-account and the admin console

Delete-account is one function the app supplies, `cb_app.delete_account`, and the console's `users/delete` runs it; a product exposes it to its own clients through a route of its own.
The store's part of the delete is the product's `delete_keys`, `delete_txn` and `sql_delete` hooks (2.2), run with the account's own rows; the function must be idempotent, since a retried delete runs it again.
A storeless app that sets it is refused at boot, since accounts live in the platform's store.

The admin console (`core/src/admin/`, served when the app sets `admin_routes` and `CBASE_ADMIN_TOKEN` is set) is one platform page with its JSON API at `/admin/api/*`: overview, collections, browse, query (SQL on SQLite, PartiQL on DynamoDB, writes only when confirmed), users, logs, config and backups.
A product adds its own calls (`admin_calls`: a method, a name under `/admin/api/`, a handler), the panels the console draws from them (`admin_panels`: a title, the list call, its rows and its cursor), and the overview's one counter line (`admin_count`).
A panel's handler answers through `core/src/admin/admin.h` (`cb_admin_send_json` over a document `core_json.c` wrote), so the JSON containment gate holds for products too.

### 2.6 The wire: the platform's kinds and its refusal

Clients speak a binary wire to cbase, and the platform's part of it is `client/include/cbase/auth_wire.h`, which the server reads too (`core/src/auth/auth_wire.c`):

| kind | owner | what |
|---|---|---|
| 1-5, 7 | platform | SIGNUP, SIGNIN, REFRESH, SIGNOUT, PASSWORD; OIDC (Sign in with Apple) |
| 6 | the first product | skipped by the platform, so the first product's numbers stand |
| 20 | platform | SESSION, the answer to signup, signin, refresh and oidc |
| anything else | the product | declared in its own wire header, which static-asserts its numbers against this one |

Every body starts with two bytes, `[CBW_V][kind]`; integers are little-endian, a str8 is a u8 length then bytes, a str16 a u16 length, a time u48 epoch ms.
Every route answers a refusal (any non-2xx) with the same 10 bytes: `u8 CBW_REFUSAL_FORMAT, u8 why, i32 code, i32 detail`, where `why` is one of `CBW_WHY_*` and an account refusal's `code` is one of `CBW_ACCOUNT_E_*`; a client decodes it into an enum the host localizes, so no host string-matches a server message.
The numbers are on the wire and append-only.

The client SDK (`client/include/cbase/client.h`) owns the session, its refresh, the socket and its calls, retries and backoff; [docs/CLIENT_DESIGN.md](CLIENT_DESIGN.md) is its design.

### 2.7 A storeless app

An app that keeps its data itself (its own database files, opened in `serve_open` and closed in `serve_close`) sets `storeless` in its `cb_app`.
Boot then opens no platform store: no schema is applied, no database file is made, and no `.serve-lock` is taken beside one.
What lives in the platform's store is off: the auth routes answer 503, the console's store views answer 503, `/ready` says `store=none`, and serve's backup restore is not run (a configured bucket is said to back up nothing).
The app's routes, jobs, schedules, console pages and counter run as for any app.
Outside `CBASE_DEV`, serve needs neither `CBASE_JWT_SECRET` nor `CBASE_PUBLIC_URL` for a storeless app: nothing signs or verifies a token, and the public URL is the token issuer, whose one other reader is the console's overview (it shows the default).
A secret that is set is not used (boot says so) and is still held to its length; a public URL that is given still may not name the wildcard address.
A storeless app that still asks for the store is refused at boot with a sentence that names what it asked for: store rows (`cb_app.store`), an account domain, `delete_account`, `CBASE_STORE`, or Sign in with Apple.
The default is false, so every app written before the field opens the provider's store as it always did.
`example/test/unit_storeless.c` serves the example app storeless through `cbase serve`'s ingress and checks that its directory stays empty.

### 2.8 Child processes

`core/src/spawn.h` runs programs from the server's loop: argv, an explicit environment, a working directory, stdin bytes, stdout and stderr each under a cap, a timeout, a kill from the caller, the exit code or signal in one callback on the loop thread.
Each child is its own process group, and serve ends every group still running when it shuts down (SIGTERM, then SIGKILL after the grace).
On Linux the leader also gets `PR_SET_PDEATHSIG`, so it dies with a server that is SIGKILLed; on macOS a SIGKILLed or crashed server cannot clean up, and its children run on until they end.
An optional socketpair on the child's fd 3 carries lines both ways; cbase knows no protocol over it, and an overlong line closes the channel, not the child.

### 2.9 Upgrading a product across the HTTP changes

A product that consumes cbase from before the CORS, path parameter and body cap changes makes three edits, and the compiler or boot names each one it misses; a local server makes a fourth.
A product from before the platform routes became opt-in makes the fifth and sixth, which nothing names for it: its `/health`, `/ready`, console or account routes simply answer 404.

1. **Route rows.** `cb_route` gained `max_body` after `inline_us`, so a positional row such as `{ "GET", "/v1/x", CB_ROUTE_POOL, h_x, 0 }` fails `-Wmissing-field-initializers`.
   Prefer designated initializers, which name what they set and survive the next field: `{ .method = "GET", .pattern = "/v1/x", .flags = CB_ROUTE_POOL, .fn = h_x }`.
   The least edit is a trailing `, 0` on each positional row: `{ "GET", "/v1/x", CB_ROUTE_POOL, h_x, 0, 0 }`, which keeps the server's `CBASE_MAX_BODY`.
2. **`cb_handle` and `cb_core_http` take a mutable `cb_http_req *`**, since they bind the route's path parameters into it before the handler runs.
   A caller that passed a `const cb_http_req *` (a test, an ingress of its own) passes a non-const one; handlers keep `const cb_http_req *req`.
3. **CORS is denied by default**, browsers' WebSocket upgrades included.
   A product that relied on the old default (`Access-Control-Allow-Origin: *` for every origin) keeps today's behaviour by declaring it in its `cb_app`:
   `static const char *const ORIGINS[] = { "*" };` with `.cors_origins = ORIGINS, .n_cors_origins = 1`.
   Better is the list of origins its pages are served from, `{ "https://app.example.com", "https://www.example.com" }`.
   The deployment's `CBASE_ALLOWED_ORIGINS="*"` does the same without a code change, but it now defaults to empty, in the templates (`cbase.yaml`, `deploy.sh`, `vm.yaml`) too.
4. **A server on the user's own machine declares its Host allow-list** (2.1), since CORS alone does not stop DNS rebinding.
   Nothing breaks without it: the default is no list, and a product behind an edge leaves it so.
   A local one adds `static const char *const HOSTS[] = { "127.0.0.1", "localhost", "[::1]" };` with `.host_allow = HOSTS, .n_host_allow = 3`, each a bare host meaning the port serve bound, so port 0 works.
   Its own tests then send a listed `Host` with the bound port: cbase's test client sends `Host: 127.0.0.1` with no port, which such a list refuses (403), and a request with none is 400.
5. **The platform's routes are declared by the app, and off by default.**
   `cb_app` gained `bool health_route` (`GET /health`), `bool ready_route` (`GET /ready`) and `bool admin_routes` (the console: `GET /admin` and `/admin/api/*`).
   One left false is not registered at all, so a request to it is the app's own 404, or the app's own route when it has that path.
   A product keeps what it had by declaring each it uses: `.health_route = true, .ready_route = true, .admin_routes = true`.
   A load balancer's or deploy check's probe needs `health_route`, and a readiness probe `ready_route`.
   `admin_routes` still serves the console only once `CBASE_ADMIN_TOKEN` is set; without it, the token is ignored and boot warns that no console is served.
6. **The account routes are declared by the app too, and off by default.**
   `cb_app` gained `bool auth_routes`: `POST /v1/auth/oidc`, `signup`, `signin`, `refresh`, `signout` and `password`.
   Left false, none is registered, so each is the app's own 404; a client that signs in needs `.auth_routes = true`.
   Accounts live in the platform's store, so a storeless app that declares them is refused at boot, with a sentence naming `cb_app.auth_routes`.

### 2.10 Serve's settings an app ships, and CBASE_ENV_FILE

A few of serve's settings have a default the app may ship in its `cb_app`, in the block "serve's defaults" at the end of `cbase/app.h`.
The order is always the same: cbase's default, then the app's, then the `CBASE_*` variable, which wins whenever it is set and not empty.
Zero or NULL in the field leaves cbase's default, so an app that sets nothing is unchanged, and boot refuses a value out of range with a sentence, even when the variable overrides it.

- `req_workers`: serve's request pool, the threads every `CB_ROUTE_POOL` handler runs on, 1 to 256 (cbase's default 1; `CBASE_REQ_WORKERS`).
  An app whose handlers hold their thread on a remote call sets as many as it wants waiting at once: amgi's Gemini calls hold one for up to about 30 s, so it ships `.req_workers = 12`.
- `slow_workers`: serve's slow pool, the threads every `CB_ROUTE_SLOW` handler runs on, apart from the request pool, 1 to 256 (cbase's default 4; `CBASE_SLOW_WORKERS`).
  Serve starts it only when some route is slow.
  A route marked `CB_ROUTE_SLOW` (a handler that holds its thread on a remote call for seconds or minutes) never holds a request worker, so the fast routes (a status poll, a file) are never queued behind it.
  amgi's Gemini calls are slow routes, so it ships `.slow_workers = 12`, and its fast routes keep cbase's one request worker.
  Under a one-shot platform (Lambda) every handler runs in place and the flag changes nothing.
- `slow_queue`: how many slow requests may wait for a slow worker once every one is busy, 0 to 4096 (cbase's default 0, no queue; `CBASE_SLOW_QUEUE`).
  With none, a slow request starts at once or is refused at once: one that waited would wait for a whole slow handler to end, minutes with nothing on the wire.
- `slow_full`: the answer to a slow request that finds every slow worker busy and the queue full, written as a handler writes one, at once and never queued.
  It runs on the loop thread and must not block; the host and origin checks have passed, and CORS is added after it.
  NULL is cbase's answer: `503` with the text body `server busy` and `Retry-After: 5`.
  A socket's REQ to a slow route is refused the same way (its status and body).
  amgi answers its own wire format's `notStarted` here.
- `log_level`: the least level the log keeps, `"debug"`, `"info"`, `"warn"` or `"error"` (cbase's default `"info"`; `CBASE_LOG_LEVEL`).
  amgi ships `"warn"`.
  The startup line on stdout, `cbase: listening <host>:<port>`, is not the log's and is printed at every level, so a parent that waits for it works whatever the app ships.

Ctrl-C is SIGTERM: serve drains on SIGINT as on SIGTERM (requests in flight finish, WebSockets get 1001), then calls the app's `serve_close` and exits 0 (`core/test/unit_serve_knobs.c` sends it to a real `cbase serve` with a request in flight).

`CBASE_ENV_FILE`, when set, names a file of `KEY=VALUE` lines that `cb_config_load` reads into the environment before anything else is read (`core/src/env_file.h`).
A key the environment already holds, even as an empty string, keeps its value, so the shell's variables override the file.
The app reads its own settings from it as it reads any variable, with `getenv`: amgi's `.env.local` holds `GEMINI_API_KEY` and `GEMINI_API_BASE`.
The format is strict, and what breaks a rule is refused rather than guessed at:

- One setting per line, nothing before the key, and no blank on either side of the `=` (refused, not trimmed).
- A key is a letter or `_`, then letters, digits and `_`, and appears once in the file.
- The value is the rest of the line as written: no shell expansion (`$HOME` stays `$HOME`) and no escapes; it may be empty.
- A value wrapped in a pair of single or double quotes loses the pair and nothing else, and a `#` inside the quotes is part of it (`"abc # x"` is `abc # x`); a value that opens a quote and does not close it at the end of the line is refused.
- In an unquoted value a `#` after a space or a tab starts a comment, as Node's `process.loadEnvFile` reads it, so a `.env.local` written for Node reads the same: `KEY=abc # my key` is `abc`, the blanks before the `#` trimmed. A `#` with no blank before it is part of the value (`a#b`). An unquoted value with no comment that ends in a blank is refused.
- A line that starts with `#` is a comment, a line of nothing or only blanks is skipped, and a `\r\n` line end reads as `\n`.

A file that cannot be read, is larger than 1 MiB, holds a NUL byte or has a line that breaks a rule refuses the boot with a sentence that names the file and the line's number, and nothing from it is set.
`core/test/unit_env_file.c` holds a case for each rule.

### 2.11 Answering with files

`core/src/net/files.h` serves a local web app's shell, assets and media from disk, in four calls a handler makes:

- `int cb_send_file(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res, const char *path, const char *content_type, const char *cache_control)` answers with the file: 200, its Content-Type (`NULL`: by extension), the given Cache-Control, and Content-Length its size; a HEAD gets the head alone, a missing file or a directory 404, an unreadable one 403.
  It returns the status, so a single-page route is one line: `cb_send_file(ctx, req, res, index_html, NULL, "no-store");`.
- `int cb_path_join(const char *base, const char *name, char *out, size_t cap)` turns one name from the request (`cb_route_param`'s, already decoded) into the real path inside `base`, refusing an empty name, `.`, `..`, a `/`, a `\` or a control byte (`CB_E_INVALID`), and anything that resolves outside `base`'s real path, which is what a symlink out of it does (`CB_E_FORBIDDEN`).
- `int cb_serve_dir(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res, const char *dir, const char *rel, const char *cache_control)` is a directory under a prefix route (`"/assets/*"`, `rel` the path below its stem): each segment percent-decoded and held to the same rule, so `%2F`, `%00` and `..` are 404, a symlink out is 403, a directory is 404 and never listed, and only GET and HEAD are answered.
- `const char *cb_mime_type(const cb_app *app, const char *path)`: the app's table `cb_app.mime_types` first (`{ "pack", "application/x-amgi-pack" }`, an extension without its dot, compared without case), then the platform's (html, css and js with `charset=utf-8`, wasm, svg, png, jpeg, gif, webp, ico, txt, json, map, the fonts, mp3, wav, ogg, m4a), else `application/octet-stream`.

A file is opened per request and never cached, so a rebuild on disk is what the next request gets.
Under serve's HTTP/1.1 the body is never held: serve writes it from the descriptor 256 KB at a time as the socket takes it (`net/server.c`), so a 40 MB file costs one chunk of memory, and a request pipelined behind it waits for its last byte.
On any other transport (a one-shot platform's event, a socket's REQ), which `cb_http_req.stream_ok` tells apart, the same call reads the file whole into the body.
Not done, on purpose: Range and Accept-Ranges (Safari's audio wants them later; the place is `cb_send_file`, answering 206 from the same descriptor), ETag, compression and listings.
`example/test/unit_web.c` serves amgi's shape over a real serve: every type, the single-page routes, every traversal shape, HEAD, a 6 MB and a 40 MB file streamed whole (the servers' resident peak stays a few MB), and a pipelined request.

## Appendix: how cbase came to be

cbase began on 2026-10-13 as the backend of foolish, a card game, written in C to replace Supabase, and ran it in production on AWS Lambda and DynamoDB from 2026-10-14, then on one small VM under `cbase serve` from 2026-10-19.
From 2026-10-17 it was split from the inside: everything that knew the game moved behind this API, a second axis put each cloud behind a provider (`providers/local`, `providers/aws`, and a `providers/gcp` slot), and gates were added so that neither side could name the other again (the name gate, the compiler gate, the symbol gate).
On 2026-11-08 it left the monorepo as a repository of its own, which products take as a submodule.
The design of that split, stage by stage, is [docs/history/PLATFORM_SPLIT.md](history/PLATFORM_SPLIT.md); foolish's own records are in its repository, https://github.com/alexsave/foolish.
