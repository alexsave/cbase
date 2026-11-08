#!/bin/bash
# vm_boot_stub_test.sh - the user-data stub (vm_boot_stub.sh, built by
# vm_boot_lib.sh's stub_build exactly as vm_deploy.sh and vm_release.sh build it)
# run against a fake curl and a temp root. No AWS calls, no network.
#
# Covered: the built size and the size refusal; the stub reads the pinned
# parameter, fetches that key with a SigV4-signed request, checks the sha256 and
# the syntax BEFORE replacing anything, keeps the previous copy, and runs the new
# one; every failed check (bad sha256, a script that does not parse, the SSM read
# failing) leaves the installed copy untouched, leaves no .new behind and does not
# run anything; and boot_assemble splices the unit in.
#
# Mutation checks (each change made to vm_boot_stub.sh in turn, the test red on
# the assertion named, then restored): the sha256sum line deleted -> "a wrong
# sha256 aborts"; the bash -n line deleted -> "a script that does not parse
# leaves the installed copy"; the cp .prev line deleted -> "the previous copy is
# kept"; the trap deleted -> "a failed check leaves no .new"; the ssm request
# signed for s3 -> "a good run exits 0"; the mv deleted -> "the new script is
# installed"; the exec deleted -> "the new script ran"; the chmod deleted -> "the
# installed script is executable by root only".
set -u
here="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck source=providers/local/ops/vm_boot_lib.sh
. "$here/vm_boot_lib.sh"
fails=0
check() { if [ "$2" = "$3" ]; then echo "ok   $1"; else echo "FAIL $1: got [$2] want [$3]"; fails=$((fails + 1)); fi; }

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

# ---- the built size ------------------------------------------------------------
stub_build "$here/vm_boot_stub.sh" "$tmp/stub.sh" 2>/dev/null
size="$(wc -c < "$tmp/stub.sh" | tr -d ' ')"
check "the stub is within the limit ($size bytes)" "$([ "$size" -le "$STUB_LIMIT" ] && echo yes)" "yes"
check "the stub's base64 fits a CloudFormation parameter with room" "$([ "$(base64 < "$tmp/stub.sh" | tr -d '\n' | wc -c)" -le 2048 ] && echo yes)" "yes"
check "no comment line survives the build" "$(sed 1d "$tmp/stub.sh" | grep -c '^#')" "0"
head -c $((STUB_LIMIT + 1)) /dev/zero | tr '\0' 'x' | sed 's/^/#!\/bin\/bash\n/' > "$tmp/fat.sh"
check "a stub over the limit is refused" "$(stub_build "$tmp/fat.sh" "$tmp/fat.out" 2>&1 >/dev/null | grep -c 'limit is 1024')" "1"
stub_build "$tmp/fat.sh" "$tmp/fat.out" 2>/dev/null; check "the refusal returns non-zero" "$?" "1"
stub_build "$here/vm_boot_stub.sh" "$tmp/stub2.sh" 100 2>/dev/null; check "an explicit LIMIT applies" "$?" "1"

# ---- the fake curl --------------------------------------------------------------
mkdir "$tmp/bin"
cat > "$tmp/bin/curl" <<'X'
#!/bin/bash
# Answers IMDS, the SSM read and the S3 fetch; logs every call. Ignores -sSf and the retries.
echo "curl $*" >> "$FAKE_LOG"
url=""; out=""; prev=""
for a in "$@"; do
  [ "$prev" = -o ] && out="$a"
  case "$a" in http://*|https://*) url="$a" ;; esac
  prev="$a"
done
case "$url" in
  */latest/api/token) printf tok ;;
  */latest/meta-data/placement/region) printf us-east-1 ;;
  */latest/meta-data/iam/security-credentials/) printf role ;;
  */latest/meta-data/iam/security-credentials/role) printf '{"AccessKeyId":"AK","SecretAccessKey":"SK","Token":"TK"}' ;;
  https://ssm.us-east-1.api.aws/)
    [ -z "${FAKE_SSM_FAILS:-}" ] || exit 22
    case "$*" in *"--aws-sigv4 aws:amz:us-east-1:ssm"*"X-Amz-Target:AmazonSSM.GetParameter"*) ;; *) echo "bad ssm call" >&2; exit 22 ;; esac
    case "$*" in *'{"Name":"/cbase-vm/release/boot"}'*) ;; *) echo "wrong parameter" >&2; exit 22 ;; esac
    printf '{"Parameter":{"Value":"%s"}}' "$FAKE_VALUE" ;;
  https://bkt.s3.dualstack.us-east-1.amazonaws.com/*)
    case "$*" in *"--aws-sigv4 aws:amz:us-east-1:s3"*) ;; *) echo "unsigned s3 call" >&2; exit 22 ;; esac
    cp "$FAKE_OBJECT" "$out" ;;
  *) echo "unexpected url $url" >&2; exit 22 ;;
esac
X
chmod +x "$tmp/bin/curl"
# GNU sha256sum is what the VM (and CI) has. A Mac's is BSD's, with no -c --quiet: stand in for it.
if ! sha256sum --version 2>&1 | grep -q GNU; then
  cat > "$tmp/bin/sha256sum" <<'X'
#!/bin/bash
# sha256sum -c --quiet, stdin "HASH  FILE": prints "FILE: FAILED" and exits 1 on a mismatch.
[ "$1" = -c ] || exec shasum -a 256 "$@"
rc=0
while read -r want file; do
  have="$(shasum -a 256 "$file" 2>/dev/null | cut -d' ' -f1)"
  [ "$have" = "$want" ] || { echo "$file: FAILED"; rc=1; }
done
exit "$rc"
X
  chmod +x "$tmp/bin/sha256sum"
fi
export FAKE_LOG="$tmp/log"

new_root() { rm -rf "$tmp/root"; mkdir -p "$tmp/root/usr/local/sbin"; : > "$FAKE_LOG"; }
install_old() { printf '#!/bin/bash\necho BOOT RAN old\n' > "$tmp/root/usr/local/sbin/cbase-vm-boot"; chmod 700 "$tmp/root/usr/local/sbin/cbase-vm-boot"; }
# run VALUE OBJECT: the stub, with the parameter reading VALUE and S3 serving OBJECT.
run() {
  FAKE_VALUE="$1" FAKE_OBJECT="$2" VM_ROOT="$tmp/root" PATH="$tmp/bin:$PATH" bash "$tmp/stub.sh" > "$tmp/out" 2>&1
}
D="$tmp/root/usr/local/sbin"

printf '#!/bin/bash\necho BOOT RAN new\n' > "$tmp/new.sh"
sum="$(sha256 "$tmp/new.sh")"

# ---- a good run -----------------------------------------------------------------
new_root; install_old
run "bkt vm/cbase-vm-boot-abc123 $sum" "$tmp/new.sh"; rc=$?
check "a good run exits 0" "$rc" "0"
check "the new script ran" "$(grep -c 'BOOT RAN new' "$tmp/out")" "1"
check "the new script is installed" "$(sha256 "$D/cbase-vm-boot")" "$sum"
check "the installed script is executable by root only" "$(find "$D/cbase-vm-boot" -perm 700 | wc -l | tr -d ' ')" "1"
check "the previous copy is kept" "$(grep -c 'BOOT RAN old' "$D/cbase-vm-boot.prev")" "1"
check "no .new is left" "$(find "$D" -name '*.new' | wc -l | tr -d ' ')" "0"
check "the pinned key was fetched, signed for s3" "$(grep -c 'aws:amz:us-east-1:s3.*https://bkt.s3.dualstack.us-east-1.amazonaws.com/vm/cbase-vm-boot-abc123' "$FAKE_LOG")" "1"
check "the parameter was read, signed for ssm" "$(grep -c 'aws:amz:us-east-1:ssm.*/cbase-vm/release/boot' "$FAKE_LOG")" "1"

# ---- first boot: nothing installed yet ------------------------------------------------
new_root
run "bkt vm/cbase-vm-boot-abc123 $sum" "$tmp/new.sh"; rc=$?
check "a first boot exits 0 and runs the script" "$rc:$(grep -c 'BOOT RAN new' "$tmp/out")" "0:1"
check "a first boot has no previous copy" "$(find "$D" -name '*.prev' | wc -l | tr -d ' ')" "0"

# ---- failed checks leave the installed copy alone -----------------------------------------
old_sum() { sha256 "$D/cbase-vm-boot"; }
new_root; install_old; before="$(old_sum)"
run "bkt vm/cbase-vm-boot-abc123 $(printf 0%.0s $(seq 1 64))" "$tmp/new.sh"; rc=$?
check "a wrong sha256 aborts" "$([ "$rc" -ne 0 ] && echo aborted)" "aborted"
check "a wrong sha256 leaves the installed copy" "$(old_sum)" "$before"
check "a wrong sha256 runs nothing" "$(grep -c 'BOOT RAN' "$tmp/out")" "0"
check "a failed check leaves no .new" "$(find "$D" -name '*.new' | wc -l | tr -d ' ')" "0"
check "a failed check makes no .prev" "$(find "$D" -name '*.prev' | wc -l | tr -d ' ')" "0"

printf '#!/bin/bash\nif then fi (\n' > "$tmp/broken.sh"
new_root; install_old; before="$(old_sum)"
run "bkt vm/cbase-vm-boot-abc123 $(sha256 "$tmp/broken.sh")" "$tmp/broken.sh"; rc=$?
check "a script that does not parse aborts" "$([ "$rc" -ne 0 ] && echo aborted)" "aborted"
check "a script that does not parse leaves the installed copy" "$(old_sum)" "$before"
check "a script that does not parse leaves no .new" "$(find "$D" -name '*.new' | wc -l | tr -d ' ')" "0"

new_root; install_old; before="$(old_sum)"
FAKE_SSM_FAILS=1 run "bkt vm/cbase-vm-boot-abc123 $sum" "$tmp/new.sh"; rc=$?
check "a failed read aborts" "$([ "$rc" -ne 0 ] && echo aborted)" "aborted"
check "a failed read leaves the installed copy" "$(old_sum)" "$before"
check "a failed read fetches nothing from s3" "$(grep -c 's3.dualstack' "$FAKE_LOG")" "0"

# ---- boot_assemble --------------------------------------------------------------
boot_assemble "$here/vm_boot.sh" "$here/cbase.service" "$tmp/boot.sh"; rc=$?
check "boot_assemble succeeds on the real files" "$rc" "0"
check "the unit is spliced in" "$(grep -c '^ExecStart=/usr/local/bin/cbase serve$' "$tmp/boot.sh")" "1"
check "the placeholder is gone" "$(grep -c '^@CBASE_SERVICE@$' "$tmp/boot.sh")" "0"
check "comments stay in the boot script" "$(grep -c '^# SIGTERM drains' "$tmp/boot.sh")" "1"
check "the assembled script parses" "$(bash -n "$tmp/boot.sh" && echo ok)" "ok"
printf '#!/bin/bash\necho no placeholder\n' > "$tmp/nop.sh"
boot_assemble "$tmp/nop.sh" "$here/cbase.service" "$tmp/nop.out" 2>/dev/null; check "a boot script without the placeholder is refused" "$?" "1"
check "boot_key is the content's first 12 hex digits" "$(boot_key 0123456789abcdef0123)" "vm/cbase-vm-boot-0123456789ab"

if [ "$fails" -eq 0 ]; then echo "vm_boot_stub: all pass"; else echo "vm_boot_stub: $fails failed"; exit 1; fi
