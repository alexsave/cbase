#!/bin/bash
# ops/s3_prune_lib.sh's prune_plan against fake bucket listings (no AWS calls),
# and prune_bucket against a fake `aws` that records its calls.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck source=ops/s3_prune_lib.sh
. "$here/s3_prune_lib.sh"
t="$(mktemp -d)"
trap 'rm -rf "$t"' EXIT
fail() { echo "s3_prune_test: $*" >&2; exit 1; }
same() { [ "$2" = "$3" ] || fail "$1: got [$2], want [$3]"; }

vm_list="$(printf '%s\n' \
  vm/cbase-serve-aaa vm/cbase-serve-aaa.sha256 vm/cbase-serve-bbb vm/cbase-serve-bbb.sha256 \
  vm/cbase-serve-ccc vm/cbase-serve-ccc.sha256 vm/amazon-ssm-agent-arm64-1.rpm templates/vm/x.json cbase-lambda-1.zip)"

# vm: keeps live and prev with their .sha256; deletes the older cbase-serve-* only.
same "vm" "$(printf '%s\n' "$vm_list" | prune_plan vm vm/cbase-serve-ccc vm/cbase-serve-bbb)" \
  "$(printf '%s\n' vm/cbase-serve-aaa vm/cbase-serve-aaa.sha256)"
# live is not the newest in the listing: the listing's order decides nothing.
same "vm live is the oldest" "$(printf '%s\n' "$vm_list" | prune_plan vm vm/cbase-serve-aaa vm/cbase-serve-bbb)" \
  "$(printf '%s\n' vm/cbase-serve-ccc vm/cbase-serve-ccc.sha256)"
# no prev: only the live release is kept.
same "vm no prev" "$(printf '%s\n' "$vm_list" | prune_plan vm vm/cbase-serve-bbb | wc -l | tr -d ' ')" 4
# sizes ride along.
same "sizes" "$(printf 'vm/cbase-serve-aaa\t10\nvm/cbase-serve-bbb\t20\n' | prune_plan vm vm/cbase-serve-bbb)" "$(printf 'vm/cbase-serve-aaa\t10')"
# lambda: keeps live and prev, never touches vm/ or templates/.
l_list="$(printf '%s\n' cbase-lambda-1.zip cbase-lambda-2.zip cbase-lambda-3.zip vm/cbase-serve-aaa templates/cbase-lambda-9.zip other.zip)"
same "lambda" "$(printf '%s\n' "$l_list" | prune_plan lambda cbase-lambda-3.zip cbase-lambda-2.zip)" cbase-lambda-1.zip
# boot: the same rule on vm/cbase-vm-boot-*, with no .sha256 beside it, and the kinds are
# disjoint: a boot plan never lists a serve binary, and a vm plan never lists a boot script.
b_list="$(printf '%s\n' vm/cbase-vm-boot-aaa vm/cbase-vm-boot-bbb vm/cbase-vm-boot-ccc vm/cbase-vm-boot-ccc.sha256 \
  vm/cbase-serve-aaa vm/cbase-serve-aaa.sha256 vm/amazon-ssm-agent-arm64-1.rpm templates/vm/x.json)"
same "boot" "$(printf '%s\n' "$b_list" | prune_plan boot vm/cbase-vm-boot-ccc vm/cbase-vm-boot-bbb)" "$(printf '%s\n' vm/cbase-vm-boot-aaa vm/cbase-vm-boot-ccc.sha256)"
same "boot no prev" "$(printf '%s\n' "$b_list" | prune_plan boot vm/cbase-vm-boot-bbb | grep -c cbase-vm-boot)" 3
same "boot never lists serve" "$(printf '%s\n' "$b_list" | prune_plan boot vm/cbase-vm-boot-ccc vm/cbase-vm-boot-bbb | grep -c cbase-serve || true)" 0
same "vm never lists boot" "$(printf '%s\n' "$b_list" | prune_plan vm vm/cbase-serve-aaa | grep -c cbase-vm-boot || true)" 0
mixed="$(printf '%s\n' vm/cbase-serve-aaa vm/cbase-serve-bbb vm/cbase-vm-boot-aaa vm/cbase-vm-boot-bbb)"
same "the live boot script survives a vm prune" "$(printf '%s\n' "$mixed" | prune_plan vm vm/cbase-serve-bbb | tr '\n' ' ')" "vm/cbase-serve-aaa "
if printf '%s\n' "$mixed" | prune_plan boot vm/cbase-serve-bbb >/dev/null 2>&1; then fail "a serve key as the live boot key planned deletions"; fi
same "a serve key as the live boot key prints no plan" "$(printf '%s\n' "$mixed" | prune_plan boot vm/cbase-serve-bbb 2>/dev/null || true)" ""
# nothing to delete is an empty plan, not an error.
same "nothing" "$(printf '%s\n' cbase-lambda-3.zip | prune_plan lambda cbase-lambda-3.zip)" ""
# fail safe: no live key, a live key the listing lacks, an unknown kind.
if printf '%s\n' "$vm_list" | prune_plan vm "" vm/cbase-serve-bbb >/dev/null 2>&1; then fail "an empty live key planned deletions"; fi
if printf '%s\n' "$vm_list" | prune_plan vm vm/cbase-serve-zzz vm/cbase-serve-bbb >/dev/null 2>&1; then fail "a live key missing from the listing planned deletions"; fi
if printf '%s\n' "$vm_list" | prune_plan other vm/cbase-serve-ccc >/dev/null 2>&1; then fail "an unknown kind planned deletions"; fi
out="$(printf '%s\n' "$vm_list" | prune_plan vm vm/cbase-serve-zzz 2>/dev/null || true)"
same "missing live prints nothing" "$out" ""

# prune_bucket against a fake aws.
mkdir "$t/bin"
cat > "$t/bin/aws" <<'S'
#!/bin/bash
echo "$*" >> "$FAKE_LOG"
case "$2" in
  list-objects-v2) [ -z "${FAKE_LIST_FAILS:-}" ] || exit 255; cat "$FAKE_LISTING" ;;
  delete-object) ;;
esac
S
chmod +x "$t/bin/aws"
export FAKE_LOG="$t/log" FAKE_LISTING="$t/listing"
printf 'vm/cbase-serve-aaa\t100\nvm/cbase-serve-aaa.sha256\t90\nvm/cbase-serve-bbb\t100\nvm/cbase-serve-ccc\t100\n' > "$t/listing"
: > "$t/log"
PATH="$t/bin:$PATH" CBASE_PRUNE_DRY_RUN=1 prune_bucket vm b vm/cbase-serve-ccc vm/cbase-serve-bbb > "$t/out"
grep -q 'delete-object' "$t/log" && fail "a dry run deleted"
grep -q 'would delete vm/cbase-serve-aaa (100 bytes)' "$t/out" || fail "the dry run did not list aaa"
grep -q '2 old object(s), 190 bytes' "$t/out" || fail "the dry run's counts are wrong: $(cat "$t/out")"
: > "$t/log"
PATH="$t/bin:$PATH" prune_bucket vm b vm/cbase-serve-ccc vm/cbase-serve-bbb > /dev/null
same "deleted keys" "$(grep delete-object "$t/log" | sed 's/.*--key //' | tr '\n' ' ')" "vm/cbase-serve-aaa vm/cbase-serve-aaa.sha256 "
: > "$t/log"
if PATH="$t/bin:$PATH" FAKE_LIST_FAILS=1 prune_bucket vm b vm/cbase-serve-ccc vm/cbase-serve-bbb >/dev/null 2>&1; then fail "a failed listing succeeded"; fi
grep -q 'delete-object' "$t/log" && fail "a failed listing deleted"
: > "$t/log"
if PATH="$t/bin:$PATH" prune_bucket vm b "" vm/cbase-serve-bbb >/dev/null 2>&1; then fail "an empty live key succeeded"; fi
grep -q 'delete-object' "$t/log" && fail "an empty live key deleted"
# prune_bucket boot lists only the boot prefix and deletes only boot objects.
printf 'vm/cbase-vm-boot-aaa\t10\nvm/cbase-vm-boot-bbb\t10\nvm/cbase-vm-boot-ccc\t10\n' > "$t/listing"
: > "$t/log"
PATH="$t/bin:$PATH" prune_bucket boot b vm/cbase-vm-boot-ccc vm/cbase-vm-boot-bbb > /dev/null
grep -q 'list-objects-v2.*--prefix vm/cbase-vm-boot-' "$t/log" || fail "the boot prune did not list the boot prefix: $(cat "$t/log")"
same "boot deleted keys" "$(grep delete-object "$t/log" | sed 's/.*--key //' | tr '\n' ' ')" "vm/cbase-vm-boot-aaa "
echo "s3_prune_test: ok"
