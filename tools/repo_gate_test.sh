#!/usr/bin/env bash
# repo_gate_test.sh - nothing in this repository reaches outside it.
#
# cbase is a repository of its own, and a product's submodule: a path that climbs above its root
# names whatever happens to sit beside it (a product's tree, another checkout), and builds or runs
# only there. So every tracked build file, source and script is read, and the gate fails, naming
# file and line, when one
#
#   * climbs above the repository root: a run of k "../" in a file k-1 or fewer directories
#     deep (paths in a Makefile, a script and an #include are taken from the file's own
#     directory, where make -C, $(dirname "$0") and the compiler start), or
#   * names a product's tree by its path: "foolish/" (the product cbase was carved out of; a
#     product reaches cbase, never the other way round).
#
# And tools/ stands apart from the platform: a project takes tools/structgen (with tools/sgcommon,
# tools/llvm.mk and lib/fnv.h) without core or a provider, so the gate also fails when
#
#   * a C file under tools/ includes a header that is not under tools/ or lib/: each
#     #include "x" must be found beside the file or in tools/sgcommon (the one -I a tool's
#     build adds, tools/llvm.mk), and be under tools/ or lib/ once found; a header found
#     nowhere there is reached through some -I into core/ or a provider, and fails too, or
#   * a Makefile or .mk under tools/ names core/ or providers/ (an -I, a source, a VPATH).
#
# And the UI framework cdom has one edge into cbase and none out of it: cbase may depend on cdom
# for its browser adapter alone (client/hosts/cdom, an adapter onto client/hosts/common, its
# test adapter client/test/unit_host_cdom.c, and the minimal app built on it, example/minimal),
# and cdom never names cbase. So the gate also fails when
#
#   * a tracked file of any kind (a binary too) outside docs/, the top README.md, client/hosts/cdom/,
#     client/test/unit_host_cdom.c and example/minimal/ holds "cdom", in any case. The client's Makefile, CI and
#     mutation check reach that host through what it declares in its own directory, never by
#     name. This file spells the word and is not read for it.
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

# tools_scan <root> <file>...: prints every include from tools/ that leaves tools/ and lib/, and
# every tools/ make file naming core/ or providers/, as "<file>:<line>: <why>: <text>". The
# files are paths from <root>, every one under tools/.
tools_scan() {
  local root="$1"; shift
  ( cd "$root" && perl -MFile::Spec -e '
    sub norm { my @o; for my $p (split m{/}, $_[0]) { next if $p eq "" || $p eq "."; if ($p eq "..") { return undef unless @o; pop @o; } else { push @o, $p; } } join "/", @o }
    for my $f (@ARGV) {
      open(my $h, "<", $f) or next;
      (my $dir = $f) =~ s{/[^/]*$}{};
      my $mk = $f =~ m{(^|/)Makefile$|\.mk$};
      while (my $l = <$h>) {
        chomp(my $t = $l);
        if ($mk) {
          next if $l =~ m{^\s*#};
          print "$f:$.: a tools/ make file names core/ or providers/: $t\n" if $l =~ m{(?<![A-Za-z0-9_.-])(core|providers)/};
          next;
        }
        next unless $l =~ m{^\s*#\s*include\s*"([^"]+)"};
        my $inc = $1; my $at;
        for my $d ($dir, "tools/sgcommon") { my $c = norm("$d/$inc"); if (defined $c && -f $c) { $at = $c; last; } }
        if (!defined $at) { print "$f:$.: includes a header found neither beside it nor in tools/sgcommon (an -I into core/ or a provider?): $t\n"; }
        elsif ($at !~ m{^(tools|lib)/}) { print "$f:$.: includes $at, outside tools/ and lib/: $t\n"; }
      }
    }' "$@" )
}

# The files read: tracked, of the kinds that hold paths, but this file (it spells the patterns).
files() {
  git -C "$ROOT" ls-files -- 'Makefile' '*/Makefile' '*.mk' '*.c' '*.h' '*.sh' '*.py' '*.mjs' '*.js' '*.ts' '*.tsx' \
                              '*.yml' '*.yaml' '*.swift' '*.json' '*.service' \
    | grep -v -e '/corpus/' -e "^$SELF\$"
}

# named_list: the tracked files (paths on stdin) read for the framework's name: all but docs/, the
# top README.md, the framework's adapter (its directory and its test adapter), the minimal app on
# it (example/minimal/) and this file.
named_list() {
  grep -v -e '^docs/' -e '^README\.md$' -e '^client/hosts/cdom/' -e '^client/test/unit_host_cdom\.c$' \
          -e '^example/minimal/' -e "^$SELF\$" || true
}

# named_scan <root> <file>...: prints every line that holds the framework's name, in any case, as
# "<file>:<line>: names cdom: <text>".
named_scan() {
  local root="$1"; shift
  ( cd "$root" && perl -e '
    for my $f (@ARGV) {
      open(my $h, "<", $f) or next;
      while (my $l = <$h>) { if ($l =~ m{cdom}i) { chomp $l; print "$f:$.: names cdom: $l\n"; } }
    }' "$@" )
}

fails=0
fail() { echo "FAIL: $*"; fails=$((fails + 1)); }

# ---- the scanner sees each kind, over planted files ------------------------------------------
P="$(mktemp -d)"
trap 'rm -rf "$P"' EXIT
mkdir -p "$P/a/b"
printf 'include ../../x.mk\n' > "$P/a/Makefile"                    # 2 up from 1 deep: out
printf '#include "../../lib/x.h"\n' > "$P/a/b/ok.c"                # 2 up from 2 deep: in
# shellcheck disable=SC2016  # planted text: the \$( must reach the file unexpanded
printf 'cd "$(dirname "$0")/../../.." && ls\n' > "$P/a/b/up.sh"    # 3 up from 2 deep: out
printf 'CB := ../../cbase/foolish/server\n' > "$P/a/b/name.mk"    # in, but names the tree
printf 'a.foolish/x and my_foolish/x are not the tree\n' > "$P/a/b/word.c"
printf 'touch ../../../x   # repo-gate: scratch tree\n... and src/...) are not climbs\n' > "$P/a/b/marked.sh"
got="$(scan "$P" a/Makefile a/b/ok.c a/b/up.sh a/b/name.mk a/b/word.c a/b/marked.sh | sed 's/: .*//' | LC_ALL=C sort | tr '\n' ' ')"
[ "$got" = "a/Makefile:1 a/b/name.mk:1 a/b/up.sh:1 " ] || fail "the scanner over planted files saw [$got], not [a/Makefile:1 a/b/name.mk:1 a/b/up.sh:1 ]"

# ---- the tools/ closure, over planted files ------------------------------------------------------
mkdir -p "$P/t/tools/sgcommon" "$P/t/tools/gen/test" "$P/t/lib" "$P/t/core/src/store" "$P/t/providers/aws/src"
touch "$P/t/tools/sgcommon/sgc.h" "$P/t/tools/gen/gen.h" "$P/t/tools/gen/test/fix.h" "$P/t/lib/fnv.h" \
      "$P/t/core/src/store/store.h" "$P/t/providers/aws/src/sigv4.h"
printf '#include "gen.h"\n#include "sgc.h"\n#include "../../lib/fnv.h"\n#include <stdio.h>\n' > "$P/t/tools/gen/ok.c"
printf '#include "fix.h"\n' > "$P/t/tools/gen/test/ok.c"
printf '#include "gen.h"\n#include "../../core/src/store/store.h"\n' > "$P/t/tools/gen/up.c"
printf '  #  include "store/store.h"\n' > "$P/t/tools/gen/path.h"
printf '#include "../../../providers/aws/src/sigv4.h"\n' > "$P/t/tools/gen/test/deep.c"
printf '# the core/ of it is in a comment\nCFLAGS += -I../sgcommon\n' > "$P/t/tools/gen/Makefile"
printf 'CFLAGS += -I../../providers/aws/src\n' > "$P/t/tools/gen/aws.mk"
got="$(tools_scan "$P/t" tools/gen/ok.c tools/gen/test/ok.c tools/gen/up.c tools/gen/path.h tools/gen/test/deep.c tools/gen/Makefile tools/gen/aws.mk \
       | sed 's/: .*//' | LC_ALL=C sort | tr '\n' ' ')"
want="tools/gen/aws.mk:1 tools/gen/path.h:1 tools/gen/test/deep.c:1 tools/gen/up.c:2 "
[ "$got" = "$want" ] || fail "the tools/ closure over planted files saw [$got], not [$want]"

# ---- the framework's name, over planted files ----------------------------------------------------
mkdir -p "$P/n/docs/sub" "$P/n/client/src" "$P/n/sub" "$P/n/tools"
printf 'int x; // the CDom adapter\n' > "$P/n/client/src/a.c"
printf 'all:\n\n# see ../cdom/Makefile\n' > "$P/n/client/Makefile"
printf 'cDOM\n' > "$P/n/sub/README.md"                            # only the top README.md is docs
printf 'cd om and c-dom are not it\n' > "$P/n/sub/ok.sh"
printf 'cdom is an example consumer\n' > "$P/n/docs/sub/x.md"
printf 'cdom\n' > "$P/n/README.md"
printf 'cdom\n' > "$P/n/$SELF"
mkdir -p "$P/n/client/hosts/cdom/sub" "$P/n/client/hosts/cdomx" "$P/n/client/test"
printf '#include "cdom.h"\n' > "$P/n/client/hosts/cdom/sub/h.c"            # the adapter's own tree
printf '#include "cdom.h"\n' > "$P/n/client/test/unit_host_cdom.c"         # and its test adapter
printf '#include "cdom.h"\n' > "$P/n/client/hosts/cdomx/h.c"               # not the adapter's
printf '#include "cdom.h"\n' > "$P/n/client/test/unit_host_cdom.c.orig"    # nor a copy beside it
printf 'cdom\n' > "$P/n/client/test/x_unit_host_cdom.c"
mkdir -p "$P/n/providers/x/client/hosts/cdom" "$P/n/providers/x/client/test"
printf '#include "cdom.h"\n' > "$P/n/providers/x/client/hosts/cdom/y.c"     # an adapter's path, nested: not it
printf '#include "cdom.h"\n' > "$P/n/providers/x/client/test/unit_host_cdom.c"
mkdir -p "$P/n/example/minimal/page" "$P/n/example/minimalx" "$P/n/x/example/minimal"
printf 'include cdom/cdom.mk\n' > "$P/n/example/minimal/Makefile"                # the minimal app: its own
printf '#include "cdom.h"\n' > "$P/n/example/minimal/page/p.c"
printf '#include "cdom.h"\n' > "$P/n/example/minimalx/p.c"                     # not it
printf '#include "cdom.h"\n' > "$P/n/example/p.c"                              # nor the example beside it
printf '#include "cdom.h"\n' > "$P/n/x/example/minimal/p.c"                    # nor its path, nested
mkdir -p "$P/n/core/src" "$P/n/.github/workflows"
printf '#include "cdom.h"\n' > "$P/n/core/src/c.c"                          # the platform: never
printf 'run: make -C client check-host-cdom\n' > "$P/n/.github/workflows/ci.yml"   # CI by its name: no
# shellcheck disable=SC2046  # one path a word
got="$(named_scan "$P/n" $(printf '%s\n' client/src/a.c client/Makefile sub/README.md sub/ok.sh docs/sub/x.md README.md "$SELF" \
         client/hosts/cdom/sub/h.c client/test/unit_host_cdom.c client/hosts/cdomx/h.c client/test/unit_host_cdom.c.orig \
         client/test/x_unit_host_cdom.c providers/x/client/hosts/cdom/y.c providers/x/client/test/unit_host_cdom.c \
         example/minimal/Makefile example/minimal/page/p.c example/minimalx/p.c example/p.c x/example/minimal/p.c \
         core/src/c.c .github/workflows/ci.yml | named_list) \
       | sed 's/: .*//' | LC_ALL=C sort | tr '\n' ' ')"
want=".github/workflows/ci.yml:1 client/Makefile:3 client/hosts/cdomx/h.c:1 client/src/a.c:1 client/test/unit_host_cdom.c.orig:1 client/test/x_unit_host_cdom.c:1 core/src/c.c:1 example/minimalx/p.c:1 example/p.c:1 providers/x/client/hosts/cdom/y.c:1 providers/x/client/test/unit_host_cdom.c:1 sub/README.md:1 x/example/minimal/p.c:1 "
[ "$got" = "$want" ] || fail "the framework's name over planted files saw [$got], not [$want]"

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

tlist="$(git -C "$ROOT" ls-files -- 'tools/*.c' 'tools/*.h' 'tools/Makefile' 'tools/*/Makefile' 'tools/*.mk')"
tn="$(echo "$tlist" | wc -l | tr -d ' ')"
[ "$tn" -ge 30 ] || fail "only $tn files under tools/ were read: did the file kinds move?"
# shellcheck disable=SC2086
tout="$(tools_scan "$ROOT" $tlist)"
if [ -n "$tout" ]; then
  echo "$tout"
  fail "$(echo "$tout" | wc -l | tr -d ' ') line(s) of tools/ reach core/ or a provider (above)"
fi

alist="$(git -C "$ROOT" ls-files | named_list)"
an="$(echo "$alist" | wc -l | tr -d ' ')"
[ "$an" -ge 1000 ] || fail "only $an files were read for the framework's name: did the listing move?"
# shellcheck disable=SC2086
aout="$(named_scan "$ROOT" $alist)"
if [ -n "$aout" ]; then
  echo "$aout"
  fail "$(echo "$aout" | wc -l | tr -d ' ') line(s) outside docs/, README.md and the framework's adapter name cdom (above): only client/hosts/cdom reaches it"
fi

if [ "$fails" -ne 0 ]; then echo "repo_gate_test: $fails failed"; exit 1; fi
echo "ok   repo_gate_test: $n files, none climbs above the root or names a product's tree; $tn under tools/ include nothing from core/ or providers/; $an name no UI framework"
