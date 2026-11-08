#!/bin/bash
# Prunes the code bucket's old releases by hand, with the same rule the deploy
# scripts apply after a release goes live (ops/s3_prune_lib.sh):
#
#   c_base/ops/s3_prune.sh [--dry-run] vm|boot|lambda
#
# Keeps the live release (read from the SSM release parameter for vm and boot, from the
# stack's CodeKey for lambda) and the one before it: for vm the parameter's
# previous distinct key, for lambda the newest other zip. It never deletes
# templates/, the amazon-ssm rpm, or the Lambda function or stack. If the live
# key cannot be read it prunes nothing.
#
# Settings: AWS_REGION (us-east-1), CBASE_CODE_BUCKET (cbase-code-<account>-<region>),
# CBASE_STACK (cbase), CBASE_VM_RELEASE_PARAM (/cbase-vm/release/serve, or
# /cbase-vm/release/boot for boot).
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
die() { echo "s3_prune.sh: $*" >&2; exit 1; }
# shellcheck source=ops/s3_prune_lib.sh
. "$here/s3_prune_lib.sh"
if [ "${1:-}" = --dry-run ]; then export CBASE_PRUNE_DRY_RUN=1; shift; fi
kind="${1:-}"
case "$kind" in vm|boot|lambda) ;; *) sed -n '2,15p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;; esac
command -v aws >/dev/null 2>&1 || die "the AWS CLI is not installed"
export AWS_REGION="${AWS_REGION:-${AWS_DEFAULT_REGION:-us-east-1}}"
account="$(aws sts get-caller-identity --query Account --output text)" || die "no AWS credentials"
bucket="${CBASE_CODE_BUCKET:-cbase-code-$account-$AWS_REGION}"

if [ "$kind" = vm ] || [ "$kind" = boot ]; then
  if [ "$kind" = vm ]; then
    param="${CBASE_VM_RELEASE_PARAM:-/cbase-vm/release/serve}"; stem=vm/cbase-serve-
  else
    param="${CBASE_VM_RELEASE_PARAM:-/cbase-vm/release/boot}"; stem=vm/cbase-vm-boot-
  fi
  live="$(aws ssm get-parameter --name "$param" --query Parameter.Value --output text | awk '{print $2}')" || die "cannot read $param; nothing is pruned"
  case "$live" in "$stem"*) ;; *) die "$param names '$live', not a $stem* key; nothing is pruned" ;; esac
  # The parameter's history, newest first: the first key that is not the live one.
  prev="$(aws ssm get-parameter-history --name "$param" --query 'reverse(Parameters[].Value)' --output text 2>/dev/null \
          | tr '\t' '\n' | awk -v live="$live" -v stem="$stem" '{ if (index($2, stem) == 1 && $2 != live) { print $2; exit } }')" || prev=""
else
  stack="${CBASE_STACK:-cbase}"
  live="$(aws cloudformation describe-stacks --stack-name "$stack" \
          --query "Stacks[0].Parameters[?ParameterKey=='CodeKey'].ParameterValue" --output text)" || die "cannot read $stack's CodeKey; nothing is pruned"
  case "$live" in cbase-lambda-*.zip) ;; *) die "$stack's CodeKey is '$live', not a cbase-lambda-*.zip key; nothing is pruned" ;; esac
  # CloudFormation keeps no history of a parameter: the rollback target is the newest other zip.
  prev="$(aws s3api list-objects-v2 --bucket "$bucket" --prefix cbase-lambda- \
          --query 'reverse(sort_by(Contents, &LastModified))[].Key' --output text | tr '\t' '\n' | awk -v live="$live" '$0 != live { print; exit }')" || prev=""
fi
echo "live: $live; previous: ${prev:-none found}"
prune_bucket "$kind" "$bucket" "$live" "$prev"
