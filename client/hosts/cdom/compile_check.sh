#!/bin/bash
# compile_check.sh <README.md> <cbase root> <cc> - the code README.md here shows but quotes from no
# file is still code: every fenced block right after "<!-- compiled: server -->" compiles as a
# product's server file does (cbase's headers, cbase.mk's warnings, -Werror), and every one after
# "<!-- compiled: page -->" is the multi-field reader, built natively with a field() that prints
# what it is handed, and run over an answer whose lines it must split: a line with no '=', a value
# that holds one, a last line with no '\n'. host.mk runs it under `make -C client check` and
# check-host-cdom; nothing is fetched.
set -u
readme=$1 root=$2 cc=$3
fail() { echo "FAIL  cdom README code: $readme: $*" >&2; exit 1; }
[ -f "$readme" ] || fail "no such file"
T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT

awk -v dir="$T" '
  want && /^```/ { on = 1; want = 0; next }
  want { printf "line %d: the %s block is not a code fence\n", NR, kind > (dir "/err"); exit }
  on && /^```$/ { on = 0; next }
  on { print > (dir "/" kind ".c"); next }
  /^<!-- compiled: (server|page) -->$/ { kind = $3; n[kind]++; want = 1 }
  END { for (k in n) if (n[k] != 1) printf "%d %s blocks, not one\n", n[k], k > (dir "/err") }
' "$readme"
[ ! -s "$T/err" ] || fail "$(cat "$T/err")"
[ -s "$T/server.c" ] || fail "no <!-- compiled: server --> block"
[ -s "$T/page.c" ] || fail "no <!-- compiled: page --> block"

{
  printf '#include "cbase/app.h"\n#include "net/http.h"\n#include <stdio.h>\n'
  cat "$T/server.c"
  printf 'void (*const used)(cb_ctx *, const cb_http_req *, cb_http_res *) = h_stats;\n'
} > "$T/s.c"
# shellcheck disable=SC2086
$cc -std=gnu11 -D_GNU_SOURCE -Wall -Wextra -Werror -Wno-unused-parameter -Wshadow -Wvla \
  -I"$root/core/include" -I"$root/client/include" -I"$root/core/src" -I"$root/lib" \
  -fsyntax-only "$T/s.c" 2> "$T/s.err" || fail "the server block does not compile: $(head -5 "$T/s.err")"

{
  printf '#include <stddef.h>\n#include <stdio.h>\n'
  printf 'static void field(const char *k, size_t kn, const char *v, size_t vn) { printf("%%.*s|%%.*s\\n", (int)kn, k, (int)vn, v); }\n'
  cat "$T/page.c"
  printf 'int main(void) {\n'
  printf '  static const char in[] = "served=3\\nno field here\\nversion=1.2\\n\\nempty=\\nsum=a=b";\n'
  printf '  read_fields(in, sizeof in - 1);\n  return 0;\n}\n'
} > "$T/p.c"
# shellcheck disable=SC2086
$cc -std=c11 -Wall -Wextra -Werror -Wshadow -Wvla "$T/p.c" -o "$T/p" 2> "$T/p.err" ||
  fail "the page block does not compile: $(head -5 "$T/p.err")"
printf 'served|3\nversion|1.2\nempty|\nsum|a=b\n' > "$T/want"
"$T/p" > "$T/got" || fail "the page block's reader did not run"
cmp -s "$T/want" "$T/got" || fail "the page block's reader split the answer as '$(tr '\n' ' ' < "$T/got")', not '$(tr '\n' ' ' < "$T/want")'"
echo "ok    cdom README code: the server block compiles, and the page block reads key=value lines"
