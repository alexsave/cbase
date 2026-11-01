#!/usr/bin/env bash
# fuzz_select_test.sh - .github/scripts/fuzz_select.sh picks the fuzz targets
# a change can reach, and only those:
#
#   * a doc or a workflow selects nothing;
#   * a target's own fz_<t>.c, or a file in its corpus, selects that target;
#   * a library source selects the targets that reach it, and not one that
#     links its own copy instead (random.c is not sequence's: FUZZ_EXCLUDE);
#   * a makefile selects by the commands it changes, against a base tree
#     (--against <dir>) that is HEAD with one makefile edited:
#       - a flag every object compiles with (CB_CFLAGS, the kernel's human
#         capacities in foolish/c's WASM_FLAGS) or the compile recipe
#         (shared/tools/c_rules.mk) selects every target;
#       - FUZZ_EXTRA_<t> or FUZZ_EXCLUDE_<t> (fuzz/fuzz.mk) selects <t> alone;
#       - a comment, a shared test's sources (shared/tools/shared_tests.mk), a
#         wasm-only flag, a generator rule the fuzz build never runs
#         (shared/tools/gen_tools.mk) selects nothing;
#       - a base whose makefile does not parse selects every target.
#
# Needs `make -C c_base fuzz` built (the selector reads its objects) and a
# working tree whose makefiles are HEAD's (the base trees are HEAD's).
#   bash c_base/fuzz/test/fuzz_select_test.sh
#
# Mutation-checked (2026-10-31), each red on the named cases and nothing else:
#   * the old rule back (any file in either build's MAKEFILE_LIST selects
#     every target): "a comment", "shared_tests.mk", "wasm-only", "gen_tools.mk"
#     and the FUZZ_EXTRA/FUZZ_EXCLUDE cases;
#   * compile commands not compared (the "C" records ignored): "c_rules.mk"
#     and "FUZZ_EXCLUDE" (CB_CFLAGS and the capacities stay green: the link
#     line carries FUZZ_CB_CFLAGS too, a second vector on purpose);
#   * the link line's objects not compared: "FUZZ_EXCLUDE" ("FUZZ_EXTRA"
#     stays green: lib_client.o is also a new compile that rt_anon reaches);
#   * a base that does not dry-run taken as no change: "does not parse" (by
#     its reason; every object is then new, so the selection alone is ALL).
# The sed programs below are make text: their $ and $(...) are meant literally.
# shellcheck disable=SC2016
set -euo pipefail

CB="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ROOT="$(cd "$CB/.." && pwd)"
SEL="$ROOT/.github/scripts/fuzz_select.sh"
cd "$ROOT"                   # the selector reads the repository it is run in
S="$(mktemp -d)"
trap 'rm -rf "$S"' EXIT
fails=0
fail() { echo "FAIL: $*"; fails=$((fails + 1)); }

all="$(MAKEFLAGS='' make -s --no-print-directory -C "$CB" print-fuzz-targets | tr -s ' \n' ' ' | sed 's/ $//')"
[ "$(echo "$all" | wc -w)" -ge 20 ] || { echo "FAIL: only [$all] fuzz targets"; exit 1; }
norm() { tr ' ' '\n' | sed '/^$/d' | LC_ALL=C sort | tr '\n' ' ' | sed 's/ $//'; }
all="$(echo "$all" | norm)"

# sel <label> <want> <selector args...>: the selection must be exactly <want>
# ("ALL" for every target).
sel() {
  local label=$1 want=$2 got; shift 2
  [ "$want" != ALL ] || want=$all
  if ! got="$("$SEL" "$@" 2> "$S/err" | norm)"; then
    fail "$label: the selector failed: $(tail -3 "$S/err")"; return
  fi
  [ "$got" = "$(echo "$want" | norm)" ] || fail "$label: selected [$got], want [$want]"
}

# ---- file changes, against HEAD ---------------------------------------------
sel "a doc" "" --files README.md
sel "a workflow" "" --files .github/workflows/c_base.yml
sel "fz_bcrypt.c" bcrypt --files c_base/core/fuzz/fz_bcrypt.c
sel "a file in bcrypt's corpus" bcrypt --files c_base/core/fuzz/corpus/bcrypt/fuzz_select_test
got="$("$SEL" --files c_base/core/src/util/random.c 2>/dev/null | norm)"
n="$(echo "$got" | wc -w | tr -d ' ')"
{ [ "$n" -gt 0 ] && [ "$n" -lt "$(echo "$all" | wc -w)" ]; } || fail "random.c selected [$got]: want some targets and not all"
case " $got " in *" sequence "*) fail "random.c selected sequence, which links its own (FUZZ_EXCLUDE_sequence)";; esac

# ---- makefile changes, against HEAD with one makefile edited ------------------
for p in c_base/Makefile c_base/fuzz/fuzz.mk shared/tools/c_rules.mk shared/tools/shared_tests.mk \
         shared/tools/gen_tools.mk foolish/c/Makefile; do
  git -C "$ROOT" diff --quiet HEAD -- "$p" || { echo "FAIL: $p differs from HEAD; the base trees are HEAD's"; exit 1; }
done
mkdir "$S/tree"
GIT_INDEX_FILE="$S/index" git -C "$ROOT" --work-tree="$S/tree" read-tree -u --reset HEAD

# edit <label> <want> <path> <sed program>: the base is HEAD with <path> edited.
edit() {
  local label=$1 want=$2 p=$3 prog=$4
  cp "$S/tree/$p" "$S/orig"
  sed "$prog" "$S/orig" > "$S/tree/$p"
  if cmp -s "$S/orig" "$S/tree/$p"; then
    fail "$label: the edit did not apply to $p (did the line move?)"
  else
    sel "$label" "$want" --against "$S/tree" --files "$p"
  fi
  cp "$S/orig" "$S/tree/$p"
}
edit "a comment in c_base/Makefile" "" c_base/Makefile '$a\
# fuzz_select_test'
edit "CB_CFLAGS" ALL c_base/Makefile 's/^CB_CFLAGS = -std=gnu11/CB_CFLAGS = -DFUZZ_SELECT_TEST -std=gnu11/'
edit "c_rules.mk's compile recipe" ALL shared/tools/c_rules.mk 's/-MD -MP -c/-MD -MP -DFUZZ_SELECT_TEST -c/'
edit "the kernel capacities (foolish/c WASM_FLAGS)" ALL foolish/c/Makefile 's/-DMAX_MOVE_CARDS=28 -DMAX_BATTLES=64 \\$/-DMAX_MOVE_CARDS=28 -DMAX_BATTLES=65 \\/'
edit "a wasm-only flag (foolish/c WASM_FLAGS)" "" foolish/c/Makefile 's/^WASM_FLAGS := --target=wasm32/WASM_FLAGS := -DFUZZ_SELECT_TEST --target=wasm32/'
edit "FUZZ_EXTRA_rt_anon" rt_anon c_base/fuzz/fuzz.mk '/^FUZZ_EXTRA_rt_anon /d'
edit "FUZZ_EXCLUDE_sequence" sequence c_base/fuzz/fuzz.mk '/^FUZZ_EXCLUDE_sequence /d'
edit "shared_tests.mk (a shared test's sources)" "" shared/tools/shared_tests.mk 's|^\(SHARED_TEST_SRC_hex_test .*\)$|\1 hex/fuzz_select_test.c|'
edit "gen_tools.mk (structgen's rule)" "" shared/tools/gen_tools.mk 's|\$(MAKE) -C \$(STRUCTGEN_DIR) build/structgen|& V=1|'
edit "a base whose c_base/Makefile does not parse" ALL c_base/Makefile '$a\
$(error fuzz_select_test)'
grep -q "does not dry-run" "$S/err" || fail "a base that does not parse: selected for another reason: $(head -2 "$S/err")"

if [ "$fails" -ne 0 ]; then echo "fuzz_select_test: $fails failure(s)"; exit 1; fi
echo "fuzz_select_test: ok"
