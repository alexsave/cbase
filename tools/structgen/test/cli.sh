#!/usr/bin/env bash
# structgen must FAIL (non-zero, with a message) on every request it cannot honour.
set -uo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
make -s -C "$here" build/structgen
SG="$here/build/structgen"
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
base=(--cwd "$here/test" --header kinds.h --root Kinds --build wasm= --ts "$tmp/out.ts")
fails=0
expect_fail() { # description pattern args...
    local what="$1" pat="$2"; shift 2
    local err; err="$("$SG" "$@" 2>&1 >/dev/null)"; local rc=$?
    if [ $rc -ne 0 ] && printf '%s' "$err" | grep -qF -- "$pat"; then echo "ok   $what"
    else echo "FAIL $what (rc=$rc): $err"; fails=$((fails + 1)); fi
}
expect_fail "missing requested field"      "no field named nope"            "${base[@]}" --fields Kinds=tag,nope
expect_fail "--fields for an unreached record" "no such record"             "${base[@]}" --fields Nope=a
expect_fail "--const prefix matching nothing" "matches no enum constant"    "${base[@]}" --const NOPE_
expect_fail "non-integer #define under --const" "compile error"             --cwd "$here/test" --header kinds.h --root Kinds --build wasm= --ts "$tmp/o.ts" --const KBAD_
expect_fail "unknown argument"             "unknown argument"               "${base[@]}" --frobnicate 1
expect_fail "flag without a value"         "needs a value"                  "${base[@]}" --root
expect_fail "two --build in one run"       "one --build per run"            "${base[@]}" --build other=
expect_fail "root that is not a record"    "not a struct or union"          --cwd "$here/test" --header kinds.h --root KEnum --build wasm= --ts "$tmp/o.ts"
expect_fail "header that does not compile" "compile error"                  --cwd "$here/test" --header missing.h --root Kinds --build wasm= --ts "$tmp/o.ts"
expect_fail "colliding generated names"     "emitted twice"                  --cwd "$here/test" --header collide.h --root Collide --build wasm= --ts "$tmp/o.ts"
snap=(--cwd "$here/test" --header snap.h --root Snap --root SMatrix --root SWithUnion --build wasm= --ts "$tmp/o.ts")
expect_fail "--snapshot of an unreached record" "no such record"            "${snap[@]}" --snapshot Nope
expect_fail "--snapshot of a 2-D array"     "2 array dimensions"             "${snap[@]}" --snapshot SMatrix
expect_fail "--snapshot of a union"         "is a union"                     "${snap[@]}" --snapshot SWithUnion
expect_fail "--snapshot-only with none"     "without a --snapshot"           "${snap[@]}" --snapshot-only
expect_fail "--count without its shape"     "TYPE.field=count_field"         "${snap[@]}" --snapshot Snap --count Snap.pairs
expect_fail "--count of a non-array"        "not a one-dimensional array"    "${snap[@]}" --snapshot Snap --count Snap.w=n_pairs
expect_fail "--count by a non-integer"      "is not an integer field"        "${snap[@]}" --snapshot Snap --count Snap.pairs=d
expect_fail "--count on no snapshot"        "is not in any snapshot"         "${snap[@]}" --snapshot SPair --count SItem.text=len
expect_fail "--writer of no snapshot"       "not a --snapshot"               "${snap[@]}" --snapshot SPair --writer Snap
ptr=(--cwd "$here/test" --header snap.h --root SPtr --build wasm= --ts "$tmp/o.ts")
ptr_counts=(--count SPtr.vals=n_vals --count SPtr.items=n_items --count SPtr.name=name_len --count SPtr.none=n_none)
expect_fail "--snapshot of an uncounted pointer" "SPtr.vals is a pointer; give --count SPtr.vals=" "${ptr[@]}" --snapshot SPtr
expect_fail "--writer reaching a pointer"   "--writer: SPtr.vals is a pointer" "${ptr[@]}" --snapshot SPtr "${ptr_counts[@]}" --writer SPtr
expect_fail "--count naming no field"       "SPtr has no field named nope"   "${ptr[@]}" --snapshot SPtr --count SPtr.vals=nope
expect_fail "--swift with no snapshot"      "--swift without a --snapshot"   "${base[@]}" --swift "$tmp/o.swift"
expect_fail "two arrays, one count, in a writer" "share the count n_pairs"    "${snap[@]}" --snapshot Snap --writer Snap --count Snap.pairs=n_pairs --count Snap.items=n_pairs
expect_fail "nothing to write at all"       "nothing to do"                  --cwd "$here/test" --header kinds.h --root Kinds --build wasm=
"$SG" "${base[@]}" --print-hash > "$tmp/hash" && grep -qE '^0x[0-9a-f]{8}$' "$tmp/hash" && echo "ok   --print-hash prints the hash" || { echo "FAIL --print-hash"; fails=$((fails + 1)); }
"$SG" "${base[@]}" --hash-ts "$tmp/hash.ts" --print-hash > "$tmp/hash2" \
  && [ "$(grep -c . "$tmp/hash.ts")" = 3 ] && grep -qx "export const LAYOUT_HASH = $(cat "$tmp/hash2");" "$tmp/hash.ts" \
  && ! grep -q LAYOUT_HASH "$tmp/out.ts" \
  && echo "ok   --hash-ts writes the hash alone, and --ts leaves it out" || { echo "FAIL --hash-ts"; fails=$((fails + 1)); }
[ $fails -eq 0 ] && echo "cli: all pass" || { echo "cli: $fails failed"; exit 1; }
