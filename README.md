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

### Start a new server

```
make -C <cbase> new DIR=<path>
```

`DIR` must not exist or be empty, and is taken from where you ran make.
It gets a `Makefile` (`CBASE` and `APP_SRC_ROOTS`, then `include $(CBASE)/cbase.mk`), `src/server.c` (a storeless app whose one route, `GET /`, answers `cbase`, beside the platform's `GET /health`) and a `.gitignore` (`build/`).
`CBASE` is this checkout's path from `DIR`.
With `GIT=1`, `DIR` is also a git repository with cbase as its submodule at this commit, and `CBASE` is `cbase`.
Then `make serve` there builds the server and prints the URL it listens at.
The server ships no test: `tools/new_test.sh`, part of cbase's `make check`, builds it, serves it and asks it.

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

## Rules the code keeps

Each line names the tool, test or record that holds the rule; the tools run in `make check` or in CI.

- **No JSON on shipped paths.** Clients speak a binary wire format; JSON exists only where a foreign API or the browser console needs it, contained in `core/src/core_json.c` and `providers/aws/src/aws_json.c` over `core/src/util/json.c` (`core/test/unit_json_gate.c`).
- **cbase defines no C library.** No cbase object may define `memcpy`, `malloc` or any other libc name, so it never fights the program's own libc (`tools/libc_gate.sh`).
- **One random source per kind.** Secret bytes come only from `cb_random_bytes` (`core/src/util/random.c`), the client core's seed from its host, and seeded streams from `lib/rng.h` (splitmix64, pinned by `lib/rng_test.c`); the client hosts' entropy from the platform's one source, `crypto.getRandomValues` in `client/ts/socket.ts` and `SecRandomCopyBytes` in `client/swift/CBaseClient/CbClient.swift`, and the two Swift UI tests' throwaway account names from `UInt32.random`, each the only one in its module; any other `rand`, `random`, `arc4random`, `RAND_bytes`, `getentropy` or `/dev/urandom` fails `core/test/unit_random_gate.c`.
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
