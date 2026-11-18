# The hello server's memory, measured, and the optimizations it points to

Measured 2026-11-17 on `example/minimal`, cbase's smallest server.
Nothing in this document was changed in code.
Every optimization below is a candidate that was not tested.

## Setup

The server is `example/minimal`, run as `minimal serve` with `CBASE_LISTEN=0.0.0.0:8642`.
The app is storeless, so no platform store is opened.
The build is the `ops/build_static.sh` recipe: alpine:3.24@sha256:294b683c..., `make STATIC=1 build/minimal`, then `strip`.
The binary is 7,252,448 bytes and static-pie (Alpine's gcc makes `-static` a static-pie link).
cdom was a copy of its HEAD, because `example/minimal` includes it.
It ran in arm64 Linux under Docker Desktop (kernel 6.12.54-linuxkit, 4 KiB pages, THP=always).
The server has 3 threads: the loop, req-0 and job-0.
Each run was captured at 2 s and 30 s idle, then after 1,000 `GET /api/hello` requests (answered "hello from cbase").

Binary layout (`readelf -l`):

- R E LOAD 0x662298 bytes (6,538 KiB): .text 3,994 KiB, .rodata 1,246 KiB, .eh_frame 644 KiB, .rela.dyn 658 KiB.
- RW LOAD: file part 0x7e4d0 bytes (505 KiB, of which .data.rel.ro 460 KiB with 27.7k relative relocations, and .data 41 KiB), plus .bss 244 KiB.
- PT_TLS memsz 0x3560 = 13,664 bytes.

## Numbers

All in kB, as the kernel prints them.

| run | VmRSS | RssAnon | RssFile | Private_Dirty | Private_Clean | Threads |
|---|---|---|---|---|---|---|
| floor: static `pause()` | 24 | 16 | 8 | 16 | 8 | 1 |
| floor: `pause()` plus 5 pthreads | 48 | 36 | 12 | 36 | 12 | 6 |
| hello, idle 2 s (THP off via prctl) | 3944 | 980 | 2964 | 980 | 2960 | 3 |
| hello, idle 30 s | 3944 | 980 | 2964 | 980 | 2960 | 3 |
| hello, after 1000 requests (VmHWM 3952) | 3952 | 988 | 2964 | 988 | 2960 | 3 |

THP on (the default) gave identical numbers, because no mapping is large enough for huge pages.
Five idle threads cost 24 kB in total, about 5 kB each, so threads are not a factor.
The 1,000 requests cost 8 kB.

## Where the 980 kB of private dirty memory is

| KiB | mapping | what |
|---|---|---|
| 512 | RW LOAD file part | .data.rel.ro 460 plus .got and .data, written by static-pie self-relocation (27.7k RELATIVE relocations) |
| 292 | anon 1,024K, merged | musl mallocng groups for OpenSSL init: 60 mmaps, 488 KiB virtual, all from `cb_random_bytes` at core/src/config.c:178 (RAND init, CRYPTO_zalloc, lhash tables) |
| 88 | anon 96K, in the hole between the two LOADs | more of the same OpenSSL mallocng groups |
| 36 | .bss 236K (24) and a 12K anon (12) | bss touched, loop and server statics |
| 24 | [stack] | main thread |
| 12 | [heap] (brk) | mallocng metadata |
| 12 | thread map 144K | req and job thread stacks and TLS |

By owner: static-pie relocation 52%, OpenSSL init about 39% (380 KiB across the merged and hole mappings), everything cbase itself (bss, stack, heap, threads) about 9%.
Not resident: the log ring (`calloc` of 573,460 bytes at core/src/util/log.c:27, 2048 entries, pages untouched) and two thread maps of 155,648 bytes virtual each.

RssFile is 2,964 kB, 75% of VmRSS.
It is clean text and rodata that fault-around mapped, and it depends on the page cache.
The binary links OpenSSL, SQLite and the sqlite store into every server (cbase.mk:216, `LDLIBS`), storeless or not.
Which text pages are executed was not measured, only which are mapped.

## Reproduce

The scripts are in `bench/mem/`.
They need Docker on an arm64 host (an Apple-silicon Mac runs the container natively).

```sh
OUT=/abs/path/out CDOM=/abs/path/to/cdom bench/mem/build.sh   # builds out/minimal, readelf output
OUT=/abs/path/out bench/mem/run.sh                            # idle, 1000 requests, floors
```

`run.sh` runs `bench/mem/inner.sh` inside Alpine.
It builds `nothp.c` (a prctl wrapper that disables THP then execs the server), `floor0.c` (a `pause()` program) and `floor5.c` (`pause()` plus 5 threads), then captures `/proc/<pid>/status`, `smaps` and `smaps_rollup` at 2 s, 30 s and after 1,000 requests.
`st.sh` is the strace pass that attributed the mappings: `strace -f -k -e trace=mmap,brk,munmap,clone,clone3,mprotect` on the unstripped binary, plus a copy of `smaps`.
Run it by hand inside the same container with `/out/minimal.unstripped` present.

## Candidate optimizations, ranked

Savings are estimates from the breakdown above, not measurements.
After 1 and 2 together, the estimate for private dirty memory is about 100 KiB.

### 1. Link non-PIE instead of static-pie

- Expected saving: about 512 KiB of private dirty memory, since the RW LOAD would no longer be rewritten at startup.
  foolish measured the same lever on its own binary: `-static -no-pie` gave 516 KiB less VmRSS, nearly all private dirty (foolish/docs/memtest/MUSL_IDLE.md, "Static non-PIE").
- Where: cbase.mk:219-220 (`ifeq ($(STATIC),1)` adds `-static`, which Alpine's gcc turns into static-pie), and the container build in ops/build_static.sh.
- Risk: loses ASLR for the static binary, and the object files need to be compiled without `-fPIE` to get the full effect.
  RELRO data that needs no relocations is the alternative.
- Not tested on cbase.

### 2. Skip the JWT secret and OpenSSL init for storeless apps

- Expected saving: about 380 KiB of private dirty memory.
- Where: core/src/config.c:176-180 generates a random CBASE_JWT_SECRET through `cb_random_bytes` (core/src/util/random.c:10) when none is set.
  That first call is what initializes OpenSSL's RAND.
  core/src/config.h:185 says a storeless app has no accounts, so nothing signs or verifies a token and the secret is not used.
- Risk: low.
  Something else may call OpenSSL later (TLS, other random draws), which would only move the cost.
  The `jwt_secret_generated` flag, and what boot prints about it, must stay consistent.
- Not tested.

### 3. A storeless build that does not link sqlite, ssl or crypto

- Expected saving: a smaller .text (3,994 KiB now) and fewer pages for fault-around to map, so a lower RssFile than 2,964 kB.
  The size of the drop was not estimated.
  This is the only option here that could bring VmRSS toward 1 MB, together with 1 and 2.
- Where: cbase.mk:216 (`LDLIBS` carries `-lsqlite3 -lssl -lcrypto` for every server), and the code that needs them (the sqlite store, TLS, `cb_random_bytes`, token signing).
  `cb_random_bytes` would need a non-OpenSSL source (the kernel's getrandom).
- Risk: highest.
  It splits the build into two configurations, and every call into those libraries must sit behind the storeless line.
- Not tested.

### 4. Drop cold text with madvise

- Expected saving: part of RssFile after boot, by `madvise(MADV_DONTNEED)` or `MADV_PAGEOUT` on text that boot used and the steady state does not.
- Where: nowhere yet.
  It would be a call at the end of boot in the serve path.
- Risk: the pages are clean and reclaimable already, so this lowers a number without freeing anything the kernel was short of.
  Re-faults cost latency on the first request that needs the pages back.
  It needs a measure of which text is hot first.
- Not tested.

Lowering fault-around is a host sysctl and not something cbase can do.

## Why macOS figures differ

README.md's "Idle memory" section quotes 4.7 MB for `cbase serve` on a Mac (2026-11-09).
That is not comparable to the Linux numbers above.
On macOS, the shared libraries come from the dyld shared cache, so their pages are shared with the system and are counted differently.
The footprint floor of a macOS process is about 1.1 MB, against 24 kB for a static `pause()` on Linux.
Those two macOS facts are stated by the notes' requester and are not in the measurement files or docs/mutation_checks.md, which only records 0.3 MB growth in a test.
Compare Linux figures to Linux figures, and compare the macOS figure only to other macOS measurements.
