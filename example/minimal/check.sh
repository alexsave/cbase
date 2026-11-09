#!/usr/bin/env bash
# check.sh - the minimal example (README.md, this directory's Makefile) as a person runs it: one
# `make serve`, which builds the server and the page (its wasm module linked by cdom.mk's APP_WASM
# rule with cbase's client and cdom adapter) and runs the server, outside dev mode, with no setting
# but a free port. Then, at the URL its startup line prints:
#
#   * GET /api/hello answers 200 "hello from cbase" (the route the page calls);
#   * GET / is cdom's default page with the shim inlined and the linked module bundled into it, as a
#     data: URL whose bytes are build/page/page.wasm; no module is served apart (GET /page.wasm 404);
#   * the page in headless Chrome (e2e.mjs, on cdom's tools/cdp.mjs) shows "hello from cbase", the
#     answer to its call. That needs Node 22 or later and Chrome (CHROME names one), which only this
#     test needs: without them it is skipped and said, unless MINIMAL_E2E=1 (CI) makes it a failure.
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
if [ "$code" = 200 ] && grep -q '<title>cbase minimal</title>' "$T/page.html" && ! grep -q 'CDOM_SHIM\|CDOM_WASM' "$T/page.html"; then
  ok "GET / is the page, with the shim inlined"
else fail "GET / answered $code, or is not the page with the shim inlined"; fi
# The module, bundled: cdom.start's first argument is a data: URL of the very module the build linked.
sed -n 's/.*cdom\.start("data:application\/wasm;base64,\([A-Za-z0-9+\/=]*\)", "app").*/\1/p' "$T/page.html" \
  | base64 -d > "$T/bundled.wasm" 2>/dev/null || true
magic="$(head -c 4 "$T/bundled.wasm" | od -An -tx1 | tr -d ' \n')"
if [ "$magic" = 0061736d ] && cmp -s "$T/bundled.wasm" "$HERE/build/page/page.wasm"; then
  ok "GET / bundles the linked module ($(wc -c < "$T/bundled.wasm" | tr -d ' ') bytes, build/page/page.wasm's)"
else fail "GET / bundles no module, or not build/page/page.wasm (magic '$magic')"; fi
code="$(curl -s --max-time 5 -o /dev/null -w '%{http_code}' "${url}page.wasm" || true)"
if [ "$code" = 404 ]; then ok "GET /page.wasm is 404: the page is the one file"
else fail "GET /page.wasm answered $code, not 404: a module served apart from the page"; fi

# The page in a browser. cdp.mjs is the cdom checkout's this app was built with.
cdp_cdom="${CDOM_DIR:-$HERE/../../client/build/cdom}"
chrome="${CHROME:-}"
if [ -z "$chrome" ]; then
  for c in "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome" "$(command -v google-chrome || true)" \
           "$(command -v chromium || true)"; do
    if [ -n "$c" ] && [ -x "$c" ]; then chrome="$c"; break; fi
  done
fi
if [ -n "$chrome" ] && node -e 'process.exit(typeof WebSocket === "function" ? 0 : 1)' 2>/dev/null; then
  CHROME="$chrome" node "$HERE/e2e.mjs" "$cdp_cdom" "$url" || fails=$((fails + 1))
elif [ "${MINIMAL_E2E:-}" = 1 ]; then
  fail "the page in Chrome: MINIMAL_E2E=1, and no Node 22 or later or no Chrome (CHROME='${CHROME:-}')"
else
  echo "skip  minimal example: the page in Chrome (it needs Node 22 or later and Chrome; CHROME names one)"
fi

if [ "$fails" -gt 0 ]; then show; echo "minimal example: $fails failure(s)"; exit 1; fi
echo "minimal example: passed"
