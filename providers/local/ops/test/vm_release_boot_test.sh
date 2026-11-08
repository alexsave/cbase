#!/bin/bash
# vm_release_boot_test.sh - vm_release.sh's boot-script path against a fake `aws`
# and `curl` (no AWS, no network), in a throwaway git clone that contains main:
#
#   --restart: uploads this tree's boot script as vm/cbase-vm-boot-<sha12> (the
#   body is exactly what boot_assemble builds), points /cbase-vm/release/boot at
#   it, THEN sends the stub over SSM (the same text as the user-data), and after
#   the checks pass prunes the boot objects older than the new one and the one it
#   replaced; a failed boot prunes nothing; no serve object is ever deleted.
#   BIN (a release): the same boot steps happen before the instance is touched.
#
# Mutation checks (change made, test red on the named assertion, restored):
# prune_boot's call deleted from --restart -> "the oldest boot object is pruned";
# point_boot deleted -> "the boot pointer names the new key"; the put-object of the
# boot script deleted -> "the boot script is uploaded"; point_boot moved after
# restart -> "the boot pointer moves before the stub runs"; the prune's pointer
# readback guard removed (the `elif [ "$live_boot" != "$boot_key" ]` branch) -> "a
# moved pointer says why" (the prune library refuses a live key the listing lacks as
# well, so the nothing-deleted outcome has two owners); the failed-boot guard cannot be mutated here (the
# fake fails every SSM call, so verify fails the release anyway).
set -u
here="$(cd "$(dirname "$0")/.." && pwd)"
cbase="$(cd "$here/../../.." && pwd)"
fails=0
check() { if [ "$2" = "$3" ]; then echo "ok   $1"; else echo "FAIL $1: got [$2] want [$3]"; fails=$((fails + 1)); fi; }
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
g() { git -c user.name=t -c user.email=t@t -c init.defaultBranch=main "$@"; }

# A clone that is main's tip, holding the files vm_release.sh uses.
g init -q --bare "$tmp/origin.git"
g clone -q "$tmp/origin.git" "$tmp/seed" 2>/dev/null
mkdir -p "$tmp/seed/cbase/ops" "$tmp/seed/cbase/providers/local/ops"
cp "$cbase/ops/require_main.sh" "$cbase/ops/s3_prune_lib.sh" "$tmp/seed/cbase/ops/"
cp "$here/vm_release.sh" "$here/vm_boot_lib.sh" "$here/vm_boot_stub.sh" "$here/vm_boot.sh" "$here/cbase.service" "$tmp/seed/cbase/providers/local/ops/"
g -C "$tmp/seed" add -A; g -C "$tmp/seed" commit -q -m A; g -C "$tmp/seed" push -q origin HEAD:main
g clone -q "$tmp/origin.git" "$tmp/work" 2>/dev/null
rel="$tmp/work/cbase/providers/local/ops/vm_release.sh"
# shellcheck source=providers/local/ops/vm_boot_lib.sh
. "$here/vm_boot_lib.sh"

mkdir "$tmp/bin" "$tmp/state"
cat > "$tmp/bin/aws" <<'X'
#!/bin/bash
# Records every call in order; keeps SSM parameters as files; serves a fixed listing.
echo "aws $*" >> "$FAKE/calls"
val() { local want="$1"; shift; local p=""; for a in "$@"; do [ "$p" = "$want" ] && { printf '%s' "$a"; return; }; p="$a"; done; }
case "$1 $2" in
  "sts get-caller-identity") echo 111122223333 ;;
  "s3api put-object") cp "$(val --body "$@")" "$FAKE/obj_$(val --key "$@" | tr / _)" ;;
  "s3api list-objects-v2") cat "$FAKE/listing" ;;
  "s3api delete-object") ;;
  "s3api get-object") exit 255 ;;
  "ssm get-parameter") f="$FAKE/param_$(val --name "$@" | tr / _)"; [ -f "$f" ] || exit 254; cat "$f" ;;
  "ssm put-parameter")
    v="$(val --value "$@")"
    # FAKE_REPOINT: another release repoints the boot pointer right after this one set it.
    case "$(val --name "$@")" in /cbase-vm/release/boot) [ -z "${FAKE_REPOINT:-}" ] || v="$FAKE_REPOINT" ;; esac
    printf '%s' "$v" > "$FAKE/param_$(val --name "$@" | tr / _)" ;;
  "autoscaling describe-auto-scaling-groups") echo i-fake ;;
  "ssm send-command") val --parameters "$@" >> "$FAKE/params"; echo >> "$FAKE/params"; echo cmd-1 ;;
  "ssm get-command-invocation")
    case "$*" in *"--query Status"*) echo "${FAKE_STATUS:-Success}" ;; *) echo ok ;; esac ;;
  *) echo "fake aws: unexpected $*" >&2; exit 99 ;;
esac
X
cat > "$tmp/bin/curl" <<'X'
#!/bin/bash
echo "ok cbase"
X
printf '#!/bin/bash\necho "ELF 64-bit LSB executable, ARM aarch64, statically linked"\n' > "$tmp/bin/file"
chmod +x "$tmp/bin/aws" "$tmp/bin/curl" "$tmp/bin/file"
export FAKE="$tmp/state" CBASE_CODE_BUCKET=bkt AWS_REGION=us-east-1 CBASE_VM_VERSION=v1 CBASE_PUBLIC_URL=https://api.example.com

# What the tree's boot script and stub are.
boot_assemble "$here/vm_boot.sh" "$here/cbase.service" "$tmp/want_boot.sh"
want_sum="$(sha256 "$tmp/want_boot.sh")"
want_key="$(boot_key "$want_sum")"
stub_build "$here/vm_boot_stub.sh" "$tmp/want_stub.sh"

reset() {
  rm -rf "$tmp/state"; mkdir "$tmp/state"
  printf 'bkt vm/cbase-vm-boot-old000000000 0000' > "$tmp/state/param__cbase-vm_release_boot"
  printf 'bkt vm/cbase-serve-live 0000' > "$tmp/state/param__cbase-vm_release_serve"
  printf '%s\n' vm/cbase-vm-boot-older0000000 vm/cbase-vm-boot-old000000000 "$want_key" vm/cbase-serve-live vm/cbase-serve-live.sha256 > "$tmp/state/listing"
  sed -i.bak 's/$/\t10/' "$tmp/state/listing"; rm -f "$tmp/state/listing.bak"
}
run() { PATH="$tmp/bin:$PATH" bash "$rel" "$@" > "$tmp/out" 2>&1; }
line() { grep -n "$1" "$FAKE/calls" | head -1 | cut -d: -f1; }
deleted() { grep 'delete-object' "$FAKE/calls" | sed 's/.*--key //' | tr '\n' ' '; }

# ---- --restart ------------------------------------------------------------------
reset
run --restart; rc=$?
check "--restart succeeds" "$rc" "0"
check "the boot script is uploaded" "$(grep -c "s3api put-object --bucket bkt --key $want_key --body" "$FAKE/calls")" "1"
check "the uploaded body is exactly the assembled boot script" "$(sha256 "$FAKE/obj_$(echo "$want_key" | tr / _)")" "$want_sum"
check "the boot pointer names the new key" "$(cat "$FAKE/param__cbase-vm_release_boot")" "bkt $want_key $want_sum"
check "the serve pointer is untouched by --restart" "$(cat "$FAKE/param__cbase-vm_release_serve")" "bkt vm/cbase-serve-live 0000"
put="$(line 'ssm put-parameter --name /cbase-vm/release/boot')"; send="$(line 'ssm send-command')"; up="$(line "put-object.*$want_key")"
check "the boot script is uploaded before the pointer moves" "$([ "$up" -lt "$put" ] && echo yes)" "yes"
check "the boot pointer moves before the stub runs" "$([ "$put" -lt "$send" ] && echo yes)" "yes"
cmd="$(grep 'base64 -d' "$FAKE/params" | head -1 | python3 -c 'import json,sys; print(json.load(sys.stdin)["commands"][0])')"
b64="$(printf '%s' "$cmd" | sed 's/.*echo \([A-Za-z0-9+\/=]*\) | base64 -d.*/\1/')"
check "the stub sent over SSM is the user-data text" "$(printf '%s' "$b64" | base64 -d | cmp - "$tmp/want_stub.sh" && echo same)" "same"
# shellcheck disable=SC2016  # the pattern is the literal command text
check "it runs from a file with stdin closed, output to the boot log, a failure shows the tail" "$(printf '%s' "$cmd" | grep -c 'bash $f < /dev/null >> /var/log/cbase-vm-boot.log 2>&1 || { rc=$?; tail -n 20')" "1"
check "the oldest boot object is pruned" "$(deleted)" "vm/cbase-vm-boot-older0000000 "
check "no serve object was deleted" "$(grep 'delete-object' "$FAKE/calls" | grep -c cbase-serve)" "0"
check "the prune comes after the checks" "$([ "$(line 'delete-object')" -gt "$(line 'ssm get-command-invocation')" ] && echo yes)" "yes"

# ---- a failed boot -------------------------------------------------------------------
reset
FAKE_STATUS=Failed run --restart; rc=$?
check "a failed boot fails the release" "$([ "$rc" -ne 0 ] && echo failed)" "failed"
check "a failed boot prunes nothing" "$(deleted)" ""

# ---- another release repoints the pointer before the prune reads it back -------------------------
reset
FAKE_REPOINT="bkt vm/cbase-vm-boot-other0000000 1111" run --restart; rc=$?
check "a moved pointer still lets the release succeed" "$rc" "0"
check "a moved pointer prunes nothing" "$(deleted)" ""
check "a moved pointer says why" "$(grep -c "boot prune skipped: the pointer reads 'vm/cbase-vm-boot-other0000000'" "$tmp/out")" "1"

# ---- a rerun of the same script: nothing new to prune ---------------------------------------
reset
printf 'bkt %s %s' "$want_key" "$want_sum" > "$FAKE/param__cbase-vm_release_boot"
: > "$FAKE/calls"
run --restart; rc=$?
check "a rerun of the same script succeeds" "$rc" "0"
check "a rerun prunes nothing (the replaced script is not known)" "$(deleted)" ""
check "a rerun says why" "$(grep -c 'boot prune skipped: the pointer already named' "$tmp/out")" "1"

# ---- no previous pointer ------------------------------------------------------------------
reset
rm -f "$FAKE/param__cbase-vm_release_boot"
: > "$FAKE/calls"
run --restart; rc=$?
check "a first pin succeeds" "$rc" "0"
check "a first pin prunes nothing" "$(deleted)" ""

# ---- BIN: the boot steps are part of a release ------------------------------------------------
reset
printf 'binary' > "$tmp/cbase.bin"
printf 'vm/cbase-serve-old\nvm/cbase-serve-old.sha256\nvm/cbase-serve-live\nvm/cbase-serve-live.sha256\nvm/cbase-serve-v1\nvm/cbase-serve-v1.sha256\nvm/cbase-vm-boot-older0000000\nvm/cbase-vm-boot-old000000000\n%s\n' "$want_key" | sed 's/$/\t10/' > "$FAKE/listing"
run "$tmp/cbase.bin"; rc=$?
check "a release succeeds" "$rc" "0"
check "a release pins the boot script" "$(cat "$FAKE/param__cbase-vm_release_boot")" "bkt $want_key $want_sum"
check "a release points serve at the new binary" "$(awk '{print $2}' "$FAKE/param__cbase-vm_release_serve")" "vm/cbase-serve-v1"
check "both pointers move before the stub runs" "$([ "$(line 'put-parameter --name /cbase-vm/release/serve')" -lt "$(line 'ssm send-command')" ] && [ "$(line 'put-parameter --name /cbase-vm/release/boot')" -lt "$(line 'ssm send-command')" ] && echo yes)" "yes"
check "the release prune keeps the live and previous of each kind" "$(deleted | tr ' ' '\n' | sort | tr '\n' ' ')" "vm/cbase-serve-old vm/cbase-serve-old.sha256 vm/cbase-vm-boot-older0000000 "

if [ "$fails" -eq 0 ]; then echo "vm_release_boot: all pass"; else echo "vm_release_boot: $fails failed"; exit 1; fi
