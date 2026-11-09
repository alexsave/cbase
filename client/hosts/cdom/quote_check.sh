#!/bin/bash
# quote_check.sh <README.md> <cbase root> - the code README.md here quotes is the code: every fenced
# block right after a line "<!-- quoted: <path> -->" (<path> from the cbase root) must be a run of
# whole, consecutive lines of that file, so a quote cannot drift from the example it walks through
# (the section "A cdom page on a cbase server, from the two starters" quotes example/minimal).
# host.mk runs it under `make -C client check` and check-host-cdom; nothing is fetched.
set -u
readme=$1 root=$2
fail() { echo "FAIL  cdom README quotes: $readme: $*" >&2; exit 1; }
[ -f "$readme" ] || fail "no such file"
T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT

# Each quote into $T/<n>.block, its file's path into $T/<n>.path.
awk -v dir="$T" '
  want && /^```/ { on = 1; want = 0; next }
  want { printf "line %d: the quote of %s is not followed by a code fence\n", NR, path > (dir "/err"); exit }
  on && /^```$/ { on = 0; next }
  on { print > (dir "/" n ".block"); next }
  /^<!-- quoted: [^ ]+ -->$/ { n++; path = $3; print path > (dir "/" n ".path"); close(dir "/" n ".path"); want = 1 }
' "$readme"
[ ! -s "$T/err" ] || fail "$(cat "$T/err")"
n=0
for p in "$T"/*.path; do
  [ -e "$p" ] || break
  n=$((n + 1))
  b="${p%.path}.block"
  src="$root/$(cat "$p")"
  [ -f "$src" ] || fail "quotes $(cat "$p"), which is not a file"
  [ -s "$b" ] || fail "the quote of $(cat "$p") is empty"
  # The block's lines must occur in the file as consecutive lines.
  awk 'NR == FNR { q[++nq] = $0; next }
       { f[++nf] = $0 }
       END {
         for (i = 1; i + nq - 1 <= nf; i++) {
           for (j = 1; j <= nq && f[i + j - 1] == q[j]; j++) ;
           if (j > nq) exit 0
         }
         exit 1
       }' "$b" "$src" || fail "a quote of $(cat "$p") is not a run of its lines: '$(head -1 "$b")' ..."
done
[ "$n" -gt 0 ] || fail "quotes nothing"
echo "ok    cdom README quotes: $readme quotes $n runs of lines, each as its file has it"
