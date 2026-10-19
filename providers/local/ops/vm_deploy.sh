#!/bin/bash
# Deploys the cbase-vm stack (vm.yaml): `cbase serve` on one t4g.nano.
#
#   c_base/providers/local/ops/vm_deploy.sh [BIN]   # create or update the stack (and upload BIN as its release)
#   c_base/providers/local/ops/vm_deploy.sh --restart   # vm_release.sh --restart
#   c_base/providers/local/ops/vm_deploy.sh --verify    # vm_release.sh --verify
#
# A new server binary is vm_release.sh's (CI runs it on every push to main): it
# moves /cbase-vm/release/serve and restarts the instance, with no stack update.
# So with no BIN this keeps the release that parameter names, and a template
# change never rolls the server back. With BIN (a static musl arm64 build, e.g.
# c_base/bench/nano/build.sh vm1 or build_bootstrap.sh serve) it uploads BIN as
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
#   4. gzips vm_boot.sh (with cbase.service inside) into the user-data;
#   5. cloudformation deploy of vm.yaml as stack cbase-vm.
#
# It does not read ~/.config/cbase/ops.env: that file holds the live `cbase`
# stack's secrets, and this stack keeps its own in SSM.
#
# Environment: AWS_REGION, CBASE_CODE_BUCKET (cbase-code-<account>-<region>),
# CBASE_VM_STACK (cbase-vm), CBASE_VM_VERSION, CBASE_VM_DESIRED (1),
# CBASE_VM_AZ (picked from the AZs that offer t4g.nano).
set -euo pipefail
ops="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(git -C "$ops" rev-parse --show-toplevel)"
die() { echo "vm_deploy.sh: $*" >&2; exit 1; }
case "${1:-}" in
  --restart|--verify) exec "$ops/vm_release.sh" "$1" ;;
  -h|--help) sed -n '2,31p' "$0" | sed 's/^# \{0,1\}//'; exit ;;
  -*) die "unknown argument $1" ;;
esac
for tool in aws openssl gzip base64 shasum curl; do command -v "$tool" >/dev/null || die "$tool is not installed"; done

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
  sum="$(shasum -a 256 "$1" | cut -d' ' -f1)"
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
if [ -n "$bin" ]; then
  serve_release="$(upload "$bin" "vm/cbase-serve-$version")"
else
  serve_release="$(aws ssm get-parameter --name /cbase-vm/release/serve --query Parameter.Value --output text 2>/dev/null || true)"
  [ -n "$serve_release" ] && [ "$serve_release" != None ] || die "no BIN and no /cbase-vm/release/serve to keep: name a binary for a first deploy"
  echo "keeping the live release"
fi
echo "serve release: $serve_release"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
curl -sSf -o "$tmp/agent.rpm" "https://s3.dualstack.$region.amazonaws.com/amazon-ssm-$region/latest/linux_arm64/amazon-ssm-agent.rpm"
agent_sum="$(shasum -a 256 "$tmp/agent.rpm" | cut -c1-16)"
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

# ---- 4. user-data -----------------------------------------------------------------
# Comment lines are dropped (a CloudFormation parameter holds 4096 bytes).
awk -v unit="$ops/cbase.service" '
  NR > 1 && /^#/ { next }
  $0 == "@CBASE_SERVICE@" { while ((getline l < unit) > 0) if (l !~ /^#/) print l; next } { print }
' "$ops/vm_boot.sh" > "$tmp/boot.sh"
grep -q '^ExecStart=/usr/local/bin/cbase serve$' "$tmp/boot.sh" || die "cbase.service was not spliced into the boot script"
user_data="$(gzip -9n < "$tmp/boot.sh" | base64 | tr -d '\n')"
[ "${#user_data}" -le 4096 ] || die "user-data is ${#user_data} bytes; a parameter holds 4096"

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
    "UserDataGz=$user_data" \
    "CloudFrontPrefixList=$pl" \
    "OriginSecret=$origin_secret"
aws cloudformation describe-stacks --stack-name "$stack" --query 'Stacks[0].Outputs[].[OutputKey,OutputValue]' --output text
