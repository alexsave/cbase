# calib - the CPU calibration suite

`calbench` measures what one CPU core, the kernel and libc cost on a box, so a model of the Docker arm64 rig (c_base/bench/nano) can be carried over to a t4g.nano.
It is one static musl arm64 binary, built with cbase serve's flags (`-std=gnu11 -D_GNU_SOURCE -O2 -g -pthread -static`, no `-mcpu`).
The binary is not part of the product and nothing in the Makefile or CI builds it.

## What it measures

Run as `calbench GROUP PASS`; one group per run, TSV on stdout.

- `env`: the box (cpuinfo, caches, THP, clocksource, perf_event_paranoid) as `#` comment lines.
- `clock`: a timing-only cycle model - a 256-add dependent chain gives GHz, then fp add/mul/div/sqrt (single and double, dependent and 8-wide) and an sdiv sweep.
- `core`: integer add/mul dependent and independent, udiv/sdiv by operand width, branches, atomics, mutex, and contended atomics and mutex (two threads).
- `kernel`: null syscalls, clock_gettime, pipe and eventfd, epoll, page faults (first touch against reuse), pwrite/pread/fdatasync/msync, io_uring.
- `net`: TCP loopback send-only, recv-only and send+recv by payload (16 B to 64 KB), fan-out to 1..16 sockets, futex/pipe/eventfd ping-pong on the same core and across cores.
- `wake`: futex and epoll wake-up after 0, 1, 10 and 100 ms idle - latency, the waker's syscall cost, the wakee's CPU time and the sleep overshoot.
- `real`: HMAC-SHA256 and SHA256 by length (OpenSSL), SQLite WAL+NORMAL insert/update/select/transaction by row size.
- `str`: memcpy/memset/memmove/memcmp/strcmp/strlen by length and alignment, strcmp/memcmp by the position of the first difference.
- `malloc`: musl mallocng by size class - single, batch of 64, and with a second thread contending.
- `mem`: dependent pointer chase from 4 KB to 256 MB, THP default and off.
- `stream`: copy, scale, add and triad.
- `ebs`: O_DIRECT queue-depth-1 latency, io_uring QD4/16/32 IOPS, 1 MB sequential.
- `sqlite`: the syscalls SQLite makes and what a transaction costs, with production's store settings (see "The sqlite group" below).
  It is not in the default run; select it with `CAL_GROUPS=sqlite`.
- `steal`: every 250 ms a 2 ms add-chain sample (frequency) and the /proc/stat steal delta, 30 s idle then 90 s with the other vCPU busy.
  Its rows are a time series: `mean_ns` is ns per cycle, `cyc` is GHz and `llcmiss` is the steal fraction over the last 250 ms.

The main thread is pinned to CPU 1 (`CAL_CPU`); the cross-core partner runs on CPU 0.

### TSV columns

`group name param k n min_ns max_ns mean_ns std_ns p50_ns p99_ns cyc insn brmiss l1dmiss llcmiss cpu pass`

- A sample times `k` back-to-back operations and divides by `k`; `n` samples are taken.
  The `_ns` columns are ns per op, so the p99 of a row with k > 1 is the p99 of k-op averages.
  Only k = 1 rows are true per-op distributions.
- `param` often carries a unit multiplier.
  `add_dep x8` is 8 dependent adds per op, and `load_latency` is 8 loads per op, so divide by the multiplier before reading a latency, and always check `k` and `param` first.
- `cyc insn brmiss l1dmiss llcmiss` are per-op means of the PMU counters (user and kernel), or -1 where there is no PMU (t4g.nano has none).
  The counters are one cycles+instructions group plus three single events, all scaled by time_enabled/time_running.
- `cpu` is the pinned CPU and `pass` the pass number.

## Running it

### Build

`./build.sh` builds `bin/calbench` in the pinned alpine 3.24 arm64 image (the same one as ../nano/build.sh).
It needs Docker; it takes a few minutes the first time, because the image installs the toolchain, OpenSSL and SQLite.

### On AWS

`run_aws.sh` launches a c6g.large and a t4g.nano (unlimited credits) that run `runner.sh`: five passes 15 minutes apart, an upload to S3 after every group, then power off.
It is the only script that creates anything.

```bash
CAL_SUBNET=subnet-... CAL_PROFILE=<instance profile name> ./run_aws.sh
./collect.sh state/<run>.env       # when runner.log says ALLDONE; syncs to results_<run>/
./cleanup.sh state/<run>.env       # terminate, delete the security group, empty and delete the bucket
```

- The subnet must be IPv6-capable; the boxes get an IPv6 address only, and the SSM agent uses the dual-stack endpoints.
- The profile's role needs SSM; the script gives that role put and get on the run's own bucket and nothing else.
- Everything it makes is named `cbase-perf-temp-cal-*` with a random suffix: the bucket, the security group and the instance and volume Names.
  Ids and names go to `state/<run>.env` (git-ignored), and `collect.sh` and `cleanup.sh` act only on what that file records, never on a wildcard or a tag search.
- Safety: `InstanceInitiatedShutdownBehavior=terminate`, `shutdown -h +185` (`CAL_MAX_MIN`), a systemd `poweroff` timer 5 minutes later, the runner's own `shutdown -h now`, and unlimited credits on t4g (a throttled box would measure the credit bucket).
- Optional env: `AWS_DEFAULT_REGION` (us-east-1), `CAL_AMI` (the latest AL2023 arm64), `CAL_ROLES` ("c6g t4g"; "t4g" for the nano alone), `CAL_PASSES` (5), `CAL_GAP_S` (900), `CAL_SWAP` (1: a 1 GB swapfile so dnf fits on the nano; the 2026-10-20 run had none), `CAL_GROUPS` (every group but sqlite, with steal on the first and last pass; a list runs only those groups in every pass), `CAL_MAX_MIN` (185: the `shutdown -h` timer in minutes, with the systemd `poweroff` 5 minutes later).
- The sqlite run of 2026-10-20 was `CAL_ROLES=t4g CAL_GROUPS=sqlite CAL_PASSES=5 CAL_GAP_S=180 CAL_MAX_MIN=60`.
- The user data embeds a 6 hour presigned URL, so it is built in a temp file and removed; it is never stored in the repo.

### On the Docker rig

`run_rig.sh [OUTDIR]` runs the same binary in an alpine arm64 container with `--cpus=2`, the container setup of ../nano.
It is documented and untested: it has never been run.
Then `./compare.py results_2026-10-20/t4g <OUTDIR>` compares the rig against the nano.
Expect the Docker VM's cross-vCPU wake-up to dominate the differences (the smoke run saw 30-50 us ping-pong round trips).

### Comparing runs

```bash
./compare.py results_2026-10-20/c6g results_2026-10-20/t4g          # rows outside +-10%
./compare.py A B --all --group wake                                 # every row of one group
```

`compare.py` reads every `.tsv` under each argument (a directory, or one file), takes the median over passes for a row keyed by (group, name, param), and prints the p50 of each side, the B/A ratio of p50, of min and of p99.
All times are ns per op; check `k` before reading a row as a per-op cost.
The TSV parsing is the one function `read_tsv`.

## First findings (2026-10-20, results_2026-10-20/)

Five passes on a c6g.large and a t4g.nano (unlimited), both Graviton2 (Neoverse N1), 2 vCPUs, Linux 6.18 AL2023.

- t4g.nano equals c6g.large: the p50 ratio is 1.00 over the 316 comparable rows (pass 3, the groups env, clock, core, kernel, mem, net, stream, wake).
  `compare.py` on the same files counts 338 rows with a ratio (it keeps the contended and ping-pong rows below), 276 of them within 10%, median 1.000.
- The exceptions are cross-vCPU wake-ups at 2-7x (worse the longer the idle: 0 ms 2x, 10 ms 5x, 100 ms 7x), a contended mutex at 51 us on the nano against 0.11 us on the c6g in pass 3, and `pwrite+fdatasync` overwrite at 2.5-2.9x.
- Both boxes sustain 2.48-2.50 GHz (the 256-add chain: 102.4 ns per 256 adds).
- There is no steal in unlimited mode.
- Same-core ping-pong round trips are faster on the nano (0.4-0.6x), and the contended atomic of pass 3 on the c6g is an outlier (8 ms); treat both as noise-bound until a rerun.
  The full five-pass data adds 1746 rows; many of the malloc two-thread rows differ between boxes there, so use the medians in `compare.py` rather than one pass.

## Known bugs and gaps

- The first run's PMU columns on the c6g are wrong: five events were opened un-grouped and the multiplexing was not scaled (add_chain256 showed about 104 instructions per 256-add iteration).
  `calbench.c` here is fixed (one cycles+instructions group plus single events, all scaled by time_enabled/time_running), but the stored results predate the fix, so ignore `cyc..llcmiss` in `results_2026-10-20/`.
- The t4g.nano reports pmu=1 but every counter reads -1: it has no PMU, and that is expected.
- The fix compiles in the alpine container and runs there (the Docker VM has no PMU, so the columns read -1); the scaled counters have not been checked on a c6g.
- The foolish kernel functions (state encode/decode, evwire) are not in the suite yet.
- `run_rig.sh` is untested.
- The raw results were trimmed to text: instance ids, the IPv6 address and the MAC in the spec files are redacted, and the tarballs are dropped.
