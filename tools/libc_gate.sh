#!/usr/bin/env bash
# libc_gate.sh - no cbase object defines a C library symbol.
#
# A C library belongs to the final program, never to a library: cbase's sources include the
# standard headers and require the symbols, and the program that links them brings the libc (the
# host's, or a freestanding module's own, like client/wasm/libc.c). A cbase object that DEFINES
# memcpy or malloc would fight the program's for the name, and on wasm it would quietly become the
# program's C library. So every object handed to this gate is read with nm, and the gate fails,
# naming object and symbol, when one defines (T t D d S s B b) any name below. Undefined
# references (U) are what a library is meant to have.
#
#   tools/libc_gate.sh --nm <nm> --cc '<compiler and flags>' <object>...
#
# --cc builds the planted object first: a file that defines memcmp, compiled the way the objects
# were, so a gate that stopped seeing (an nm whose output moved, a symbol prefix it does not strip)
# fails instead of passing everything. cbase.mk runs it over the platform's objects and lib/'s,
# client/Makefile over the client's, natively and for wasm32 (client/wasm, the module's own C
# library, is not handed to it).
set -euo pipefail

NM="nm"
CC=
while [ $# -gt 0 ]; do
  case "$1" in
    --nm) NM="$2"; shift 2 ;;
    --cc) CC="$2"; shift 2 ;;
    --) shift; break ;;
    -*) echo "libc_gate: unknown option $1" >&2; exit 2 ;;
    *) break ;;
  esac
done
[ -n "$CC" ] || { echo "libc_gate: --cc is required (the planted object is built with it)" >&2; exit 2; }
[ $# -gt 0 ] || { echo "libc_gate: no objects given" >&2; exit 2; }

# The C library's names: string.h, stdlib.h's allocator and conversions, stdio.h's formatters,
# and math.h (each double function with its float and long double forms).
MATH="sin cos tan asin acos atan atan2 sinh cosh tanh asinh acosh atanh exp exp2 expm1 log log2 log10
      log1p pow sqrt cbrt hypot floor ceil round lround llround trunc rint lrint nearbyint fmod
      remainder fabs fmin fmax fma ldexp frexp modf sincos"
NAMES="memcpy memmove memset memcmp memchr strlen strnlen strcmp strncmp strcpy strncpy strcat strncat
       strchr strrchr strstr strdup strndup malloc free calloc realloc aligned_alloc posix_memalign
       atoi atol atoll strtol strtoul strtoll strtoull strtod strtof abort exit
       printf fprintf sprintf snprintf vprintf vfprintf vsprintf vsnprintf puts putchar"
for m in $MATH; do NAMES="$NAMES $m ${m}f ${m}l"; done
# One line of names separated by single spaces: awk -v takes no newline.
# shellcheck disable=SC2086  # the split is the point
NAMES="$(printf "%s " $NAMES)"

# scan <object>...: prints "<object>: defines <name>" for each definition of a banned name. nm
# prints "<value> <type> <name>" for a defined symbol; Mach-O spells C names with a leading "_".
scan() {
  for o in "$@"; do
    "$NM" "$o" 2>/dev/null | awk -v obj="$o" -v names="$NAMES" '
      BEGIN { n = split(names, a, /[ \t\n]+/); for (i = 1; i <= n; i++) if (a[i] != "") ban[a[i]] = 1 }
      NF >= 3 && $(NF-1) ~ /^[TtDdSsBb]$/ { s = $NF; sub(/^_/, "", s); if (s in ban) print obj ": defines " s }'
  done
}

T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT
printf 'typedef __SIZE_TYPE__ size_t;\nint memcmp(const void *a, const void *b, size_t n)\n{ const unsigned char *x = a, *y = b; for (; n; n--, x++, y++) if (*x != *y) return *x - *y; return 0; }\n' > "$T/planted.c"
# shellcheck disable=SC2086  # CC is a command line: its words are meant to split
$CC -c "$T/planted.c" -o "$T/planted.o"
if [ "$(scan "$T/planted.o")" != "$T/planted.o: defines memcmp" ]; then
  echo "libc_gate: the planted object defines memcmp and $NM did not show it: the gate cannot see" >&2
  exit 1
fi

out="$(scan "$@")"
if [ -n "$out" ]; then
  echo "$out" >&2
  echo "libc_gate: $(echo "$out" | wc -l | tr -d ' ') C library definition(s) in cbase objects (above): a library only requires them; the final program defines them" >&2
  exit 1
fi
echo "libc gate: $# objects define no C library symbol"
