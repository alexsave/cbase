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
#      CBASE_JWT_SECRET and CBASE_ADMIN_TOKEN. Later runs reuse them, so
#      sessions survive a redeploy. Those two go to SSM Parameter Store as
#      SecureStrings (<SecretsPrefix>jwt-secret, admin-token), written only
#      when missing or changed; the function reads them from there;
#   3. creates the code bucket if it is missing (private, public access blocked);
#   4. builds the Lambda zip (ops/package.sh: a static arm64 binary, Docker)
#      and uploads it under a name carrying its sha256, so a new build is a
#      new CodeKey and CloudFormation updates the function's code;
#   5. a change set of ops/cbase.yaml, printed and refused when it replaces a
#      resource (CBASE_ALLOW_REPLACE=1 to mean it), then executed and waited for;
#   6. prints the stack's outputs (FunctionUrl, WebSocketUrl,
#      DistributionDomain, TableName, PublicUrl);
#   7. seeds the 49 bots into a table that has none (cbase-import --seed-bots),
#      moves rating rows to the leaderboard's index key (--reindex-ratings),
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
#   CBASE_ORIGIN_ENFORCED  yes (no: send the origin header without checking it,
#                          for the first deploy of a new or rotated secret)
#   CBASE_ALARM_EMAIL      where the alarms are emailed ("" for nobody)
#   CBASE_IOT              yes: realtime over AWS IoT Core too (the account's
#                          iot:Data-ATS endpoint is read and passed as
#                          IotEndpoint); no: none of it
#
# The JWT secret and the admin token travel to SSM through a 0600 file; the
# origin secret travels to CloudFormation as a NoEcho parameter, so it appears
# in this Mac's process list while the deploy runs (CloudFront holds it in the
# distribution anyway). Nothing else ever prints them.
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
for tool in aws openssl docker unzip jq; do
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
ensure_secret CBASE_ADMIN_TOKEN
# The origin secret (cbase.yaml OriginSecret) is new to a stack made before it:
# generating it there is safe as long as the same deploy only sends it and does
# not yet check it (CBASE_ORIGIN_ENFORCED=no; RUNBOOK "Closing the origin").
origin_live=no
if [ "$stack_exists" = yes ] && aws cloudformation describe-stacks --region "$region" --stack-name "${CBASE_STACK:-cbase}" \
     --query "Stacks[0].Parameters[?ParameterKey=='OriginSecret'].ParameterKey" --output text 2>/dev/null | grep -q OriginSecret; then
  origin_live=yes
fi
cbase_ops_secret CBASE_ORIGIN_SECRET "$env_file" "$origin_live" || exit 1
if [ "$origin_live" = no ] && [ "${CBASE_ORIGIN_ENFORCED:-yes}" = yes ] && [ "$stack_exists" = yes ]; then
  die "the stack does not send X-Cbase-Origin yet: deploy once with CBASE_ORIGIN_ENFORCED=no, wait for the distribution to deploy, then again"
fi
secrets_prefix="${CBASE_SECRETS_PREFIX:-/cbase/}"
# The live JWT secret: SSM's, else (a stack from before SSM) the function's environment.
ssm_value() {
  aws ssm get-parameter --region "$region" --name "$1" --with-decryption --query Parameter.Value --output text 2>/dev/null || true
}
if [ "$stack_exists" = yes ]; then
  live_jwt="$(ssm_value "${secrets_prefix}jwt-secret")"
  if [ -z "$live_jwt" ] || [ "$live_jwt" = None ]; then
    live_jwt="$(aws lambda get-function-configuration --region "$region" --function-name "${CBASE_FUNCTION_NAME:-cbase}" \
      --query 'Environment.Variables.CBASE_JWT_SECRET' --output text 2>/dev/null || true)"
  fi
  if [ -n "$live_jwt" ] && [ "$live_jwt" != None ] && [ "$live_jwt" != "$CBASE_JWT_SECRET" ]; then
    die "CBASE_JWT_SECRET in $env_file is not the live one: deploying it would sign every user out (RUNBOOK, \"The ops settings file\")"
  fi
fi
# The two secrets into SSM, as SecureStrings, before the stack points the
# function at them. Through a 0600 file, so no value is on a command line.
put_secret() {
  local name="$1" value="$2" current tmp
  current="$(ssm_value "$name")"
  [ "$current" = "$value" ] && return 0
  tmp="$(umask 077; mktemp)"
  jq -n --arg n "$name" --arg v "$value" '{Name: $n, Value: $v, Type: "SecureString", Overwrite: true, Tier: "Standard"}' > "$tmp"
  aws ssm put-parameter --region "$region" --cli-input-json "file://$tmp" >/dev/null || { rm -f "$tmp"; die "could not write $name"; }
  rm -f "$tmp"
  echo "wrote $name (SecureString)"
}
put_secret "${secrets_prefix}jwt-secret" "$CBASE_JWT_SECRET"
put_secret "${secrets_prefix}admin-token" "$CBASE_ADMIN_TOKEN"

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
# The distribution's own id goes back in as a parameter (CBASE_CF_DISTRIBUTION,
# the edge cache's invalidations; cbase.yaml DistributionId says why it cannot
# be a reference). Empty for a stack that has none yet.
dist_id=""
if [ "$stack_exists" = yes ]; then
  dist_id="$(aws cloudformation describe-stack-resource --region "$region" --stack-name "$stack" \
    --logical-resource-id Distribution --query StackResourceDetail.PhysicalResourceId --output text 2>/dev/null || true)"
  [ "$dist_id" = None ] && dist_id=""
fi
echo "distribution: ${dist_id:-(none yet)}"

# Realtime over IoT Core (cbase.yaml IotEndpoint): the account's data endpoint
# in this region, which only the IoT API names (CloudFormation cannot).
iot_endpoint=""
if [ "${CBASE_IOT:-yes}" = yes ]; then
  iot_endpoint="$(aws iot describe-endpoint --region "$region" --endpoint-type iot:Data-ATS \
    --query endpointAddress --output text)" || die "could not read the IoT Core endpoint (CBASE_IOT=no to deploy without it)"
fi
echo "iot endpoint: ${iot_endpoint:-(none)}"

# The change set is made, read, and only then executed: a change that would
# REPLACE a resource (the table, the distribution, the domain's alias) stops
# the run unless CBASE_ALLOW_REPLACE=1. CloudFormation's own deploy would
# execute it unseen.
aws cloudformation deploy \
  --region "$region" \
  --stack-name "$stack" \
  --template-file "$here/ops/cbase.yaml" \
  --s3-bucket "$bucket" --s3-prefix templates \
  --capabilities CAPABILITY_IAM CAPABILITY_NAMED_IAM \
  --no-fail-on-empty-changeset \
  --no-execute-changeset \
  --parameter-overrides \
    "CodeBucket=$bucket" \
    "CodeKey=$key" \
    "SecretsPrefix=$secrets_prefix" \
    "OriginSecret=$CBASE_ORIGIN_SECRET" \
    "OriginEnforced=${CBASE_ORIGIN_ENFORCED:-yes}" \
    "AlarmEmail=${CBASE_ALARM_EMAIL:-}" \
    "DomainName=$domain" \
    "CertificateArn=$cert" \
    "AllowedOrigins=$origins" \
    "FunctionName=$function_name" \
    "TableName=$table" \
    "DistributionId=$dist_id" \
    "IotEndpoint=$iot_endpoint" | tee "$here/build/deploy-changeset.txt"
changeset="$(grep -o 'arn:aws:cloudformation:[^ ]*changeSet/[^ ]*' "$here/build/deploy-changeset.txt" | head -1 || true)"
if [ -n "$changeset" ]; then
  aws cloudformation describe-change-set --region "$region" --change-set-name "$changeset" \
    --query 'Changes[].ResourceChange.[Action,LogicalResourceId,ResourceType,Replacement]' --output table
  # What may be recreated: ops/changeset_replacements.jq (a "Conditional" on
  # an attribute of a resource that is only modified is not).
  aws cloudformation describe-change-set --region "$region" --change-set-name "$changeset" --output json \
    > "$here/build/deploy-changeset.json"
  jq -r '.Changes[].ResourceChange | select(.Replacement == "Conditional")
         | "  conditional: \(.LogicalResourceId) <- \([.Details[] | select(.Target.RequiresRecreation != "Never")
           | "\(.ChangeSource) \(.CausingEntity // .Target.Name)"] | join(", "))"' "$here/build/deploy-changeset.json"
  replacing="$(jq -r -f "$here/ops/changeset_replacements.jq" "$here/build/deploy-changeset.json")"
  if [ -n "$replacing" ] && [ "$replacing" != None ] && [ "${CBASE_ALLOW_REPLACE:-}" != 1 ]; then
    aws cloudformation delete-change-set --region "$region" --change-set-name "$changeset" >/dev/null || true
    die "the change set replaces: $replacing. Read it, then rerun with CBASE_ALLOW_REPLACE=1 if that is meant"
  fi
  aws cloudformation execute-change-set --region "$region" --change-set-name "$changeset"
  if [ "$stack_exists" = yes ]; then
    aws cloudformation wait stack-update-complete --region "$region" --stack-name "$stack"
  else
    aws cloudformation wait stack-create-complete --region "$region" --stack-name "$stack"
  fi
  echo "stack $stack updated"
else
  echo "no changes to the stack"
fi

# ---- 6. what the stack made -------------------------------------------------------
output() {
  aws cloudformation describe-stacks --region "$region" --stack-name "$stack" \
    --query "Stacks[0].Outputs[?OutputKey=='$1'].OutputValue" --output text
}
aws cloudformation describe-stacks --region "$region" --stack-name "$stack" \
  --query 'Stacks[0].Outputs[].[OutputKey,OutputValue]' --output table
cat <<MSG

Next (RUNBOOK.md): point the DNS CNAME for $domain at $(output DistributionDomain),
then build the web app with NEXT_PUBLIC_BACKEND_URL=$(output PublicUrl).
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
  # The import tool invalidates what it changed at the edge (app/edge.h).
  export CBASE_CF_DISTRIBUTION="$(output DistributionId)"
  CBASE_STORE=dynamo CBASE_TABLE="$stack_table" "$here/build/cbase-import" --seed-bots
  # The leaderboard's sparse, rating-ordered index key: rows from before it
  # move once; afterwards this rewrites nothing.
  CBASE_STORE=dynamo CBASE_TABLE="$stack_table" "$here/build/cbase-import" --reindex-ratings
  if [ -n "$import_dir" ]; then
    [ -d "$import_dir" ] || die "--import: $import_dir is not a directory"
    echo "importing $import_dir into DynamoDB table $stack_table"
    CBASE_STORE=dynamo CBASE_TABLE="$stack_table" "$here/build/cbase-import" "$import_dir"
  fi
)
if [ -n "$import_dir" ]; then
  echo "import done. The export holds password hashes: rm -rf $import_dir"
fi
