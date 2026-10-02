#!/usr/bin/env bash
# The layout hash is a fact about LAYOUT, not about how libclang spells a type.
#
# c/Makefile bakes `structgen --print-hash` into every wasm module
# (wasm_layout_hash) and the generated TS (--hash-ts) carries the same number as
# LAYOUT_HASH; the hosts refuse a module whose hash differs. CI runs structgen
# on Linux's libclang, the Mac on Homebrew's, and the two can render one type
# differently (typedef sugar, "struct X" vs "X", unnamed-record spellings), so
# the hash must be computed from field paths, offsets, sizes, kinds, bit ranges
# and constant values only.
#
#   same hash      every typedef and struct tag renamed, layout untouched
#   different hash each single change of a layout fact
set -uo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
make -s -C "$here" build/structgen
SG="$here/build/structgen"
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
fails=0
ok() { echo "ok   $1"; }
bad() { echo "FAIL $1"; fails=$((fails + 1)); }

# hash_of DIR -> the hash of kinds.h in DIR; also writes DIR/out.ts
hash_of() {
    "$SG" --cwd "$1" --header kinds.h --root Kinds --build wasm= --const K_ --const KFLAG_ \
        --ts "$1/out.ts" --print-hash
}
variant() { # name perl-substitutions -> a copy of kinds.h with the edit applied
    mkdir -p "$tmp/$1"
    perl -pe "$2" "$here/test/kinds.h" > "$tmp/$1/kinds.h"
    cmp -s "$here/test/kinds.h" "$tmp/$1/kinds.h" && { echo "test bug: $1 edit matched nothing" >&2; exit 2; }
}
mkdir -p "$tmp/base" && cp "$here/test/kinds.h" "$tmp/base/kinds.h"
base="$(hash_of "$tmp/base")" || { bad "base run"; exit 1; }

# ---- spellings must not move the hash ----------------------------------------
variant renamed 's/\bKCard\b/KCardRenamed/g; s/\bKNamed\b/KNamedRenamed/g; s/\bKPacked\b/KPackedRenamed/g; s/\bKEnum\b/KEnumRenamed/g; s/\bKPos\b/KPosRenamed/g; s/\bKNode\b/KNodeRenamed/g; s/\bKOpaque\b/KOpaqueRenamed/g; s/\bKFn\b/KFnRenamed/g'
got="$(hash_of "$tmp/renamed")"
if cmp -s "$tmp/base/out.ts" "$tmp/renamed/out.ts"; then
    bad "typedef rename: the generated TS did not change, so the test proves nothing"
elif [ "$got" = "$base" ]; then
    ok "typedef and struct tags renamed, layout identical: same hash ($base)"
else
    bad "typedef and struct tags renamed, layout identical: hash moved $base -> $got"
fi

# ---- every layout fact must move it ----------------------------------------------
n_variant=0
differs() { # description perl-substitutions
    n_variant=$((n_variant + 1))
    variant "v$n_variant" "$2"
    local h; h="$(hash_of "$tmp/v$n_variant")" || { bad "$1: structgen failed"; return; }
    if [ "$h" != "$base" ]; then ok "$1 moves the hash"; else bad "$1 left the hash at $base"; fi
}
differs "a bit range (KCard.s 3 -> 4 bits)"      's/int8_t s : 3; int8_t v : 5;/int8_t s : 4; int8_t v : 4;/'
differs "a kind (KNamed.a int16 -> uint16)"      's/struct KNamed { int16_t a;/struct KNamed { uint16_t a;/'
differs "an array length (text[5] -> text[6])"   's/char text\[5\];/char text[6];/'
differs "an offset (f and d swapped)"            's/^    float f;$/    double d0;/; s/^    double d;$/    float f;/; s/double d0;/double d;/'
differs "a field name (i32 -> i33)"              's/int32_t i32;/int32_t i33;/'
differs "a size (int32 i32 -> int64)"            's/int32_t i32;/int64_t i32;/'
differs "a constant value (KFLAG_LOW 3 -> 4)"    's/#define KFLAG_LOW  3/#define KFLAG_LOW  4/'
differs "a char array becoming bytes"            's/char text\[5\];/int8_t text[5];/'
differs "a pointer becoming a u32 (vals)"        's/int16_t \*vals;/uint32_t vals;/'
differs "a scalar pointee (vals int16 -> int32)" 's/int16_t \*vals;/int32_t *vals;/'
differs "a record pointee (hand KCard -> KNamed)" 's/const KCard \*hand;/const struct KNamed *hand;/'
differs "void * becoming char *"                 's/void \*opaque;/char *opaque;/'
differs "a record only a pointer reaches (KNode.v int32 -> int16)" 's/struct KNode \{ int32_t v;/struct KNode { int16_t v;/'

[ $fails -eq 0 ] && echo "hash: all pass" || { echo "hash: $fails failed"; exit 1; }
