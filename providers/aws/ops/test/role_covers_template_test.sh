#!/bin/bash
# ops/role_covers_template.sh against small templates: a covered read passes,
# a read the role lacks fails, and a resource type the check does not know
# fails; then the real template and role pass.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
t="$(mktemp -d)"
trap 'rm -rf "$t"' EXIT
fail() { echo "role_covers_template_test: $*" >&2; exit 1; }
cat > "$t/tpl.yaml" <<'Y'
Resources:
  Fn:
    Type: AWS::Lambda::Function
  Auth:
    Type: AWS::IoT::Authorizer
    Properties:
      AuthorizerFunctionArn: !GetAtt Fn.Arn
  Perm:
    Type: AWS::Lambda::Permission
    Properties:
      SourceArn: !GetAtt Auth.Arn
Y
printf '      Action: lambda:GetFunction\n      Action: [iot:DescribeAuthorizer, iot:ListTagsForResource]\n' > "$t/ok.yaml"
printf '      Action: lambda:GetFunction\n      Action: iot:DescribeAuthorizer\n' > "$t/short.yaml"
"$here/role_covers_template.sh" "$t/tpl.yaml" "$t/ok.yaml" >/dev/null || fail "a covered template was refused"
if "$here/role_covers_template.sh" "$t/tpl.yaml" "$t/short.yaml" 2>/dev/null; then fail "a missing iot:ListTagsForResource passed"; fi
sed 's/AWS::IoT::Authorizer/AWS::Some::Thing/' "$t/tpl.yaml" > "$t/odd.yaml"
if "$here/role_covers_template.sh" "$t/odd.yaml" "$t/ok.yaml" 2>/dev/null; then fail "an unknown resource type passed"; fi
"$here/role_covers_template.sh" >/dev/null || fail "the real template and role do not agree"
echo "role_covers_template_test: ok"
