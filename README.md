# cbase

cbase is a backend platform written in C that an app links into its own server binary.
It has a core (HTTP/1.1 ingress, the router and handlers, auth, realtime over WebSocket, jobs and schedules, an admin console), providers chosen at link time, client SDKs over one C protocol core, and an outbound HTTP/2 and gRPC client.
It is taken a la carte: a product links only the parts it needs, and build gates prove that nothing else got in.

- **Core** (`core/`): the server, the app API (`core/include/cbase/app.h`), auth, realtime, jobs, schedules, the admin console, and child processes run from the server's loop (`core/src/spawn.h`).
- **Providers** (`providers/`): `local` is one process on SQLite (`cbase serve`), which can also run storeless, with the app keeping its own data; `aws` is Lambda, DynamoDB and IoT Core.
- **Client** (`client/`): a sans-IO C core (`client/include/cbase/client.h`) with a POSIX host, a wasm build with TypeScript over it, and Swift over it.
- **HTTP/2 and gRPC** (`core/src/net/h2.h`, `core/src/net/grpc.h`): unary gRPC calls from C with no protobuf library, usable on their own ([docs/H2.md](docs/H2.md)).

## Status

The current release is the one the `VERSION` file names, and its tag is `v<VERSION>`.
Its first consumer is amgi, a local flashcard app that runs `cbase serve` on the user's own machine.
The API is pre-1.0 and will change.
Breaking changes, and how to upgrade a product across them, are recorded in [docs/PLATFORM.md](docs/PLATFORM.md) section 2.9.
The cdom a browser app pairs with this cbase is the commit [client/hosts/cdom/README.md](client/hosts/cdom/README.md) names under "The cdom pin", at the same cbase commit.

## Architecture

Five pictures of how the parts fit, each drawn from the files its caption names.

### A request becomes a handler call

```
  client              LOOP THREAD (one; owns every socket)
    |
    | connect     on_accept: accept(), non-blocking, TCP_NODELAY,
    +-----------> cb_loop_add(fd, READ)
                         |
                  wait: kqueue on macOS and the BSDs, epoll on Linux
                  (io_uring with CBASE_IO_URING=1); each watch carries
                  the loop's generation, and a ready event whose tag
                  is stale (its fd removed and reused) is skipped
                         |
                  conn_io -> conn_try_parse: cb_http_parse
                         |
                  dispatch: cb_router_match picks the route
        +----------------+-------------------+
        |                |                   |
  CB_ROUTE_LOOP,    default:            CB_ROUTE_SLOW:
  OPTIONS or no     socket paused,      socket paused,
  route: runs       REQUEST POOL        SLOW POOL (bounded;
  right here,       worker runs it      full: 503 at once)
  must not block         |                   |
        |                +--cb_loop_post-----+
        |                |   back to the loop thread
        +----------------+
                         |
                  finish: cb_res_serialize into the socket's out
                  buffer; at the end of the loop pass flush_dirty
                  makes one send() per socket, pushes included
                         |
                  a file body (cb_send_file): the fd is kept and
                  pread() 256 KiB at a time as the socket drains
```

The loop is `core/src/net/loop.c` over `core/src/net/poller.c`, the connection and dispatch code is `core/src/net/server.c`, where a handler runs is `core/src/net/router.h` and `core/src/handler.c` (which also answers CORS, 404 and 405), the pools are `core/src/util/pool.c` (started in `core/src/ingress_process.c`), and file bodies are `core/src/net/files.c`.
A `CB_ROUTE_INLINE` route is a pool route that may run on the loop thread instead while the server's time budget allows (`cb_server_inline_ok` in `core/src/net/server.c`).

### The WebSocket and its pushes

```
  GET /ws (CB_ROUTE_LOOP) -> cb_ws_upgrade -> 101 written -> the
                                              socket is a cb_ws
  frames in (one message is one lib/stream_frame.h frame):
    SF_SUB, SF_UNSUB, SF_PING -> answered on the loop thread
    SF_HELLO, SF_REQ          -> request pool (slow pool for a
                                 CB_ROUTE_SLOW path), the reply
                                 posted back with cb_loop_post
  a push, from any thread:
    cb_rt_publish -> queue -> cb_loop_post -> on the loop thread,
    cb_ws_send to each socket that hears it -> flushed with the
    pass's other writes, one send() per socket
```

The socket hub is `providers/local/src/realtime/rt_local.c`, the frame codec `core/src/net/ws.c`, and the upgrade and the write path `core/src/net/server.c`.
An `SF_REQ` runs the route its HTTP path would, and a `CB_ROUTE_INLINE` one may run on the loop thread as its HTTP request may.

### One wire, shared by the server and every client

```
  C headers both ends compile (little-endian binary, never JSON)
  +--------------------------------------------------------------+
  | client/include/cbase/auth_wire.h  /v1/auth/* bodies, refusal |
  | lib/stream_frame.h                socket frames SF_*         |
  +--------------------------------------------------------------+
        |                                      |
        v                                      v
  SERVER                               CLIENT CORE, sans-IO
  core/src/auth/auth_http.c            client/src/client.c
  providers/local/src/realtime/        (cbase/client.h: bytes and
    rt_local.c                          time in, bytes out; no
                                        socket, clock or malloc)
                                               |
          +---------------------+--------------+------+
          |                     |                     |
  SHARED HOST (C)          wasm build            linked as C
  client/hosts/common      client/wasm           into an iOS app
     |          |               |                     |
  posix host  cdom host    TypeScript host       Swift host
  (a native   (a cdom page (client/ts/           (client/swift;
   C loop)     in wasm)     socket.ts)            HTTP half so far)

  tools/structgen reads cbase/client.h through libclang and writes
  the accessors TypeScript (client_layout.ts) and Swift
  (ClientLayout.swift) read the core's structs with: no host
  spells a byte offset
```

The headers are `client/include/cbase/client.h`, `client/include/cbase/auth_wire.h` and `lib/stream_frame.h`; the hosts are `client/hosts/common/cbc_host.h`, `client/hosts/posix/cbc_posix.h`, `client/hosts/cdom/cbc_cdom.h`, `client/ts/socket.ts` and `client/swift/CBaseClient/CbClient.swift`.
The structgen runs are in `client/Makefile` (`build/layout.hash` and `swift`); the TypeScript and Swift hosts drive the core directly, not through the shared host.

### The store

```
  handler (on a request pool thread)
     |
     | store.h wrappers over cb_store_ops, safe from any thread
     v
  providers/local: store_sqlite.c
     one connection (SQLITE_OPEN_FULLMUTEX), each operation
     under `mu`; PRAGMA journal_mode = WAL, synchronous = NORMAL,
     a 5 s busy timeout
     |
     +-- reads: rows collected under `mu`, handed out after
     |
     +-- writes: cb_sql_write, a GROUP COMMIT: the first writer
     |   to find no group running leads it: BEGIN, a SAVEPOINT
     |   per queued write, COMMIT; every caller returns after
     |   that COMMIT (no thread of its own)
     |
     +-- WAL hook --wakes--> CHECKPOINTER THREAD (its own
                             connection; PASSIVE checkpoints
                             while the writers keep committing)

  storeless app (cb_app.storeless): no store is opened, no
  accounts, no auth routes; the app keeps its own data
```

The store interface is `core/src/store/store.h`, the SQLite backend `providers/local/src/store/store_sqlite.c` (its WAL, group commit and checkpointer comments), and storeless boot `core/src/boot.c` and `core/src/ingress_process.c`.
`cb_store_side_start` (`core/src/store/store.c`) gives a handler a thread of its own for a second independent read; DynamoDB (`providers/aws`) is the other backend behind `store.h`, parked for now.

### The deployment shape

```
  browser --> CloudFront: one distribution per app; by default
              /api/* goes to the VM and * to the static files
              (CBASE_ROUTE lines replace the defaults)
                  |                                |
                  | /api/*                         | *
                  v                                v
  VM GROUP: one EC2 instance,        STATIC GROUP: one S3 bucket,
  tag cbase:vm-group=<g>             tag cbase:static-group=<g>
  +------------------------------+   +-------------------------+
  | cbase-shop.service    :8700  |   | shop/   blog/   site/   |
  | cbase-blog.service    :8701  |   | (a folder per app)      |
  | one systemd service and one  |   +-------------------------+
  | port (8700-8799) per app     |
  +------------------------------+
  members: s3://cbase-state-<account>/vm/<g>.txt and static/<g>.txt
```

The tool is `tools/cbase` with its parts in `tools/cbase.d/`; the "./cbase deploy tool" section below has the whole flow.

## Quick start

Requirements:

- macOS: Xcode's clang and `brew install openssl@3 sqlite` (Homebrew's SQLite, not Apple's system one).
- Ubuntu 24.04: `sudo apt-get install build-essential libssl-dev libsqlite3-dev`.

Nothing else is needed, and the build fetches nothing.
An app with a page on cdom (`example/minimal/`) also needs a clang that targets wasm32, which Xcode's does not: on macOS `brew install llvm`, which cdom's `cdom.mk` finds at `/opt/homebrew/opt/llvm/bin/clang` (`WASM_CC=<clang>` names another).

```
git clone https://github.com/alexsave/cbase.git
cd cbase
make -j2 check
```

`make check` builds everything, runs every unit test and smoke, the client's tests and the repository's gates, then builds the example app again as a local-only product in a stripped tree with the network denied and runs its checks there.
`make` alone builds `build/cbase`, which is `cbase serve` with the example app (`example/`) linked in.

Run it in development mode:

```
CBASE_DEV=1 make serve
```

`make serve` builds `build/cbase` and runs it as `cbase serve` from the repository root, on `127.0.0.1:8642` (`PORT=4100` for another port, `CBASE_LISTEN=127.0.0.1:0` for one the OS picks), with its database in `./cbase.sqlite`.
8642 is the default because the common dev servers' ports (3000 for React, Next and Rails, 5173 for Vite, 8000, 8080 and the rest) are taken, and `/etc/services` assigns it nothing.
Once the socket is bound, serve prints exactly one line to stdout, such as `cbase: listening 127.0.0.1:8642 http://127.0.0.1:8642/`, for a person to click or a parent process to read (the address is its third word); the log goes to stderr.
Then:

```
curl http://127.0.0.1:8642/health
curl http://127.0.0.1:8642/v1/hello
```

Settings are `CBASE_*` environment variables, listed in `core/src/config.h`.
A start logs one `config:` line at info (the app, the provider, the store, the listen address), and with `CBASE_LOG_LEVEL=debug` every setting on a line of its own, secrets redacted; warnings and errors are logged at every level.
`CBASE_ENV_FILE=.env.local` reads `KEY=VALUE` lines from that file first, for the app's secrets as well as `CBASE_*` settings; a variable already in the environment wins, an unquoted value's ` # comment` is dropped as Node drops it (`KEY=abc # note` is `abc`, `a#b` stays `a#b`, a quoted `"a # b"` keeps it), and a bad line refuses the start, naming its number (the format is in docs/PLATFORM.md 2.10).
An app may ship its own defaults for a few settings (`cb_app.req_workers`, `cb_app.log_level`); the variable still overrides them.
`CBASE_DEV=1` allows a random JWT secret per process and a public URL derived from the listen address.
Outside it, an app that keeps the platform's store needs `CBASE_JWT_SECRET` (at least 32 characters), which signs its users' tokens, and `CBASE_PUBLIC_URL`, the tokens' issuer.
A storeless app (`.storeless = true`) has no accounts, so nothing signs or checks a token, and it needs neither: it runs outside dev mode with no setting at all (`core/src/config.h`, `cb_config_check_serve`; `core/test/smoke_boot.c` boots one so).
One that is set is still checked: a secret shorter than 32 characters, or a public URL naming `0.0.0.0`, refuses the start.
`CBASE_ADMIN_TOKEN` (at least 32 characters) opens the console at `/admin`.

## Using it in a product

A product with a browser page on cdom starts from the two starters and follows [A cdom page on a cbase server, from the two starters](client/hosts/cdom/README.md#a-cdom-page-on-a-cbase-server-from-the-two-starters): one Makefile, the page served at `/`, the client call, a query, the make targets and a test in Chrome, worked through `example/minimal`.

### Start a new server

```
make -C <cbase> new DIR=<path>
```

`DIR` must not exist or be empty, and is taken from where you ran make.
It gets a `Makefile` (`CBASE`, `APP_SRC_ROOTS` and `APP_SERVE`, then `include $(CBASE)/cbase.mk`), `src/server.c` (a storeless app whose one route, `GET /`, answers `cbase`, beside the platform's `GET /health`) and a `.gitignore` (`build/`).
The server is named for `DIR`: its base name, with every byte but `A-Z a-z 0-9 _ . -` made `_`, is the `cb_app`'s `.name` and `APP_SERVE`, so `make` builds `build/<name>`.
A name `cbase.mk` keeps for `build/`'s own directories and tools (`CB_BUILD_OWN`, such as `obj` and `test`) gets `-server` after it, and `cbase.mk` stops a product that names its program one of them.
`CBASE` is the path to this checkout as `tools/new.sh` writes it: relative to `DIR` (`../cbase`) when the two share a directory other than `/`, and absolute when they share only `/` (a `DIR` under `/tmp`, which is `/private/tmp` on macOS, with cbase under `/Users`).
It is a plain `CBASE := <path>` line: edit it to move cbase, or override it for one run on the command line, `make CBASE=<path> serve`.
With `GIT=1`, `DIR` is also a git repository with cbase as its submodule at this commit, and `CBASE` is `cbase`.
Then `make serve` there builds the server and prints the URL it listens at.
The server ships no test: `tools/new_test.sh`, part of cbase's `make check`, builds it, serves it and asks it.

### A page on this server

The starter's `GET /` answers `cbase` until a page takes `/` over.
For a page on [cdom](https://github.com/alexsave/cdom), run cdom's starter too (`make -C <cdom> new DIR=<path>`) and follow [A cdom page on a cbase server, from the two starters](client/hosts/cdom/README.md#a-cdom-page-on-a-cbase-server-from-the-two-starters), which walks through [example/minimal/](example/minimal/), the app to copy.

### By hand

Add cbase as a submodule at `cbase/`, by its https URL so that anyone can clone the product:

```
git submodule add https://github.com/alexsave/cbase.git cbase
```

The product's Makefile sets a few variables and includes `cbase.mk`, whose header documents every variable a product may set:

```make
CBASE           := cbase   # required: the cbase root
CBASE_PROVIDERS := local   # local, aws, or "local aws"
APP_SRC_ROOTS   := src     # every .c under these is the product's
APP_TEST_ROOTS  := test    # its unit_*.c, lib_*.c and smokes
APP_INC         := -Isrc   # the product's include flags
SERVER_CFLAGS   :=         # flags for every server object, the platform's too
SERVER_LDFLAGS  :=         # more libraries and link flags for every link
APP_LIB_SRC     :=         # single sources outside the roots, linked into every program
include $(CBASE)/cbase.mk
```

`C_ROOT` is the directory every source file outside the Makefile's own directory lives under; the build mirrors those files under `build/obj/<variant>/_root/` rather than following `..`.
The default (`$(CBASE)`) fits a product with cbase as a submodule; set it only when the Makefile compiles files outside both its own directory and cbase, as amgi's `server/` does with `..`.

`SERVER_CFLAGS` and `SERVER_LDFLAGS` are the native server's flags; `APP_CFLAGS` and `APP_LDFLAGS` are cdom.mk's, for a browser UI's wasm module, so one Makefile can include both fragments and give each side its own.
There are no file lists to edit: a new `.c` under a source root is part of every program, and a new `unit_*.c` under a test root is a new test.
`make`, `make check` and `make check-asan` then work in the product as they do in cbase.

### The targets cbase.mk owns

`cbase.mk` defines these targets in the product's Makefile, each with its recipe:

- `all`, the default goal (`.DEFAULT_GOAL := all`, whatever the product's first rule): the serve binary (the Lambda bootstrap too with the aws provider) and every program `APP_PROGS` names;
- `test` (the compiler gate, `lib/`'s tests and every unit test), `smoke`, and `check` (the two together, and the client's check);
- `check-asan`, `test-asan`, `smoke-asan`, `coverage`, `bench`, `serve`, `clean`, and the `fuzz` targets (`fuzz/fuzz.mk`).

A product never writes a recipe for one of them: a second `test:` with a recipe draws make's `warning: overriding commands for target 'test'`, and the recipe read last replaces the other, so either cbase's tests or the product's stop running.
A product adds its own work under a name of its own and joins it to cbase's target with a prerequisite line, which has no recipe and draws no warning, before or after the `include`:

```make
.PHONY: e2e
e2e: build/$(APP_SERVE)
	bash test/e2e.sh
check: e2e          # make check runs it too
```

`test: e2e` joins it to `make test` the same way, and `serve: <file>` builds something before `make serve` (below).
A unit test or a smoke needs no line at all: a `unit_*.c` under `APP_TEST_ROOTS` is one, and `APP_SMOKES` names the smokes.
cbase's own `Makefile` and `example/Makefile` add their checks with `check:` lines.

### Running it: `make serve`

`make serve` builds the product's serve binary (`build/$(APP_SERVE)`) through its usual rule and then runs it as `serve`, so every product has the same run line and none writes its own launcher.
It needs the local provider in `CBASE_PROVIDERS`, and `tools/serve.sh` is the one place that says how it runs:

- It runs from `APP_SERVE_DIR`, where serve reads the app's files from its working directory; the default is the directory of the Makefile make read.
  A product whose Makefile sits in `server/` and whose files are at the repository root sets `APP_SERVE_DIR := ..`.
- When `APP_ENV_FILE` exists (default `.env.local`, from `APP_SERVE_DIR` unless absolute) and `CBASE_ENV_FILE` is not already set, it is named to serve as `CBASE_ENV_FILE`; an absent file is no error, and an empty `APP_ENV_FILE` names none.
  The script never reads the file: serve reads it strictly, so a bad line refuses the boot with serve's own sentence naming the line.
- `CBASE_LISTEN` is `APP_SERVE_LISTEN` unless the environment sets it; the default is `127.0.0.1` on `PORT`, and without one serve's own default, `127.0.0.1:8642` (`CB_LISTEN_DEFAULT` in `core/src/config.h`, the one place it is written).
  A product that wants another sets its own in its Makefile (`APP_SERVE_LISTEN := 127.0.0.1:4100`), or a run sets one: `PORT=4100 make serve`, or `CBASE_LISTEN=127.0.0.1:0 make serve` for a free port the OS picks.
  Once the socket is bound, serve prints where on stdout, one line: `cbase: listening 127.0.0.1:<port> http://127.0.0.1:<port>/` (`core/src/ingress_process.h`).
- The binary is exec'd, so make's child is the server itself: Ctrl-C or a SIGTERM sent to it reaches serve, which drains and exits.

```make
APP_SERVE     := myapp     # build/myapp
APP_SERVE_DIR := ..        # run from the repository root, not server/
include $(CBASE)/cbase.mk
```

`make -C server serve`, `PORT=4100 make -C server serve` and `CBASE_LISTEN=0.0.0.0:8080 make -C server serve` then all work, and `tools/serve_test.sh` (part of `make check`) holds each rule against the example.

A product extends `make serve` with lines of its own, and needs no knob for it:

```make
serve: ui/out/app.wasm ui/out/index.html   # built before serve runs (a page the server reads)
serve: export CBASE_DEV := 1                 # in serve's environment (a local dev app)
```

The first adds prerequisites, which make builds before the run; the second is a target-specific exported variable, which make puts in the environment of serve's recipe, and so in the server's.
Either goes before or after the `include`, and `tools/serve_test.sh` holds both.
`tools/serve.sh` and its arguments are cbase.mk's internal interface: a product never calls it, so its own `start` rule is `start: serve` with the lines above, never a copy of serve's recipe.

The product defines one const struct, `cb_app`, and points `cb_main_app` at it (`core/include/cbase/app.h`).
A whole product can be one file, such as `src/myapp.c`:

```c
#include "cbase/app.h"   // cb_app, cb_route, cb_main_app
#include "net/http.h"    // cb_http_req, cb_http_res, cb_res_text, cb_http_query_find
#include "util/err.h"    // CB_E_NOT_FOUND, CB_E_FULL, CB_E_INVALID

#include <stdio.h>

// GET /v1/hello?name=Ada answers "hello, Ada"; with no name, "hello, world".
static void h_hello(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    char name[64], text[96];
    // The value's length; 0 for "?name=", CB_E_NOT_FOUND for no name at all: both greet the world.
    int n = cb_http_query_find(req, "name", name, sizeof name);
    if (n == CB_E_FULL || n == CB_E_INVALID) {   // longer than name holds; a query that does not read
        cb_res_text(res, 400, "bad name\n");
        return;
    }
    snprintf(text, sizeof text, "hello, %s\n", n > 0 ? name : "world");
    cb_res_text(res, 200, text);
}

static const cb_route ROUTES[] = {
    { .method = "GET", .pattern = "/v1/hello", .flags = CB_ROUTE_LOOP, .fn = h_hello },
};
static const cb_route_list ROUTE_LIST = { ROUTES, (int)(sizeof ROUTES / sizeof ROUTES[0]) };
static const cb_route_list *const ROUTE_LISTS[] = { &ROUTE_LIST };
static const char *const ORIGINS[] = { "http://localhost:5173" };

static const cb_app APP = {
    .name = "myapp",
    .routes = ROUTE_LISTS, .n_route_lists = 1,
    .cors_origins = ORIGINS, .n_cors_origins = 1,   // CORS is denied unless listed
    .storeless = true,                              // optional: the app keeps its own data
};
const cb_app *const cb_main_app = &APP;
```

A handler is `void h(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res)` (`cb_handler`, `core/src/net/router.h`), and answers with the `cb_res_*` calls in `core/src/net/http.h` (`cb_res_text`, `cb_res_send`, `cb_res_header`).
`CB_ROUTE_LOOP` runs the handler on the server's one loop thread, which serves every socket, so it must never block (no store, no lock another thread holds, nothing that waits); a route without it (`CB_ROUTE_POOL`, 0) runs on the request pool, where it may block.
Every `CB_ROUTE_LOOP` handler runs on that one thread, one at a time, so a counter only they touch needs no lock; pool routes run on several workers at once (a `CB_ROUTE_INLINE` one on the loop thread too), so state one shares needs a lock or an `_Atomic` (`core/src/net/router.h`, "WHERE A HANDLER RUNS").
The `CB_E_*` codes are in `core/src/util/err.h`.
Serving a page: `core/src/net/files.h` answers with a file (`cb_send_file`) or a directory under a prefix route (`cb_serve_dir`), read when asked for, so a rebuilt page needs no restart.
A route pattern is exact, a prefix (`/v1/items/*`) or has path parameters (`/decks/:id`).
Jobs, schedules, admin panels, metrics and the store's rows are further fields of the same struct.
A storeless app opens no platform store, and opens its own in the `serve_open` hook ([docs/PLATFORM.md](docs/PLATFORM.md) 2.7).

[example/](example/) is the template to copy: a two-route app with a job and a schedule (`example/src/example_app.c`), its Makefile (`example/Makefile`, local provider only, with the symbol gate as its link check), a smoke and a storeless test.
A C or wasm client includes `client/client.mk` and adds `$(CBC_SRC)` and `$(CBC_INC)` to its build.
Which client mode: an app with no accounts sets `cbc_cfg.anonymous`, and its calls go over plain HTTP, with a socket only while it holds a room; an app with accounts leaves it 0 and signs in (`cbc_sign_in`), and its calls ride the socket once a user is signed in (`client/include/cbase/client.h`, `cbc_cfg`).

### The smallest app: a server and a page that calls it

[example/minimal/](example/minimal/) is the smallest whole app, server and browser page, and the skeleton to copy for one: one Makefile, a server with one route, and a page in C on [cdom](https://github.com/alexsave/cdom).
[A cdom page on a cbase server, from the two starters](client/hosts/cdom/README.md#a-cdom-page-on-a-cbase-server-from-the-two-starters) walks through it from the two starters.
Its Makefile includes four fragments, each once and in this order:

```make
include $(CBASE)/cbase.mk                     # the server
include $(CDOM)/cdom.mk                       # cdom's sources, flags and page rules
include $(CBASE)/client/client.mk             # the client core
include $(CBASE)/client/hosts/cdom/adapter.mk # the cdom adapter
```

`cbase.mk` comes first because `cdom.mk` turns on `.SECONDEXPANSION` for every rule read after it, and cbase's rules are tested without it; `adapter.mk` comes last because it needs `cdom.mk`'s and `client.mk`'s variables, and stops, naming the fix, when either is missing.
The server is one `cb_app` with `GET /api/hello` answering `hello from cbase` (`example/minimal/server/minimal.c`).
The page is cdom's default page, with the wasm module bundled into it, so the server serves one file at `GET /`.
`make -C example/minimal serve` builds both and runs the server, outside dev mode with no setting at all (it is storeless); open the URL its startup line prints.
cbase's CI builds it against the cdom pin and checks it as a person runs it (`example/minimal/check.sh`).

The client call, in C (`example/minimal/page/page.c`, on the cdom adapter, `client/hosts/cdom/cbc_cdom.h`):

```c
static cbc_cdom client;   // static and zeroed: the handle lives for the page

static void on_event(void *ctx, cbc *c, const cbc_event *e, const uint8_t *bytes) {
    if (e->kind == CBC_EV_DONE && e->status == 200 && bytes)
        show((const char *)bytes + e->off, e->len);   // the page's own: "hello from cbase\n"
}

// once, at start:
cbc_cfg cfg;
cbc_cfg_default(&cfg);
cfg.anonymous = 1;   // no accounts: every call is plain HTTP on the page's origin
cbc_cdom_start(&client, &cfg, NULL, on_event, NULL, NULL);
cbc_call(cbc_cdom_client(&client), CBC_GET, CBC_C_IDEMPOTENT | CBC_C_FAIL_FAST, "/api/hello", 10, NULL, 0, cbc_cdom_now());
cbc_cdom_pump(&client);   // after every call into the client
```

`CBC_C_IDEMPOTENT` says the call is safe to send again, and `CBC_C_FAIL_FAST` that a server that is down is reported at once (`client/include/cbase/client.h`).
`cbc_call` sends its path as given (the parameter is `encoded_path`), so a query is built with `cbc_query_add`, which percent-encodes each key and value and adds `?` or `&`:

```c
char path[CBC_PATH_MAX] = "/v1/hello";
size_t len = 9;
if (cbc_query_add(path, sizeof path, &len, "name", name, name_len) == 0)   // CBC_E_FULL when it does not fit
    cbc_call(cbc_cdom_client(&client), CBC_GET, CBC_C_IDEMPOTENT | CBC_C_FAIL_FAST, path, len, NULL, 0, cbc_cdom_now());
```

A raw value would break on its first `&`, `=`, `+` or `#`, which the browser's `fetch` does not encode.

- TypeScript: coming soon.
- Swift: coming soon.

## ./cbase deploy tool

`tools/cbase` deploys an app: its service on a VM, its static files in a bucket folder, and a CloudFront distribution with its routes, all from a `cbase.conf` in the app's repository.
It is a bash script that runs straight from the app's cbase submodule, with no build step and no provider linked: `cbase/tools/cbase <command>`.
An app that wants the short spelling keeps a one-line wrapper at its root, `./cbase`:

```
#!/bin/sh
exec "$(dirname "$0")/cbase/tools/cbase" "$@"
```

It needs bash (macOS's own 3.2 is enough) and AWS CLI v2, and every AWS call it makes is one AWS CLI call read with `--query ... --output text`, so the tool parses no JSON.

The commands:

- `cbase/tools/cbase group-init vm|static GROUP [--type TYPE] [--yes]`: makes a group, the one command that makes an instance or a group's bucket.
- `cbase/tools/cbase deploy [--dry-run] [--accept-drift] [--prune]`: the app's service on its group's VM, its static files, and its distribution.
- `cbase/tools/cbase undeploy [--delete-files] [--delete-data]`: the app's service and distribution gone, its files and data kept unless a flag says otherwise.
- `cbase/tools/cbase group-destroy vm|static GROUP [--delete-data] [--yes]`: an empty group's stack deleted.
- `cbase/tools/cbase apps [GROUP]`: a group's members, with each one's port, service and memory.
- `cbase/tools/cbase doctor` and `config check`: the config, the account, the groups.
- `cbase/tools/cbase template` and `group-template vm|static GROUP`: the stacks deploy and group-init make, printed with no AWS call.
- `cbase/tools/cbase state join` and `state leave`: the membership step deploy and undeploy run, for repair.

The tool is complete; what remains is phase 4's polish of these docs.

### cbase.conf

A full app is one line: its own VM and bucket, `/` from the bucket and `/api/*` to the VM, at its distribution's `cloudfront.net` address.

```conf
CBASE_APP = shop
```

A static site has no VM:

```conf
CBASE_APP = site
CBASE_VM = none
```

A real domain is one more line; deploy finds an issued certificate for it or requests one:

```conf
CBASE_APP = shop
CBASE_DOMAIN = shop.example.com
```

The file is one `KEY = value` per line, committed with the app, and a line starting with `#` is a comment.
An unknown key or a bad value stops the tool with the line's number and a sentence; `tools/cbase.d/conf.sh` documents every rule.

`CBASE_APP` is the one required key: the app's name, `[a-z][a-z0-9-]*`, at most 32 characters.
It names the app's service (`cbase-<app>.service`), its port claim and its bucket folder (`<app>/`).

Optional, each with its default:

- `CBASE_VM = <group> | none`: the VM group the app runs in; default `CBASE_APP`, its own; `none` makes a static site.
- `CBASE_STATIC = <group> | none`: the static group (a bucket) its files go to; default `CBASE_APP`; `none` makes a VM-only app (not both `none`).
- `CBASE_MEMORY = <n>M | <n>G`: the memory a VM app declares, 16M to 64G; default undeclared, so the service has no hard cap and the group's fit check counts it at a conservative 128M and says so ("shop: memory not declared, counted as 128M"); declared, it is a real cap and an exact count.
- `CBASE_ROUTE = <pattern> vm|static|ws [cache|nocache] [ALL|GET,HEAD,...]`, repeated, matched in order; default `/api/* vm` and `* static` for an app with both, `* static` for a static site, `* vm` for a VM-only app.
  Routes are written only to differ from the default, and any `CBASE_ROUTE` line replaces the defaults rather than adding to them; written routes with no `*` route get `* static` (or `* vm`) last, since CloudFront needs a default behaviour.
  `ws` is the VM uncached, every method, long timeouts; a route's cache defaults to `cache` for static and `nocache` for vm, its methods to `GET,HEAD` for static and `ALL` for vm.
- `CBASE_DOMAIN = <host>`: the app's hostname; default none, and the app is at its `cloudfront.net` address.
- `CBASE_API_HOST = <host>`: a second hostname whose every request goes to the VM; default none.
- `CBASE_CERT_ARN = arn:aws:acm:us-east-1:...`: an override; default, deploy finds an issued us-east-1 certificate covering the hostnames (wildcards and SANs included) or requests one with DNS validation.
- `CBASE_ZONE = Z...`: an override; default, deploy finds the account's Route 53 hosted zone that covers the domain and writes the records there, or with none prints the records to add.
- `CBASE_STATIC_DIR = <dir>`: the directory deploy uploads; default `build` for a static site, `ui/out` for an app with a VM.
- `CBASE_HASHED = <glob> ...`: the content-hashed files, cached for a year; default none.
- `CBASE_SHELL = <path>`: the app shell, the page of the build served with a 200 for every path that is neither a file nor a page; default `/index.html`.

The file is public, so the tool refuses any line that looks like a secret, naming the line and never echoing it: a key named with SECRET, TOKEN, PASSWORD, PASSWD, PRIVATE or CREDENTIAL, an AWS access key id, a PEM block, or a long random-looking string.

### Groups and the state file

A group is found by its tag through the Resource Groups Tagging API: `cbase:vm-group=<name>` on its one EC2 instance, `cbase:static-group=<name>` on its one bucket.
The tool never creates one: a missing group stops with the `./cbase group-init` command that makes it.
Who is in a group is its state file alone, `s3://cbase-state-<account>/vm/<group>.txt` or `.../static/<group>.txt`, plain text:

```
cbase-state 1 vm shop
app=shop port=8700 service=cbase-shop.service mem=~128

cbase-state 1 static shop
app=shop folder=shop/
```

`mem` is the declared MiB, or `~128` for an app that declares none (the estimate it is counted at).
A join or leave reads the file with its ETag and writes it back with S3's `If-Match` (`If-None-Match: *` for a new file).
A 412 or 409 means another run wrote first: the file is read again and the change made again, up to 8 times, with a fixed backoff (no randomness: `tools/cbase.d/state.sh` says why).
A VM app claims the lowest free port in 8700-8799 in the same write, so two racing deploys never hold one port.

### Making a group: group-init

A group is one CloudFormation stack in the CLI's region, and `group-init` is the only command that makes one; deploy never makes a VM or a bucket.

```
cbase/tools/cbase group-init vm shop --yes
cbase/tools/cbase group-init static shop
```

`group-init vm GROUP` makes `cbase-group-vm-<group>`: one EC2 instance tagged `cbase:vm-group=<group>`, a t4g.nano (arm64) by default on the newest Amazon Linux 2023, or the arm64 type `--type` names.
It has an Elastic IP, so its public DNS name survives a stop, IMDSv2, an encrypted 8 GB disk, and an instance profile with SSM's managed-instance policy, read of the state bucket's `artifacts/` and of the SSM parameters under `/cbase/`.
Its security group opens the apps' ports, 8700-8799, to CloudFront's origin-facing managed prefix list and nothing else: there is no SSH and no reverse proxy, and each app's port is its own CloudFront origin.
A VM costs money, so the change set is printed with its cost class ("a t4g.nano runs about $3/month") and runs only with `--yes` or `yes` typed at the terminal.

`group-init static GROUP` makes `cbase-group-static-<group>`: the bucket, tagged `cbase:static-group=<group>`, versioned, with public access blocked, and kept if its stack is deleted.
Its one bucket policy is set there and nowhere else: `s3:GetObject` and `s3:ListBucket` (so a missing file is a 404) for CloudFront, on `aws:SourceAccount`.

The first `group-init` also makes the state bucket `cbase-state-<account>`, and each writes its group's empty state file.
A re-run is an update through the same change set and guards as a deploy: drift stops it, and a change set that would replace or remove the bucket, or the instance with every app's data on its disk, is refused.
A re-run keeps the instance's image, and its type unless `--type` names another.

### Deploying a static site

A static site is two lines of `cbase.conf`, plus its static group, which `cbase/tools/cbase group-init static site` makes once.

```conf
CBASE_APP = site
CBASE_VM = none
```

`cbase/tools/cbase deploy` then, in order:

1. finds the static group's bucket by its tag, and stops with the `group-init` command when there is none (deploy never creates a group);
2. reads the static dir, `CBASE_STATIC_DIR`, which must hold `index.html`;
3. finds or makes the certificate when there is a `CBASE_DOMAIN` (below);
4. makes a change set of the app's own stack, `cbase-app-<app>` in us-east-1, prints it, and holds it to the safety rules (below);
5. joins the static group's state file, which records the app's folder, `<app>/`;
6. uploads the files into `s3://<bucket>/<app>/`: the content-hashed files first, then the rest, the html last, so no page names a file that has not landed;
7. with `--prune` (or `CBASE_PRUNE=1`), deletes what the folder holds and this build does not, and never a key outside `<app>/`, whatever the bucket's listing says;
8. executes the change set and waits, invalidates `/*` on a stack that existed, writes the hostname's Route 53 alias, and prints the URL.

`CBASE_STATIC_DIR = <dir>` is the directory deploy uploads, relative to the repository's root; its default is `build` for a static site (what cdom's `make new` builds) and `ui/out` for an app with a VM (the starter's page, so a server binary under `build/` is never published).
A path with a part that starts with `.` is not uploaded (`.git`, `.DS_Store`), but `.well-known/`.
`CBASE_HASHED = <glob> ...` names the content-hashed files (`CBASE_HASHED = assets/*`; `*` crosses `/`), which are cached for a year, immutable; its default is none.
Each file's Content-Type comes from one table, `tools/cbase.d/static_rules.tsv`, by extension (`.wasm` is `application/wasm`).
Its Cache-Control is `no-cache` for html (a browser and CloudFront revalidate it, so a deploy is seen at once), `public, max-age=31536000, immutable` for a hashed file (never html), and `no-cache` for the rest.

The stack holds one distribution, its Origin Access Control and a CloudFront Function that routes every app like an SPA.
A path with an extension, or under `/.well-known/`, is the file itself, and a missing one is S3's 404, never the shell, so a broken asset link fails loudly.
A clean URL of a page in the build is that page (`/about` is `about.html`, `/docs/` is `docs/index.html`), from the page list deploy renders into the function.
Any other path (`/abc123`, `/game/7`) is the app shell, `CBASE_SHELL` (default `/index.html`), with a 200, and the app routes it client-side and shows its own 404.
Its default behaviour is the bucket, read through the Origin Access Control with `OriginPath /<app>`, so an app reads its own folder and nothing else.
The bucket's policy belongs to the static group: `group-init` sets it once per bucket, on `aws:SourceAccount`, and deploy never edits it.
That policy lets any distribution in the account read the bucket, so a shared static group holds public content only.

### Deploying a full app

A full app (`CBASE_APP = shop` alone) deploys the same static part, and its distribution adds each `CBASE_ROUTE` as a behaviour on the VM origin, the group VM's public DNS name and the app's port, which the VM side of the deploy (below) supplies as the stack's `VmOrigin` and `VmPort` parameters.
A route's methods map to the smallest of CloudFront's three method sets that covers them (`GET,HEAD`; `GET,HEAD,OPTIONS`; all seven), `cache` is AWS's CachingOptimized policy and `nocache` CachingDisabled, and a VM route forwards every viewer header but `Host`.
A `ws` route is its own origin for its long timeouts (CloudFront sets them per origin): 60 s to read and to keep alive, uncached, every method.
`CBASE_API_HOST` is a second distribution whose every request goes to the VM.
`cbase/tools/cbase template` prints the stack an app's `cbase.conf` makes, to read before a first deploy.

The VM side of a deploy, in order:

1. finds the VM group's instance by its tag, and stops with the `group-init` command when there is none;
2. builds the server for the instance: `make serve-binary` in the app's server directory (`.` or `server/`, the first with a Makefile), cbase's static linux-arm64 build, which needs Docker; `CBASE_SERVE_BIN` names a binary built elsewhere instead;
3. joins the group's state file, claiming the lowest free port and checking the memory fit in the same conditional write (below);
4. uploads the binary to `s3://cbase-state-<account>/artifacts/<app>/<sha256>`;
5. runs one SSM command on the instance, with no SSH: the user `cbase-<app>`, `/srv/cbase/<app>/data` owned by the service, the port checked free before a first start, the binary fetched and its sha256 checked, the unit `cbase-<app>.service` installed and only it restarted, then its `/health` (or its startup line) waited for;
6. sets the stack's `VmOrigin` and `VmPort`, so `/api/*` and any `ws` route reach the app's port.

The service gets `CBASE_LISTEN=0.0.0.0:<port>`, `CBASE_DB` in its data folder, `Restart=on-failure`, and `MemoryMax` only when `CBASE_MEMORY` is declared.
Its settings are SSM Parameter Store parameters under `/cbase/<app>/`, which the instance writes into the env file serve reads (`CBASE_ENV_FILE`); no value passes through the tool.
Only the remote script's own status lines are printed, never the rest of the command's output, and a failure points at `journalctl -u cbase-<app>` on the instance.
A port another process holds on the VM refuses the deploy and releases the claim it made.

### Many apps on one VM

Several apps share a group by naming it, `CBASE_VM = shop`, and each gets its own port, service and data folder.
Before an app joins, deploy adds every member's memory to the app's and refuses when the total exceeds the instance's memory less 192 MiB kept for the OS:

```
  blog: memory not declared, counted as 128M
  memory: blog ~128, big 256, shop 64 = 448 MiB of the 320 MiB for apps (t4g.nano: 512 MiB less 192 for the OS)
cbase: big does not fit on vm group shop: ...
```

An app that declares no `CBASE_MEMORY` is counted at 128M and has no hard cap; one that declares it is counted exactly and capped there.
The check is in the write that claims the port, so two racing deploys cannot both take the last room.
`cbase/tools/cbase apps shop` lists each member's port, service and memory (`512M`, or `~128 counted`).

### Undeploy and group-destroy

`cbase/tools/cbase undeploy` deletes the app's stack (its distribution), stops, disables and removes its unit, and leaves both groups, releasing its port.
It keeps the app's files in the bucket unless `--delete-files` (its folder alone), and `/srv/cbase/<app>/data` on the VM unless `--delete-data`.
A record pointing `CBASE_DOMAIN` at the distribution is left for the person to remove.

`cbase/tools/cbase group-destroy vm|static GROUP --yes` deletes the group's stack and state file, and refuses while the state file names any member.
A VM group is also refused while its instance holds an app's kept data, unless `--delete-data`, since the disk goes with the instance.
A static group's bucket is kept with its files and its group tag removed, so the name can be made again; deleting the bucket is left to `aws s3 rb`.

### Certificates

With no `CBASE_DOMAIN` there is no certificate: the app is at its distribution's `cloudfront.net` name, which deploy prints.
With one, deploy looks in ACM in us-east-1 (the one region CloudFront reads) for an ISSUED certificate that covers every hostname, the domain and `CBASE_API_HOST`.
A certificate's names are its domain and SANs, and a wildcard covers one label: `*.example.com` covers `www.example.com` and neither `example.com` nor `a.b.example.com`.
Without one, it reuses a certificate it requested before that is still pending, or requests one with DNS validation.
It then looks for the account's public Route 53 hosted zone whose name is the longest suffix of each hostname.
With a zone, deploy writes the validation CNAME there, waits for ACM to issue the certificate, and after the stack writes the hostname's alias (A and AAAA) to the distribution.
With none, it prints the exact records to add where the domain's DNS is served and stops with status 3, having changed nothing else; the next deploy goes on once ACM has issued the certificate, and prints the record that points the hostname at the distribution.
`CBASE_CERT_ARN` and `CBASE_ZONE` are overrides, for a certificate or a zone deploy would not pick.
Deploy never registers a domain or makes a hosted zone.

### The safety rules

Every stack change is a change set, made and printed before anything runs.

- **A bucket is never replaced or removed.**
  A change set that would replace or remove an `AWS::S3::Bucket` is refused and deleted, and nothing is executed or uploaded: a bucket replaced is every file in it lost.
  This is the one hard stop.
- **A distribution may be replaced.**
  A new distribution is a new `cloudfront.net` name and a brief outage; deploy allows it and says so, and rewrites a Route 53 alias itself.
- **Drift stops a deploy.**
  On a stack that exists, CloudFormation's drift detection runs first, and a resource changed by hand is printed and the deploy stops, since the update would undo the change; `--accept-drift` (or `CBASE_ACCEPT_DRIFT=1`) overwrites it.
- **A dry run changes nothing.**
  `--dry-run` (or `DRY_RUN=1`) makes and prints the change set, deletes it unexecuted (with the empty stack a first change set leaves), and prints the upload it would make.
- **No secret is printed.**
  A change set's and a drift's values are shown, but never one under an origin's custom headers or set by a parameter not known to be plain: those print as `origin header <where> changed`.
  The tool's `--query` filters keep such values out of what the CLI answers, and any line of an AWS error that names one is masked.

### Credentials

The AWS CLI's own chain: `AWS_PROFILE` and `~/.aws` (an `aws login` or SSO session) on a laptop, the OIDC role's environment in CI.
The tool reads, prints and passes no credential, and with none it says to run `aws login` or set `AWS_PROFILE`.
`make check` runs `tools/cbase.d/test/cbase_test.sh`, which drives the tool through the real AWS CLI against a local fake of S3, STS, the Tagging API, CloudFormation, ACM, Route 53, CloudFront, EC2 and SSM (`tools/cbase.d/test/fake_aws.c`), with dummy keys; no test calls AWS.
`tools/cbase.d/test/cbase_vm_test.sh` covers the VM side, and runs the scripts SSM would run under a scratch root with stand-ins for `systemctl`, `ss` and `aws`.

## Rules the code keeps

Each line names the tool, test or record that holds the rule; the tools run in `make check` or in CI.

- **No JSON on shipped paths.** Clients speak a binary wire format; JSON exists only where a foreign API or the browser console needs it, contained in `core/src/core_json.c` and `providers/aws/src/aws_json.c` over `core/src/util/json.c` (`core/test/unit_json_gate.c`).
- **cbase defines no C library.** No cbase object may define `memcpy`, `malloc` or any other libc name, so it never fights the program's own libc (`tools/libc_gate.sh`).
- **One random source per kind.** Secret bytes come only from `cb_random_bytes` (`core/src/util/random.c`), the client core's seed from its host, and seeded streams from `lib/rng.h` (splitmix64, pinned by `lib/rng_test.c`); the client hosts' entropy from the platform's one source, `crypto.getRandomValues` in `client/ts/socket.ts` and `SecRandomCopyBytes` in `client/swift/CBaseClient/CbClient.swift`, and the two Swift UI tests' throwaway account names from `UInt32.random`, each the only one in its module; any other `rand`, `random`, `arc4random`, `RAND_bytes`, `getentropy` or `/dev/urandom` fails `core/test/unit_random_gate.c`, and so does a stream seeded from the clock (a time call inside `rng_seed64(...)`).
- **Mutation-checked tests.** Every test was seen to fail with the code it covers broken; the records are in [docs/mutation_checks.md](docs/mutation_checks.md).
- **Fuzzing.** The parsers and codecs have libFuzzer targets (`core/fuzz`, `providers/*/fuzz`, `client/fuzz`), run in CI, with the corpus replayed under ASan and UBSan (`make fuzz-replay`).
- **A la carte, proved.** A local-only binary carries no symbol of the aws provider (`tools/symbol_gate.sh`), the local-only example builds and passes its checks in a tree without `providers/aws/` or `ops/` and with the network denied (`tools/offline_build_test.sh`), and the HTTP/2 module links alone (`make h2-alacarte`).
- **Sides stay apart.** Core names no provider or product and a product names no provider (`core/test/unit_name_gate.c`), and nothing in the repository reaches outside it (`tools/repo_gate_test.sh`).
- **Coverage floors.** `make coverage` fails below `tools/cov_floor.txt` or with a function no test calls.

## Docs

- [cbase.mk](cbase.mk): its header lists every variable a product may set.
- [docs/PLATFORM.md](docs/PLATFORM.md): the app API (routes, CORS, storeless apps, child processes, store, realtime, the upgrade guide), and how cbase came to be.
- [docs/H2.md](docs/H2.md): the HTTP/2 and gRPC client, its threading, retry rule and limits.
- [docs/AUTH_APPLE.md](docs/AUTH_APPLE.md): Sign in with Apple, on the server and in the clients.
- [docs/COVERAGE.md](docs/COVERAGE.md): what the coverage gate measures.
- [docs/mutation_checks.md](docs/mutation_checks.md): the mutation check behind each test.
- [DESIGN.md](DESIGN.md): the platform design record, each module's as-built design and the reasons for it.
- [RUNBOOK.md](RUNBOOK.md): operating a cbase product on a local machine, a VM stack and the aws stack, and in CI.
- [docs/CLIENT_DESIGN.md](docs/CLIENT_DESIGN.md): the client SDK design.
- History, written before cbase was split out of the monorepo it came from: [docs/history/PLATFORM_SPLIT.md](docs/history/PLATFORM_SPLIT.md), [docs/history/REPORT_2026-10-13.md](docs/history/REPORT_2026-10-13.md), [docs/history/BENCH_VM_VS_DYNAMO.md](docs/history/BENCH_VM_VS_DYNAMO.md), [docs/history/DIRECT_DYNAMO.md](docs/history/DIRECT_DYNAMO.md).

## License

MIT, see [LICENSE](LICENSE).
