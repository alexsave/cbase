#!/bin/bash
# Ships a built Lambda zip to the cbase stack: the code only, never the
# template. .github/workflows/c_base.yml runs it on every push to main, as the
# role ops/github_oidc.yaml makes; it runs the same from this Mac.
#
#   c_base/ops/lambda_deploy.sh c_base/build/cbase-lambda.zip
#
# In order:
#   0. refuses a tree that does not contain origin/main (require_main.sh);
#   1. names the zip by its sha256 (cbase-lambda-<16 hex>.zip, as deploy.sh
#      does), and if the stack's CodeKey is already that name, ships nothing;
#   2. uploads it to the stack's CodeBucket;
#   3. updates the stack on its PREVIOUS template with every parameter kept
#      but CodeKey, so CloudFormation stays the record of what is live and a
#      rollback is the same update with the old key;
#   4. checks the function's CodeSha256 is this zip's, and that
#      <PublicUrl>/health answers "ok cbase".
#
# Settings: CBASE_STACK (cbase), AWS_REGION (us-east-1).
set -euo pipefail

repo="$(cd "$(dirname "$0")/../.." && pwd)"
stack="${CBASE_STACK:-cbase}"
export AWS_REGION="${AWS_REGION:-${AWS_DEFAULT_REGION:-us-east-1}}"

die() { echo "lambda_deploy.sh: $*" >&2; exit 1; }
[ $# -eq 1 ] || { sed -n '2,6p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }
zip="$1"
[ -f "$zip" ] || die "$zip is not a file"
command -v aws >/dev/null 2>&1 || die "the AWS CLI is not installed"

# shellcheck source=c_base/ops/require_main.sh
. "$repo/c_base/ops/require_main.sh"
require_main "$repo"

# ---- 1. the name --------------------------------------------------------------------
if command -v sha256sum >/dev/null 2>&1; then
  sum="$(sha256sum "$zip" | awk '{print $1}')"
else
  sum="$(shasum -a 256 "$zip" | awk '{print $1}')"
fi
if [ -f "$zip.sha256" ]; then
  [ "$(awk '{print $1}' "$zip.sha256")" = "$sum" ] || die "$zip does not match $zip.sha256"
fi
key="cbase-lambda-${sum:0:16}.zip"
code_sha="$(openssl dgst -sha256 -binary "$zip" | base64)"

param() {
  aws cloudformation describe-stacks --stack-name "$stack" \
    --query "Stacks[0].Parameters[?ParameterKey=='$1'].ParameterValue" --output text
}
output() {
  aws cloudformation describe-stacks --stack-name "$stack" \
    --query "Stacks[0].Outputs[?OutputKey=='$1'].OutputValue" --output text
}

current="$(param CodeKey)"
if [ "$current" = "$key" ]; then
  echo "the $stack stack already runs $key; nothing to ship"
else
  # ---- 2. upload ----------------------------------------------------------------------
  bucket="$(param CodeBucket)"
  [ -n "$bucket" ] && [ "$bucket" != None ] || die "the $stack stack has no CodeBucket parameter"
  aws s3 cp "$zip" "s3://$bucket/$key" --only-show-errors
  echo "uploaded s3://$bucket/$key (was $current)"

  # ---- 3. the stack, previous template, CodeKey only ------------------------------------
  args=()
  for k in $(aws cloudformation describe-stacks --stack-name "$stack" --query 'Stacks[0].Parameters[].ParameterKey' --output text); do
    if [ "$k" = CodeKey ]; then
      args+=("ParameterKey=CodeKey,ParameterValue=$key")
    else
      args+=("ParameterKey=$k,UsePreviousValue=true")
    fi
  done
  aws cloudformation update-stack --stack-name "$stack" --use-previous-template \
    --capabilities CAPABILITY_IAM CAPABILITY_NAMED_IAM --parameters "${args[@]}" >/dev/null
  if ! aws cloudformation wait stack-update-complete --stack-name "$stack"; then
    aws cloudformation describe-stack-events --stack-name "$stack" --max-items 15 \
      --query 'StackEvents[].[LogicalResourceId,ResourceStatus,ResourceStatusReason]' --output text >&2 || true
    die "the $stack stack update did not complete (CloudFormation rolled it back to $current)"
  fi
  echo "the $stack stack now runs $key"
fi

# ---- 4. is it live ------------------------------------------------------------------
fn="$(output FunctionName)"
live_sha="$(aws lambda get-function-configuration --function-name "$fn" --query CodeSha256 --output text)"
[ "$live_sha" = "$code_sha" ] || die "function $fn runs CodeSha256 $live_sha, not this zip's $code_sha"
url="$(output PublicUrl)"
for i in 1 2 3 4 5 6; do
  body="$(curl -fsS --max-time 20 "$url/health" || true)"
  case "$body" in
    "ok cbase "*) echo "$url/health: $body"; exit 0 ;;
  esac
  echo "attempt $i: [$body]"
  sleep 5
done
die "$url/health did not answer ok cbase"
