#!/usr/bin/env bash
# serve_test.sh - `make serve` (cbase.mk, tools/serve.sh) as a person runs it: the example product's
# `make -C example serve`, each case a real make with a real server behind it.
#
#   bash tools/serve_test.sh      (cbase's make check runs it after building example/build/cbase)
#
# The cases:
#   * absent env file, CBASE_LISTEN given: serve boots (no env file is no error), listens where
#     CBASE_LISTEN says and not on PORT, answers GET /health 200, runs in APP_SERVE_DIR (a path
#     with a space in it), and make's child IS the server: SIGTERM sent to that child stops it,
#     make returns, and nothing is left answering;
#   * present env file, no CBASE_LISTEN: the run directory's .env.local is read (it holds
#     CBASE_DEV=1, which serve needs to boot without a JWT secret, and the environment does not
#     hold it), and the listen default is 127.0.0.1 on PORT;
#   * a bad line in it: the boot is refused with the strict reader's own sentence;
#   * CBASE_ENV_FILE already set: it wins over the run directory's file;
#   * the default run directory: with APP_SERVE_DIR unset serve runs in the directory of the
#     Makefile make read (example/).
#   * the default port: with no PORT and no CBASE_LISTEN serve listens on 127.0.0.1:8642 (the
#     port README.md and cbase.mk name, core/src/config.h CB_LISTEN_DEFAULT); skipped, and said,
#     when something on this machine already listens there.
#   * a product's own serve lines (cbase.mk's header): `serve: <prerequisite>` is built before serve
#     runs, and `serve: export VAR := value` reaches the server's environment.
#   * the old flag names (cbase.mk's header, SERVER_CFLAGS): APP_CFLAGS in a Makefile with no
#     APP_WASM, and APP_LDLIBS, stop the build with a sentence naming the new name; APP_CFLAGS with
#     an APP_WASM (set in a second makefile, read after the product's) is the UI module's, and the
#     build goes on.
# Every make is started with a clean environment (env -i), so no CBASE_* of the caller's and no
# parent make's flags reach it. Everything started is stopped by PID on exit.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
EX="$ROOT/example"
[ -x "$EX/build/cbase" ] || { echo "FAIL: no $EX/build/cbase (make -C example build/cbase first)"; exit 1; }
MAKE_BIN="${MAKE:-make}"

T="$(mktemp -d)"
T="$(cd "$T" && pwd -P)"
RUN="$T/run dir"
mkdir -p "$RUN"
PIDS=()
cleanup() {
  local p
  for p in "${PIDS[@]+"${PIDS[@]}"}"; do kill -TERM "$p" 2>/dev/null || true; done
  sleep 0.3
  for p in "${PIDS[@]+"${PIDS[@]}"}"; do kill -KILL "$p" 2>/dev/null || true; done
  rm -rf "$T"
}
trap cleanup EXIT

fails=0
fail() { echo "FAIL: $*"; fails=$((fails + 1)); }
ok() { echo "ok   $*"; }
# is <got> <want> <ok sentence> <fail sentence>
is() { if [ "$1" = "$2" ]; then ok "$3"; else fail "$4"; fi; }

# start <label> <env assignments...> -- <make arguments...>: `make -C example serve` in the
# background, stdout to $T/<label>.out and stderr to $T/<label>.err. Sets MPID.
start() {
  local label="$1"; shift
  local envs=()
  while [ "$1" != "--" ]; do envs+=("$1"); shift; done
  shift
  env -i PATH="$PATH" HOME="$HOME" TMPDIR="${TMPDIR:-/tmp}" CBASE_DB=:memory: "${envs[@]+"${envs[@]}"}" \
    "$MAKE_BIN" --no-print-directory -C "$EX" serve "$@" > "$T/$label.out" 2> "$T/$label.err" &
  MPID=$!
  PIDS+=("$MPID")
}

# children <pid>: the pids whose parent is <pid>.
children() { ps -A -o pid= -o ppid= | awk -v p="$1" '$2 == p { print $1 }'; }

# listening <label>: waits up to 30 s for serve's "cbase: listening <host>:<port> <url>" line and sets
# GOT to the port, or to nothing when make ended first or the time ran out. Records every process
# under make, so the cleanup stops a server even when it is not make's child.
listening() {
  local label="$1" n=0 line="" c g
  until line="$(grep -m1 '^cbase: listening ' "$T/$label.out" 2>/dev/null)" || [ "$n" -gt 150 ]; do
    kill -0 "$MPID" 2>/dev/null || break
    sleep 0.2; n=$((n + 1))
  done
  for c in $(children "$MPID"); do
    PIDS+=("$c")
    for g in $(children "$c"); do PIDS+=("$g"); done
  done
  GOT=""
  # "cbase: listening <host>:<port> <url>": the port ends the third word.
  [ -z "$line" ] || { line="${line#cbase: listening }"; line="${line%% *}"; GOT="${line##*:}"; }
}

# ended <pid> <seconds>: true once <pid> has exited, within the time.
ended() {
  local n=0
  while kill -0 "$1" 2>/dev/null; do
    [ "$n" -gt $(( $2 * 10 )) ] && return 1
    sleep 0.1; n=$((n + 1))
  done
}

code() { curl -s -o /dev/null -w '%{http_code}' --max-time 5 "http://127.0.0.1:$1$2" || true; }

# cwd_of <pid>: the process's working directory.
cwd_of() {
  if [ -e "/proc/$1/cwd" ]; then readlink "/proc/$1/cwd"
  else lsof -a -p "$1" -d cwd -Fn 2>/dev/null | sed -n 's/^n//p'
  fi
}

# comm_of <pid>: the base name of the process's program.
comm_of() { basename "$(ps -o comm= -p "$1")"; }

# stop <label>: SIGTERM to make's children (the server), then make is reaped. A make that does not
# end within 15 s is a failure.
stop() {
  local c
  for c in $(children "$MPID"); do kill -TERM "$c" 2>/dev/null || true; done
  ended "$MPID" 15 || fail "make serve ($1) did not stop on SIGTERM to its child"
  wait "$MPID" 2>/dev/null || true
}

show() { echo "---- $1 stdout"; cat "$T/$1.out"; echo "---- $1 stderr"; cat "$T/$1.err"; }

# ---- 1. no env file, CBASE_LISTEN given, APP_SERVE_DIR with a space, SIGTERM to make's child ---
start absent CBASE_DEV=1 PORT=1 CBASE_LISTEN=127.0.0.1:0 -- "APP_SERVE_DIR=$RUN"
listening absent; port="$GOT"
if [ -z "$port" ] && grep -q 'listen 127.0.0.1:1:' "$T/absent.err"; then
  fail "CBASE_LISTEN=127.0.0.1:0 did not override the listen default: serve tried PORT=1"; show absent
elif [ -z "$port" ]; then
  fail "make serve with no .env.local did not come up listening (an absent env file must be no error)"; show absent
else
  if [ "$port" != 1 ]; then ok "CBASE_LISTEN overrides the default (listening on $port, not PORT=1)"
  else fail "CBASE_LISTEN=127.0.0.1:0 did not override the listen default: serve took PORT=1"; fi
  c="$(code "$port" /health)"
  is "$c" 200 "make serve with no .env.local answers GET /health 200" "GET /health answered $c, not 200"
  child="$(children "$MPID" | head -1)"
  if [ -z "$child" ]; then
    fail "make has no child process"
  else
    w="$(cwd_of "$child")"
    is "$w" "$RUN" "serve runs in APP_SERVE_DIR ($RUN)" "serve runs in '$w', not APP_SERVE_DIR '$RUN'"
    w="$(comm_of "$child")"
    is "$w" cbase "make's child is the server itself (exec'd)" "make's child is '$w', not the serve binary: the launcher did not exec it"
    kill -TERM "$child"
    if ended "$MPID" 15; then ok "SIGTERM to make's child ends make serve"
    else fail "make serve still runs 15 s after SIGTERM to its child"; fi
    wait "$MPID" 2>/dev/null || true
    n=0; while [ "$(code "$port" /health)" != 000 ] && [ "$n" -lt 50 ]; do sleep 0.1; n=$((n + 1)); done
    is "$(code "$port" /health)" 000 "nothing answers on $port once SIGTERM reached the server" \
      "a server still answers on $port after SIGTERM to make's child: the signal did not reach serve"
  fi
fi

# ---- 2. a present .env.local is read, and the listen default is 127.0.0.1:$PORT ----------------
# The port is the one case 1's server just let go of, so it is free.
if [ -n "${port:-}" ]; then
  printf '# the settings make serve names to serve\nCBASE_DEV=1\n' > "$RUN/.env.local"
  start present PORT="$port" -- "APP_SERVE_DIR=$RUN"
  listening present; got="$GOT"
  if [ "$got" = "$port" ]; then
    ok "the listen default is 127.0.0.1 on PORT ($port), and the present .env.local was read (it alone sets CBASE_DEV=1)"
    c="$(code "$port" /health)"
    is "$c" 200 "make serve on .env.local answers GET /health 200" "GET /health answered $c, not 200"
  elif [ -n "$got" ]; then
    fail "with PORT=$port and no CBASE_LISTEN serve listens on $got, not $port"
  else
    fail "make serve did not boot on a .env.local holding CBASE_DEV=1 (was the file named to serve as CBASE_ENV_FILE?)"; show present
  fi
  stop present
fi

# ---- 3. a bad line stops the boot with the strict reader's sentence -----------------------------
printf 'this is not a setting\n' > "$RUN/.env.local"
start bad CBASE_DEV=1 CBASE_LISTEN=127.0.0.1:0 -- "APP_SERVE_DIR=$RUN"
if ended "$MPID" 30; then
  want="CBASE_ENV_FILE $RUN/.env.local line 1 is not KEY=VALUE (no '=')"
  if wait "$MPID"; then fail "make serve exited 0 on a bad .env.local line"
  elif grep -qF "$want" "$T/bad.err"; then ok "a bad .env.local line refuses the boot: $want"
  else fail "a bad .env.local line did not refuse the boot with the reader's sentence ($want)"; show bad; fi
else
  fail "make serve still runs on a bad .env.local line"; show bad
fi

# ---- 4. CBASE_ENV_FILE already set wins over the run directory's file --------------------------
printf 'CBASE_DEV=1\n' > "$T/given.env"
start given CBASE_ENV_FILE="$T/given.env" CBASE_LISTEN=127.0.0.1:0 -- "APP_SERVE_DIR=$RUN"
listening given; p4="$GOT"
if [ -n "$p4" ] && [ "$(code "$p4" /health)" = 200 ]; then
  ok "a CBASE_ENV_FILE already set is kept (the run directory's bad .env.local is not named)"
else
  fail "with CBASE_ENV_FILE set make serve did not boot on it"; show given
fi
stop given

# ---- 5. the default run directory is the Makefile's -------------------------------------------
start default CBASE_DEV=1 CBASE_LISTEN=127.0.0.1:0 -- APP_ENV_FILE=
listening default; p5="$GOT"
child="$(children "$MPID" | head -1)"
if [ -n "$p5" ] && [ -n "$child" ]; then
  w="$(cwd_of "$child")"
  is "$w" "$EX" "with APP_SERVE_DIR unset serve runs in the Makefile's directory ($EX)" \
    "with APP_SERVE_DIR unset serve runs in '$w', not $EX"
else
  fail "make serve in the default run directory did not come up"; show default
fi
stop default

# ---- 6. the default port, with no PORT and no CBASE_LISTEN -------------------------------------
DEFAULT_PORT=8642
if [ "$(code "$DEFAULT_PORT" /health)" != 000 ]; then
  echo "skip the default port: something already answers on 127.0.0.1:$DEFAULT_PORT"
else
  start dport CBASE_DEV=1 -- APP_ENV_FILE=
  listening dport; p6="$GOT"
  if [ "$p6" = "$DEFAULT_PORT" ] && [ "$(code "$p6" /health)" = 200 ]; then
    ok "with no PORT and no CBASE_LISTEN serve listens on 127.0.0.1:$DEFAULT_PORT"
  elif [ -n "$p6" ]; then
    fail "with no PORT and no CBASE_LISTEN serve listens on $p6, not the default $DEFAULT_PORT"
  else
    fail "make serve with no PORT and no CBASE_LISTEN did not come up"; show dport
  fi
  stop dport
fi

# ---- 7. a product extends serve with its own lines, and no knob ---------------------------------
# cbase.mk's header and README.md: `serve: <prerequisite>` builds something first (a page), and
# `serve: export VAR := value` puts a variable in serve's environment. Here a second makefile, read
# after the product's, does both: its prerequisite writes a mark, and its export is CBASE_DEV=1,
# which the run's environment does not hold and without which the example (it keeps a store) does
# not boot with no JWT secret. So the server answering proves the export reached it.
MARK="$T/prereq.mark"
printf 'serve: serve-test-prereq
serve: export CBASE_DEV := 1
.PHONY: serve-test-prereq
serve-test-prereq:
	@echo built > "%s"
' "$MARK" > "$T/ext.mk"
start ext CBASE_LISTEN=127.0.0.1:0 -- APP_ENV_FILE= -f Makefile -f "$T/ext.mk"
listening ext; p7="$GOT"
if [ -n "$p7" ] && [ "$(code "$p7" /health)" = 200 ]; then
  ok "serve: export CBASE_DEV := 1 in the product's Makefile reaches the server (it boots with no JWT secret)"
else
  fail "with serve: export CBASE_DEV := 1 the server did not boot: the product's export did not reach serve"; show ext
fi
if [ "$(cat "$MARK" 2>/dev/null)" = built ]; then ok "serve: <prerequisite> in the product's Makefile is built before serve runs"
else fail "the product's serve: <prerequisite> was not built before serve ran"; fi
stop ext

# ---- 8. the old flag names stop the build, naming the new ones ---------------------------------
# build/cbase is built, so each make reaches only the flag stamp (tools/c_rules.mk), which reads
# CB_CFLAGS every run: that is where the APP_CFLAGS check is, once the whole Makefile is read.
# oldflag <label> <want in stderr> <make arguments...>: the make must fail and say <want>.
oldflag() {
  local label="$1" want="$2"; shift 2
  if env -i PATH="$PATH" HOME="$HOME" TMPDIR="${TMPDIR:-/tmp}" "$MAKE_BIN" --no-print-directory -C "$EX" "$@" build/cbase \
       > "$T/$label.out" 2> "$T/$label.err"; then
    fail "make with $label exited 0: the old name was taken silently"
  elif grep -qF "$want" "$T/$label.err"; then ok "$label stops the build: $want"
  else fail "make with $label failed without naming the new name ($want)"; show "$label"; fi
}
oldflag APP_CFLAGS "name the server's flags SERVER_CFLAGS" APP_CFLAGS=-DOLD_NAME
oldflag APP_LDLIBS "name the server's link flags SERVER_LDFLAGS" APP_LDLIBS=-lm
printf '# a UI module, named after cbase.mk is read: APP_CFLAGS is its flags\nAPP_WASM := ui/app.wasm\n' > "$T/ui.mk"
if env -i PATH="$PATH" HOME="$HOME" TMPDIR="${TMPDIR:-/tmp}" "$MAKE_BIN" --no-print-directory -C "$EX" \
     -f Makefile -f "$T/ui.mk" APP_CFLAGS=-DOLD_NAME build/cbase > "$T/ui.out" 2> "$T/ui.err"; then
  ok "APP_CFLAGS with an APP_WASM is the UI module's: the server's build goes on"
else
  fail "APP_CFLAGS with an APP_WASM (set after cbase.mk) stopped the server's build"; show ui
fi

if [ "$fails" -gt 0 ]; then echo "serve test: $fails failure(s)"; exit 1; fi
echo "serve test: make serve passed every case"
