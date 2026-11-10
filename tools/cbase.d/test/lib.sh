# shellcheck shell=bash
# lib.sh - what cbase_test.sh and cbase_vm_test.sh share: the fake AWS (fake_aws.c, built here) the
# real AWS CLI is pointed at, the dummy credentials, and the helpers that run the tool as an app
# does. No call reaches AWS: the endpoint is the fake for every service, and the credentials are a
# key AWS has never issued. Sourced, with set -euo pipefail.
set -euo pipefail
export LC_ALL=C
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
tool="$root/tools/cbase"
t="$(mktemp -d "${TMPDIR:-/tmp}/cbase-test.XXXXXX")"
FAKE_PID=""
stop_fake() {
  if [ -n "$FAKE_PID" ]; then kill "$FAKE_PID" 2> /dev/null || true; wait "$FAKE_PID" 2> /dev/null || true; fi
  FAKE_PID=""
}
trap 'stop_fake; rm -rf "$t"' EXIT
fail() { echo "cbase_test: FAIL: $*" >&2; exit 1; }
pass=0
ok() { pass=$((pass + 1)); }

command -v aws > /dev/null 2>&1 || fail "the AWS CLI (aws) is not on PATH: the tests drive the real CLI against a local fake"
${CC:-cc} -std=gnu11 -D_GNU_SOURCE -O2 -Wall -Wextra -Werror -pthread "$root/tools/cbase.d/test/fake_aws.c" -o "$t/fake_aws"

ACCOUNT=111122223333
BUCKET="cbase-state-$ACCOUNT"
# The test's AWS: nothing of the person's (no profile, no config, no metadata service), dummy keys.
unset AWS_PROFILE AWS_DEFAULT_PROFILE AWS_SESSION_TOKEN AWS_ENDPOINT_URL_S3 AWS_ENDPOINT_URL_STS \
      AWS_ENDPOINT_URL_RESOURCE_GROUPS_TAGGING_API AWS_DEFAULT_REGION AWS_CA_BUNDLE
export AWS_ACCESS_KEY_ID=AKIACBASEFAKE0000000 AWS_SECRET_ACCESS_KEY=cbase-test-dummy-secret-key \
       AWS_REGION=us-east-1 AWS_CONFIG_FILE="$t/no-aws-config" AWS_SHARED_CREDENTIALS_FILE="$t/no-aws-credentials" \
       AWS_EC2_METADATA_DISABLED=true AWS_PAGER="" AWS_MAX_ATTEMPTS=1 AWS_ENDPOINT_URL=http://127.0.0.1:1

# start_fake ARGS...: a fresh fake (no objects), its log at $t/log, AWS_ENDPOINT_URL at it.
start_fake() {
  stop_fake
  # Every call the last fake answered was signed with the dummy key: none was refused as unsigned
  # (a call that had found other credentials).
  if [ -f "$t/log" ] && grep -q unsigned "$t/log"; then fail "a call reached the fake without the test's credentials"; fi
  rm -f "$t/port" "$t/log"
  "$t/fake_aws" --port-file "$t/port" --log "$t/log" --account "$ACCOUNT" "$@" &
  FAKE_PID=$!
  local n=0
  until [ -s "$t/port" ] || [ $n -gt 100 ]; do sleep 0.05; n=$((n + 1)); done
  [ -s "$t/port" ] || fail "the fake did not start"
  AWS_ENDPOINT_URL="http://127.0.0.1:$(cat "$t/port")"
  export AWS_ENDPOINT_URL
}
VM_G1="cbase:vm-group g1 ec2:instance arn:aws:ec2:us-east-1:$ACCOUNT:instance/i-0g1"
VM_G2="cbase:vm-group g2 ec2:instance arn:aws:ec2:us-east-1:$ACCOUNT:instance/i-0g2"
ST_G1="cbase:static-group g1 s3 arn:aws:s3:::g1-site-bucket"
# shellcheck disable=SC2034 # cbase_test.sh reads these three
CF_OPTIMIZED=658327ea-f89d-4fab-a63d-7e88639e58f6
# shellcheck disable=SC2034
CF_DISABLED=4135ea2d-6df8-44a3-9df3-4b5a84be39ad
# shellcheck disable=SC2034
everything=(--bucket "$BUCKET" --resource "$VM_G1" --resource "$VM_G2" --resource "$ST_G1")

# run DIR ARGS...: the tool from DIR (an app's repository), its stdin /dev/null (never a terminal
# that could answer a question), its stdout in $t/out, stderr in $t/err, status in $rc.
rc=0
run() {
  local dir="$1"; shift
  rc=0
  (cd "$dir" && "$tool" "$@") < /dev/null > "$t/out" 2> "$t/err" || rc=$?
}
expect_rc() { [ "$rc" = "$1" ] || { cat "$t/out" "$t/err" >&2; fail "$2: status $rc, not $1"; }; }
err_has() { grep -Fq -- "$1" "$t/err" || { cat "$t/err" >&2; fail "$2: stderr lacks [$1]"; }; }
out_has() { grep -Fq -- "$1" "$t/out" || { cat "$t/out" >&2; fail "$2: stdout lacks [$1]"; }; }
log_count() { grep -cF -- "$1" "$t/log" || true; }
app() {   # app NAME LINES...: an app repository with cbase.conf of LINES
  local d="$t/apps/$1"; shift
  mkdir -p "$d"
  printf '%s\n' "$@" > "$d/cbase.conf"
  printf '%s' "$d"
}
state_of() {   # state_of KIND GROUP: the state file as the fake holds it
  aws s3api get-object --bucket "$BUCKET" --key "$1/$2.txt" "$t/state" > /dev/null 2>&1 || { echo "(none)"; return; }
  cat "$t/state"
}
fake_set() { curl -fsS -X POST "$AWS_ENDPOINT_URL/_fake/set?${1// /%20}" > /dev/null; }
# A reader that stops at its first match (`since "$m" | grep -q X || fail`) closes the pipe while the
# writer may still be writing: the writer dies of SIGPIPE (141), and under pipefail the pipeline
# failed though grep matched. template_of and since are the writers whose output runs past one
# write (a log with SSM's base64 scripts, a template), so a closed reader is not their failure.
template_of() {
  curl -fsS "$AWS_ENDPOINT_URL/_fake/template?stack=$1" -o "$t/template_of" || return
  cat "$t/template_of" || [ $? -eq 141 ]
}
mark() { wc -l < "$t/log" | tr -d ' '; }    # the log's length now: what a run adds is after it
since() { sed -n "$(( $1 + 1 )),\$p" "$t/log" || [ $? -eq 141 ]; }   # since MARK: the log lines after MARK
