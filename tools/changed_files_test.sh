#!/usr/bin/env bash
# changed_files_test.sh - tools/changed_files.sh over a scratch repository of its own: the files
# two commits differ in, each under the prefix (default cbase/, or the one given, or none), a
# deleted and an added file both listed, and a name that is no commit refused with status 2.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
t="$(mktemp -d)"
trap 'rm -rf "$t"' EXIT
fail() { echo "changed_files_test: $*" >&2; exit 1; }
g() { git -c user.name=t -c user.email=t@t -c init.defaultBranch=main -C "$t" "$@"; }
g init -q
mkdir -p "$t/tools" "$t/lib"
cp "$here/changed_files.sh" "$t/tools/"
echo a > "$t/lib/a.c"; echo b > "$t/lib/b.c"
g add -A; g commit -q -m one
old="$(g rev-parse HEAD)"
echo a2 > "$t/lib/a.c"; rm "$t/lib/b.c"; echo c > "$t/lib/c.c"
g add -A; g commit -q -m two
new="$(g rev-parse HEAD)"
got="$("$t/tools/changed_files.sh" "$old" "$new" | tr '\n' ' ')"
[ "$got" = "cbase/lib/a.c cbase/lib/b.c cbase/lib/c.c " ] || fail "default prefix: [$got]"
got="$("$t/tools/changed_files.sh" "$old" "$new" vendor/cb/ | tr '\n' ' ')"
[ "$got" = "vendor/cb/lib/a.c vendor/cb/lib/b.c vendor/cb/lib/c.c " ] || fail "a prefix given: [$got]"
got="$("$t/tools/changed_files.sh" "$old" "$new" "" | tr '\n' ' ')"
[ "$got" = "lib/a.c lib/b.c lib/c.c " ] || fail "no prefix: [$got]"
rc=0; "$t/tools/changed_files.sh" "$old" 0000000000000000000000000000000000000000 2>/dev/null || rc=$?
[ "$rc" = 2 ] || fail "a name that is no commit: status $rc, not 2"
echo "changed_files_test: ok"
