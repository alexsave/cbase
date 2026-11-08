# Coverage

`make coverage` measures what cbase's tests run, and fails when it went down.
CI runs it as the `coverage` job of `.github/workflows/ci.yml`.

## What is measured

Every source under `core/src` and each linked provider's `src/`, and lib/'s codecs `lib/b64/b64.c`, `lib/hex/hex.c` and `lib/mqtt/mqtt.c` (cbase.mk's `COV_FILTER`).
A product adds its own scope with `APP_COV_FILTER`, and its own floors with `APP_COV_FLOOR`.
The objects are built a third way, beside the normal and the sanitized ones: `build/obj/cov` at `-O0 --coverage`.
Every cbase object in it, `bcrypt.c` included, is built with `-DCB_INSTRUMENTED`, so `unit_bcrypt` checks the cost but not its CPU budget there: an instrumented build's timings are not the shipped binary's (`bcrypt.c` at `-O2` with `--coverage` measured 120 ms and at `-O0` 699 ms, against about 60 ms shipped).

The runs, all at once, are every unit test and every smoke (`smoke_boot`, `smoke_lambda`, and the product's, `smoke_example` here).
The product's first smoke and the loop-heavy unit tests run once more with every loop on io_uring, where the runner allows it.

gcovr turns the counts into an LCOV tracefile, `build/cov/coverage.lcov`, and `tools/cov_gate.c` reads it.

## The gate

`tools/cov_floor.txt` holds the floors:

- `line` and `branch`: the whole scope's line and branch coverage may not fall below them;
- `zero <file> <function> <reason>`: a function allowed to be called by no test.

Every other function with a body that no test calls fails the gate by name.
So coverage can only go up: when it climbs, raise the floors in the same change, and a new function nothing calls is a red build until a test calls it.
The floors are the numbers CI measures (gcc and gcov on Linux), which differ from a Mac's clang and llvm-cov, mostly in branches.
The header of `tools/cov_floor.txt` records why the floors were re-baselined when cbase first ran on its own.

`build/cov/report.txt` lists every file's line and branch coverage and every function under 100% of its lines, worst first; CI keeps it as an artifact.

## Running it on a Mac

```
make coverage
```

needs gcovr (`brew install gcovr`); cbase.mk hands it `xcrun llvm-cov gcov` on macOS and `gcov` elsewhere.

## The failure seam the tests can use

`core/test/oom.h` is a failing allocator: included by exactly one file of a test program, it replaces malloc, calloc, realloc and free for that program only, fails one chosen allocation of the calling thread, and tracks the blocks it handed out, so a sweep can fail every allocation of a request in turn and see the answer and the leaks.
Under a sanitizer the allocator is the sanitizer's and the sweeps run once, unfailed.
It needs no hook in the code under test.
No test in this repository includes it yet: its one user stayed in the monorepo cbase was split from.
