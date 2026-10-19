#!/bin/bash
# role_covers_release.sh [SCRIPT] [github_oidc.yaml]: the GitHub deploy role
# grants every AWS call the CI deploy makes.
#
# The role is least privilege, so a call the script gains and the role lacks
# fails the deploy on main, after every test passed, with production still on
# the old binary. This reads each `aws <service> <command>` in SCRIPT
# (providers/local/ops/vm_release.sh by default) and requires the IAM action
# it needs in the role. A command this table does not know stops the check: add
# its action here and to the role.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
script="${1:-$here/../../local/ops/vm_release.sh}"
role="${2:-$here/github_oidc.yaml}"

# The IAM action(s) a CLI command needs, space-separated; "-" for none.
action_of() {
  case "$1" in
    "sts get-caller-identity") echo - ;;
    "s3api get-object") echo s3:GetObject ;;
    "s3api head-object") echo s3:GetObject ;;
    "s3api put-object") echo s3:PutObject ;;
    "ssm put-parameter") echo ssm:PutParameter ;;
    "ssm send-command") echo ssm:SendCommand ;;
    "ssm get-command-invocation") echo ssm:GetCommandInvocation ;;
    "autoscaling describe-auto-scaling-groups") echo autoscaling:DescribeAutoScalingGroups ;;
    *) return 1 ;;
  esac
}

calls="$(grep -v '^[[:space:]]*#' "$script" | grep -oE '(^|[^A-Za-z_-])aws [a-z0-9]+ [a-z-]+' | sed -E 's/^[^a]*aws //' | sort -u)"
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
[ "$bad" = 0 ] && echo "role_covers_release: ok ($(wc -l <<<"$calls" | tr -d ' ') calls)"
exit "$bad"
