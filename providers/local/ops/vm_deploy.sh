#!/bin/bash
# Deploys the cbase-vm stack (vm.yaml): `cbase serve` on one t4g.nano.
#
#   cbase/providers/local/ops/vm_deploy.sh [BIN]   # create or update the stack (and upload BIN as its release)
#   cbase/providers/local/ops/vm_deploy.sh --restart   # vm_release.sh --restart
#   cbase/providers/local/ops/vm_deploy.sh --verify    # vm_release.sh --verify
#
# A new server binary is vm_release.sh's (CI runs it on every push to main): it
# moves /cbase-vm/release/serve and restarts the instance, with no stack update.
# So with no BIN this keeps the release that parameter names, and a template
# change never rolls the server back. With BIN (a static musl arm64 build, e.g.
# a product's perf build, or `make serve-binary`) it uploads BIN as
# vm/cbase-serve-<short sha> (CBASE_VM_VERSION overrides) and the stack points
# the parameter at it; run --restart after to swap the binary.
#
# What it does, in order:
#   1. credentials and region (aws sts; region from AWS_REGION or aws configure);
#   2. uploads BIN to s3://<CodeBucket>/vm/cbase-serve-<version> with a .sha256,
#      and pins the SSM agent rpm (minimal AL2023 has none) the same way;
#   3. makes the VM's own secrets once, as SSM SecureStrings under
#      /cbase-vm/env/ (CBASE_JWT_SECRET, CBASE_ADMIN_TOKEN, CBASE_ORIGIN_SECRET);
#      never regenerated, never printed;
#   4. pins the boot script: vm_boot.sh with cbase.service inside goes up as
#      vm/cbase-vm-boot-<sha12> and /cbase-vm/release/boot names it. The
#      user-data is only vm_boot_stub.sh (fetch that script, check it, run it;
#      refused over 1024 bytes), so the boot script has no size limit and a
#      change to it reaches the running instance with --restart, no stack
#      update and no new instance;
#   5. cloudformation deploy of vm.yaml as stack cbase-vm;
#   6. with BIN, prunes the bucket (cbase/ops/s3_prune_lib.sh): every other
#      vm/cbase-serve-* and its .sha256 goes, except the release the parameter
#      now names and the one it named before this run (which the instance still
#      runs until --restart). Never templates/ or the amazon-ssm rpm. A prune
#      that fails warns. The same for vm/cbase-vm-boot-* (the new script and the
#      one before it). CBASE_PRUNE_DRY_RUN=1 only prints what it would delete.
#
# Like vm_release.sh it refuses a tree that does not contain origin/main: the
# boot script it pins is this tree's.
#
# It does not read ~/.config/cbase/ops.env: that file holds the live `cbase`
# stack's secrets, and this stack keeps its own in SSM.
#
# Environment: AWS_REGION, CBASE_CODE_BUCKET (cbase-code-<account>-<region>),
# CBASE_VM_STACK (cbase-vm), CBASE_VM_VERSION, CBASE_VM_DESIRED (1),
# CBASE_VM_AZ (picked from the AZs that offer t4g.nano); and the product's instance of the stack,
# both required: CBASE_PUBLIC_URL (https://api.example.com) and CBASE_VM_ORIGIN_DOMAIN (the AAAA
# name CloudFront reaches the instance at, vm-origin.example.com).
set -euo pipefail
ops="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cb="$(cd "$ops/../../.." && pwd)"   # the cbase root
# shellcheck source=ops/require_main.sh
. "$cb/ops/require_main.sh"   # require_main, deploy_repo
repo="$(deploy_repo "$cb")"
die() { echo "vm_deploy.sh: $*" >&2; exit 1; }
case "${1:-}" in
  --restart|--verify) exec "$ops/vm_release.sh" "$1" ;;
  -h|--help) sed -n '2,45p' "$0" | sed 's/^# \{0,1\}//'; exit ;;
  -*) die "unknown argument $1" ;;
esac
# shellcheck source=ops/s3_prune_lib.sh
. "$cb/ops/s3_prune_lib.sh"
require_main "$repo"
public_url="${CBASE_PUBLIC_URL:-}"
origin_domain="${CBASE_VM_ORIGIN_DOMAIN:-}"
[ -n "$public_url" ] && [ -n "$origin_domain" ] || die "set CBASE_PUBLIC_URL and CBASE_VM_ORIGIN_DOMAIN (the product's instance of the stack)"
# shellcheck source=providers/local/ops/vm_boot_lib.sh
. "$ops/vm_boot_lib.sh"   # sha256, boot_assemble, boot_key, stub_build
for tool in aws openssl base64 curl; do command -v "$tool" >/dev/null || die "$tool is not installed"; done

account="$(aws sts get-caller-identity --query Account --output text 2>/dev/null)" || die "no AWS credentials (aws login)"
region="${AWS_REGION:-${AWS_DEFAULT_REGION:-$(aws configure get region 2>/dev/null || true)}}"
[ -n "$region" ] || die "no region"
export AWS_REGION="$region"
stack="${CBASE_VM_STACK:-cbase-vm}"
bucket="${CBASE_CODE_BUCKET:-cbase-code-$account-$region}"

# ---- 2. the releases --------------------------------------------------------------
bin="${1:-}"
if [ -n "$bin" ]; then
  [ -f "$bin" ] || die "no binary at $bin"
  file "$bin" | grep -Eq 'ARM aarch64.*(statically|static-pie) linked' || die "$bin is not a static arm64 binary"
fi
version="${CBASE_VM_VERSION:-$(git -C "$repo" rev-parse --short=9 HEAD)}"
aws s3api head-bucket --bucket "$bucket" 2>/dev/null || die "no bucket $bucket (providers/aws/ops/deploy.sh makes it)"

upload() {   # upload FILE KEY: puts FILE and KEY.sha256 unless KEY is there; prints "<bucket> <key> <sha256>"
  local sum
  sum="$(sha256 "$1")"
  if ! aws s3api head-object --bucket "$bucket" --key "$2" >/dev/null 2>&1; then
    aws s3 cp "$1" "s3://$bucket/$2" --only-show-errors >&2
    printf '%s  %s\n' "$sum" "$(basename "$2")" | aws s3 cp - "s3://$bucket/$2.sha256" --only-show-errors >&2
  else
    local had
    had="$(aws s3 cp "s3://$bucket/$2.sha256" - 2>/dev/null | cut -d' ' -f1)"
    [ "$had" = "$sum" ] || die "s3://$bucket/$2 exists with a different sha256 (set CBASE_VM_VERSION)"
  fi
  printf '%s %s %s\n' "$bucket" "$2" "$sum"
}
prev_key=""
if [ -n "$bin" ]; then
  # The key the parameter names now, which the prune keeps; unreadable means no prune.
  if old_value="$(aws ssm get-parameter --name /cbase-vm/release/serve --query Parameter.Value --output text 2>/dev/null)"; then
    prev_key="$(echo "$old_value" | awk '{print $2}')"
  fi
  serve_release="$(upload "$bin" "vm/cbase-serve-$version")"
else
  serve_release="$(aws ssm get-parameter --name /cbase-vm/release/serve --query Parameter.Value --output text 2>/dev/null || true)"
  if [ -z "$serve_release" ] || [ "$serve_release" = None ]; then die "no BIN and no /cbase-vm/release/serve to keep: name a binary for a first deploy"; fi
  echo "keeping the live release"
fi
echo "serve release: $serve_release"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
curl -sSf -o "$tmp/agent.rpm" "https://s3.dualstack.$region.amazonaws.com/amazon-ssm-$region/latest/linux_arm64/amazon-ssm-agent.rpm"
agent_sum="$(sha256 "$tmp/agent.rpm" | cut -c1-16)"
agent_release="$(upload "$tmp/agent.rpm" "vm/amazon-ssm-agent-arm64-$agent_sum.rpm")"
echo "agent release: $agent_release"

# ---- 3. the VM's own secrets (made once; values never leave this process) ---------
for name in CBASE_JWT_SECRET CBASE_ADMIN_TOKEN CBASE_ORIGIN_SECRET; do
  p="/cbase-vm/env/$name"
  if aws ssm describe-parameters --parameter-filters "Key=Name,Values=$p" --query 'Parameters[0].Name' --output text | grep -q "^$p$"; then
    echo "secret $p: kept"
  else
    openssl rand -hex 32 | tr -d '\n' > "$tmp/v"
    aws ssm put-parameter --name "$p" --type SecureString --value "file://$tmp/v" --no-overwrite >/dev/null
    rm -f "$tmp/v"
    echo "secret $p: made"
  fi
done

# ---- 4. the boot script, and the user-data ---------------------------------------
# The full script is an object the instance fetches by its sha256; the user-data
# is the stub, which a CloudFormation parameter (4096 bytes) holds with room to spare.
prev_boot_key=""
if old_boot="$(aws ssm get-parameter --name /cbase-vm/release/boot --query Parameter.Value --output text 2>/dev/null)"; then
  prev_boot_key="$(echo "$old_boot" | awk '{print $2}')"
fi
boot_assemble "$ops/vm_boot.sh" "$ops/cbase.service" "$tmp/boot.sh" || die "cannot build the boot script"
boot_sum="$(sha256 "$tmp/boot.sh")"
boot_obj="$(boot_key "$boot_sum")"
aws s3 cp "$tmp/boot.sh" "s3://$bucket/$boot_obj" --only-show-errors
aws ssm put-parameter --name /cbase-vm/release/boot --type String --overwrite --value "$bucket $boot_obj $boot_sum" >/dev/null
echo "boot script: $boot_obj ($boot_sum)"
stub_build "$ops/vm_boot_stub.sh" "$tmp/stub.sh" || die "the user-data stub is too big"
user_data="$(base64 < "$tmp/stub.sh" | tr -d '\n')"
echo "user-data: ${#user_data} bytes of base64 (the stub is $(wc -c < "$tmp/stub.sh" | tr -d ' ') bytes)"

# ---- 5. the stack -----------------------------------------------------------------
pl="$(aws ec2 describe-managed-prefix-lists --filters Name=prefix-list-name,Values=com.amazonaws.global.ipv6.cloudfront.origin-facing \
      --query 'PrefixLists[0].PrefixListId' --output text)"
case "$pl" in pl-*) ;; *) die "no IPv6 CloudFront origin-facing prefix list in $region" ;; esac
az="${CBASE_VM_AZ:-$(aws cloudformation describe-stacks --stack-name "$stack" \
      --query "Stacks[0].Parameters[?ParameterKey=='AvailabilityZone'].ParameterValue | [0]" --output text 2>/dev/null || true)}"
if [ -z "$az" ] || [ "$az" = None ]; then
  az="$(aws ec2 describe-instance-type-offerings --location-type availability-zone \
        --filters Name=instance-type,Values=t4g.nano --query 'InstanceTypeOfferings[].Location' --output text | tr '\t' '\n' | sort | head -1)"
fi
# The VM's own origin secret, for the staging distribution's header (never printed).
origin_secret="$(aws ssm get-parameter --name /cbase-vm/env/CBASE_ORIGIN_SECRET --with-decryption --query Parameter.Value --output text)"
[ "${#origin_secret}" -ge 32 ] || die "no /cbase-vm/env/CBASE_ORIGIN_SECRET"
echo "stack $stack in $region ($az), prefix list $pl"
aws cloudformation deploy \
  --stack-name "$stack" \
  --template-file "$ops/vm.yaml" \
  --s3-bucket "$bucket" --s3-prefix templates/vm \
  --capabilities CAPABILITY_IAM \
  --no-fail-on-empty-changeset \
  --parameter-overrides \
    "AvailabilityZone=$az" \
    "DesiredCapacity=${CBASE_VM_DESIRED:-1}" \
    "CodeBucket=$bucket" \
    "ServeRelease=$serve_release" \
    "AgentRelease=$agent_release" \
    "BootStub=$user_data" \
    "CloudFrontPrefixList=$pl" \
    "OriginSecret=$origin_secret" \
    "PublicUrl=$public_url" \
    "OriginDomain=$origin_domain"
aws cloudformation describe-stacks --stack-name "$stack" --query 'Stacks[0].Outputs[].[OutputKey,OutputValue]' --output text

# ---- 6. the prune -------------------------------------------------------------------
if [ -n "$bin" ]; then
  live_key="$(aws ssm get-parameter --name /cbase-vm/release/serve --query Parameter.Value --output text 2>/dev/null | awk '{print $2}')" || live_key=""
  if [ -z "$prev_key" ] || [ "$prev_key" = None ]; then
    echo "prune skipped: the pointer's previous value could not be read"
  elif [ "$live_key" != "vm/cbase-serve-$version" ]; then
    echo "prune skipped: the pointer reads '$live_key', not vm/cbase-serve-$version"
  elif [ "$prev_key" = "$live_key" ]; then
    echo "prune skipped: the pointer already named $live_key (the release it replaced is not known)"
  else
    prune_bucket vm "$bucket" "$live_key" "$prev_key" || echo "warning: the prune failed; the deploy stands" >&2
  fi
fi

# The boot script: the new one and the one it replaced stay (same rule as the binary).
if [ -z "$prev_boot_key" ] || [ "$prev_boot_key" = None ]; then
  echo "boot prune skipped: the pointer's previous value could not be read"
elif [ "$prev_boot_key" = "$boot_obj" ]; then
  echo "boot prune skipped: the pointer already named $boot_obj"
else
  prune_bucket boot "$bucket" "$boot_obj" "$prev_boot_key" || echo "warning: the boot prune failed; the deploy stands" >&2
fi
