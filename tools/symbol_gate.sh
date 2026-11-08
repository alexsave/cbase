#!/usr/bin/env bash
# symbol_gate.sh - a binary built without the aws provider carries nothing of it.
#
#   bash tools/symbol_gate.sh <binary> [<object>...]
#
# Every global symbol of <binary> (nm -g, defined or not) is read, and the gate fails, naming
# each hit, when one
#
#   * is DEFINED BY ONE OF THE OBJECTS given: the objects are every object under providers/aws
#     that cbase's own build compiles (the top-level Makefile passes them), and each global
#     symbol one of them defines but `main` is banned, so whatever the provider grows is banned
#     with it, by its real name, without anyone keeping a list; or
#   * hits THE WORD LIST below, the second net: it needs no aws object (a product's local-only
#     build runs it alone, example/Makefile link-check), and it catches a provider symbol of a
#     name the first net cannot know (a static helper made global, a copy pasted into core).
#
# A symbol list too short to judge (a stripped binary, a wrong path) fails too, and so does a
# derived set too small to mean anything (the objects were never built, or the wrong ones).
# cbase's `make check` runs it over the local-only example's build/cbase with every aws
# object; CI runs make check.
set -euo pipefail

# THE WORD LIST: the aws provider's parts as they are spelled in a symbol (its store, push
# transport, signer, runtime and its API, object store, instance metadata, parameter store,
# edge cache, scheduler) and the VM's boot. Matched without case, anywhere in a name.
WORDS='aws|dynamo|iot|sigv4|lambda|imds|cloudfront|vm_boot|runtime_api|eventbridge|ssm|s3_|_s3'

[ $# -ge 1 ] || { echo "usage: symbol_gate.sh <binary> [<object>...]" >&2; exit 2; }
bin="$1"; shift
[ -f "$bin" ] || { echo "symbol gate: no binary $bin" >&2; exit 1; }
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT

# nm -P (POSIX output: name, type, value, size) reads the same on macOS and on GNU binutils.
nm -g -P "$bin" | awk '{print $1}' | LC_ALL=C sort -u > "$tmp/bin"
nbin="$(wc -l < "$tmp/bin" | tr -d ' ')"
[ "$nbin" -gt 100 ] || { echo "symbol gate: $bin shows $nbin global symbols, too few to judge"; exit 1; }

fails=0
if [ $# -gt 0 ]; then
  # name<TAB>object for every strong global an object defines (U is undefined; w, W, v, V weak,
  # which another object may define too).
  for o in "$@"; do
    nm -g -P "$o" | awk -v o="$o" '$2 !~ /^[UwWvV]$/ && $1 != "main" && $1 != "_main" {print $1 "\t" o}'
  done | LC_ALL=C sort -u -k1,1 > "$tmp/banned"
  nban="$(wc -l < "$tmp/banned" | tr -d ' ')"
  [ "$nban" -ge 50 ] || { echo "symbol gate: the $# object(s) given define $nban global symbols, too few to judge"; exit 1; }
  LC_ALL=C join -t "$(printf '\t')" "$tmp/bin" "$tmp/banned" > "$tmp/hits" || true
  if [ -s "$tmp/hits" ]; then
    head -20 "$tmp/hits" | awk -F '\t' '{print "  " $1 "  (defined by " $2 ")"}'
    echo "symbol gate: $bin holds $(wc -l < "$tmp/hits" | tr -d ' ') symbol(s) an aws provider object defines"
    fails=1
  fi
  derived=", $nban symbols of $# aws objects"
fi
if grep -Ei "$WORDS" "$tmp/bin" > "$tmp/words"; then
  head -20 "$tmp/words" | sed 's/^/  /'
  echo "symbol gate: $bin holds $(wc -l < "$tmp/words" | tr -d ' ') symbol(s) on the aws word list ($WORDS)"
  fails=1
fi
[ "$fails" -eq 0 ] || exit 1
echo "symbol gate: $bin holds nothing of the aws provider ($nbin symbols read${derived:-}, and the word list)"
