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
- The exceptions are what crosses the two vCPUs (five-pass medians, analysis_2026-10-20/NANO_MODEL.md 2.1 and 2.2).
  A cross-vCPU futex wake-up takes 9.4 / 12.0 / 23.2 / 38.7 us after 0 / 1 / 10 / 100 ms of idle on the nano against 4.55 / 5.08 / 5.84 / 6.49 us on the c6g: 2.1x, 2.4x, 4.0x and 6.0x (through epoll and an eventfd: 1.9x to 5.9x).
  A contended mutex costs 3.5 us per lock+unlock on the nano (p50; p99 43 us) against 0.14 us, and a contended atomic add 1.2 us against 41 ns.
- `pwrite+fdatasync` is not a nano penalty: a 4 KiB overwrite and its fdatasync take 0.90 ms (p50) on both boxes over the five passes.
  The "2.5-2.9x slower" of the first note compared pass 1 with the c6g's occasional 0.35 ms mode (two of its five passes), which the nano never showed.
- Both boxes sustain 2.48-2.50 GHz (the 256-add chain: 102.4 ns per 256 adds).
- There is no steal in unlimited mode.
- Same-core ping-pong round trips and wake-ups look faster on the nano (0.4-0.6x), but that is the c6g's measurement: its measuring thread held five hardware perf events, and every switch of it traps the PMU accesses to the hypervisor (NANO_MODEL.md 2.4); the nano's PMU reads failed, so it paid none. The contended atomic of pass 3 on the c6g is an outlier (8 ms).
  The full five-pass data adds 1746 rows; many of the malloc two-thread rows differ between boxes there, so use the medians in `compare.py` rather than one pass.

## The sqlite group (2026-10-20, results_2026-10-20_sqlite/)

### What it runs

`calbench sqlite PASS` (sqlite_group.inc) uses the SQLite that serve links (alpine 3.24's sqlite-static, 3.53.4, built in the same pinned image) with the store's settings from store_sqlite.c.
Those are `SQLITE_OPEN_FULLMUTEX`, busy_timeout 5000, WAL, synchronous=NORMAL, the default page_size (4096) and cache_size (-2000, so 2 MB), mmap_size 0, the store's pooled allocator, and prepared `BEGIN IMMEDIATE` / `COMMIT`.
The writer's automatic checkpoint is replaced by a PASSIVE checkpoint every 1000 WAL frames, as the store's checkpointer does; here it runs untimed between samples, so the transaction rows carry no checkpoint, and the checkpoint is its own row.
The table is foolish's `games` (random 16-hex `id TEXT PRIMARY KEY`, so every row also writes the autoindex), filled with 20000 rows of 1500 B state before the timed rows, on the root EBS volume (gp3, XFS) under /var/tmp.

- Syscalls on scratch files of the same filesystem: fcntl F_SETLK lock+unlock pairs on SQLite's byte ranges (the 510-byte SHARED range, the WAL write lock byte, a read mark), F_GETLK, each uncontended and with another process holding the locks a reading connection holds, and an F_SETLK that conflicts (EAGAIN); getpid; fstat; pwrite into a WAL-shaped file at 24 B (frame header), 4096 B and 4120 B; pread of a page from the page cache and cold (POSIX_FADV_DONTNEED before each sample); pwrite+fdatasync and pwrite+fsync of a frame; the -shm header read and a seq-cst store into it; mapping a 32 KB -shm region.
- Transactions: one-row INSERT (128, 600, 1200, 1500 B state) and UPDATE (1200, 1500 B); INSERT of N rows in one transaction (N 1, 10, 100, 1000 or 2000; 128, 600 and 1500 B); SELECT by primary key warm (50 hot rows), SQLite-cache miss (any of 20000 rows; the OS page cache holds the file) and cold (SQLite's cache released and the file dropped from the page cache); PASSIVE checkpoint of 1, 10, 100 and 1000 pages.
- The same INSERT, UPDATE and SELECT with another process holding an open read transaction on the WAL.

Every transaction row is k = 1 (one transaction per sample), so p50 and p99 are true per-transaction numbers.

### How syscalls are counted

Two independent ways, and they agree.

- In calbench: build.sh links with `-Wl,--wrap=` for every libc entry point SQLite's unix VFS uses (fcntl split by command and lock type, pread, pwrite, fsync, fdatasync, getpid, fstat, mmap and the rest), so each call passes a counter that is on only while a sample runs.
  Each such call is one syscall in musl.
  The `#sys` comment line after each row gives the mean per transaction, with /proc/self/io `syscr`/`syscw` beside it as a kernel-side check (they match the pread and pwrite counts exactly).
  It does not see musl's own internal syscalls (malloc's mmap).
- strace: after the last pass the runner installs strace and runs `calbench sqlitecount OP N` under `strace -f -c` with N = 0 and N = 500 (strace_<op>_n<N>.txt); the difference over 500 is every syscall per transaction, malloc and the amortised checkpoints included.
  It shows no mmap, munmap, brk or getpid per transaction (the pooled allocator does its job; SQLite calls getpid 5 times at open and never per transaction) and that musl issues pwrite as `pwritev2`.

### Results (t4g.nano, unlimited, 5 passes 3 minutes apart; medians over passes of each row's p50)

| operation | p50 | p99 | syscalls per op (wrap; strace) |
|---|---|---|---|
| fcntl F_SETLK lock+unlock pair | 0.88 us (0.44 per call) | 1.0 us | 2 |
| same, another process holds the locks | 0.88-0.89 us | 1.0 us | 2 |
| fcntl F_GETLK | 0.28 us (0.29 held) | 0.30 us | 1 |
| fcntl F_SETLK conflicting (EAGAIN) | 0.40 us | 0.43 us | 1 |
| getpid | 0.11 us | 0.12 us | 1 (SQLite: 0 per transaction) |
| fstat | 0.24 us | 0.24 us | 1 |
| pwrite to the WAL, 24 B / 4096 B / 4120 B | 0.68 / 1.27 / 1.17 us | 1.0 / 1.5 / 1.3 us | 1 |
| pread of a page, page cache | 1.22 us | 1.6 us | 1 |
| pread of a page, cold (EBS) | 0.20 ms (passes 1-2) and 0.57 ms (passes 3-5) | 0.64 ms | 1 |
| pwrite+fdatasync / pwrite+fsync of a frame | 0.92 / 0.94 ms (0.43-0.47 in one pass) | 1.0 / 2.0 ms | 2 |
| -shm header read (2 x 48 B and compare) | 0.05 us | 0.05 us | 0 |
| -shm map 32 KB + touch + unmap (once per connection) | 6.3 us | 9.5 us | 2 |
| INSERT 1 row, 128 / 600 B | 15.9 / 16.2 us | 41 / 42 us | 11.0 / 11.7 |
| INSERT 1 row, 1200 / 1500 B | 21.5 / 24.3 us | 65 / 50 us | 12.2 / 12.9 (strace 14.1 with amortised checkpoints) |
| UPDATE 1 row, 1200 / 1500 B | 14.7 / 14.4 us | 26 / 23 us | 9.1 / 9.1 (strace 9.3) |
| INSERT 10 rows (128 / 600 / 1500 B) | 90 / 101 / 95 us | 1.8 / 1.8 / 2.0 ms | 39 / 46 / 43 (strace 59 at 1500 B) |
| INSERT 100 rows (128 / 600 / 1500 B) | 0.72 / 0.85 / 0.79 ms | 2.6 / 2.7 / 2.9 ms | 268 / 333 / 292 (strace 503 at 1500 B) |
| INSERT 1000 rows (128 / 600 B) | 7.2 / 9.0 ms | 9.2 / 11.1 ms | 2940 / 4201 |
| INSERT 2000 rows, 1500 B | 18.2 ms | 20.0 ms | 6589 |
| SELECT by pk, warm | 3.9 us | 8.7 us | 2.2 (strace 2.1) |
| SELECT by pk, SQLite-cache miss | 8.0 us | 15.9 us | 3.6 (strace 3.3) |
| SELECT by pk, cold | 0.95-1.8 ms (by pass, as the cold pread) | 2.5 ms | 9 (7 preads) |
| PASSIVE checkpoint, 1 / 10 / 100 / 1000 pages | 2.9 / 3.4 / 7.8 / 13.2 ms | 4.0 / 4.6 / 9.2 / 14.1 ms | 12 / 30 / 210 / 2009 |

What the syscalls are:

- Every write transaction takes 6 fcntl F_SETLK calls (1 F_RDLCK, 2 F_WRLCK, 3 F_UNLCK, across the db file and the -shm locks), every autocommit SELECT 2 (F_RDLCK and F_UNLCK).
  A second process holding locks does not change the count or the price.
- Every WAL frame is 2 pwrites (the 24-byte header, then the page).
  A one-row INSERT writes about 3.4 frames (the table leaf, the random index leaf, and a split now and then), a one-row UPDATE 1 frame.
- pread is a page SQLite's 2 MB cache does not hold: 0.04 to 1 per small transaction, and 1 per row once a transaction touches more than about 500 pages (1000 rows and up), when it also starts spilling dirty pages to the WAL mid-transaction.
- A checkpoint is exactly 10 + 2 x pages syscalls: a pread and a pwrite per page, 2 fsyncs, 1 fstat, 1 ftruncate and 6 fcntl.
- In the transaction rows syscalls are about 40% of the time: a 1500 B INSERT spends 2.6 us in fcntl and 6.7 us in pwrite of its 24.3 us, an UPDATE 5.9 of 14.4 us.

### The formula

For an INSERT transaction of N rows of B bytes into a table like `games` (random text key, so one index leaf per row), with the page cache warm:

    T(N, B) = 9 + 8.2 x N us                 (N >= 10, B 128 to 1500: within 15% of every row; B moves it no more than that)
    T(1, B) = 16 us (B <= 600), 21.5 us (B 1200), 24.3 us (B 1500)
    S(N)    = 6 + 3 x N syscalls             (N <= 100: 6 fcntl, 2 pwrites per frame at about 1.4 frames per row, a few preads)
    S(N)    = 6 + 3.3 x N syscalls           (N >= 1000: plus a pread per row once the transaction outgrows the 2 MB cache)
    UPDATE of one row: 14.5 us, 9 syscalls (6 fcntl, 2 pwrite, 1 pread)

and, off the request path, every 1000 WAL frames the checkpointer spends about 13 ms of wall time (2009 syscalls, 2 fsyncs; it is waiting on EBS, not CPU, for most of it).

To price SQLite's syscalls one by one (all hitting the page cache):

    t_sys = 0.44 x n_fcntl + 0.68 x n_frames + 1.27 x n_frames + 1.22 x n_pread  us
          = about 0.7 us per syscall in SQLite's mix (INSERT 9.3 us / 12.9, UPDATE 5.9 us / 9.05)

### How it plugs into NANO_MODEL.md

The transform in NANO_MODEL.md section 3 builds the nano's cycles per move from the rig's compute C, which has its syscalls taken out, and prices sends, hand-offs, wake-ups and fsyncs, but not SQLite's other syscalls.
Add the term

    + 0.7 us x n_sql     (n_sql: SQLite syscalls per move, from the `#sys` counts above or strace)

or, where the mix is known, `0.44 x n_fcntl + 1.95 x n_frames + 1.22 x n_pread`.
For BASE sat8 and its 48 SQLite syscalls per move this adds 34 us: 155 + 34 = 189 us against 187.2 measured, so the -17% becomes +1%.
NOW does not touch SQLite on the move path, so its predictions are unchanged.
For a flush or a game end, use T(N, B) and S(N) per transaction instead of the per-syscall price: they carry SQLite's own CPU as well.
The nano and the c6g price syscalls the same (see First findings), so these numbers stand for a c6g too.

### Caveats

- With another process holding an open read transaction, the INSERT and SELECT rows barely move (37 against 24 us, 4.0 against 3.9 us), but a one-row UPDATE goes from 14 to 406 us p50 with 0.8 more preads per transaction at disk latency, in both runs, and a 100-row INSERT from 0.79 to 1.75 ms.
  It is not lock waiting (the fcntl count is unchanged) and the first run's cold page cache was ruled out (the second run warms the file first and saw the same).
  The cause is not identified; production meets it only while a second process (cbase-import, a backup) has the database open.
- Pass 1 ran right after `dnf install strace` on a box with 250 MB available, and its SQLite-cache-miss SELECT (and in the first run its UPDATEs) read from disk at about 0.35-0.4 ms; passes 2-5 do not, and the medians above are not affected.
- EBS latency is bimodal from pass to pass (a cold page read 0.2 or 0.57 ms, a frame's fsync 0.45 or 0.95 ms), so the cold and fsync rows are ranges, not one number.
- results_2026-10-20_sqlite/t4g_run1/ is the first run on another nano: every row agrees with the second run except the reader rows, which read a file the cold-SELECT row had just dropped from the page cache (fixed: the group now warms the file before the transaction rows).
- The `--wrap` counters sit in front of every libc call in the binary, so the other groups' pwrite and fcntl rows now pay one predictable branch (under a nanosecond) more than the 2026-10-20 run did.

## Known bugs and gaps

- The first run's PMU columns on the c6g are wrong: five events were opened un-grouped and the multiplexing was not scaled (add_chain256 showed about 104 instructions per 256-add iteration).
  `calbench.c` here is fixed (one cycles+instructions group plus single events, all scaled by time_enabled/time_running), but the stored results predate the fix, so ignore `cyc..llcmiss` in `results_2026-10-20/`.
- The t4g.nano reports pmu=1 but every counter reads -1: it has no PMU, and that is expected.
- The fix compiles in the alpine container and runs there (the Docker VM has no PMU, so the columns read -1); the scaled counters have not been checked on a c6g.
- The foolish kernel functions (state encode/decode, evwire) are not in the suite yet.
- `run_rig.sh` is untested.
- The raw results were trimmed to text: instance ids, the IPv6 address and the MAC in the spec files are redacted, and the tarballs are dropped.
