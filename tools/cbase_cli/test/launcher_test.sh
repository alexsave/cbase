#!/bin/bash
# launcher_test.sh - tools/cbase and cbase.py before the tool runs: each refusal a sentence and status 1,
# never a traceback, a hang or an install dialog. No AWS is called: every run is --help or a refusal.
#   * no python3 on PATH: the launcher says the tool is Python and needs python3 3.9 or newer;
#   * a Mac without the Command Line Tools (uname says Darwin, xcode-select -p fails, and python3 is
#     /usr/bin/python3, Apple's stub): the launcher says so instead of running the stub;
#   * a Python older than 3.9: cbase.py says which one, before any module that needs 3.9 is read;
#   * the launcher passes its arguments through unchanged and leaves no __pycache__ behind (-B).
#
#   bash tools/cbase_cli/test/launcher_test.sh      (make cbase-py-check runs it, part of check)
set -euo pipefail
export LC_ALL=C
tools="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
t="$(mktemp -d "${TMPDIR:-/tmp}/cbase-launcher.XXXXXX")"
trap 'rm -rf "$t"' EXIT
fail() { echo "launcher_test: FAIL: $*" >&2; exit 1; }
pass=0
# expect NAME RC TEXT: the last run ended with RC and its stderr holds TEXT, with no traceback.
expect() {
  [ "$rc" = "$2" ] || { cat "$t/out" "$t/err" >&2; fail "$1: status $rc, not $2"; }
  grep -Fq -- "$3" "$t/err" || { cat "$t/err" >&2; fail "$1: stderr lacks [$3]"; }
  ! grep -q Traceback "$t/err" || { cat "$t/err" >&2; fail "$1: a traceback"; }
  pass=$((pass + 1))
}
# A PATH of the tools the launcher uses (sh's dirname, uname, xcode-select), symlinked one by one,
# and never a python3: the "no python3" case.
mkdir -p "$t/nopy"
for c in dirname uname; do ln -s "$(command -v "$c")" "$t/nopy/$c"; done
rc=0; PATH="$t/nopy" /bin/sh "$tools/cbase" --help > "$t/out" 2> "$t/err" || rc=$?
expect "no python3" 1 "cbase: the deploy tool is Python: it needs python3 (3.9 or newer) on PATH"

# The stub: a uname that says Darwin and an xcode-select that finds no tools, before /usr/bin, whose
# python3 is then the one found. Run only where /usr/bin/python3 exists (macOS, and Linux CI).
if [ -x /usr/bin/python3 ]; then
  mkdir -p "$t/stub"
  printf '#!/bin/sh\necho Darwin\n' > "$t/stub/uname"
  printf '#!/bin/sh\necho "xcode-select: error: unable to get active developer directory" >&2\nexit 2\n' > "$t/stub/xcode-select"
  chmod +x "$t/stub/uname" "$t/stub/xcode-select"
  rc=0; PATH="$t/stub:/usr/bin:/bin" /bin/sh "$tools/cbase" --help > "$t/out" 2> "$t/err" || rc=$?
  expect "the Command Line Tools stub" 1 "cbase: /usr/bin/python3 is macOS's stub until the Command Line Tools are installed"
fi

# An older Python: cbase.py read with sys.version_info set to 3.8.
rc=0; python3 -I -B -c 'import sys; sys.version_info = (3, 8, 18, "final", 0); f = sys.argv[1]; sys.argv = [f, "--help"]; exec(compile(open(f).read(), f, "exec"))' "$tools/cbase_cli/cbase.py" > "$t/out" 2> "$t/err" || rc=$?
expect "Python 3.8" 1 "cbase: the deploy tool needs Python 3.9 or newer, and"
grep -Fq "is 3.8" "$t/err" || fail "Python 3.8: the message does not name 3.8"
[ ! -s "$t/out" ] || fail "Python 3.8: printed the usage too"

# Through: the arguments reach the tool unchanged (the usage on --help, status 0), and no __pycache__
# is written into the checkout. (Apple's /usr/bin/python3 writes none anyway, so the last check bites
# on any other Python: Linux's, Homebrew's.)
rc=0; "$tools/cbase" --help > "$t/out" 2> "$t/err" || rc=$?
[ "$rc" = 0 ] || { cat "$t/err" >&2; fail "--help: status $rc"; }
grep -Fq "usage: cbase/tools/cbase [-f FILE] COMMAND" "$t/out" || fail "--help: no usage on stdout"
rc=0; "$tools/cbase" -f "$t/none.conf" "config" "check" > "$t/out" 2> "$t/err" || rc=$?
[ "$rc" = 1 ] || { cat "$t/err" >&2; fail "-f missing: status $rc, not 1"; }
grep -Fq "$t/none.conf" "$t/err" || { cat "$t/err" >&2; fail "-f missing: the file's name did not come through"; }
[ -z "$(find "$tools/cbase_cli" -name __pycache__ -print -quit)" ] || fail "a __pycache__ under tools/cbase_cli"
pass=$((pass + 2))
echo "launcher_test: ok ($pass checks)"
