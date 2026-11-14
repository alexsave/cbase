#!/bin/bash
# vm_render_test.sh - the tool's VM renders, byte for byte against their golden files: the unit,
# the socket, the deploy script (vm_remote.sh spliced), the undeploy and data scripts, the boot
# user-data, the artifact path, the memory fit. The golden files are under test/golden/vm_render/,
# one per render, named by the case: they are the bytes the shell tool's vm.sh functions rendered
# when the shell tool was retired, and vm.sh is gone, so they are the record now. The harness is
# test/vm_render.py, which imports the tool's own modules (vm, vm_remote, vm_boot, conf).
# No AWS is called: the region and the state bucket are given, as the AWS-reading steps would have
# read them.
#
#   bash tools/cbase_cli/test/vm_render_test.sh      (make cbase-render-test runs it)
#
# A case whose render differs from its golden file prints the diff and fails; the golden directory
# must hold exactly one file per case, no more, no less.
set -euo pipefail
export LC_ALL=C
cli="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
golden="$cli/test/golden/vm_render"
t="$(mktemp -d "${TMPDIR:-/tmp}/cbase-render.XXXXXX")"
end=""
# The end mark (lib.sh says why): macOS bash 3.2 exits 0 when set -u stops a script with an EXIT trap.
trap 'rm -rf "$t"; [ "$end" = 1 ] || { echo "vm_render_test: FAIL: the run stopped before its end (above)" >&2; exit 1; }' EXIT
fail() { echo "vm_render_test: FAIL: $*" >&2; exit 1; }
pass=0

# gold_name NAME: the golden file of a case, the case's name with ": " as "." and each space as "_".
gold_name() { printf '%s' "$1" | sed -e 's/: /./' -e 's/ /_/g'; }

# check NAME CONF MODE ARGS...: the render is the golden file, byte for byte.
check() {
  local name="$1" conf="$2"; shift 2
  local gold
  gold="$golden/$(gold_name "$name")"
  [ -f "$gold" ] || fail "$name: no golden file $gold"
  python3 -I -B "$cli/test/vm_render.py" "$conf" "$@" > "$t/c.out" 2> "$t/c.err" || { cat "$t/c.err" >&2; fail "$name: the render failed"; }
  if ! cmp -s "$gold" "$t/c.out"; then
    { diff -u "$gold" "$t/c.out" || true; } > "$t/diff"
    head -40 "$t/diff" >&2
    fail "$name: the render differs from its golden file"
  fi
  [ -s "$t/c.out" ] || fail "$name: the render is empty"
  pass=$((pass + 1))
}

# The cases: each a cbase.conf the tool reads the same way.
conf_plain="$t/plain.conf"; printf '%s\n' "CBASE_APP = shop" "CBASE_VM = g9" "CBASE_STATIC = none" "CBASE_MEMORY = 64M" > "$conf_plain"
conf_undeclared="$t/undeclared.conf"; printf '%s\n' "CBASE_APP = shop" "CBASE_VM = g9" "CBASE_STATIC = none" > "$conf_undeclared"
conf_adopted="$t/adopted.conf"; printf '%s\n' "CBASE_APP = amgi" "CBASE_VM = veteran" "CBASE_VM_STACK = cbase-vm" "CBASE_STATIC = none" "CBASE_MEMORY = 128M" > "$conf_adopted"
conf_legacy="$t/legacy.conf"; printf '%s\n' "CBASE_APP = game" "CBASE_VM = g9" "CBASE_STATIC = none" "CBASE_MEMORY = 16M" \
  "CBASE_VM_PORT = 8080" "CBASE_VM_DATA = /var/lib/game" "CBASE_VM_REPLACES = old.service" "CBASE_BACKUP_KEY = cbase/latest.sqlite" > "$conf_legacy"
conf_paths="$t/paths.conf"; printf '%s\n' "CBASE_APP = x" "CBASE_VM = g9" "CBASE_STATIC = none" "CBASE_MEMORY = 32M" \
  "CBASE_BACKUP_BUCKET = bk-bucket" "CBASE_VM_SETTINGS = /s/x/" "CBASE_VM_ARTIFACTS = s3://art-b/p/" > "$conf_paths"

SHA=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
for conf in "$conf_plain" "$conf_undeclared" "$conf_adopted" "$conf_legacy" "$conf_paths"; do
  name="$(basename "$conf" .conf)"
  check "$name: unit" "$conf" unit 8700 us-east-1 state-bucket-1
  check "$name: socket" "$conf" socket 8700
  check "$name: deploy" "$conf" deploy 8700 art-bucket artifacts/k/$SHA "$SHA" us-east-1 state-bucket-1
  check "$name: undeploy kept" "$conf" undeploy 0
  check "$name: undeploy --delete-data" "$conf" undeploy 1
  check "$name: data" "$conf" data
  check "$name: artifacts" "$conf" artifacts 111122223333 us-east-1 state-bucket-1
  check "$name: fit" "$conf" fit t4g.nano 512
done
check "boot user-data" "$conf_plain" boot
# The encoding a script travels in: every padding case (n % 3 = 1, 2, 0) and a multi-line script.
for text in a ab abc abcd abcde abcdef 'set -e
echo "x y" > /tmp/f
exit 0' '#!/bin/bash
echo hi'; do
  check "base64 of ${#text} bytes" "$conf_plain" b64 "$text"
done

# Every golden file is a case: a file no case reads is a render this test no longer checks.
[ "$(find "$golden" -type f | wc -l | tr -d ' ')" = "$pass" ] || fail "the golden directory holds $(find "$golden" -type f | wc -l | tr -d ' ') files for $pass cases"
end=1
echo "vm_render_test: ok ($pass renders equal their golden files)"
