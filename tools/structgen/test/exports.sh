#!/usr/bin/env bash
# An exports run (structgen --link): the interface it writes from a link line
# and the C definitions, every request it must refuse, and --check-wasm against
# modules this script links itself.
#
# The last part needs a clang that links wasm32 (WASM_CC, found the way
# tools/wasm_cc.mk finds it). It is not skipped without one: an interface that
# was never held against a real module is the hand-typed one over again.
set -uo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
make -s -C "$here" build/structgen
SG="$here/build/structgen"
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
fails=0
ok() { echo "ok   $1"; }
bad() { echo "FAIL $1"; fails=$((fails + 1)); }

LIST="-Wl,--export=ex_add -Wl,--export=ex_buf_ptr -Wl,--export=ex_scale -Wl,--export=ex_wide \
  -Wl,--export=ex_reset -Wl,--export=ex_flag -Wl,--export=ex_words -Wl,--export=ex_pasted -Wl,--export=ex_unnamed"
FULL="-Wl,--no-entry -Wl,--export-memory $LIST"
SMALL="-Wl,--no-entry,--export=ex_add -Wl,--export=ex_reset"
run=(--cwd "$here/test" --build wasm= --source exports.c --source exports_more.c --source exports_pasted.c)

# ---- the interface, to the byte ----------------------------------------------
# One run, two links of the same sources: each gets the exports ITS line names.
cat > "$tmp/want.full.ts" <<'TS'
export interface FixtureExports {
    readonly memory: WebAssembly.Memory;
    ex_add(leftSide: number, rightSide: number): number;   // (i32, i32) -> i32, exports.c
    ex_buf_ptr(): number;   // () -> i32, exports.c
    ex_flag(on: number, kind: number, name: number, bytes: number): number;   // (i32, i32, i32, i32) -> i32, exports.c
    ex_pasted(seat: number): number;   // (i32) -> i32, exports_pasted.c
    ex_renamed(a: number): number;   // (i32) -> i32, exports.c
    ex_reset(): void;   // () -> void, exports.c
    ex_scale(value: number, by: number): number;   // (f64, f32) -> f64, exports.c
    ex_unnamed(arg0: number, second: number): number;   // (i32, i32) -> i32, exports_more.c
    ex_wide(big: bigint, bigger: bigint, small: number): bigint;   // (i64, i64, i32) -> i64, exports.c
    ex_words(in_: number, new_: number): number;   // (i32, i32) -> i32, exports.c
}
TS
cat > "$tmp/want.small.ts" <<'TS'
export interface SmallExports {
    readonly memory: WebAssembly.Memory;
    ex_add(leftSide: number, rightSide: number): number;   // (i32, i32) -> i32, exports.c
    ex_renamed(a: number): number;   // (i32) -> i32, exports.c
    ex_reset(): void;   // () -> void, exports.c
}
TS
if "$SG" "${run[@]}" --link "$FULL" --exports-name FixtureExports --exports-ts "$tmp/full.ts" \
                     --link "$SMALL" --exports-name SmallExports --exports-ts "$tmp/small.ts"; then
    for which in full small; do
        # The interface itself; the banner above it is prose.
        sed -n '/^export interface/,$p' "$tmp/$which.ts" > "$tmp/got.$which.ts"
        if diff "$tmp/want.$which.ts" "$tmp/got.$which.ts"; then ok "the $which link's interface, to the byte"
        else bad "the $which link's interface differs (above: < wanted, > written)"; fi
    done
    head -2 "$tmp/full.ts" | grep -qx '// build: wasm; link: FixtureExports (10 functions)' \
        && ok "the banner names the build, the link and its function count" || bad "banner: $(head -2 "$tmp/full.ts")"
else
    bad "the fixture run failed"
fi

# ---- which sources are read ------------------------------------------------------
# A source that spells no wanted name is read only while a wanted name is still
# undefined. So one that does not even compile costs a run nothing when every
# export is found before it - and is read, and refused, the moment one is not.
printf '#error "this source was read"\n' > "$tmp/unread.c"
lazy=(--cwd "$here/test" --build wasm= --source exports.c --source "$tmp/unread.c")
lazy_out=(--exports-name X --exports-ts "$tmp/x.ts")
if "$SG" "${lazy[@]}" --link "$SMALL" "${lazy_out[@]}" 2>/dev/null; then ok "a source that names no export is not read while every export is defined"
else bad "a source that names no export was parsed although nothing was missing"; fi
if err="$("$SG" "${lazy[@]}" --link "$SMALL -Wl,--export=ex_pasted" "${lazy_out[@]}" 2>&1)"; then bad "an undefined export did not send the run to the remaining sources"
elif printf '%s' "$err" | grep -qF 'this source was read'; then ok "...and is read once an export is still undefined"
else bad "an undefined export was refused without reading the remaining sources: $err"; fi

# ---- requests it must refuse ----------------------------------------------------
expect_fail() { # description pattern args...
    local what="$1" pat="$2"; shift 2
    local err; err="$("$SG" "$@" 2>&1 >/dev/null)"; local rc=$?
    if [ $rc -ne 0 ] && printf '%s' "$err" | grep -qF -- "$pat"; then ok "$what"
    else bad "$what (rc=$rc): $err"; fi
}
out=(--exports-name X --exports-ts "$tmp/x.ts")
expect_fail "an export nothing defines"         "exports ex_nowhere, and no --source defines" "${run[@]}" --link "-Wl,--export=ex_nowhere" "${out[@]}"
expect_fail "an export that is only a prototype" "exports ex_add, and no --source defines"    --cwd "$here/test" --build wasm= --source exports_more.c --link "-Wl,--export=ex_add" "${out[@]}"
expect_fail "an export that is static"          "exports ex_helper, and no --source defines"  "${run[@]}" --link "-Wl,--export=ex_helper" "${out[@]}"
expect_fail "--export-all"                      "which no list can be read out of"            "${run[@]}" --link "-Wl,--export-all" "${out[@]}"
expect_fail "flags that export nothing"         "the flags export nothing"                    "${run[@]}" --link "-Wl,--no-entry" "${out[@]}"
expect_fail "--link without --source"           "--link without a --source"                   --cwd "$here/test" --build wasm= --link "$SMALL" "${out[@]}"
expect_fail "--source without --link"           "--source without a --link"                   "${run[@]}"
expect_fail "--exports-ts before any --link"    "before any --link"                           "${run[@]}" --exports-ts "$tmp/x.ts" --link "$SMALL"
expect_fail "--link without --exports-name"     "--link needs --exports-name"                 "${run[@]}" --link "$SMALL" --exports-ts "$tmp/x.ts"
expect_fail "--link with nothing to write"      "nothing to do for X"                         "${run[@]}" --link "$SMALL" --exports-name X
expect_fail "two links of one name"             "two --link groups are named X"               "${run[@]}" --link "$SMALL" "${out[@]}" --link "$FULL" "${out[@]}"
expect_fail "--link mixed with a layout run"    "--link is a run of its own"                  "${run[@]}" --header kinds.h --root Kinds --link "$SMALL" "${out[@]}"
expect_fail "a target with no module"           "exports are a wasm32 module's"               "${run[@]}" --target arm64-apple-ios17.0 --link "$SMALL" "${out[@]}"
shape() { # name, C source -> a one-file run exporting ex_shape
    printf '%s\n' "$2" > "$tmp/$1.c"
    shaped=(--cwd "$tmp" --build wasm= --source "$1.c" --link "-Wl,--export=ex_shape" "${out[@]}")
}
shape byvalue 'typedef struct { int a, b; } Pair; int ex_shape(Pair p) { return p.a + p.b; }'
expect_fail "a struct parameter by value"       "parameter 1 p does not cross the module boundary" "${shaped[@]}"
shape retstruct 'typedef struct { int a, b; } Pair; Pair ex_shape(void) { Pair p = { 1, 2 }; return p; }'
expect_fail "a struct result by value"          "returns a type that does not cross"          "${shaped[@]}"
shape longdouble 'int ex_shape(long double x) { return (int)x; }'
expect_fail "a long double parameter"           "parameter 1 x does not cross the module boundary" "${shaped[@]}"
shape variadic 'int ex_shape(int n, ...) { return n; }'
expect_fail "a variadic export"                 "is variadic"                                 "${shaped[@]}"
shape twonames 'int ex_shape(int a_b, int aB) { return a_b + aB; }'
expect_fail "two parameters of one camelCase name" "are both aB once named in camelCase"      "${shaped[@]}"

# ---- against a module the linker wrote ----------------------------------------------
if [ -z "${WASM_CC:-}" ]; then
    if [ -x /opt/homebrew/opt/llvm/bin/clang ]; then WASM_CC=/opt/homebrew/opt/llvm/bin/clang; else WASM_CC=clang; fi
fi
link() { # out.wasm, extra compile flags..., then the linker flags as ONE argument
    local out="$1"; shift
    local lflags="${*: -1}"; set -- "${@:1:$(($# - 1))}"
    # shellcheck disable=SC2086
    "$WASM_CC" --target=wasm32 -nostdlib -ffreestanding -O1 -Wno-everything "$@" $lflags \
        "$here/test/exports.c" "$here/test/exports_more.c" "$here/test/exports_pasted.c" -o "$out"
}
if ! link "$tmp/full.wasm" "$FULL" || ! link "$tmp/small.wasm" "$SMALL"; then
    bad "WASM_CC=$WASM_CC cannot link the fixture for wasm32 (this part is not optional)"
else
    if "$SG" "${run[@]}" --link "$FULL" --exports-name FixtureExports --check-wasm "$tmp/full.wasm" \
                         --link "$SMALL" --exports-name SmallExports --check-wasm "$tmp/small.wasm"; then
        ok "--check-wasm: each interface is its own module, name for name and type for type"
    else bad "--check-wasm refused modules linked from the very flags it was given"; fi

    expect_check() { # description pattern module link-flags
        local err; err="$("$SG" "${run[@]}" --link "$4" --exports-name FixtureExports --check-wasm "$3" 2>&1 >/dev/null)"; local rc=$?
        if [ $rc -ne 0 ] && printf '%s' "$err" | grep -qF -- "$2"; then ok "$1"
        else bad "$1 (rc=$rc): $err"; fi
    }
    # The module exports more than the line the run was given, and less.
    expect_check "--check-wasm: an export the interface does not have" \
        "ex_scale: exported by the module (a function), not in FixtureExports" "$tmp/full.wasm" "${FULL/-Wl,--export=ex_scale/}"
    expect_check "--check-wasm: a member the module does not export" \
        "ex_scale: in FixtureExports, not exported by the module" "$tmp/small.wasm" "$SMALL -Wl,--export=ex_scale"
    # The linker exports the memory unless the module imports it, so a line
    # that does not mention it has one and a line that imports it has none.
    link "$tmp/imports.wasm" "$SMALL -Wl,--import-memory" || bad "the memory-importing fixture did not link"
    expect_check "--check-wasm: a memory the module does not export" \
        "memory: in FixtureExports, not exported by the module" "$tmp/imports.wasm" "$SMALL"
    expect_check "--check-wasm: a memory the interface does not have" \
        "memory: exported by the module (a memory), not in FixtureExports" "$tmp/small.wasm" "$SMALL -Wl,--import-memory"
    # ...and the one a name check cannot see: the same exports, built from a
    # source whose signature has moved on.
    link "$tmp/drift.wasm" -DEX_DRIFT "$FULL" || bad "the drifted fixture did not link"
    expect_check "--check-wasm: a signature that is not its C definition's" \
        "ex_add: (i32, i32) -> i32 from its C definition in exports.c, (f64, i32) -> i32 in the module" "$tmp/drift.wasm" "$FULL"
    expect_check "--check-wasm: every difference is counted before the refusal" \
        "disagree about 1 export (above)" "$tmp/drift.wasm" "$FULL"
    # One name, two kinds: a module whose `memory` is a function.
    printf 'int memory(void) { return 0; }\n' > "$tmp/kind.c"
    "$WASM_CC" --target=wasm32 -nostdlib -ffreestanding -Wno-everything -Wl,--no-entry -Wl,--import-memory -Wl,--export=memory "$tmp/kind.c" -o "$tmp/kind.wasm" \
        || bad "the kind fixture did not link"
    # (--export-if-defined of a name nothing defines is no member at all.)
    err="$("$SG" --cwd "$tmp" --build wasm= --source kind.c --link "-Wl,--export-if-defined=ex_absent -Wl,--export=__stack_pointer" --exports-name X --check-wasm "$tmp/kind.wasm" 2>&1)"; rc=$?
    if [ $rc -ne 0 ] && printf '%s' "$err" | grep -qF 'memory: a memory in X, a function in the module'; then ok "--check-wasm: an export of another kind than the interface gives it"
    else bad "--check-wasm: an export of another kind (rc=$rc): $err"; fi
    printf 'not a module' > "$tmp/junk.wasm"
    expect_check "--check-wasm: a file that is not a module" "is not a wasm module" "$tmp/junk.wasm" "$FULL"
    head -c 40 "$tmp/full.wasm" > "$tmp/cut.wasm"
    expect_check "--check-wasm: a module cut short" "is not a whole wasm module" "$tmp/cut.wasm" "$FULL"
    expect_check "--check-wasm: a module that is not there" "cannot read" "$tmp/absent.wasm" "$FULL"
fi

[ $fails -eq 0 ] && echo "exports: all pass" || { echo "exports: $fails failed"; exit 1; }
