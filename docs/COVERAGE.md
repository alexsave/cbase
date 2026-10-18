# Coverage

`make -C c_base coverage` measures what c_base's tests run, and fails when it went down.
CI runs it as the `coverage` job of `.github/workflows/c_base.yml`.

## What is measured

Every source in `c_base/src`, and of the kernel the files the binary surface is made of, `foolish/c/src/netwire.c` and `netclient.c`, and the stream's MQTT codec `shared/c/mqtt/mqtt.c` (its tests are c_base's, `test/unit_mqtt.c`).
The rest of the kernel is measured by foolish/c's own coverage job.
The objects are built a third way, beside the normal and the sanitized ones: `build/obj/cov` at `-O0 --coverage`, and `build/obj/cov-kernel`, where only `netwire.c` and `netclient.c` are instrumented, so the bots in the tests play at the kernel's own speed.
`bcrypt.c` keeps `-O2`, because `unit_bcrypt` holds cost 10 to GoTrue's CPU budget and `-O0` misses it.

The runs are every unit test (in parallel), `smoke` without its game (`CBASE_SMOKE_GAME=0`), `smoke_lambda`, `smoke_lambda_after`, `smoke_authz`, `smoke_matrix` and `smoke_boot`.
The smoke game is left out because it is slow by design (the bots pace themselves like people) and covers nothing the other runs do not: measured on 2026-10-17, leaving it out cost 9 lines of 12,761.
`smoke_matrix` plays a whole three-player game instead, with no bots and so no pacing.

gcovr turns the counts into an LCOV tracefile, `build/cov/coverage.lcov`, and `tools/cov_gate.c` reads it.

## The gate

`tools/cov_floor.txt` holds the floors:

- `line` and `branch`: the whole scope's line and branch coverage may not fall below them;
- `zero <file> <function> <reason>`: a function allowed to be called by no test.

Every other function with a body that no test calls fails the gate by name.
So coverage can only go up: when it climbs, raise the floors in the same change, and a new function nothing calls is a red build until a test calls it.
The floors are the numbers CI measures (gcc and gcov on Linux), which differ from a Mac's clang and llvm-cov, mostly in branches.

`build/cov/report.txt` lists every file's line and branch coverage and every function under 100% of its lines, worst first; CI keeps it as an artifact.

## Running it on a Mac

```
make -C c_base coverage
```

needs gcovr (`brew install gcovr`); the Makefile hands it `xcrun llvm-cov gcov` on macOS and `gcov` elsewhere.

## The two failure seams the tests use

- `test/oom.h`: `unit_oom.c` replaces malloc, calloc, realloc and free for its own program only, fails one chosen allocation of the calling thread, and tracks the blocks it handed out, so a sweep can fail every allocation of a request in turn and see the answer and the leaks. Under a sanitizer the allocator is the sanitizer's and the sweeps run once, unfailed.
- `test/lib_faultstore.c`: a store that forwards to a real one and fails one chosen call, so `unit_store_faults.c` can fail every store call of a request in turn.

Neither needs a hook in the code under test.
