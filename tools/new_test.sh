#!/usr/bin/env bash
# new_test.sh - `make -C <cbase> new DIR=<path>` (tools/new.sh) as a person runs it, so the starter
# cannot rot: the server it writes ships no test of its own, and this is the one that holds it.
#
#   bash tools/new_test.sh      (cbase's make check runs it; CHECK_JOBS caps the build's -j)
#
# The cases, each under build/new_test/ (a fresh tree every run, inside this repository's ignored
# build/, so none of the repository's gates reads it):
#   * `make new DIR=hello`, run from build/new_test/: DIR is taken from where make ran, the three
#     files are written, CBASE is this checkout as a path from DIR (three levels up), and the
#     server is named for DIR: APP_SERVE := hello;
#   * `make serve` in it, with a clean environment and only CBASE_LISTEN=127.0.0.1:0: it builds
#     build/hello (and no build/cbase) and prints the "cbase: listening ... http://127.0.0.1:<port>/"
#     line, GET / answers 200 with exactly "cbase\n", and GET /health answers 200; SIGTERM to make's
#     child (the server) stops it;
#   * a DIR named for one of cbase.mk's own build/ names (CB_BUILD_OWN), "obj", gets a server named
#     obj-server, a DIR whose name holds a space and a colon gets one with each made _, and
#     cbase.mk itself stops a product whose APP_SERVE is obj;
#   * a DIR that is not empty, and a DIR that is a file, are refused and left as they were; an
#     empty one is taken;
#   * GIT=1, its file output alone and with no network: git's url.<this checkout>.insteadOf points
#     the submodule's URL here and only the file protocol is allowed, so .gitmodules must name the
#     public URL, the gitlink must be this checkout's commit, every file must be staged, and
#     CBASE must be cbase. Skipped, and
#     said, when this cbase is not a git checkout;
#   * the README's whole product (its ```c block after `src/myapp.c`) as a starter's src/server.c:
#     it builds and serves, and GET /v1/hello answers as its comment says, an empty name as none.
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
got="$(sed -n 's/^APP_SERVE := //p' "$H/Makefile")"
if [ "$got" = hello ]; then ok "the server is named for DIR: APP_SERVE := hello"
else fail "APP_SERVE is '$got', not hello (DIR's name)"; fi
if grep -qx 'build/' "$H/.gitignore"; then ok ".gitignore holds build/"
else fail ".gitignore does not hold build/"; fi

# serve_up <dir> <log>: `make serve` in <dir>, with a clean environment and only
# CBASE_LISTEN=127.0.0.1:0, in the background (MPID); once it prints its startup line, url is the
# URL that line names. A server that never comes up fails the run.
serve_up() {
  env -i PATH="$PATH" HOME="$HOME" TMPDIR="${TMPDIR:-/tmp}" CBASE_LISTEN=127.0.0.1:0 \
    "$MAKE_BIN" --no-print-directory -C "$1" -j"$JOBS" serve > "$T/$2.out" 2> "$T/$2.err" &
  MPID=$!
  local line="" n=0
  until line="$(grep -m1 '^cbase: listening ' "$T/$2.out" 2>/dev/null)" || [ "$n" -gt 3000 ]; do
    kill -0 "$MPID" 2>/dev/null || break
    sleep 0.2; n=$((n + 1))
  done
  if [ -z "$line" ]; then
    fail "make serve in $1 did not come up listening"
    echo "---- make serve stdout"; cat "$T/$2.out"; echo "---- make serve stderr"; tail -40 "$T/$2.err"; exit 1
  fi
  read -r _ _ _ url _ <<< "$line"
  case "$url" in
    http://127.0.0.1:*/) ;;
    *) fail "the startup line names no URL: $line"; exit 1 ;;
  esac
}
# serve_down: SIGTERM to make's child (the server); true when make then exits.
serve_down() {
  local server n=0
  server="$(pgrep -P "$MPID" | head -1 || true)"
  if [ -n "$server" ]; then kill -TERM "$server" 2>/dev/null || true; fi
  while kill -0 "$MPID" 2>/dev/null && [ "$n" -lt 150 ]; do sleep 0.1; n=$((n + 1)); done
  if kill -0 "$MPID" 2>/dev/null; then return 1; fi
  wait "$MPID" 2>/dev/null || true; MPID=""
}
# asks <url> <code> <body> <what>: GET <url> answers <code> with exactly <body> (printf's format).
asks() {
  local code
  code="$(curl -s --max-time 5 -o "$T/ask.body" -w '%{http_code}' "$1" || true)"
  # shellcheck disable=SC2059
  if [ "$code" = "$2" ] && printf "$3" | cmp -s - "$T/ask.body"; then ok "$4: $2 \"$(tr '\n' ' ' < "$T/ask.body" | sed 's/ $//')\""
  else fail "$4: $code '$(cat "$T/ask.body" 2>/dev/null)', not $2 '$3'"; fi
}

# ---- 2. make serve there: GET / is "cbase\n", GET /health is 200 -------------------------------
serve_up "$H" serve
ok "make serve built it and prints $url"
if [ -x "$H/build/hello" ] && [ ! -e "$H/build/cbase" ]; then ok "the binary is build/hello, named for the app"
else fail "make serve built no build/hello, or a build/cbase: $(cd "$H/build" && printf '%s ' *)"; fi

code="$(curl -s --max-time 5 -o "$T/root.body" -w '%{http_code}' "$url" || true)"
if [ "$code" = 200 ] && printf 'cbase\n' | cmp -s - "$T/root.body"; then ok "GET / answers 200 with exactly \"cbase\\n\""
else fail "GET / answered $code with '$(cat "$T/root.body" 2>/dev/null)', not 200 \"cbase\\n\""; fi
code="$(curl -s --max-time 5 -o /dev/null -w '%{http_code}' "${url}health" || true)"
if [ "$code" = 200 ]; then ok "GET /health answers 200"
else fail "GET /health answered $code, not 200"; fi

if serve_down; then ok "SIGTERM to make's child stopped the server"
else fail "make serve did not stop on SIGTERM to its child"; fi

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

# ---- 3b. the server's name: one of cbase.mk's own build/ names, and bytes make cannot take -------
if new obj "DIR=obj" && [ "$(sed -n 's/^APP_SERVE := //p' "$T/obj/Makefile")" = obj-server ] &&
   grep -q '\.name = "obj-server"' "$T/obj/src/server.c"; then ok "DIR=obj, a name of cbase.mk's build/, gets the server obj-server"
else fail "DIR=obj did not get the server obj-server: $(grep '^APP_SERVE' "$T/obj/Makefile" 2>/dev/null)"; cat "$T/obj.log"; fi
if new odd "DIR=my app:1" && [ "$(sed -n 's/^APP_SERVE := //p' "$T/my app:1/Makefile")" = my_app_1 ]; then
  ok "DIR='my app:1' gets the server my_app_1"
else fail "DIR='my app:1' did not get the server my_app_1: $(grep '^APP_SERVE' "$T/my app:1/Makefile" 2>/dev/null)"; cat "$T/odd.log"; fi
if "$MAKE_BIN" --no-print-directory -C "$H" -n APP_SERVE=obj > "$T/own.log" 2>&1; then
  fail "cbase.mk took APP_SERVE=obj, one of its own build/ names"
elif grep -q 'build/obj is cbase.mk.s own' "$T/own.log"; then ok "cbase.mk stops a product whose APP_SERVE is obj: $(grep -o 'build/obj is.*' "$T/own.log" | head -1)"
else fail "make with APP_SERVE=obj failed without saying why"; cat "$T/own.log"; fi

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

# ---- 5. the README's whole product, src/myapp.c, served in a starter -----------------------------
# The README's ```c block after the line that names `src/myapp.c`, as the starter's src/server.c:
# it builds, and its GET /v1/hello greets a name, the world for no name and for an empty one alike,
# decodes an escaped & and space, and answers 400 for a name too long or a query that does not read.
R="$T/readme"
if new readme "DIR=readme"; then
  awk '/such as `src\/myapp\.c`/ { want = 1; next }
       want && /^```c$/ { on = 1; next }
       on && /^```$/ { exit }
       on { print }' "$ROOT/README.md" > "$R/src/server.c"
  if grep -q 'cb_main_app' "$R/src/server.c"; then
    ok "the README's src/myapp.c is $(wc -l < "$R/src/server.c" | tr -d ' ') lines, put in $R"
    serve_up "$R" readme
    ok "the README's product builds and serves at $url"
    asks "${url}v1/hello?name=Ada" 200 'hello, Ada\n' "README product: ?name=Ada"
    asks "${url}v1/hello" 200 'hello, world\n' "README product: no name"
    asks "${url}v1/hello?name=" 200 'hello, world\n' "README product: ?name= (empty)"
    asks "${url}v1/hello?name=Ada%20%26%20Grace" 200 'hello, Ada & Grace\n' "README product: an escaped & and space"
    asks "${url}v1/hello?name=$(printf '%064d' 0)" 400 'bad name\n' "README product: a name of 64 bytes"
    asks "${url}v1/hello?name=%zz" 400 'bad name\n' "README product: a bad escape"
    serve_down || fail "the README's product did not stop on SIGTERM"
  else
    fail "no README.md block after src/myapp.c with cb_main_app in it"
  fi
else
  fail "make new DIR=readme failed"; cat "$T/readme.log"
fi

if [ "$fails" -gt 0 ]; then echo "make new: $fails failure(s)"; exit 1; fi
echo "make new: passed"
