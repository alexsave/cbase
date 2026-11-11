#!/bin/bash
# vm_render_test.sh - the C port's VM renders against the shell's, byte for byte: the unit, the
# socket, the deploy script (vm_remote.sh spliced), the undeploy and data scripts, the boot
# user-data, the artifact path, the memory fit. The shell side is tools/cbase.d/vm.sh's own
# functions, sourced with the cbase.conf of each case; the C side is test/vm_render.c, built here
# from the same sources as build/cbase. No AWS is called: the region and the state bucket are
# given, as the AWS-reading steps would have read them.
#
#   bash tools/cbase_cli/test/vm_render_test.sh      (make cbase-cli-vm-render-test runs it)
#
# CC names the compiler (cc by default). The renders compare equal for every case below; a case
# that differs prints the diff and fails.
set -euo pipefail
export LC_ALL=C
cli="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
tools="$(cd "$cli/.." && pwd)"
t="$(mktemp -d "${TMPDIR:-/tmp}/cbase-render.XXXXXX")"
end=""
# The end mark (lib.sh says why): macOS bash 3.2 exits 0 when set -u stops a script with an EXIT trap.
trap 'rm -rf "$t"; [ "$end" = 1 ] || { echo "vm_render_test: FAIL: the run stopped before its end (above)" >&2; exit 1; }' EXIT
fail() { echo "vm_render_test: FAIL: $*" >&2; exit 1; }
pass=0

# The C side links the tool's own objects (the Makefile's build, which vm.c's neighbours need to link),
# less main.o, with test/vm_render.c in main's place.
make -C "$cli" build/cbase > /dev/null
objs=()
while IFS= read -r o; do objs+=("$o"); done < <(find "$cli/build/obj" -name '*.o' ! -name main.o | sort)
${CC:-cc} -std=gnu11 -D_GNU_SOURCE -O2 -Wall -Wextra -Werror -Wshadow -Wvla -Wformat=2 \
  -DCBC_TOOLS_DIR="\"$tools\"" \
  "$cli/test/vm_render.c" "${objs[@]}" -o "$t/vm_render"

# shell_render CONF MODE ARGS...: the shell's render, in a subshell with the tool's functions.
shell_render() {
  local conf="$1" mode="$2"; shift 2
  (
    here="$tools"
    CB_TMP="$t/shtmp"
    mkdir -p "$CB_TMP"
    # shellcheck source=tools/cbase.d/conf.sh
    . "$tools/cbase.d/conf.sh"
    # shellcheck source=tools/cbase.d/vm.sh
    . "$tools/cbase.d/vm.sh"
    conf_load "$conf"
    # The AWS-reading steps are given, not called: the region the CLI names, the state bucket.
    vm_region() { printf '%s' "$VR_REGION"; }
    state_bucket() { printf '%s' "$VR_STATE"; }
    case "$mode" in
      unit) VR_REGION="$2"; VR_STATE="$3"; vm_unit "$1" ;;
      socket) vm_socket "$1" ;;
      deploy) VR_REGION="$5"; VR_STATE="$6"; vm_remote_deploy "$1" "$2" "$3" "$4" "$5" ;;
      undeploy) vm_remote_undeploy "$1" ;;
      data) vm_remote_data ;;
      boot) awk 'NR == 1 || (!/^[[:space:]]*#/ && !/^$/)' "$tools/cbase.d/vm_boot.sh" ;;
      artifacts) AWS_ACCOUNT="$1"; VR_REGION="$2"; VR_STATE="$3"; vm_artifacts ;;
      b64) printf '%s' "$1" | base64 | tr -d '\n' ;;
      fit) VM_TYPE="$1"; VM_MIB="$2"; vm_fit_set; printf '%s\n%s\n' "$STATE_FIT_LIMIT" "$STATE_FIT_WHAT" ;;
    esac
  )
}

# check NAME CONF MODE ARGS...: the two renders are equal; the shell's is the oracle.
check() {
  local name="$1" conf="$2"; shift 2
  shell_render "$conf" "$@" > "$t/shell.out" 2> "$t/shell.err" || { cat "$t/shell.err" >&2; fail "$name: the shell's render failed"; }
  "$t/vm_render" "$conf" "$@" > "$t/c.out" 2> "$t/c.err" || { cat "$t/c.err" >&2; fail "$name: the C render failed"; }
  if ! cmp -s "$t/shell.out" "$t/c.out"; then
    { diff -u "$t/shell.out" "$t/c.out" || true; } > "$t/diff"
    head -40 "$t/diff" >&2
    fail "$name: the C render differs from the shell's"
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

end=1
echo "vm_render_test: ok ($pass renders equal the shell's)"
