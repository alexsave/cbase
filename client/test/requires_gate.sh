#!/usr/bin/env bash
# requires_gate.sh - the client core needs of its program what cbase/client.h says, and no more.
#
#   client/test/requires_gate.sh --cc '<compiler and flags>' --nm <nm> --link native|wasm \
#                                --requires '<names>' --header <client.h> <object>...
#
# cbase/client.h's REQUIRES section (and client/client.mk's CBC_REQUIRES, which the Makefile passes
# as --requires) is the whole of what the core's objects may take from the program they are linked
# into: memcpy, memmove, memset, memcmp and strlen. Three checks, each failing with the names:
#
#   * the header's <string.h> line names exactly --requires (the prose and the list stay one);
#   * every symbol the objects leave undefined among themselves (nm) is on the list;
#   * the objects LINK against a stub that defines the listed names and nothing else, with no C
#     library at all (-nostdlib): natively a static executable whose entry is the stub's (ld64 and
#     GNU ld both refuse an undefined symbol there, and macOS links no dynamic output without
#     libSystem), for wasm32 a module (wasm-ld refuses one too). Whatever the first check's nm
#     could miss, the linker cannot.
#
# The stub's functions are named, not written (void name(void) {}): the gate needs their symbols, not
# their behaviour, and nothing runs. client/Makefile runs it over the native objects under test
# (requires-gate) and over the wasm32 objects under wasm (wasm-requires-gate).
set -euo pipefail

CC='' NM=nm LINK='' REQ='' HDR=''
while [ $# -gt 0 ]; do
  case "$1" in
    --cc) CC="$2"; shift 2 ;;
    --nm) NM="$2"; shift 2 ;;
    --link) LINK="$2"; shift 2 ;;
    --requires) REQ="$2"; shift 2 ;;
    --header) HDR="$2"; shift 2 ;;
    --) shift; break ;;
    -*) echo "requires_gate: unknown option $1" >&2; exit 2 ;;
    *) break ;;
  esac
done
[ -n "$CC" ] && [ -n "$LINK" ] && [ -n "$REQ" ] && [ -n "$HDR" ] || { echo "requires_gate: --cc, --link, --requires and --header are required" >&2; exit 2; }
[ $# -gt 0 ] || { echo "requires_gate: no objects given" >&2; exit 2; }
T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT
fail() { echo "requires gate: $*" >&2; exit 1; }
words() { tr ' \t' '\n' | sed '/^$/d' | LC_ALL=C sort -u; }

# ---- the header says the list ------------------------------------------------------------------
printf '%s\n' "$REQ" | words > "$T/req"
sed -n 's|^//   <string.h>  *||p' "$HDR" | words > "$T/hdr"
[ -s "$T/hdr" ] || fail "$HDR has no REQUIRES line '//   <string.h>  <names>'"
diff "$T/req" "$T/hdr" > /dev/null || fail "$HDR's <string.h> line names [$(tr '\n' ' ' < "$T/hdr")], CBC_REQUIRES [$(tr '\n' ' ' < "$T/req")]"

# ---- the stub: the listed names, defined; how this nm spells a C name ----------------------------
{ while read -r f; do printf 'void %s(void) {}\n' "$f"; done < "$T/req"; printf 'void cbc_requires_entry(void) {}\n'; } > "$T/stub.c"
# shellcheck disable=SC2086  # CC is a command line
$CC -w -fno-builtin -c "$T/stub.c" -o "$T/stub.o"
first="$(head -1 "$T/req")"
if "$NM" -P "$T/stub.o" | awk '{print $1}' | grep -qx "_$first"; then pre=_
elif "$NM" -P "$T/stub.o" | awk '{print $1}' | grep -qx "$first"; then pre=
else fail "the stub defines $first and $NM -P does not show it: the gate cannot see"; fi

# ---- what the objects leave undefined ------------------------------------------------------------
: > "$T/def"; : > "$T/undef"
for o in "$@"; do
  "$NM" -P "$o" | awk -v o="$o" -v d="$T/def" -v u="$T/undef" \
    '$2 == "U" { print $1 "\t" o >> u; next } $2 ~ /^[TDBRSCVWI]$/ { print $1 >> d }'
done
LC_ALL=C sort -u -o "$T/def" "$T/def"
LC_ALL=C sort -u -k1,1 -o "$T/undef" "$T/undef"
LC_ALL=C join -t "$(printf '\t')" -v 1 "$T/undef" "$T/def" \
  | awk -F '\t' -v pre="$pre" '{ s = $1; if (pre != "" && index(s, pre) == 1) s = substr(s, 2); print s "\t" $2 }' \
  | LC_ALL=C sort -u -k1,1 > "$T/need"
# wasm-ld defines a module's own globals (the stack pointer, the function table, the memory and table
# bases, the heap's start) in every link: an object leaves them undefined for it, not for the program.
if [ "$LINK" = wasm ]; then
  printf '%s\n' __stack_pointer __indirect_function_table __memory_base __table_base __heap_base __data_end __dso_handle
fi | cat "$T/req" - | LC_ALL=C sort -u > "$T/allowed"
LC_ALL=C join -t "$(printf '\t')" -v 1 "$T/need" "$T/allowed" > "$T/extra" || true
if [ -s "$T/extra" ]; then
  awk -F '\t' '{ print "  " $1 "  (needed by " $2 ")" }' "$T/extra" >&2
  fail "the core's objects need $(wc -l < "$T/extra" | tr -d ' ') symbol(s) cbase/client.h does not list (above)"
fi

# ---- and they link against the stub alone ------------------------------------------------------
case "$LINK" in
  native)
    lflags=(-static -nostdlib "-Wl,-e,${pre}cbc_requires_entry"); out="$T/core.bin" ;;
  wasm) lflags=(-nostdlib "-Wl,--no-entry" "-Wl,--export-all"); out="$T/core.wasm" ;;
  *) fail "--link is native or wasm, not $LINK" ;;
esac
# shellcheck disable=SC2086  # CC is a command line
if ! $CC "${lflags[@]}" "$@" "$T/stub.o" -o "$out" 2> "$T/link.err"; then
  cat "$T/link.err" >&2
  fail "the core's objects do not link against [$(tr '\n' ' ' < "$T/req")] alone (above)"
fi
echo "requires gate: $# objects ($LINK) need [$(tr '\n' ' ' < "$T/req" | sed 's/ $//')] and nothing else, and link against them alone"
