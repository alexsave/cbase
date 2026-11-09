#!/bin/bash
# adapter_mk_check.sh [make] - adapter.mk's two rules, read by make itself (run from client/, as
# host.mk's check-cdom-adapter-mk does, in `make check`; nothing is fetched):
#
#   * it needs cdom.mk: read without cdom.mk's variables (CDOM_CFLAGS) it stops, naming the fix, and
#     with them (a stand-in for cdom.mk) it reads;
#   * every file is in one list: across the lists an app on this adapter adds (client.mk's CBC_SRC,
#     CBC_OIDC_SRC and CBC_B64_SRC, adapter.mk's CBC_CDOM_SRC) no file is twice, and lib/b64/b64.c is
#     there exactly once, so no app dedupes;
#   * one include order: every run of include lines in the docs that show one (the top README.md,
#     README.md here, this adapter.mk's header, example/minimal/Makefile) that reaches past cbase.mk
#     is exactly cbase.mk, cdom.mk, client/client.mk, adapter.mk (a server's Makefile, cbase.mk
#     alone, is its own run), and each of them shows the four at least once.
set -u
MAKE_BIN=${1:-make}
fail() { echo "FAIL  cdom adapter.mk: $*" >&2; exit 1; }

# frag <before>: a Makefile that includes client.mk and adapter.mk as an app does, <before> first,
# and prints the lists an app adds, one per line.
frag() {
  printf '%s\n' "$1" 'CBASE := ..' 'include $(CBASE)/client/client.mk' 'include $(CBASE)/client/hosts/cdom/adapter.mk' \
    '$(info $(CBC_SRC) $(CBC_OIDC_SRC) $(CBC_B64_SRC) $(CBC_CDOM_SRC))' 'all: ; @:'
}
run() { frag "$1" | "$MAKE_BIN" -s --no-print-directory -f - all 2>&1; }

# Without cdom.mk: refused, with the fix named.
if out=$(run ''); then fail "read without cdom.mk (CDOM_CFLAGS unset), and should stop: $out"; fi
case "$out" in
  *'adapter.mk: include $(CDOM)/cdom.mk'*) ;;
  *) fail "without cdom.mk it stopped, but not naming the fix: $out" ;;
esac

# With cdom.mk's variables: read, and every file in one list.
out=$(run 'CDOM_CFLAGS := -Istand-in-for-cdom.mk') || fail "with CDOM_CFLAGS set it does not read: $out"
files=$(printf '%s\n' "$out" | tr ' ' '\n' | sed '/^$/d')
[ -n "$files" ] || fail "the lists an app adds are empty"
dup=$(printf '%s\n' "$files" | sort | uniq -d)
[ -z "$dup" ] || fail "in two of CBC_SRC, CBC_OIDC_SRC, CBC_B64_SRC and CBC_CDOM_SRC: $dup"
n=$(printf '%s\n' "$files" | grep -c '/lib/b64/b64\.c$')
[ "$n" = 1 ] || fail "lib/b64/b64.c is in those lists $n times, not once"
echo "ok    cdom adapter.mk: stops without cdom.mk; $(printf '%s\n' "$files" | wc -l | tr -d ' ') files, each in one list, lib/b64 once"

# One include order in every doc that shows one (adapter.mk's header says why this order).
want='cbase.mk cdom.mk client.mk adapter.mk'
for f in ../README.md hosts/cdom/README.md hosts/cdom/adapter.mk ../example/minimal/Makefile; do
  [ -f "$f" ] || fail "no $f to read the include order from"
  runs=$(awk '
    { line = $0; sub(/^#[ \t]*/, "", line) }
    line ~ /^include \$\((CBASE|CDOM)\)\// {
      split(line, w, /[ \t]+/); n = split(w[2], p, "/"); run = run (run == "" ? "" : " ") p[n]; next
    }
    run != "" { print run; run = "" }
    END { if (run != "") print run }' "$f")
  printf '%s\n' "$runs" | grep -qx "$want" || fail "$f shows no run of the four includes, '$want'"
  while IFS= read -r r; do
    [ "$r" = "$want" ] || [ "$r" = cbase.mk ] || fail "$f includes in the order '$r', not '$want'"
  done <<< "$runs"
done
echo "ok    cdom adapter.mk: README.md, README.md here, adapter.mk and example/minimal/Makefile show one include order"
