# c_base

The foolish backend as one C program: AWS Lambda and DynamoDB in production (`bootstrap`), a local server on SQLite for development and tests (`cbase serve`).
It speaks the Supabase surface the shipped web and iOS clients already use (GoTrue auth, PostgREST reads, Realtime over Phoenix, the four edge functions byte for byte), so a client moves to it by changing a URL and an anon key.
The design, every decision and each module's as-built record are in [DESIGN.md](DESIGN.md); the deploy is [RUNBOOK.md](RUNBOOK.md); the summary for Alex is [REPORT.md](REPORT.md).

## Build and test

```
make -C c_base             # build/cbase, build/bootstrap (native), build/cbase-import
make -C c_base check       # every unit test, then both smoke lanes (the full game smoke takes about 1.5 minutes)
make -C c_base check-asan  # the same, everything (the kernel too) under ASan + UBSan
make -C c_base check-dynamo  # the store, admin and import tests and the game smoke on DynamoDB Local (Docker)
make -C c_base bench-dynamo  # the DynamoDB store's per-operation latency against DynamoDB Local
make -C c_base bootstrap   # dist/bootstrap.zip: static Linux arm64 for Lambda (needs Docker)
```

macOS needs Xcode's clang and `brew install openssl@3`; Ubuntu 24.04 needs `build-essential libssl-dev libsqlite3-dev`.
The kernel is built from `../foolish/c` with its own source list; nothing is copied.
`CBASE_SMOKE_GAME=0 make -C c_base smoke` skips the game scenario when only the plumbing is of interest.
`c_base/ops/dynamo_local.sh` starts DynamoDB Local with the stack's table and prints the environment to export (`--stop` removes it); `CBASE_LOG_LEVEL=debug` logs every request's DynamoDB requests and capacity units.

## Run locally

```
cd c_base
CBASE_ANON_KEY=dev-anon-key-0123456789 CBASE_JWT_SECRET=$(openssl rand -hex 32) \
CBASE_DB=/tmp/cbase.sqlite CBASE_ADMIN_TOKEN=dev-admin-token-0123456789 build/cbase serve
curl http://127.0.0.1:8080/health
```

Settings come from `CBASE_*` environment variables, listed in `src/config.h` and logged at start with secrets redacted.
The console is at http://127.0.0.1:8080/admin (paste the admin token).

## Point the web app at it

In `foolish/`, create `.env.local` (gitignored) with the server's URL and anon key, then start Next:

```
NEXT_PUBLIC_SUPABASE_URL=http://127.0.0.1:8080
NEXT_PUBLIC_SUPABASE_KEY=dev-anon-key-0123456789
```

```
cd foolish && npm ci && npm run gen && WASM_CC=/opt/homebrew/opt/llvm/bin/clang npm run wasm && npx next dev
```

Sign up, create a table, open it in a second browser profile, and play; DESIGN.md "Acceptance in a real browser" records what was verified this way.

## Deploy

`c_base/ops/deploy.sh` after `aws configure`, as RUNBOOK.md explains; the console click-path is its fallback.
