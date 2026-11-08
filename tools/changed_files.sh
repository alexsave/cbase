#!/usr/bin/env bash
# changed_files.sh OLD NEW [PREFIX]: the files that changed in cbase between two of its commits,
# one per line, each as the product names it (PREFIX, default "cbase/": where it mounts cbase).
#
# For a product's CI, which sees a bump of the cbase submodule as one changed path ("cbase", the
# gitlink) and nothing inside it: a path filter or a selector that reads `git diff --name-only`
# feeds this the gitlink's old and new commits and gets the paths a change in the product's own
# tree would have listed, e.g.
#
#   old=$(git rev-parse "$BASE:cbase") new=$(git rev-parse "$HEAD:cbase")
#   cbase/tools/changed_files.sh "$old" "$new"
#
# Both commits must be in the submodule's object store (a fetch of its history first, in a shallow
# CI checkout). Exits 2 when either is not a commit there.
set -euo pipefail
cb="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
[ $# -ge 2 ] || { echo "usage: changed_files.sh OLD NEW [PREFIX]" >&2; exit 2; }
prefix="${3-cbase/}"
for c in "$1" "$2"; do
  git -C "$cb" cat-file -e "$c^{commit}" 2>/dev/null || { echo "changed_files.sh: $c is not a commit of $cb" >&2; exit 2; }
done
git -C "$cb" diff --name-only --no-renames "$1" "$2" | sed "s|^|$prefix|"
