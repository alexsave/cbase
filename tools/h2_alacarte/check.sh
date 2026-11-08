#!/usr/bin/env bash
# check.sh - the HTTP/2 and gRPC module taken a la carte (make h2-alacarte, part of check).
#
# build/h2_alacarte is linked from the module's objects alone (the Makefile's
# H2_ALACARTE_SRC), so its link already failed if the module reached a provider or the
# product. This holds the result to it by name: no global symbol the binary defines or
# needs is one that a provider's or the product's objects define, and nothing it needs is
# a c_base symbol left unresolved. Then the program runs.
#
# usage: tools/h2_alacarte/check.sh <binary> <provider and product objects>...
set -euo pipefail

BIN="$1"; shift
[ -x "$BIN" ] || { echo "h2-alacarte: no $BIN"; exit 1; }
[ $# -gt 0 ] || { echo "h2-alacarte: no provider or product objects to hold it against"; exit 1; }
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# Every global symbol defined by the objects it must not reach (nm: "<addr> <type> <name>").
nm -g "$@" 2> /dev/null | awk 'NF == 3 && $2 != "U" { print $3 }' | sort -u > "$work/forbidden"
[ -s "$work/forbidden" ] || { echo "h2-alacarte: read no symbols from the provider and product objects"; exit 1; }
# Every global symbol the binary defines or needs.
nm -g "$BIN" | awk '{ print $NF }' | sort -u > "$work/used"
comm -12 "$work/forbidden" "$work/used" > "$work/both"
if [ -s "$work/both" ]; then
  echo "h2-alacarte: the module reaches provider or product code:"
  sed 's/^/  /' "$work/both"
  exit 1
fi
# Nothing of c_base's own may be left to the dynamic linker.
nm -u "$BIN" | awk '{ print $NF }' | grep -E '^_?cb_' > "$work/unresolved" || true
if [ -s "$work/unresolved" ]; then
  echo "h2-alacarte: c_base symbols left unresolved:"
  sed 's/^/  /' "$work/unresolved"
  exit 1
fi
echo "h2-alacarte: $(wc -l < "$work/used" | tr -d ' ') symbols, none of the $(wc -l < "$work/forbidden" | tr -d ' ') that providers and the product define"
"$BIN"
