# shellcheck shell=bash
# aws.sh - every AWS call the deploy tool makes, each one AWS CLI call (README.md "./cbase deploy
# tool"). Sourced by tools/cbase.
#
# CREDENTIALS are the AWS CLI's own chain and nothing of ours: AWS_PROFILE and ~/.aws (an SSO or
# `aws login` session), or the environment's keys (CI's OIDC role sets AWS_ACCESS_KEY_ID,
# AWS_SECRET_ACCESS_KEY and AWS_SESSION_TOKEN). There is no ~/.cbase. The tool never reads,
# prints or passes a credential: it runs `aws`, which does.
#
# OUTPUT is always `--query ... --output text`: this code never parses JSON or XML. A failed call's
# stderr is kept in a file and classified by the error code the CLI prints ("(NoSuchKey)"); it is
# shown to the person only for a failure the tool does not know, and the CLI's errors carry no
# credential or header value. Never run this with `set -x`.
#
# ENDPOINTS: AWS_ENDPOINT_URL (or the CLI's per-service AWS_ENDPOINT_URL_S3, _STS,
# _RESOURCE_GROUPS_TAGGING_API) points every call elsewhere, as the CLI reads it; the tests point
# them at a local fake (tools/cbase.d/test/fake_aws.c).

AWS_ERR=""   # the last failed call's stderr file

aws_init() {
  command -v aws > /dev/null 2>&1 \
    || { echo "cbase: the AWS CLI (aws) is not on PATH: install AWS CLI v2 (https://docs.aws.amazon.com/cli/)" >&2; exit 1; }
  AWS_ERR="$(mktemp "${TMPDIR:-/tmp}/cbase-aws.XXXXXX")"
  CB_CLEANUP+=("$AWS_ERR")
  export AWS_PAGER=""
}

# aws_call ARGS...: the CLI's stdout on success; on failure its status, with its stderr in $AWS_ERR.
aws_call() {
  aws --no-cli-pager "$@" 2> "$AWS_ERR"
}

# aws_code: the error code of the last failure ("NoSuchKey"), or "".
aws_code() {
  sed -n 's/.*An error occurred (\([A-Za-z0-9.]*\)).*/\1/p' "$AWS_ERR" | head -1
}

# aws_fail WHAT: stops the tool on a failed call the caller does not handle, with what the person
# can do about it.
aws_fail() {
  local what="$1" code
  code="$(aws_code)"
  if grep -qE 'Unable to locate credentials|Token has expired|token.*expired|expired.*token|ExpiredToken|The SSO session|InvalidClientTokenId|refresh failed|no valid credential' "$AWS_ERR"; then
    echo "cbase: $what: no usable AWS credentials. Run aws login (or aws sso login), or set AWS_PROFILE." >&2
  elif grep -q 'Unknown options: --if-' "$AWS_ERR"; then
    echo "cbase: $what: this AWS CLI is too old for S3 conditional writes (put-object --if-match): update AWS CLI v2." >&2
  elif [ "$code" = AccessDenied ] || [ "$code" = AccessDeniedException ] || [ "$code" = UnauthorizedOperation ]; then
    echo "cbase: $what: AWS refused it (AccessDenied) for $(aws_whoami_hint). The deploy role needs the permission." >&2
  else
    echo "cbase: $what failed:" >&2
    aws_mask < "$AWS_ERR" | sed 's/^/  /' | head -5 >&2
  fi
  exit 1
}

# aws_mask: AWS's text (an error, a stack event's reason) on stdin, with every line that could carry
# a secret's value replaced: one that names an origin's custom headers (OriginCustomHeaders,
# HeaderValue), or a parameter's value (ParameterValue). The CLI never prints a credential, and
# this is the second guard of stack.sh's: its --query filters keep a change set's and a drift's
# secret values out of what the CLI answers; an error's message is free text, so it is masked here.
# AWS_MASK_VALUES: secret values this run holds (the origin secret), each of letters, digits, _
# and - alone (vm.sh checks), so each is a literal sed pattern. A line holding one is masked too.
AWS_MASK_VALUES=()
aws_mask() {
  local v e=(-e '/OriginCustomHeaders|HeaderValue|ParameterValue/I s/.*/(a line naming an origin header or a parameter value, not shown)/')
  for v in ${AWS_MASK_VALUES[@]+"${AWS_MASK_VALUES[@]}"}; do
    e+=(-e "/$v/ s/.*/(a line naming an origin header or a parameter value, not shown)/")
  done
  sed -E "${e[@]}"
}

aws_whoami_hint() {
  if [ -n "${AWS_PROFILE:-}" ]; then printf 'profile %s' "$AWS_PROFILE"; else printf 'these credentials'; fi
}

aws_region() {
  local r="${AWS_REGION:-${AWS_DEFAULT_REGION:-}}"
  [ -n "$r" ] || r="$(aws configure get region 2> /dev/null || true)"
  printf '%s' "${r:-the configured region}"
}

# aws_region_required: the region the CLI calls (AWS_REGION, AWS_DEFAULT_REGION, the profile's), or
# the tool stops: a group's stack, its instance and its buckets are made there.
aws_region_required() {
  local r="${AWS_REGION:-${AWS_DEFAULT_REGION:-}}"
  [ -n "$r" ] || r="$(aws configure get region 2> /dev/null || true)"
  [[ "$r" =~ ^[a-z]{2}(-[a-z]+)+-[0-9]$ ]] || { echo "cbase: no AWS region is set: set AWS_REGION (us-east-1), or the profile's region (aws configure set region us-east-1)" >&2; exit 1; }
  printf '%s' "$r"
}

# aws_account_load: AWS_ACCOUNT, the twelve-digit account the credentials are in (STS
# GetCallerIdentity), asked once.
AWS_ACCOUNT=""
aws_account_load() {
  [ -z "$AWS_ACCOUNT" ] || return 0
  AWS_ACCOUNT="$(aws_call sts get-caller-identity --query Account --output text)" || aws_fail "sts get-caller-identity"
  [[ "$AWS_ACCOUNT" =~ ^[0-9]{12}$ ]] || { echo "cbase: STS named the account \"${AWS_ACCOUNT:0:40}\", which is not twelve digits" >&2; exit 1; }
}

# aws_group_find KIND NAME: the group's resource, found by its tag (the Resource Groups Tagging
# API): a vm group is the one EC2 instance tagged cbase:vm-group=NAME, and prints its instance id;
# a static group is the one bucket tagged cbase:static-group=NAME, and prints the bucket's name.
# A missing group stops with the group-init command that makes it; nothing is ever created here.
aws_group_find() {
  local kind="$1" name="$2" tag type text n arns=()
  if [ "$kind" = vm ]; then tag=cbase:vm-group; type=ec2:instance; else tag=cbase:static-group; type=s3; fi
  text="$(aws_call resourcegroupstaggingapi get-resources --tag-filters "Key=$tag,Values=$name" \
            --resource-type-filters "$type" --query 'ResourceTagMappingList[].ResourceARN' --output text)" \
    || aws_fail "finding $kind group $name (tag:GetResources)"
  read -r -a arns <<< "$text"
  n="${#arns[@]}"   # the CLI's text output: the ARNs on one line, tab-separated
  if [ "$n" -eq 0 ]; then
    echo "cbase: there is no $kind group named $name (nothing tagged $tag=$name in $(aws_region)). Create it with: ./cbase group-init $kind $name" >&2
    exit 1
  fi
  if [ "$n" -gt 1 ]; then
    echo "cbase: $n resources are tagged $tag=$name, and a $kind group is one $([ "$kind" = vm ] && echo instance || echo bucket): ${arns[*]}" >&2
    exit 1
  fi
  case "$kind:${arns[0]}" in
    vm:arn:aws*:ec2:*:instance/i-*) printf '%s\n' "${arns[0]##*/}" ;;
    static:arn:aws*:s3:::*) printf '%s\n' "${arns[0]##*:::}" ;;
    *) echo "cbase: $tag=$name is on ${arns[0]}, which is not a $type" >&2; exit 1 ;;
  esac
}

# aws_s3_get BUCKET KEY FILE: the object into FILE and its ETag on stdout (status 0), or status 3
# when there is no such key. A missing bucket stops the tool.
aws_s3_get() {
  local bucket="$1" key="$2" file="$3" etag
  if etag="$(aws_call s3api get-object --bucket "$bucket" --key "$key" "$file" --query ETag --output text)"; then
    printf '%s\n' "$etag"
    return 0
  fi
  case "$(aws_code)" in
    NoSuchKey) return 3 ;;
    NoSuchBucket)
      echo "cbase: there is no state bucket s3://$bucket in this account: ./cbase group-init makes it with the first group" >&2
      exit 1 ;;
  esac
  aws_fail "reading s3://$bucket/$key"
}

# aws_s3_put BUCKET KEY FILE IF: writes FILE under KEY only if IF holds, IF being the ETag the
# object must still have, or "*" for "only if there is none yet". Status 0 when written, 3 when the
# precondition failed (412 PreconditionFailed, or 409 ConditionalRequestConflict when another
# conditional write to the key was in flight): someone else wrote first, so re-read and retry.
aws_s3_put() {
  local bucket="$1" key="$2" file="$3" cond="$4" pre
  if [ "$cond" = "*" ]; then pre=(--if-none-match "*"); else pre=(--if-match "$cond"); fi
  if aws_call s3api put-object --bucket "$bucket" --key "$key" --body "$file" --content-type 'text/plain; charset=utf-8' \
       "${pre[@]}" --query ETag --output text > /dev/null; then
    return 0
  fi
  case "$(aws_code)" in
    PreconditionFailed|ConditionalRequestConflict) return 3 ;;
    NoSuchKey) return 3 ;;   # If-Match on a key deleted since it was read: re-read
    NoSuchBucket)
      echo "cbase: there is no state bucket s3://$bucket in this account: ./cbase group-init makes it with the first group" >&2
      exit 1 ;;
  esac
  aws_fail "writing s3://$bucket/$key"
}
