# shellcheck shell=bash
# aws.sh - every AWS call the deploy tool makes, each one AWS CLI call (README.md "./cbase deploy
# tool"), and what every call shares: its error, the mask on what AWS says, and the bounded wait.
# Sourced by tools/cbase.
#
# CREDENTIALS are the AWS CLI's own chain and nothing of ours: AWS_PROFILE and ~/.aws (an SSO or
# `aws login` session), or the environment's keys (CI's OIDC role sets AWS_ACCESS_KEY_ID,
# AWS_SECRET_ACCESS_KEY and AWS_SESSION_TOKEN). There is no ~/.cbase. The tool never reads,
# prints or passes a credential: it runs `aws`, which does.
#
# OUTPUT is always `--query ... --output text`: this code never parses JSON or XML. A failed call's
# stderr is kept in a file and classified by the error code the CLI prints ("(NoSuchKey)"); it is
# shown to the person only for a failure the tool does not know, masked (aws_show). Never run this
# with `set -x`.
#
# ENDPOINTS: AWS_ENDPOINT_URL (or the CLI's per-service AWS_ENDPOINT_URL_S3, _STS,
# _RESOURCE_GROUPS_TAGGING_API) points every call elsewhere, as the CLI reads it; the tests point
# them at a local fake (tools/cbase.d/test/fake_aws.c).

AWS_ERR=""       # the last failed call's stderr file
AWS_ACCOUNT=""   # the twelve-digit account the credentials are in
POLL_SECONDS="${CBASE_POLL_SECONDS:-5}"   # between two reads of a bounded wait (aws_poll)

# aws_init: the CLI found, and AWS_ACCOUNT asked of STS.
aws_init() {
  command -v aws > /dev/null 2>&1 \
    || { echo "cbase: the AWS CLI (aws) is not on PATH: install AWS CLI v2 (https://docs.aws.amazon.com/cli/)" >&2; exit 1; }
  AWS_ERR="$(mktemp "${TMPDIR:-/tmp}/cbase-aws.XXXXXX")"
  CB_CLEANUP+=("$AWS_ERR")
  export AWS_PAGER=""
  AWS_ACCOUNT="$(aws_call sts get-caller-identity --query Account --output text)" || aws_fail "sts get-caller-identity"
  [[ "$AWS_ACCOUNT" =~ ^[0-9]{12}$ ]] || { echo "cbase: STS named the account \"${AWS_ACCOUNT:0:40}\", which is not twelve digits" >&2; exit 1; }
}

# aws_call ARGS...: the CLI's stdout on success; on failure its status, with its stderr in $AWS_ERR.
# A call AWS throttled (aws_throttled) is tried again, AWS_ATTEMPTS times in all, attempt k waiting
# 2^(k-1) s (1, 2, 4, 8) plus the app's stagger (aws_stagger_ms): several ./cbase runs started at
# once share one account's limits, and the `aws login` token refresh (CreateOAuth2Token) answers
# 429 to them. Any other failure returns at once, as the last attempt's does.
AWS_ATTEMPTS=5
aws_call() {
  local k=1 rc ms
  while :; do
    rc=0
    aws --no-cli-pager "$@" 2> "$AWS_ERR" || rc=$?
    [ "$rc" -ne 0 ] || return 0
    { [ "$k" -lt "$AWS_ATTEMPTS" ] && aws_throttled; } || return "$rc"
    ms=$(( (1000 << (k - 1)) + $(aws_stagger_ms "${CONF_APP:-}") ))
    sleep "$(printf '%d.%03d' $((ms / 1000)) $((ms % 1000)))"
    k=$((k + 1))
  done
}

# aws_throttled: the last failure was AWS's throttling: a throttling error code (Throttling,
# ThrottlingException, TooManyRequestsException, RequestLimitExceeded), or HTTP 429 named as a
# status (a bare 429 could be part of a request ID).
aws_throttled() {
  grep -qiE 'Throttling|TooManyRequests|Too Many Requests|RequestLimitExceeded|(HTTP|status|code)[^0-9A-Za-z]{0,8}429([^0-9]|$)|\(429\)' "$AWS_ERR"
}

# aws_stagger_ms NAME: 0-99, NAME's cksum mod 100. Deterministic, not random (state.sh says why):
# two apps retrying at once do not retry in step, and the same run always waits the same.
aws_stagger_ms() {
  printf '%d' $(( $(printf '%s' "$1" | cksum | cut -d' ' -f1) % 100 ))
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
    echo "cbase: $what: AWS refused it (AccessDenied) for ${AWS_PROFILE:+profile }${AWS_PROFILE:-these credentials}. The deploy role needs the permission." >&2
  else
    echo "cbase: $what failed:" >&2
    aws_show 5
  fi
  exit 1
}

# THE MASK, the one owner of what of AWS's text reaches the person. The CLI never prints a
# credential; what could carry a secret's value is a line that names an origin's custom headers
# (OriginCustomHeaders, HeaderValue) or a parameter's value (ParameterValue), or one that holds a
# secret this run holds (AWS_MASK_VALUES: the origin secret, of letters, digits, _ and - alone, as
# vm.sh checks, so each is a literal sed pattern). aws_mask replaces each such line. A change set's
# and a drift's values are the other way out, and aws_secret_filter keeps those out of what the CLI
# answers at all.
AWS_MASK_VALUES=()
aws_mask() {
  local v e=(-e '/OriginCustomHeaders|HeaderValue|ParameterValue/I s/.*/(a line naming an origin header or a parameter value, not shown)/')
  for v in ${AWS_MASK_VALUES[@]+"${AWS_MASK_VALUES[@]}"}; do
    e+=(-e "/$v/ s/.*/(a line naming an origin header or a parameter value, not shown)/")
  done
  sed -E "${e[@]}"
}

# aws_show LINES: the first LINES of $AWS_ERR, masked and indented, on stderr.
aws_show() {
  aws_mask < "$AWS_ERR" | head -"$1" | sed 's/^/  /' >&2
}

# AWS_PLAIN_PARAMS: the stack parameters whose values may be printed; any other one's are not.
AWS_PLAIN_PARAMS="VmOrigin VmPort InstanceType ImageId CloudFrontPrefixList ArtifactBucket"

# aws_secret_filter PATH BEFORE AFTER [CAUSE]: the JMESPath filter that holds a detail (a change
# set's Target, a drift's property difference) secret: its path or either value names an origin's
# custom headers, or (with CAUSE, a change set) a parameter caused it that is not plain. stack.sh's
# queries take their one resource with a pipe ("Changes[?...] | [0].Details[?filter]"): a filter
# inside a projection's projection reads the wrong node in the CLI's JMESPath, every field null, and
# "not secret" would hold for everything. The plain query also asks for a path that is there, so a
# filter reading the wrong node prints nothing rather than everything.
aws_secret_filter() {
  local p="$1" b="$2" a="$3" f="" w q
  for w in OriginCustomHeaders HeaderValue; do
    f="$f${f:+ || }contains($p || '', '$w') || contains($b || '', '$w') || contains($a || '', '$w')"
  done
  if [ -n "${4:-}" ]; then
    f="$f || (ChangeSource == 'ParameterReference'"
    for q in $AWS_PLAIN_PARAMS; do f="$f && CausingEntity != '$q'"; done
    f="$f)"
  fi
  printf '%s' "$f"
}

# aws_poll TRIES FN: THE BOUNDED WAIT, the one loop each of the tool's own waits is (macOS has no
# `timeout`). FN reads once and is true when the wait is over; it runs at most TRIES times,
# POLL_SECONDS apart, and aws_poll is false when it never was. FN stops the tool itself on a failed
# call: errexit does not hold inside it.
aws_poll() {
  local n
  for ((n = 1; ; n++)); do
    "$2" && return 0
    [ "$n" -lt "$1" ] || return 1
    sleep "$POLL_SECONDS"
  done
}

# aws_region: the region the CLI calls (AWS_REGION, AWS_DEFAULT_REGION, the profile's), for a
# message. aws_region_required: the same, or the tool stops: a group's stack, its instance and its
# buckets are made there.
aws_region_get() {
  local r="${AWS_REGION:-${AWS_DEFAULT_REGION:-}}"
  [ -n "$r" ] || r="$(aws configure get region 2> /dev/null || true)"
  printf '%s' "$r"
}
aws_region() {
  local r
  r="$(aws_region_get)"
  printf '%s' "${r:-the configured region}"
}
aws_region_required() {
  local r
  r="$(aws_region_get)"
  [[ "$r" =~ ^[a-z]{2}(-[a-z]+)+-[0-9]$ ]] || { echo "cbase: no AWS region is set: set AWS_REGION (us-east-1), or the profile's region (aws configure set region us-east-1)" >&2; exit 1; }
  printf '%s' "$r"
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

aws_no_state_bucket() {
  echo "cbase: there is no state bucket s3://$1 in this account: ./cbase group-init makes it with the first group" >&2
  exit 1
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
    NoSuchBucket) aws_no_state_bucket "$bucket" ;;
  esac
  aws_fail "reading s3://$bucket/$key"
}

# aws_s3_put BUCKET KEY FILE IF: writes FILE under KEY only if IF holds, IF being the ETag the
# object must still have, or "*" for "only if there is none yet". Status 0 when written, 3 when the
# precondition failed (412 PreconditionFailed, 409 ConditionalRequestConflict when another
# conditional write to the key was in flight, or NoSuchKey for an If-Match on a key deleted since
# it was read): someone else wrote first, so re-read and retry.
aws_s3_put() {
  local bucket="$1" key="$2" file="$3" cond="$4" pre
  if [ "$cond" = "*" ]; then pre=(--if-none-match "*"); else pre=(--if-match "$cond"); fi
  if aws_call s3api put-object --bucket "$bucket" --key "$key" --body "$file" --content-type 'text/plain; charset=utf-8' \
       "${pre[@]}" --query ETag --output text > /dev/null; then
    return 0
  fi
  case "$(aws_code)" in
    PreconditionFailed|ConditionalRequestConflict|NoSuchKey) return 3 ;;
    NoSuchBucket) aws_no_state_bucket "$bucket" ;;
  esac
  aws_fail "writing s3://$bucket/$key"
}
