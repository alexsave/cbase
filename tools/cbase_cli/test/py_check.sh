#!/bin/bash
# py_check.sh - the deploy tool's Python, checked without running it (tools/cbase_cli/README.md "The rules"):
#   * every module parses as Python 3.9 (ast's feature_version), the floor macOS's /usr/bin/python3 sets;
#   * every module imports with warnings as errors (a deprecated call fails here, not on a newer Python);
#   * every module starts with `from __future__ import annotations` (annotations stay text, so a newer
#     typing form in one is never evaluated by 3.9), but cbase.py, which any Python 3 must parse;
#   * no module names json: the tool reads AWS's answers as --output text and writes no JSON.
# The last three are run over planted files first, so a check that stopped seeing fails too.
#
#   bash tools/cbase_cli/test/py_check.sh      (make cbase-py-check runs it, part of check)
#
# PYTHON names the interpreter (python3 by default).
set -euo pipefail
export LC_ALL=C
cli="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
py="${PYTHON:-python3}"
t="$(mktemp -d "${TMPDIR:-/tmp}/cbase-pycheck.XXXXXX")"
trap 'rm -rf "$t"' EXIT
fails=0
fail() { echo "py_check: FAIL: $*" >&2; fails=$((fails + 1)); }

# parse39 DIR: every *.py of DIR parses as Python 3.9; prints the ones that do not.
parse39() {
  "$py" -I -B -W error - "$1"/*.py <<'PY'
import ast, sys
bad = 0
for f in sys.argv[1:]:
    try:
        with open(f, encoding="utf-8") as fh:
            ast.parse(fh.read(), f, feature_version=(3, 9))
    except SyntaxError as e:
        print("%s:%s: not Python 3.9: %s" % (f, e.lineno, e.msg))
        bad = 1
sys.exit(bad)
PY
}
# imports DIR: every module of DIR imports with warnings as errors.
imports() {
  local d="$1" m out=0
  for f in "$d"/*.py; do
    m="$(basename "$f" .py)"
    [ "$m" = cbase ] && continue   # the entry runs the tool when imported
    "$py" -I -B -W error -c "import sys; sys.path.insert(0, sys.argv[1]); import $m" "$d" || { echo "$f: does not import with -W error"; out=1; }
  done
  return $out
}
# futures DIR: every module but cbase.py that lacks the annotations import (Python holds a __future__
# import to the top of its module itself).
futures() {
  local f out=0
  for f in "$1"/*.py; do
    [ "$(basename "$f")" = cbase.py ] && continue
    grep -qx 'from __future__ import annotations' "$f" || { echo "$f: no 'from __future__ import annotations'"; out=1; }
  done
  return $out
}
# nojson DIR: every line of DIR's modules that names json, in any case.
nojson() { ! grep -Hni 'json' "$1"/*.py; }

# ---- the checks see each kind, over planted files ----------------------------------------------
mkdir -p "$t/plant"
printf 'from __future__ import annotations\ndef f(x):\n    match x:\n        case 1:\n            return 1\n' > "$t/plant/m39.py"
parse39 "$t/plant" > /dev/null && fail "a match statement parsed as Python 3.9"
rm "$t/plant/m39.py"
printf 'from __future__ import annotations\nimport warnings\nwarnings.warn("old", DeprecationWarning)\n' > "$t/plant/warn.py"
imports "$t/plant" > /dev/null 2>&1 && fail "a module that warns imported under -W error"
rm "$t/plant/warn.py"
printf 'import os\n' > "$t/plant/nofuture.py"
futures "$t/plant" > /dev/null && fail "a module without the annotations import passed"
rm "$t/plant/nofuture.py"
printf 'from __future__ import annotations\nimport JSON as j\n' > "$t/plant/js.py"
nojson "$t/plant" > /dev/null && fail "a module naming json passed"

# ---- the tool --------------------------------------------------------------------------------------
parse39 "$cli" || fail "a module is not Python 3.9 (above)"
imports "$cli" || fail "a module does not import with warnings as errors (above)"
futures "$cli" || fail "a module lacks the annotations import (above)"
nojson "$cli" || fail "a module names json (above): AWS's answers are read as --output text"
[ "$fails" = 0 ] || exit 1
echo "py_check: ok ($(find "$cli" -maxdepth 1 -name '*.py' | wc -l | tr -d ' ') modules, $("$py" -c 'import sys; print("%d.%d" % sys.version_info[:2])'))"
