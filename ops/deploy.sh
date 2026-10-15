#!/bin/bash
# Deploys c_base to AWS from this Mac with the AWS CLI: the first deploy and
# every later one, the same script.
#
#   aws configure            # or: aws sso login --profile <p>; export AWS_PROFILE=<p>
#   c_base/ops/deploy.sh                 # build, upload, create or update the stack
#   c_base/ops/deploy.sh --import DIR    # and then import a migrate/export.sh export into DynamoDB
#
# What it does, in order:
#   1. checks the CLI has credentials and a region (aws sts get-caller-identity);
#   2. reads the ops settings file (ops/ops_env.sh: $CBASE_OPS_ENV, else
#      ~/.config/cbase/ops.env, else ops/.env). For a stack that does not exist
#      yet it generates the missing secrets into it (openssl rand -hex 32):
#      CBASE_JWT_SECRET, CBASE_ANON_KEY, CBASE_ADMIN_TOKEN. Later runs reuse
#      them, so sessions and the shipped anon key survive a redeploy;
#   3. creates the code bucket if it is missing (private, public access blocked);
#   4. builds the Lambda zip (ops/package.sh: a static arm64 binary, Docker)
#      and uploads it under a name carrying its sha256, so a new build is a
#      new CodeKey and CloudFormation updates the function's code;
#   5. aws cloudformation deploy of ops/cbase.yaml, which waits for the stack;
#   6. prints the stack's outputs (FunctionUrl, WebSocketUrl,
#      DistributionDomain, TableName, PublicUrl);
#   7. seeds the 49 bots into a table that has none (cbase-import --seed-bots),
#      and with --import DIR runs build/cbase-import against the stack's table,
#      both with temporary credentials exported from the CLI.
#
#      Against an existing stack it never generates one: a missing secret
#      stops the run, and a JWT secret that differs from the live function's
#      stops it too (either would sign every user out).
# Settings, from the environment or the settings file (the environment wins):
#   CBASE_CERT_ARN         required: the ACM certificate for the domain, in
#                          us-east-1 (CloudFront reads certificates only there)
#   CBASE_DOMAIN           api.foolish.cards
#   CBASE_STACK            cbase (the CloudFormation stack name)
#   CBASE_FUNCTION_NAME    cbase
#   CBASE_TABLE            cbase
#   CBASE_CODE_BUCKET      cbase-code-<account>-<region>
#   CBASE_ALLOWED_ORIGINS  *
#
# The secrets travel to CloudFormation as parameters (NoEcho for the two
# secret ones), so they appear in this Mac's process list while the deploy
# runs; nothing else ever prints them.
set -euo pipefail

here="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck source=c_base/ops/ops_env.sh
. "$here/ops/ops_env.sh"
env_file="$(cbase_ops_env_file "$here")"
import_dir=""

usage() {
  sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'
  exit 2
}
while [ $# -gt 0 ]; do
  case "$1" in
    --import) [ $# -ge 2 ] || usage; import_dir="$2"; shift 2 ;;
    -h|--help) usage ;;
    *) echo "deploy.sh: unknown argument $1" >&2; usage ;;
  esac
done

die() { echo "deploy.sh: $*" >&2; exit 1; }
for tool in aws openssl docker unzip; do
  command -v "$tool" >/dev/null 2>&1 || die "$tool is not installed"
done

# ---- 1. credentials and region ----------------------------------------------------
account="$(aws sts get-caller-identity --query Account --output text 2>/dev/null)" \
  || die "the AWS CLI has no working credentials: run 'aws configure' (or 'aws sso login' and export AWS_PROFILE)"
region="${AWS_REGION:-${AWS_DEFAULT_REGION:-$(aws configure get region 2>/dev/null || true)}}"
[ -n "$region" ] || die "no region: run 'aws configure set region us-east-1' (or export AWS_REGION)"
echo "account $account, region $region"

# ---- 2. the settings file and the secrets -----------------------------------------
cbase_ops_read "$env_file"
echo "settings: $env_file"
stack_exists=no
if aws cloudformation describe-stacks --region "$region" --stack-name "${CBASE_STACK:-cbase}" >/dev/null 2>&1; then
  stack_exists=yes
fi
ensure_secret() { cbase_ops_secret "$1" "$env_file" "$stack_exists" || exit 1; }
ensure_secret CBASE_JWT_SECRET
ensure_secret CBASE_ANON_KEY
ensure_secret CBASE_ADMIN_TOKEN
if [ "$stack_exists" = yes ]; then
  live_jwt="$(aws lambda get-function-configuration --region "$region" --function-name "${CBASE_FUNCTION_NAME:-cbase}" \
    --query 'Environment.Variables.CBASE_JWT_SECRET' --output text 2>/dev/null || true)"
  if [ -n "$live_jwt" ] && [ "$live_jwt" != None ] && [ "$live_jwt" != "$CBASE_JWT_SECRET" ]; then
    die "CBASE_JWT_SECRET in $env_file is not the live function's: deploying it would sign every user out (RUNBOOK, \"The ops settings file\")"
  fi
fi

domain="${CBASE_DOMAIN:-api.foolish.cards}"
stack="${CBASE_STACK:-cbase}"
function_name="${CBASE_FUNCTION_NAME:-cbase}"
table="${CBASE_TABLE:-cbase}"
bucket="${CBASE_CODE_BUCKET:-cbase-code-$account-$region}"
origins="${CBASE_ALLOWED_ORIGINS:-*}"
cert="${CBASE_CERT_ARN:-}"
if [ -z "$cert" ]; then
  cat >&2 <<MSG
deploy.sh: CBASE_CERT_ARN is not set. CloudFront needs an ACM certificate for
$domain in us-east-1. Request one, add the DNS record it asks for, wait for
it to be ISSUED, then put its ARN in $env_file as CBASE_CERT_ARN=...:

  aws acm request-certificate --region us-east-1 --domain-name $domain --validation-method DNS
  aws acm describe-certificate --region us-east-1 --certificate-arn <arn> \\
      --query 'Certificate.[Status,DomainValidationOptions[0].ResourceRecord]'
MSG
  exit 1
fi
case "$cert" in
  arn:aws:acm:us-east-1:*) ;;
  *) die "CBASE_CERT_ARN must be an ACM certificate in us-east-1 (CloudFront's rule), got $cert" ;;
esac

# ---- 3. the code bucket -----------------------------------------------------------
if ! aws s3api head-bucket --bucket "$bucket" >/dev/null 2>&1; then
  echo "creating s3://$bucket"
  if [ "$region" = us-east-1 ]; then
    aws s3api create-bucket --bucket "$bucket" --region "$region" >/dev/null
  else
    aws s3api create-bucket --bucket "$bucket" --region "$region" \
      --create-bucket-configuration "LocationConstraint=$region" >/dev/null
  fi
  aws s3api put-public-access-block --bucket "$bucket" --public-access-block-configuration \
    BlockPublicAcls=true,IgnorePublicAcls=true,BlockPublicPolicy=true,RestrictPublicBuckets=true
fi

# ---- 4. build and upload the zip --------------------------------------------------
"$here/ops/package.sh"
zip="$here/build/cbase-lambda.zip"
sum="$(awk '{print $1}' "$zip.sha256")"
key="cbase-lambda-${sum:0:16}.zip"
aws s3 cp "$zip" "s3://$bucket/$key" --only-show-errors
echo "uploaded s3://$bucket/$key"

# ---- 5. the stack -----------------------------------------------------------------
aws cloudformation deploy \
  --region "$region" \
  --stack-name "$stack" \
  --template-file "$here/ops/cbase.yaml" \
  --s3-bucket "$bucket" --s3-prefix templates \
  --capabilities CAPABILITY_IAM CAPABILITY_NAMED_IAM \
  --no-fail-on-empty-changeset \
  --parameter-overrides \
    "CodeBucket=$bucket" \
    "CodeKey=$key" \
    "JwtSecret=$CBASE_JWT_SECRET" \
    "AnonKey=$CBASE_ANON_KEY" \
    "AdminToken=$CBASE_ADMIN_TOKEN" \
    "DomainName=$domain" \
    "CertificateArn=$cert" \
    "AllowedOrigins=$origins" \
    "FunctionName=$function_name" \
    "TableName=$table"

# ---- 6. what the stack made -------------------------------------------------------
output() {
  aws cloudformation describe-stacks --region "$region" --stack-name "$stack" \
    --query "Stacks[0].Outputs[?OutputKey=='$1'].OutputValue" --output text
}
aws cloudformation describe-stacks --region "$region" --stack-name "$stack" \
  --query 'Stacks[0].Outputs[].[OutputKey,OutputValue]' --output table
cat <<MSG

Next (RUNBOOK.md): point the DNS CNAME for $domain at $(output DistributionDomain),
then set the web app's NEXT_PUBLIC_SUPABASE_URL=$(output PublicUrl) and
NEXT_PUBLIC_SUPABASE_KEY to CBASE_ANON_KEY from $env_file.
MSG

# ---- 7. the bots, and the import ----------------------------------------------------
# A fresh table has no bots (nothing else seeds DynamoDB; SQLite seeds them at
# open), so every deploy seeds the schema's 49 into a table that has none and
# leaves a table with bots alone. The import then replaces them with the
# hosted rows, as it does on SQLite.
make -s -C "$here" build/cbase-import
stack_table="$(output TableName)"
(
  # Temporary credentials for this subshell only (SSO and assumed roles included).
  eval "$(aws configure export-credentials --format env)"
  export AWS_REGION="$region"
  CBASE_STORE=dynamo CBASE_TABLE="$stack_table" "$here/build/cbase-import" --seed-bots
  if [ -n "$import_dir" ]; then
    [ -d "$import_dir" ] || die "--import: $import_dir is not a directory"
    echo "importing $import_dir into DynamoDB table $stack_table"
    CBASE_STORE=dynamo CBASE_TABLE="$stack_table" "$here/build/cbase-import" "$import_dir"
  fi
)
if [ -n "$import_dir" ]; then
  echo "import done. The export holds password hashes: rm -rf $import_dir"
fi
