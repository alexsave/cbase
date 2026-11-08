#!/bin/bash
# role_covers_release.sh [SCRIPT] [ROLE.yaml]: the GitHub deploy role (ROLE.yaml, or
# CBASE_DEPLOY_ROLE: the product's template of it)
# grants every AWS call the CI deploy makes.
#
# The role is least privilege, so a call the script gains and the role lacks
# fails the deploy on main, after every test passed, with production still on
# the old binary. This reads each `aws <service> <command>` in SCRIPT
# (providers/local/ops/vm_release.sh and the libraries it sources,
# ops/s3_prune_lib.sh and providers/local/ops/vm_boot_lib.sh, by default) and requires the IAM action
# it needs in the role. A command this table does not know stops the check: add
# its action here and to the role.
#
# The actions are not the whole grant: the role scopes them to the release
# parameters and the object-key prefixes. So every SSM parameter name
# (cbase-vm/release/<name>) and every object-key prefix (vm/cbase-<name>-) the
# scripts mention must also appear in the role, or a new pointer or key family
# would pass the actions and still fail the deploy on main with AccessDenied.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
script="${1:-$here/../../local/ops/vm_release.sh}"
# The default also reads the libraries vm_release.sh sources (the prune, and the one
# that names the boot script's key).
files=("$script")
[ -n "${1:-}" ] || files+=("$here/../../../ops/s3_prune_lib.sh" "$here/../../local/ops/vm_boot_lib.sh")
# The role is the product's deploy identity (its template, beside its own stack definition).
role="${2:-${CBASE_DEPLOY_ROLE:-}}"
[ -n "$role" ] || { echo "role_covers_release.sh: name the deploy role's template (argument 2, or CBASE_DEPLOY_ROLE)" >&2; exit 2; }

# The IAM action(s) a CLI command needs, space-separated; "-" for none.
action_of() {
  case "$1" in
    "sts get-caller-identity") echo - ;;
    "s3api get-object") echo s3:GetObject ;;
    "s3api head-object") echo s3:GetObject ;;
    "s3api put-object") echo s3:PutObject ;;
    "s3api list-objects-v2") echo s3:ListBucket ;;
    "s3api delete-object") echo s3:DeleteObject ;;
    "ssm put-parameter") echo ssm:PutParameter ;;
    "ssm get-parameter") echo ssm:GetParameter ;;
    "ssm send-command") echo ssm:SendCommand ;;
    "ssm get-command-invocation") echo ssm:GetCommandInvocation ;;
    "autoscaling describe-auto-scaling-groups") echo autoscaling:DescribeAutoScalingGroups ;;
    *) return 1 ;;
  esac
}

calls="$(cat "${files[@]}" | grep -v '^[[:space:]]*#' | grep -oE '(^|[^A-Za-z_-])aws [a-z0-9]+ [a-z0-9-]+' | sed -E 's/^[^a]*aws //' | sort -u)"
[ -n "$calls" ] || { echo "role_covers_release: no aws call in $script" >&2; exit 1; }
bad=0
while IFS= read -r c; do
  if ! actions="$(action_of "$c")"; then
    echo "role_covers_release: \`aws $c\` in $script is a call this check does not know: add its action here and to $role" >&2
    bad=1; continue
  fi
  [ "$actions" = - ] && continue
  for a in $actions; do
    if ! grep -qE "^[[:space:]]*-?[[:space:]]*(Action:[[:space:]]*)?\[?.*\b$a\b" "$role"; then
      echo "role_covers_release: the deploy role lacks $a, which \`aws $c\` needs" >&2
      bad=1
    fi
  done
done <<<"$calls"
names="$(cat "${files[@]}" | grep -v '^[[:space:]]*#' | grep -oE 'cbase-vm/release/[a-z]+|vm/cbase-[a-z]+(-[a-z]+)*-' | sort -u)" || names=""
while IFS= read -r n; do
  [ -n "$n" ] || continue
  if ! grep -v '^[[:space:]]*#' "$role" | grep -qF -- "$n"; then
    echo "role_covers_release: the deploy role does not name $n, which $script uses" >&2
    bad=1
  fi
done <<<"$names"
[ "$bad" = 0 ] && echo "role_covers_release: ok ($(wc -l <<<"$calls" | tr -d ' ') calls)"
exit "$bad"
