#!/bin/bash
# aws_resident_test.sh - the resident AWS CLI (aws_resident.py and aws_client.c, CBASE_TEST_AWS=resident)
# is the CLI a fresh `aws` is: a fixed list of calls runs twice against the recorded local fake
# (fake_aws.c), through the real per-process `aws` and through the resident client, and stdout,
# stderr and the exit code must match byte for byte. Then the server is killed, before a call and in
# the middle of one, and the client must say so and not hang.
#
#   bash tools/cbase.d/test/aws_resident_test.sh
#
# Needs the Python awscli (lib.sh refuses a frozen build). No call reaches AWS.
set -euo pipefail
wanted_resident="${CBASE_TEST_AWS:-}"
unset CBASE_TEST_AWS   # this test starts its own server, and keeps the per-process aws on PATH
# shellcheck source=tools/cbase.d/test/lib.sh
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

# A frozen aws (the zip installer's, on some runners) cannot be loaded by Python, so the resident CLI
# cannot run there. Where resident mode was asked for (CBASE_TEST_AWS=resident, the macOS deploy tool
# job) that is a failure, and resident_start says so; elsewhere (make check) the test is skipped, loudly.
if [ "$wanted_resident" != resident ] && ! resident_python > /dev/null; then
  CBASE_TEST_END=1
  echo "aws_resident_test: SKIPPED: $(command -v aws) is not a Python awscli (set CBASE_TEST_AWS=resident to require it)"
  exit 0
fi

REAL_AWS="$(command -v aws)"
start_fake --bucket "$BUCKET"
resident_start
RES_AWS="$RESIDENT_BIN/aws"
GOOD_ENDPOINT="$AWS_ENDPOINT_URL"
mkdir -p "$t/cwd-real" "$t/cwd-res" "$t/cwd-other"
printf 'relative body\n' > "$t/cwd-real/rel.txt"
printf 'relative body\n' > "$t/cwd-res/rel.txt"
"$REAL_AWS" s3api put-object --bucket "$BUCKET" --key dir/one.txt --body "$t/cwd-real/rel.txt" > /dev/null
"$REAL_AWS" s3api put-object --bucket "$BUCKET" --key dir/two.txt --body "$t/cwd-real/rel.txt" > /dev/null

# call WHICH CWD ENV... -- ARGS...: one call as WHICH (real or res) from CWD with ENV (NAME=VALUE...)
# added, its stdout, stderr and status in $t/WHICH.out, .err and .rc.
call() {
  local which="$1" cwd="$2" bin st=0; shift 2
  local envs=()
  while [ "$1" != -- ]; do envs+=("$1"); shift; done
  shift
  if [ "$which" = real ]; then bin="$REAL_AWS"; else bin="$RES_AWS"; fi
  (cd "$cwd" && env ${envs[@]+"${envs[@]}"} "$bin" "$@") < /dev/null > "$t/$which.out" 2> "$t/$which.err" || st=$?
  echo "$st" > "$t/$which.rc"
}
# same NAME ENV... -- ARGS...: the call through both, from each one's own directory (so a relative
# path is the same text), and the three results equal.
same() {
  local name="$1"; shift
  call real "$t/cwd-real" "$@"
  call res "$t/cwd-res" "$@"
  cmp -s "$t/real.rc" "$t/res.rc" || fail "$name: exit status $(cat "$t/real.rc") through aws, $(cat "$t/res.rc") resident"
  cmp -s "$t/real.out" "$t/res.out" || { diff "$t/real.out" "$t/res.out" >&2 || true; fail "$name: stdout differs"; }
  cmp -s "$t/real.err" "$t/res.err" || { diff "$t/real.err" "$t/res.err" >&2 || true; fail "$name: stderr differs"; }
  ok
}
nonzero() { [ "$(cat "$t/real.rc")" != 0 ] || fail "$1: the call was expected to fail, and did not"; }
nonempty() { [ -s "$t/real.$2" ] || fail "$1: the $2 was expected, and is empty"; }

same "sts" -- sts get-caller-identity --query Account --output text
grep -Fq "$ACCOUNT" "$t/real.out" || fail "sts: the account is not in the answer"
same "a missing key" -- s3api get-object --bucket "$BUCKET" --key no/such/key "$t/missing.out"
nonzero "a missing key"; nonempty "a missing key" err
same "list --query, text" -- s3api list-objects-v2 --bucket "$BUCKET" --prefix dir/ --query 'Contents[].Key' --output text
nonempty "list --query, text" out
same "s3 cp up, a relative path" -- s3 cp --no-progress rel.txt "s3://$BUCKET/up/rel.txt"
nonempty "s3 cp up" out
same "s3 cp down, a relative path" -- s3 cp --no-progress "s3://$BUCKET/up/rel.txt" got.txt
cmp -s "$t/cwd-real/got.txt" "$t/cwd-res/got.txt" || fail "s3 cp down: the file each call wrote differs"
cmp -s "$t/cwd-real/got.txt" "$t/cwd-real/rel.txt" || fail "s3 cp down: the file is not the one uploaded"
# A call from a directory other than the test's, with a relative --body, the other way round.
same "s3 cp from another cwd" -- s3 cp --no-progress "s3://$BUCKET/dir/one.txt" ./other-got.txt
# The environment is the caller's, whole, for each call: a dead endpoint, then the good one again.
same "an environment change: a dead endpoint" AWS_ENDPOINT_URL=http://127.0.0.1:9 -- sts get-caller-identity --query Account --output text
nonzero "a dead endpoint"; nonempty "a dead endpoint" err
same "after it, the good environment" AWS_ENDPOINT_URL="$GOOD_ENDPOINT" -- sts get-caller-identity --query Account --output text
grep -Fq "$ACCOUNT" "$t/res.out" || fail "after the dead endpoint: the resident call did not reach the fake again"
# A variable the server never had, and one set empty, reach the call (a region in the answer's text).
same "a region only the caller set" AWS_REGION=eu-west-1 -- s3api get-bucket-location --bucket "$BUCKET"
same "an option error" -- s3api get-object --bucket
nonzero "an option error"

# ---- a signal to the client reaches the CLI's child ------------------------------------------
# TERM and INT sent to the client (a test's `kill`, a terminal's Ctrl-C to the process group the client
# is in and the server's child is not) are forwarded to the child, which ends by it, and the client then
# ends by the same signal. A call is held in the fake (gather n=2: the next GetObject waits for a second
# one) so the child is alive when the signal comes; the second GetObject lets the fake go. The client
# ending by the signal proves little (a client that forwards nothing dies of TERM too): the child's end
# is the check (the server has no child left).
children() { pgrep -P "$RESIDENT_PID" 2> /dev/null | wc -l | tr -d ' '; }
held_signal() {
  local sig="$1" num="$2" held st=0 n=0
  curl -fsS -X POST "$AWS_ENDPOINT_URL/_fake/gather?n=2" > /dev/null
  # perl: a background job of this script has INT ignored, and an ignored signal stays ignored
  perl -e '$SIG{INT} = "DEFAULT"; $SIG{TERM} = "DEFAULT"; exec @ARGV' "$RES_AWS" s3api get-object --bucket "$BUCKET" --key dir/one.txt "$t/sig.body" < /dev/null > "$t/sig.out" 2> "$t/sig.err" &
  held=$!
  until [ "$(children)" = 1 ] || [ $n -ge 50 ]; do sleep 0.1; n=$((n + 1)); done
  [ "$(children)" = 1 ] || fail "$sig: the held call has no child in the server (the test would prove nothing)"
  kill "-$sig" "$held"
  wait "$held" 2> /dev/null || st=$?
  [ "$st" = $((128 + num)) ] || fail "$sig: the client ended with status $st, not $((128 + num))"
  n=0
  until [ "$(children)" = 0 ] || [ $n -ge 30 ]; do sleep 0.1; n=$((n + 1)); done
  [ "$(children)" = 0 ] || fail "$sig: the CLI's child is still running 3 s after its client got the signal (not forwarded)"
  "$REAL_AWS" s3api get-object --bucket "$BUCKET" --key dir/two.txt "$t/release.body" > /dev/null   # lets the fake go
  ok
}
held_signal TERM 15
held_signal INT 2

# ---- the server is gone ----------------------------------------------------------------------
# bounded_client TENTHS OUT ERR ARGS...: the resident client in the background, waited for at most
# TENTHS tenths of a second ($t/bounded.rc is its status; $t/bounded.hung is 1 when it was still there).
bounded_client() {
  local tenths="$1" out="$2" err="$3" pid n=0; shift 3
  rm -f "$t/bounded.rc" "$t/bounded.hung"
  "$RES_AWS" "$@" < /dev/null > "$out" 2> "$err" &
  pid=$!
  until ! kill -0 "$pid" 2> /dev/null || [ $n -ge "$tenths" ]; do sleep 0.1; n=$((n + 1)); done
  if kill -0 "$pid" 2> /dev/null; then
    echo 1 > "$t/bounded.hung"; kill -9 "$pid" 2> /dev/null || true; wait "$pid" 2> /dev/null || true; echo 137 > "$t/bounded.rc"
  else
    echo 0 > "$t/bounded.hung"; st=0; wait "$pid" 2> /dev/null || st=$?; echo "$st" > "$t/bounded.rc"
  fi
}
NOT_RUNNING="aws (cbase test client): the resident AWS CLI at $CBASE_TEST_AWS_SOCKET is not running"
DIED="aws (cbase test client): the resident AWS CLI died during the call"

kill -9 "$RESIDENT_PID"; wait "$RESIDENT_PID" 2> /dev/null || true; RESIDENT_PID=""
bounded_client 10 "$t/dead.out" "$t/dead.err" sts get-caller-identity
[ "$(cat "$t/bounded.hung")" = 0 ] || fail "the client with no server did not end within a second"
[ "$(cat "$t/bounded.rc")" = 255 ] || fail "the client with no server: status $(cat "$t/bounded.rc"), not 255"
[ "$(cat "$t/dead.err")" = "$NOT_RUNNING" ] || { cat "$t/dead.err" >&2; fail "the client with no server: stderr is not the not-running sentence"; }
[ ! -s "$t/dead.out" ] || fail "the client with no server wrote stdout"
ok

# Killed in the middle of a call: the fake holds the next GetObject until a second one is asked (gather
# n=2), so the first call waits in the CLI's child. The server is killed then. The client must end with
# the died sentence, and not hang. The second GetObject (the per-process aws) lets the held call end.
resident_start
curl -fsS -X POST "$AWS_ENDPOINT_URL/_fake/gather?n=2" > /dev/null
"$RES_AWS" s3api get-object --bucket "$BUCKET" --key dir/one.txt "$t/held.body" < /dev/null > "$t/held.out" 2> "$t/held.err" &
held=$!
sleep 1
kill -0 "$held" 2> /dev/null || { cat "$t/held.err" >&2; fail "the held call ended before the server was killed (the test would prove nothing)"; }
kill -9 "$RESIDENT_PID"; wait "$RESIDENT_PID" 2> /dev/null || true; RESIDENT_PID=""
n=0
until ! kill -0 "$held" 2> /dev/null || [ $n -ge 50 ]; do sleep 0.1; n=$((n + 1)); done
if kill -0 "$held" 2> /dev/null; then kill -9 "$held" 2> /dev/null || true; fail "the client did not end within 5 s of the server's death in the middle of a call (it hangs)"; fi
st=0; wait "$held" || st=$?
"$REAL_AWS" s3api get-object --bucket "$BUCKET" --key dir/two.txt "$t/release.body" > /dev/null   # lets the orphaned child's held call answer
[ "$st" = 255 ] || fail "the client of a killed server: status $st, not 255"
[ "$(cat "$t/held.err")" = "$DIED" ] || { cat "$t/held.err" >&2; fail "the client of a killed server: stderr is not the died-during sentence"; }
ok

CBASE_TEST_END=1
echo "aws_resident_test: ok ($pass checks)"
