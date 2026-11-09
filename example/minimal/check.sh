#!/usr/bin/env bash
# check.sh - the minimal example (README.md, this directory's Makefile) as a person runs it: one
# `make serve`, which builds the server and the page (its wasm module linked by cdom.mk's APP_WASM
# rule with cbase's client and cdom adapter) and runs the server, outside dev mode, with no setting
# but a free port. Then, at the URL its startup line prints:
#
#   * GET /api/hello answers 200 "hello from cbase" (the route the page calls);
#   * GET / is the page with cdom's shim inlined, and GET /page.wasm the linked module.
#
#   bash example/minimal/check.sh [<make>] [<cdom checkout>]
#
# client/hosts/cdom/host.mk's check-minimal-example runs it with cdom at the pin (part of
# check-host-cdom, so of `make -C client check-hosts` and CI's client hosts job). The make runs
# with a clean environment (env -i, with WASM_CC passed on when set), so no CBASE_* of the caller's
# reaches serve: a storeless app needs none.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
MAKE_BIN="${1:-make}"
CDOM_DIR="${2:-}"
T="$(mktemp -d)"
MPID=""
cleanup() {
  if [ -n "$MPID" ]; then
    pkill -TERM -P "$MPID" 2>/dev/null || true
    kill -TERM "$MPID" 2>/dev/null || true
    wait "$MPID" 2>/dev/null || true
  fi
  rm -rf "$T"
}
trap cleanup EXIT

fails=0
fail() { echo "FAIL: minimal example: $*"; fails=$((fails + 1)); }
ok() { echo "ok    minimal example: $*"; }
show() { echo "---- make serve stdout"; cat "$T/out"; echo "---- make serve stderr"; tail -40 "$T/err"; }

args=(serve)
[ -z "$CDOM_DIR" ] || args+=("CDOM=$CDOM_DIR")
env -i PATH="$PATH" HOME="$HOME" TMPDIR="${TMPDIR:-/tmp}" ${WASM_CC:+WASM_CC="$WASM_CC"} CBASE_LISTEN=127.0.0.1:0 \
  "$MAKE_BIN" --no-print-directory -C "$HERE" "${args[@]}" > "$T/out" 2> "$T/err" &
MPID=$!

# The startup line, "cbase: listening <host>:<port> <url>": its fourth word is the URL. The build
# comes first, so the wait is long.
line="" n=0
until line="$(grep -m1 '^cbase: listening ' "$T/out" 2>/dev/null)" || [ "$n" -gt 1500 ]; do
  kill -0 "$MPID" 2>/dev/null || break
  sleep 0.2; n=$((n + 1))
done
if [ -z "$line" ]; then
  fail "make serve did not come up listening (outside dev mode, with no CBASE_* setting)"; show
  exit 1
fi
read -r _ _ _ url _ <<< "$line"
case "$url" in
  http://127.0.0.1:*/) ok "make serve built the server and the page and listens at $url" ;;
  *) fail "the startup line names no URL: $line"; show; exit 1 ;;
esac

got="$(curl -s --max-time 5 -w ' %{http_code}' "${url}api/hello" || true)"
if [ "$got" = "hello from cbase
 200" ]; then ok "GET /api/hello answers 200 'hello from cbase'"
else fail "GET /api/hello answered '$got', not 200 'hello from cbase'"; fi

code="$(curl -s --max-time 5 -o "$T/page.html" -w '%{http_code}' "$url" || true)"
if [ "$code" = 200 ] && grep -q 'cdom.start("page.wasm", "app")' "$T/page.html" && ! grep -q 'CDOM_SHIM' "$T/page.html"; then
  ok "GET / is the page, with the shim inlined"
else fail "GET / answered $code, or is not the page with the shim inlined"; fi

code="$(curl -s --max-time 5 -o "$T/page.wasm" -w '%{http_code}' "${url}page.wasm" || true)"
magic="$(head -c 4 "$T/page.wasm" 2>/dev/null | od -An -tx1 | tr -d ' \n')"
if [ "$code" = 200 ] && [ "$magic" = 0061736d ]; then ok "GET /page.wasm is the linked module ($(wc -c < "$T/page.wasm" | tr -d ' ') bytes)"
else fail "GET /page.wasm answered $code with magic '$magic', not a wasm module"; fi

if [ "$fails" -gt 0 ]; then show; echo "minimal example: $fails failure(s)"; exit 1; fi
echo "minimal example: passed"
