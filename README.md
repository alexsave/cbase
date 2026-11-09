# cbase

cbase is a backend platform written in C that an app links into its own server binary.
It has a core (HTTP/1.1 ingress, the router and handlers, auth, realtime over WebSocket, jobs and schedules, an admin console), providers chosen at link time, client SDKs over one C protocol core, and an outbound HTTP/2 and gRPC client.
It is taken a la carte: a product links only the parts it needs, and build gates prove that nothing else got in.

- **Core** (`core/`): the server, the app API (`core/include/cbase/app.h`), auth, realtime, jobs, schedules, the admin console, and child processes run from the server's loop (`core/src/spawn.h`).
- **Providers** (`providers/`): `local` is one process on SQLite (`cbase serve`), which can also run storeless, with the app keeping its own data; `aws` is Lambda, DynamoDB and IoT Core.
- **Client** (`client/`): a sans-IO C core (`client/include/cbase/client.h`) with a POSIX host, a wasm build with TypeScript over it, and Swift over it.
- **HTTP/2 and gRPC** (`core/src/net/h2.h`, `core/src/net/grpc.h`): unary gRPC calls from C with no protobuf library, usable on their own ([docs/H2.md](docs/H2.md)).

## Status

v0.1.1 is the first public tag.
Its first consumer is amgi, a local flashcard app that runs `cbase serve` on the user's own machine.
The API is pre-1.0 and will change.
Breaking changes, and how to upgrade a product across them, are recorded in [docs/PLATFORM.md](docs/PLATFORM.md) section 2.9.

## Quick start

Requirements:

- macOS: Xcode's clang and `brew install openssl@3 sqlite` (Homebrew's SQLite, not Apple's system one).
- Ubuntu 24.04: `sudo apt-get install build-essential libssl-dev libsqlite3-dev`.

Nothing else is needed, and the build fetches nothing.

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

`make serve` builds `build/cbase` and runs it as `cbase serve` from the repository root, on `127.0.0.1:3000` (`PORT=4000` for another port, `CBASE_LISTEN=127.0.0.1:0` for one the OS picks), with its database in `./cbase.sqlite`.
Once the socket is bound, serve prints exactly one line to stdout, such as `cbase: listening 127.0.0.1:3000`, for a person or a parent process to read; the log goes to stderr.
Then:

```
curl http://127.0.0.1:3000/health
curl http://127.0.0.1:3000/v1/hello
```

Settings are `CBASE_*` environment variables, listed in `core/src/config.h` and logged at start with secrets redacted.
`CBASE_ENV_FILE=.env.local` reads `KEY=VALUE` lines from that file first, for the app's secrets as well as `CBASE_*` settings; a variable already in the environment wins, an unquoted value's ` # comment` is dropped as Node drops it (`KEY=abc # note` is `abc`, `a#b` stays `a#b`, a quoted `"a # b"` keeps it), and a bad line refuses the start, naming its number (the format is in docs/PLATFORM.md 2.10).
An app may ship its own defaults for a few settings (`cb_app.req_workers`, `cb_app.log_level`); the variable still overrides them.
`CBASE_DEV=1` allows a random JWT secret per process and a public URL derived from the listen address.
Outside it, serve needs `CBASE_JWT_SECRET` (at least 32 characters) and `CBASE_PUBLIC_URL`.
`CBASE_ADMIN_TOKEN` (at least 32 characters) opens the console at `/admin`.

## Using it in a product

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
APP_CFLAGS      :=         # flags for every object, the platform's too
APP_LDLIBS      :=         # more libraries for every link
APP_LIB_SRC     :=         # single sources outside the roots, linked into every program
include $(CBASE)/cbase.mk
```

There are no file lists to edit: a new `.c` under a source root is part of every program, and a new `unit_*.c` under a test root is a new test.
`make`, `make check` and `make check-asan` then work in the product as they do in cbase.

### Running it: `make serve`

`make serve` builds the product's serve binary (`build/$(APP_SERVE)`) through its usual rule and then runs it as `serve`, so every product has the same run line and none writes its own launcher.
It needs the local provider in `CBASE_PROVIDERS`, and `tools/serve.sh` is the one place that says how it runs:

- It runs from `APP_SERVE_DIR`, where serve reads the app's files from its working directory; the default is the directory of the Makefile make read.
  A product whose Makefile sits in `server/` and whose files are at the repository root sets `APP_SERVE_DIR := ..`.
- When `APP_ENV_FILE` exists (default `.env.local`, from `APP_SERVE_DIR` unless absolute) and `CBASE_ENV_FILE` is not already set, it is named to serve as `CBASE_ENV_FILE`; an absent file is no error, and an empty `APP_ENV_FILE` names none.
  The script never reads the file: serve reads it strictly, so a bad line refuses the boot with serve's own sentence naming the line.
- `CBASE_LISTEN` is `APP_SERVE_LISTEN` unless the environment sets it; the default is `127.0.0.1` on `PORT`, or on 3000 without one.
- The binary is exec'd, so make's child is the server itself: Ctrl-C or a SIGTERM sent to it reaches serve, which drains and exits.

```make
APP_SERVE     := myapp     # build/myapp
APP_SERVE_DIR := ..        # run from the repository root, not server/
include $(CBASE)/cbase.mk
```

`make -C server serve`, `PORT=4000 make -C server serve` and `CBASE_LISTEN=0.0.0.0:8080 make -C server serve` then all work, and `tools/serve_test.sh` (part of `make check`) holds each rule against the example.

The product defines one const struct, `cb_app`, and points `cb_main_app` at it (`core/include/cbase/app.h`):

```c
static const cb_route ROUTES[] = {
    { .method = "GET", .pattern = "/v1/hello", .flags = CB_ROUTE_LOOP, .fn = h_hello },
};
static const cb_route_list ROUTE_LIST = { ROUTES, 1 };
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

A route pattern is exact, a prefix (`/v1/items/*`) or has path parameters (`/decks/:id`).
Jobs, schedules, admin panels, metrics and the store's rows are further fields of the same struct.
A storeless app opens no platform store, and opens its own in the `serve_open` hook ([docs/PLATFORM.md](docs/PLATFORM.md) 2.7).

[example/](example/) is the template to copy: a two-route app with a job and a schedule (`example/src/example_app.c`), its Makefile (`example/Makefile`, local provider only, with the symbol gate as its link check), a smoke and a storeless test.
A C or wasm client includes `client/client.mk` and adds `$(CBC_SRC)` and `$(CBC_INC)` to its build.

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
- History, written before cbase was split out of the monorepo it came from: [docs/history/PLATFORM_SPLIT.md](docs/history/PLATFORM_SPLIT.md), [docs/history/REPORT_2026-10-13.md](docs/history/REPORT_2026-10-13.md), [docs/history/VERCEL.md](docs/history/VERCEL.md), [docs/history/BENCH_VM_VS_DYNAMO.md](docs/history/BENCH_VM_VS_DYNAMO.md), [docs/history/DIRECT_DYNAMO.md](docs/history/DIRECT_DYNAMO.md).

## License

MIT, see [LICENSE](LICENSE).
