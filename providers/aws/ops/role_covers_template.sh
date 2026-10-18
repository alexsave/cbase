#!/bin/bash
# role_covers_template.sh [cbase.yaml] [github_oidc.yaml]: the GitHub deploy
# role can apply a code deploy to the stack.
#
# lambda_deploy.sh updates the cbase stack's CodeKey on its previous template,
# and CloudFormation applies that update with the role's credentials: it
# re-resolves the GetAtt of every resource the update touches, each a read of
# that resource's service as the role. One such read missing fails the update
# and rolls it back (2026-10-18: IotAuthorizer.Arn needed iot:DescribeAuthorizer
# and iot:ListTagsForResource, and no server code shipped until both were granted). So every resource type the
# template reads an attribute of must have its read action in the role. A type
# this table does not know stops the check: add its read here and to the role.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
tpl="${1:-$here/cbase.yaml}"
role="${2:-$here/github_oidc.yaml}"

# Each type's reads, space-separated: all of them must be in the role.
read_action() {
  case "$1" in
    AWS::Lambda::Function) echo lambda:GetFunction ;;
    AWS::Lambda::Url) echo lambda:GetFunctionUrlConfig ;;
    AWS::IAM::Role) echo iam:GetRole ;;
    AWS::CloudFront::Distribution) echo cloudfront:GetDistribution ;;
    AWS::CloudFront::Function) echo cloudfront:DescribeFunction ;;
    AWS::DynamoDB::Table) echo dynamodb:DescribeTable ;;
    AWS::IoT::Authorizer) echo iot:DescribeAuthorizer iot:ListTagsForResource ;;
    *) return 1 ;;
  esac
}

# Logical id -> type, from the Resources section (two-space ids, four-space Type).
types="$(awk '/^Resources:/{r=1; next} /^[A-Za-z]/{r=0} r && /^  [A-Za-z0-9]+:$/{id=$1; sub(":","",id)} r && /^    Type: /{print id, $2}' "$tpl")"
targets="$(grep -oE '!GetAtt [A-Za-z0-9]+\.' "$tpl" | sed 's/!GetAtt //; s/\.$//' | sort -u)"
bad=0
for t in $targets; do
  type="$(awk -v id="$t" '$1 == id {print $2}' <<<"$types")"
  if [ -z "$type" ]; then echo "role_covers_template: GetAtt of $t, which is not a resource of $tpl" >&2; bad=1; continue; fi
  if ! action="$(read_action "$type")"; then
    echo "role_covers_template: $t is a $type, whose read this check does not know: add it here and to $role" >&2
    bad=1; continue
  fi
  for a in $action; do
    if ! grep -qE "^[[:space:]]*-?[[:space:]]*(Action:[[:space:]]*)?\[?.*\b$a\b" "$role"; then
      echo "role_covers_template: the deploy role lacks $a, which a code deploy needs to resolve $t ($type)" >&2
      bad=1
    fi
  done
done
[ "$bad" = 0 ] && echo "role_covers_template: ok ($(wc -w <<<"$targets" | tr -d ' ') resources read)"
exit "$bad"
