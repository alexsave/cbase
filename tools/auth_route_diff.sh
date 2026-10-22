#!/bin/bash
# auth_route_diff.sh - the differential proof that a change leaves the session routes (/v1/auth/*)
# answering exactly as before: BASE (default origin/main) is exported with git archive and its
# `cbase` built, the working tree's `cbase` and core/test/auth_diff.c are built, and auth_diff sends
# both servers the same requests and compares every answer (auth_diff.c says what is compared).
#
#   c_base/tools/auth_route_diff.sh [-b BASE]
#
# Writes under c_base/build/auth_diff/ (the base tree and the transcript). Builds with -j4.
set -euo pipefail
CB=$(cd "$(dirname "$0")/.." && pwd)
ROOT=$(cd "$CB/.." && pwd)
BASE=origin/main
if [ "${1:-}" = "-b" ]; then BASE=$2; shift 2; fi
OUT=$CB/build/auth_diff
rm -rf "$OUT"
mkdir -p "$OUT/tree"
echo "base: $BASE ($(git -C "$ROOT" rev-parse --short "$BASE")), new: the working tree ($(git -C "$ROOT" rev-parse --short HEAD) + edits)"
git -C "$ROOT" archive "$BASE" c_base shared foolish | tar -xf - -C "$OUT/tree"
make -s -C "$OUT/tree/c_base" -j4 build/cbase > "$OUT/base_make.log" 2>&1 || { cat "$OUT/base_make.log"; echo "base: build failed"; exit 1; }
make -s -C "$CB" -j4 build/cbase build/test/auth_diff > "$OUT/new_make.log" 2>&1 || { cat "$OUT/new_make.log"; echo "new: build failed"; exit 1; }
set +e
"$CB/build/test/auth_diff" "$OUT/tree/c_base/build/cbase" "$CB/build/cbase" > "$OUT/transcript.txt" 2> "$OUT/servers.log"
rc=$?
set -e
grep -E "^(same|DIFF)" "$OUT/transcript.txt" | awk '{print $1}' | sort | uniq -c
grep -A40 "^DIFF" "$OUT/transcript.txt" || true
tail -1 "$OUT/transcript.txt"
echo "transcript: $OUT/transcript.txt"
exit $rc
