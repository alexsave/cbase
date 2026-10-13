# c_base

The foolish backend as C: AWS Lambda in production (`bootstrap`), a local server for development (`cbase serve`).
The design, the decisions and each module's status are in [DESIGN.md](DESIGN.md).

## Build and test

```
make            # build/cbase, build/bootstrap (native), build/cbase-import
make check      # unit tests + both smoke lanes
make check-asan # the same under ASan + UBSan
make bootstrap  # dist/bootstrap.zip: static Linux arm64 for Lambda (needs Docker)
```

macOS needs Xcode's clang and `brew install openssl@3`; Ubuntu 24.04 needs `build-essential libssl-dev libsqlite3-dev`.
The kernel is built from `../foolish/c` with its own source list; nothing is copied.

## Run locally

```
CBASE_ANON_KEY=dev-anon CBASE_JWT_SECRET=$(openssl rand -hex 32) build/cbase serve
curl http://127.0.0.1:8080/health
```

Settings come from `CBASE_*` environment variables, listed in `src/config.h` and logged at start with secrets redacted.
