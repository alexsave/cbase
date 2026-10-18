#!/usr/bin/env bash
# fuzz_mk_test.sh - fuzz/fuzz.mk's bookkeeping, with no fuzzer and no build:
#
#   * print-fuzz-targets lists exactly the fz_*.c files of every side's fuzz/
#     (core/fuzz, providers/*/fuzz, ../foolish/server/fuzz), found here by a
#     directory walk and not by fuzz.mk's own FUZZ_ROOTS, so a side whose fuzz/
#     fuzz.mk does not list is a failure;
#   * two roots holding one target name are refused (the Makefile will not parse);
#   * fuzz-seeds writes each target's seeds into the corpus beside its own
#     file, and refuses seeds for a name that is no target before it writes
#     any corpus.
#
# The last two run fuzz.mk's own rules over scratch roots (FUZZ_ROOTS) with a
# stand-in seed program (FUZZ_SEEDS_PROG, FUZZ_SEEDS_OUT). Run from anywhere:
#   bash c_base/fuzz/test/fuzz_mk_test.sh
set -euo pipefail

CB="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
S="$(mktemp -d)"
trap 'rm -rf "$S"' EXIT
fails=0
fail() { echo "FAIL: $*"; fails=$((fails + 1)); }
mk() { MAKEFLAGS='' make -s --no-print-directory -C "$CB" "$@"; }

# ---- print-fuzz-targets is every side's fz_*.c ----------------------------------
want="$(cd "$CB" && find core/fuzz providers/*/fuzz ../foolish/server/fuzz -maxdepth 1 -name 'fz_*.c' 2>/dev/null \
        | sed 's|.*/fz_||; s|\.c$||' | LC_ALL=C sort | tr '\n' ' ' | sed 's/ $//')"
got="$(mk print-fuzz-targets | tr -s ' \n' ' ' | sed 's/ $//' | tr ' ' '\n' | LC_ALL=C sort | tr '\n' ' ' | sed 's/ $//')"
[ "$(echo "$want" | wc -w)" -ge 20 ] || fail "found only $(echo "$want" | wc -w) fz_*.c files - did the sides move?"
[ "$got" = "$want" ] || fail "print-fuzz-targets lists [$got], the sides hold [$want]"

# ---- two roots, one name: refused ---------------------------------------------
mkdir -p "$S/a" "$S/b"
touch "$S/a/fz_one.c" "$S/b/fz_two.c"
got="$(mk print-fuzz-targets FUZZ_ROOTS="$S/a $S/b" 2>&1)" || fail "two roots with two names refused: $got"
[ "$got" = "one two" ] || fail "two scratch roots list [$got], not [one two]"
touch "$S/b/fz_one.c"
if got="$(mk print-fuzz-targets FUZZ_ROOTS="$S/a $S/b" 2>&1)"; then
  fail "two roots holding fz_one.c were taken: [$got]"
else
  case "$got" in *"two fuzz roots hold a target of the same name"*) ;; *) fail "two roots with one name refused, but not for that: $got";; esac
fi
rm "$S/b/fz_one.c"

# ---- fuzz-seeds: each corpus beside its target; a stray name refused ----------
cat > "$S/seeds" <<'SEEDS'
#!/bin/sh
# The stand-in for build/asan/fuzz/seeds: <dir>/<target>/<seed>, as seeds.c writes them.
mkdir -p "$1/one" "$1/two" && echo 1 > "$1/one/s1" && echo 2 > "$1/two/s2"
[ -z "${STRAY:-}" ] || { mkdir -p "$1/$STRAY" && echo x > "$1/$STRAY/s"; }
SEEDS
chmod +x "$S/seeds"
seeds() { mk fuzz-seeds FUZZ_ROOTS="$S/a $S/b" FUZZ_SEEDS_PROG="$S/seeds" FUZZ_SEEDS_OUT="$S/out" 2>&1; }

out="$(seeds)" || fail "fuzz-seeds over two scratch roots failed: $out"
[ -f "$S/a/corpus/one/s1" ] || fail "one's seed is not in a/corpus/one, beside fz_one.c"
[ -f "$S/b/corpus/two/s2" ] || fail "two's seed is not in b/corpus/two, beside fz_two.c"
[ ! -e "$S/a/corpus/two" ] && [ ! -e "$S/b/corpus/one" ] || fail "a target's seeds landed beside the other root's target"

rm -rf "$S/a/corpus" "$S/b/corpus"
if out="$(STRAY=three seeds)"; then
  fail "seeds for three, which is no target, were taken"
else
  case "$out" in *"seeds for three, which is no target"*) ;; *) fail "stray seeds refused, but not for that: $out";; esac
fi
[ ! -e "$S/a/corpus" ] && [ ! -e "$S/b/corpus" ] || fail "seeds for no target were refused only after corpora were written"
# A stray name that a target's name contains is still no target.
if out="$(STRAY=on seeds)"; then fail "seeds for on (a part of one) were taken"; fi

if [ "$fails" -ne 0 ]; then echo "fuzz_mk_test: $fails failed"; exit 1; fi
echo "ok   fuzz_mk_test: print-fuzz-targets, one name per root, fuzz-seeds beside each target"
