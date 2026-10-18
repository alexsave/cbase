# c_base

The foolish backend as one C program: AWS Lambda and DynamoDB in production (`bootstrap`), a local server on SQLite for development and tests (`cbase serve`).
Clients speak its binary surface: `/v1` over HTTP and MQTT over WebSocket at `/mqtt` (AWS IoT Core in production), every byte written and read by the kernel's codec (`foolish/c/src/netwire.h`) and client library (`netclient.h`), which the web reaches through `web.wasm` and iOS through its xcframework (DESIGN.md, "Binary surface").
The design, every decision and each module's as-built record are in [DESIGN.md](DESIGN.md); the deploy and the operations are [RUNBOOK.md](RUNBOOK.md); [REPORT.md](REPORT.md) is the dated report of 2026-10-13 that chose this shape.

## Build and test

```
make -C c_base             # build/cbase, build/bootstrap (native), build/cbase-import
make -C c_base check       # every unit test and every smoke, all at once (about 15 s once built)
make -C c_base check-asan  # the same, everything (the kernel too) under ASan + UBSan
make -C c_base coverage    # every test against an instrumented build; fails below tools/cov_floor.txt or with a function no test calls (docs/COVERAGE.md; needs gcovr)
make -C c_base check-dynamo  # the store, admin and import tests and the game smoke on DynamoDB Local (Docker)
make -C c_base bench-dynamo  # the DynamoDB store's per-operation latency against DynamoDB Local
make -C c_base bootstrap   # dist/bootstrap.zip: static Linux arm64 for Lambda (needs Docker)
make -C c_base bench       # build/cbase-bench: latency and CPU numbers (DESIGN.md, "The bench")
```

macOS needs Xcode's clang and `brew install openssl@3`; Ubuntu 24.04 needs `build-essential libssl-dev libsqlite3-dev`.
The kernel is built from `../foolish/c` with its own source list; nothing is copied.
`CBASE_SMOKE_GAME=0 make -C c_base smoke` skips the game scenario when only the plumbing is of interest.
The game smoke runs its server with `CBASE_BOT_SPEEDUP=20` (config.h): every bot timer, and the board's time the kernel paces by, 20 times faster, so the game, the crash's lease expiry and the idle human's rest take seconds; Lambda refuses to start with it set.
`c_base/ops/dynamo_local.sh` starts DynamoDB Local with the stack's table and prints the environment to export (`--stop` removes it); `CBASE_LOG_LEVEL=debug` logs every request's DynamoDB requests and capacity units.

## Run locally

```
cd c_base
CBASE_JWT_SECRET=$(openssl rand -hex 32) \
CBASE_DB=/tmp/cbase.sqlite CBASE_ADMIN_TOKEN=dev-admin-token-0123456789 build/cbase serve
curl http://127.0.0.1:8080/health
```

Settings come from `CBASE_*` environment variables, listed in `src/config.h` and logged at start with secrets redacted.
The console is at http://127.0.0.1:8080/admin (paste the admin token).

## Point the web app at it

In `foolish/`, create `.env.local` (gitignored) with the server's URL, then start Next:

```
NEXT_PUBLIC_BACKEND_URL=http://127.0.0.1:8080
```

```
cd foolish && npm ci && npm run gen && WASM_CC=/opt/homebrew/opt/llvm/bin/clang npm run wasm && npx next dev
```

Sign up, create a table, open it in a second browser profile, and play; DESIGN.md "Acceptance in a real browser" records what was verified this way.

## Deploy

Production is `https://api.foolish.cards`, and every push to `main` that touches c_base or the kernel deploys it from GitHub Actions through an OIDC role (`.github/workflows/c_base.yml`, RUNBOOK.md step 13).
A template change, or a new stack, is `c_base/ops/deploy.sh` after `aws configure`, by hand, as RUNBOOK.md explains; the console click-path is its fallback.
