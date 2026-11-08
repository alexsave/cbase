#!/bin/bash
# providers/aws/ops/role_covers_release.sh against small scripts: a covered
# script passes, a call the role lacks fails, and a call the check does not know
# fails; and with no role named it refuses (the role is the product's).
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
# Actions alone are not enough: a pointer or key prefix the role does not name fails.
printf '      Action: [s3:PutObject, ssm:PutParameter]\n      Resource: arn:x:parameter/cbase-vm/release/serve\n' > "$t/scoped.yaml"
printf '#!/bin/bash\naws ssm put-parameter --name /cbase-vm/release/serve\naws s3api put-object --key vm/cbase-serve-v1\n' > "$t/scoped.sh"
if "$here/role_covers_release.sh" "$t/scoped.sh" "$t/scoped.yaml" 2>/dev/null; then fail "a key prefix the role does not name passed"; fi
printf '      Resource: arn:x:s3:::b/vm/cbase-serve-*\n' >> "$t/scoped.yaml"
"$here/role_covers_release.sh" "$t/scoped.sh" "$t/scoped.yaml" >/dev/null || fail "a script whose pointer and prefix the role names was refused"
printf 'aws ssm put-parameter --name /cbase-vm/release/boot\n' >> "$t/scoped.sh"
if "$here/role_covers_release.sh" "$t/scoped.sh" "$t/scoped.yaml" 2>/dev/null; then fail "a pointer the role does not name passed"; fi
# With no role named it refuses, saying so: the role is a product's, not c_base's. A product checks
# its own: CBASE_DEPLOY_ROLE=<its role template> providers/aws/ops/role_covers_release.sh
if env -u CBASE_DEPLOY_ROLE "$here/role_covers_release.sh" "$t/scoped.sh" 2>"$t/err"; then fail "no role named, and it passed"; fi
grep -q "name the deploy role" "$t/err" || fail "no role named: refused, but not saying why"
echo "role_covers_release_test: ok"
