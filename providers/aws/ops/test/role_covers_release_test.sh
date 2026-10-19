#!/bin/bash
# providers/aws/ops/role_covers_release.sh against small scripts: a covered
# script passes, a call the role lacks fails, and a call the check does not know
# fails; then the real vm_release.sh and role pass.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
t="$(mktemp -d)"
trap 'rm -rf "$t"' EXIT
fail() { echo "role_covers_release_test: $*" >&2; exit 1; }
cat > "$t/deploy.sh" <<'S'
#!/bin/bash
# aws iam delete-role is in a comment: not a call
id="$(aws autoscaling describe-auto-scaling-groups --auto-scaling-group-names x)"
aws ssm send-command --instance-ids "$id"
aws sts get-caller-identity
S
printf '      Action: autoscaling:DescribeAutoScalingGroups\n      Action: [ssm:SendCommand]\n' > "$t/ok.yaml"
printf '      Action: autoscaling:DescribeAutoScalingGroups\n' > "$t/short.yaml"
"$here/role_covers_release.sh" "$t/deploy.sh" "$t/ok.yaml" >/dev/null || fail "a covered script was refused"
if "$here/role_covers_release.sh" "$t/deploy.sh" "$t/short.yaml" 2>/dev/null; then fail "a missing ssm:SendCommand passed"; fi
echo 'aws lambda update-function-code --function-name x' >> "$t/deploy.sh"
if "$here/role_covers_release.sh" "$t/deploy.sh" "$t/ok.yaml" 2>/dev/null; then fail "an unknown call passed"; fi
"$here/role_covers_release.sh" >/dev/null || fail "the real vm_release.sh and role do not agree"
echo "role_covers_release_test: ok"
