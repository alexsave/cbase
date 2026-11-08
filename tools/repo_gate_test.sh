#!/usr/bin/env bash
# repo_gate_test.sh - nothing in this repository reaches outside it.
#
# c_base is a repository of its own, and a product's submodule: a path that climbs above its root
# names whatever happens to sit beside it (a product's tree, another checkout), and builds or runs
# only there. So every tracked build file, source and script is read, and the gate fails, naming
# file and line, when one
#
#   * climbs above the repository root: a run of k "../" in a file k-1 or fewer directories
#     deep (paths in a Makefile, a script and an #include are taken from the file's own
#     directory, where make -C, $(dirname "$0") and the compiler start), or
#   * names a product's tree by its path: "foolish/" (the product c_base was carved out of; a
#     product reaches c_base, never the other way round).
#
# Documentation (*.md, *.html) may describe the history and is not read; neither are the fuzz
# corpora (inputs, not paths). A test that builds a scratch tree of its own in a temporary
# directory (tools/c_rules_test.sh) marks each line that climbs inside it "repo-gate: scratch tree". The checks are run over planted files first, so a gate that
# stopped seeing fails too. Run from anywhere: bash tools/repo_gate_test.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SELF="tools/repo_gate_test.sh"

# scan <root> <file>...: prints every violation as "<file>:<line>: <why>: <text>".
scan() {
  local root="$1"; shift
  ( cd "$root" && perl -e '
    for my $f (@ARGV) {
      open(my $h, "<", $f) or next;
      my $depth = () = $f =~ m{/}g;
      while (my $l = <$h>) {
        next if $l =~ m{repo-gate: scratch tree};
        if ($l =~ m{(?<![A-Za-z0-9_.-])foolish/}) { chomp $l; print "$f:$.: names the product tree foolish/: $l\n"; }
        while ($l =~ m{(?<!\.)((?:\.\.(?:/|(?=[\x22\x27\x60)\s]|$)))+)}g) {
          my $k = () = $1 =~ m{\.\.}g;
          if ($k > $depth) { chomp(my $t = $l); print "$f:$.: climbs $k above a file $depth deep: $t\n"; last; }
        }
      }
    }' "$@" )
}

# The files read: tracked, of the kinds that hold paths, but this file (it spells the patterns).
files() {
  git -C "$ROOT" ls-files -- 'Makefile' '*/Makefile' '*.mk' '*.c' '*.h' '*.sh' '*.py' '*.mjs' '*.js' '*.ts' '*.tsx' \
                              '*.yml' '*.yaml' '*.swift' '*.json' '*.service' \
    | grep -v -e '/corpus/' -e "^$SELF\$"
}

fails=0
fail() { echo "FAIL: $*"; fails=$((fails + 1)); }

# ---- the scanner sees each kind, over planted files ------------------------------------------
P="$(mktemp -d)"
trap 'rm -rf "$P"' EXIT
mkdir -p "$P/a/b"
printf 'include ../../x.mk\n' > "$P/a/Makefile"                    # 2 up from 1 deep: out
printf '#include "../../lib/x.h"\n' > "$P/a/b/ok.c"                # 2 up from 2 deep: in
printf 'cd "$(dirname "$0")/../../.." && ls\n' > "$P/a/b/up.sh"    # 3 up from 2 deep: out
printf 'CB := ../../c_base/foolish/server\n' > "$P/a/b/name.mk"    # in, but names the tree
printf 'a.foolish/x and my_foolish/x are not the tree\n' > "$P/a/b/word.c"
printf 'touch ../../../x   # repo-gate: scratch tree\n... and src/...) are not climbs\n' > "$P/a/b/marked.sh"
got="$(scan "$P" a/Makefile a/b/ok.c a/b/up.sh a/b/name.mk a/b/word.c a/b/marked.sh | sed 's/: .*//' | LC_ALL=C sort | tr '\n' ' ')"
[ "$got" = "a/Makefile:1 a/b/name.mk:1 a/b/up.sh:1 " ] || fail "the scanner over planted files saw [$got], not [a/Makefile:1 a/b/name.mk:1 a/b/up.sh:1 ]"

# ---- the repository ------------------------------------------------------------------------------
list="$(files)"
n="$(echo "$list" | wc -l | tr -d ' ')"
[ "$n" -ge 300 ] || fail "only $n files were read: did the file kinds move?"
# shellcheck disable=SC2086
out="$(scan "$ROOT" $list)"
if [ -n "$out" ]; then
  echo "$out"
  fail "$(echo "$out" | wc -l | tr -d ' ') line(s) reach outside the repository (above)"
fi

if [ "$fails" -ne 0 ]; then echo "repo_gate_test: $fails failed"; exit 1; fi
echo "ok   repo_gate_test: $n files, none climbs above the root or names a product's tree"
