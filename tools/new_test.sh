#!/usr/bin/env bash
# new_test.sh - `make -C <cbase> new DIR=<path>` (tools/new.sh) as a person runs it, so the starter
# cannot rot: the server it writes ships no test of its own, and this is the one that holds it.
#
#   bash tools/new_test.sh      (cbase's make check runs it; CHECK_JOBS caps the build's -j)
#
# The cases, each under build/new_test/ (a fresh tree every run, inside this repository's ignored
# build/, so none of the repository's gates reads it):
#   * `make new DIR=hello`, run from build/new_test/: DIR is taken from where make ran, the three
#     files are written, and CBASE is this checkout as a path from DIR (three levels up);
#   * `make serve` in it, with a clean environment and only CBASE_LISTEN=127.0.0.1:0: it builds and
#     prints the "cbase: listening ... http://127.0.0.1:<port>/" line, GET / answers 200 with
#     exactly "cbase\n", and GET /health answers 200; SIGTERM to make's child (the server) stops it;
#   * a DIR that is not empty, and a DIR that is a file, are refused and left as they were; an
#     empty one is taken;
#   * GIT=1, its file output alone and with no network: git's url.<this checkout>.insteadOf points
#     the submodule's URL here and only the file protocol is allowed, so .gitmodules must name the
#     public URL, the gitlink must be this checkout's commit, every file must be staged, and
#     CBASE must be cbase. Skipped, and
#     said, when this cbase is not a git checkout.
# Everything started is stopped by PID on exit.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
MAKE_BIN="${MAKE:-make}"
JOBS="${CHECK_JOBS:-2}"
URL=https://github.com/alexsave/cbase
T="$ROOT/build/new_test"
rm -rf "$T"
mkdir -p "$T"

MPID=""
cleanup() {
  if [ -n "$MPID" ]; then
    pkill -TERM -P "$MPID" 2>/dev/null || true
    kill -TERM "$MPID" 2>/dev/null || true
    wait "$MPID" 2>/dev/null || true
  fi
}
trap cleanup EXIT

fails=0
fail() { echo "FAIL: make new: $*"; fails=$((fails + 1)); }
ok() { echo "ok   make new: $*"; }

# new <log> <make arguments...>: `make -C <cbase> new ...` from $T, as a shell there runs it.
new() {
  local log="$1"; shift
  (cd "$T" && export PWD="$T" && "$MAKE_BIN" --no-print-directory -C "$ROOT" new "$@") > "$T/$log.log" 2>&1
}
# tree <dir>: every path under <dir> with its contents' checksum, to see that a refusal left it be.
tree() { (cd "$1" && find . -type f -exec cksum {} + | sort; find . | sort); }

# ---- 1. make new DIR=hello, relative to where make ran ------------------------------------------
H="$T/hello"
if new hello "DIR=hello"; then ok "make new DIR=hello from $T wrote $H"
else fail "make new DIR=hello failed"; cat "$T/hello.log"; exit 1; fi
for f in Makefile src/server.c .gitignore; do
  [ -f "$H/$f" ] || fail "no $f in $H"
done
got="$(sed -n 's/^CBASE := //p' "$H/Makefile")"
want=../../..   # repo-gate: scratch tree (build/new_test/hello to this checkout)
if [ "$got" = "$want" ]; then ok "CBASE is this checkout as a path from DIR ($want)"
else fail "CBASE is '$got', not $want"; fi
if grep -qx 'build/' "$H/.gitignore"; then ok ".gitignore holds build/"
else fail ".gitignore does not hold build/"; fi

# ---- 2. make serve there: GET / is "cbase\n", GET /health is 200 -------------------------------
env -i PATH="$PATH" HOME="$HOME" TMPDIR="${TMPDIR:-/tmp}" CBASE_LISTEN=127.0.0.1:0 \
  "$MAKE_BIN" --no-print-directory -C "$H" -j"$JOBS" serve > "$T/serve.out" 2> "$T/serve.err" &
MPID=$!
line="" n=0
until line="$(grep -m1 '^cbase: listening ' "$T/serve.out" 2>/dev/null)" || [ "$n" -gt 3000 ]; do
  kill -0 "$MPID" 2>/dev/null || break
  sleep 0.2; n=$((n + 1))
done
show() { echo "---- make serve stdout"; cat "$T/serve.out"; echo "---- make serve stderr"; tail -40 "$T/serve.err"; }
if [ -z "$line" ]; then
  fail "make serve in the new server did not come up listening"; show; exit 1
fi
read -r _ _ _ url _ <<< "$line"
case "$url" in
  http://127.0.0.1:*/) ok "make serve built it and prints $url" ;;
  *) fail "the startup line names no URL: $line"; show; exit 1 ;;
esac

code="$(curl -s --max-time 5 -o "$T/root.body" -w '%{http_code}' "$url" || true)"
if [ "$code" = 200 ] && printf 'cbase\n' | cmp -s - "$T/root.body"; then ok "GET / answers 200 with exactly \"cbase\\n\""
else fail "GET / answered $code with '$(cat "$T/root.body" 2>/dev/null)', not 200 \"cbase\\n\""; fi
code="$(curl -s --max-time 5 -o /dev/null -w '%{http_code}' "${url}health" || true)"
if [ "$code" = 200 ]; then ok "GET /health answers 200"
else fail "GET /health answered $code, not 200"; fi

server="$(pgrep -P "$MPID" | head -1 || true)"
if [ -n "$server" ]; then kill -TERM "$server" 2>/dev/null || true; fi
n=0
while kill -0 "$MPID" 2>/dev/null && [ "$n" -lt 150 ]; do sleep 0.1; n=$((n + 1)); done
if kill -0 "$MPID" 2>/dev/null; then fail "make serve did not stop on SIGTERM to its child"
else wait "$MPID" 2>/dev/null || true; MPID=""; ok "SIGTERM to make's child stopped the server"; fi

# ---- 3. a DIR that is not empty, or not a directory, is refused and left as it was --------------
before="$(tree "$H")"
if new again "DIR=hello"; then fail "make new over the non-empty $H succeeded"
elif grep -q 'is not empty' "$T/again.log"; then ok "a non-empty DIR is refused: $(head -1 "$T/again.log")"
else fail "make new over the non-empty $H failed without saying why"; cat "$T/again.log"; fi
[ "$(tree "$H")" = "$before" ] || fail "the refused make new changed $H"
: > "$T/afile"
if new file "DIR=afile"; then fail "make new over a file succeeded"
elif [ -f "$T/afile" ] && [ ! -s "$T/afile" ]; then ok "a DIR that is a file is refused and left as it was"
else fail "the refused make new changed the file $T/afile"; fi
mkdir "$T/empty"
if new empty "DIR=$T/empty" && [ -f "$T/empty/src/server.c" ]; then ok "an empty DIR, named absolutely, is taken"
else fail "make new into the empty $T/empty failed"; cat "$T/empty.log"; fi

# ---- 4. GIT=1: the submodule's file output, with no network --------------------------------------
if sha="$(git -C "$ROOT" rev-parse --verify HEAD 2>/dev/null)"; then
  G="$T/withgit"
  if GIT_CONFIG_COUNT=2 GIT_CONFIG_KEY_0="url.$ROOT.insteadOf" GIT_CONFIG_VALUE_0="$URL" \
     GIT_CONFIG_KEY_1=protocol.file.allow GIT_CONFIG_VALUE_1=always GIT_ALLOW_PROTOCOL=file \
     new withgit "DIR=withgit" GIT=1; then
    got="$(git config -f "$G/.gitmodules" --get submodule.cbase.url || true)"
    if [ "$got" = "$URL" ]; then ok "GIT=1: .gitmodules names $URL"
    else fail "GIT=1: .gitmodules names '$got', not $URL"; fi
    got="$(git -C "$G" ls-files -s cbase | awk '{ print $1, $2 }')"
    if [ "$got" = "160000 $sha" ]; then ok "GIT=1: cbase is a submodule at this checkout's commit"
    else fail "GIT=1: the index holds '$got' for cbase, not '160000 $sha'"; fi
    got="$(git -C "$G" ls-files | tr '\n' ' ')"
    if [ "$got" = ".gitignore .gitmodules Makefile cbase src/server.c " ]; then ok "GIT=1: every file is staged"
    else fail "GIT=1: the index holds '$got', not the four files and cbase"; fi
    got="$(sed -n 's/^CBASE := //p' "$G/Makefile")"
    if [ "$got" = cbase ]; then ok "GIT=1: CBASE is cbase"
    else fail "GIT=1: CBASE is '$got', not cbase"; fi
    if sed 's/"withgit"/"hello"/' "$G/src/server.c" | cmp -s - "$H/src/server.c" && cmp -s "$G/.gitignore" "$H/.gitignore"; then
      ok "GIT=1: the same server.c (but for its name) and .gitignore"
    else fail "GIT=1: server.c or .gitignore differs from the plain run's"; fi
  else
    fail "make new GIT=1 failed"; cat "$T/withgit.log"
  fi
else
  echo "skip make new: GIT=1 ($ROOT is not a git checkout)"
fi

if [ "$fails" -gt 0 ]; then echo "make new: $fails failure(s)"; exit 1; fi
echo "make new: passed"
