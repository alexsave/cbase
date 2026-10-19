#!/bin/bash
# Ships a built `cbase serve` binary to the running cbase-vm instance: the code
# only, never the stack. .github/workflows/c_base.yml runs it on every push to
# main, as the role providers/aws/ops/github_oidc.yaml makes; it runs the same
# from this Mac.
#
#   c_base/providers/local/ops/vm_release.sh BIN   # upload, point at it, restart, verify
#   c_base/providers/local/ops/vm_release.sh --restart   # the instance re-reads SSM, refetches, restarts; verify
#   c_base/providers/local/ops/vm_release.sh --verify    # is-active, the binary's sha256, local /health and /ready, the edge's /health
#
# BIN in order:
#   0. refuses a tree that does not contain origin/main, except a CI run of a
#      commit of main that a newer push's run is queued behind (require_main.sh);
#   1. checks BIN is a static arm64 binary and matches BIN.sha256 when there is one;
#   2. uploads it to s3://<CodeBucket>/vm/cbase-serve-<short sha> with a .sha256
#      (an existing key with another sha256 stops the deploy);
#   3. points /cbase-vm/release/serve ("<bucket> <key> <sha256>") at it. The stack
#      (vm.yaml ReleaseServe) made that parameter; vm_deploy.sh keeps its value
#      when it updates the stack without a BIN, so a template change does not
#      roll the server back;
#   4. runs /usr/local/sbin/cbase-vm-boot on the instance through SSM (it fetches
#      and checks the binary named there, and restarts cbase): the swap happens
#      only after the upload, so a failed upload leaves the old binary serving;
#   5. --verify: cbase active, /usr/local/bin/cbase has this sha256, the
#      instance's own /health and /ready answer 200, and <PublicUrl>/health
#      answers "ok cbase". Any of them not so fails the deploy.
#
# Every AWS call here is one the GitHub role grants (role_covers_release.sh
# holds the two together). Settings: AWS_REGION (us-east-1), CBASE_VM_STACK
# (cbase-vm, the auto-scaling group's name too), CBASE_CODE_BUCKET
# (cbase-code-<account>-<region>), CBASE_VM_VERSION (the short SHA),
# CBASE_PUBLIC_URL (https://api.foolish.cards).
set -euo pipefail
ops="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$ops/../../../.." && pwd)"
die() { echo "vm_release.sh: $*" >&2; exit 1; }
[ $# -eq 1 ] || { sed -n '2,9p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }
command -v aws >/dev/null 2>&1 || die "the AWS CLI is not installed"

sha256() { if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1; else shasum -a 256 "$1" | cut -d' ' -f1; fi; }

mode=release
case "$1" in
  --restart) mode=restart ;;
  --verify) mode=verify ;;
  -h|--help) sed -n '2,32p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
  -*) die "unknown argument $1" ;;
  *)
    bin="$1"
    [ -f "$bin" ] || die "$bin is not a file"
    # shellcheck source=c_base/ops/require_main.sh
    . "$repo/c_base/ops/require_main.sh"
    require_main "$repo"
    file "$bin" | grep -Eq 'ARM aarch64.*(statically|static-pie) linked' || die "$bin is not a static arm64 binary"
    sum="$(sha256 "$bin")"
    if [ -f "$bin.sha256" ]; then
      [ "$(cut -d' ' -f1 "$bin.sha256")" = "$sum" ] || die "$bin does not match $bin.sha256"
    fi
    ;;
esac

export AWS_REGION="${AWS_REGION:-${AWS_DEFAULT_REGION:-us-east-1}}"
stack="${CBASE_VM_STACK:-cbase-vm}"
public="${CBASE_PUBLIC_URL:-https://api.foolish.cards}"
param=/cbase-vm/release/serve

# run_on_vm COMMAND...: the commands as one AWS-RunShellScript on the group's
# instance; prints their output, and fails unless the invocation succeeded.
run_on_vm() {
  local id cmd params status
  # shellcheck disable=SC2016 # the backticks are a JMESPath literal
  id="$(aws autoscaling describe-auto-scaling-groups --auto-scaling-group-names "$stack" \
        --query 'AutoScalingGroups[0].Instances[?LifecycleState==`InService`].InstanceId | [0]' --output text)"
  [ -n "$id" ] && [ "$id" != None ] || die "no InService instance in $stack"
  params="$(printf '%s\n' "$@" | python3 -c 'import json,sys; print(json.dumps({"commands": sys.stdin.read().splitlines()}))')"
  cmd="$(aws ssm send-command --instance-ids "$id" --document-name AWS-RunShellScript \
        --timeout-seconds 600 --parameters "$params" --query Command.CommandId --output text)"
  # `aws ssm wait command-executed` gives up after 100 s; the boot script alone sleeps 30.
  for _ in $(seq 1 120); do
    status="$(aws ssm get-command-invocation --command-id "$cmd" --instance-id "$id" --query Status --output text 2>/dev/null || echo Pending)"
    case "$status" in Pending|InProgress|Delayed) sleep 5 ;; *) break ;; esac
  done
  echo "instance $id: $status"
  aws ssm get-command-invocation --command-id "$cmd" --instance-id "$id" \
    --query '[StandardOutputContent,StandardErrorContent]' --output text
  [ "$status" = Success ]
}

verify() {   # verify [SHA256]: the checks of step 5
  local want="${1:-}" sumcheck=""
  [ -z "$want" ] || sumcheck="echo \"$want  /usr/local/bin/cbase\" | sha256sum -c"
  echo "verify: on the instance"
  run_on_vm "set -e" \
            "systemctl is-active cbase" \
            "$sumcheck" \
            "for i in \$(seq 1 30); do curl -sf -o /dev/null http://127.0.0.1:8080/ready && break; sleep 2; done" \
            "curl -sS -o /dev/null -w 'GET /health -> %{http_code}\n' --fail http://127.0.0.1:8080/health" \
            "curl -sS -o /dev/null -w 'GET /ready  -> %{http_code}\n' --fail http://127.0.0.1:8080/ready" \
            "journalctl -u cbase --no-pager | tail -n 5 | cut -c1-200" \
    || die "the instance is not healthy"
  echo "verify: $public/health"
  local body=""
  for _ in $(seq 1 12); do
    body="$(curl -sS --max-time 10 "$public/health" 2>/dev/null || true)"
    case "$body" in "ok cbase"*) break ;; esac
    sleep 5
  done
  case "$body" in "ok cbase"*) echo "$public/health: $body" ;; *) die "$public/health answered '${body:0:80}', not 'ok cbase'" ;; esac
}

restart() {
  run_on_vm "/usr/local/sbin/cbase-vm-boot >/dev/null 2>&1" "systemctl is-active cbase" "tail -n 3 /var/log/cbase-vm-boot.log" \
    || die "the instance's boot script failed: the binary it had is still in place unless the log says it moved"
}

case "$mode" in
  verify) verify; exit ;;
  restart) restart; verify; exit ;;
esac

# ---- 2. the upload ----------------------------------------------------------------
account="$(aws sts get-caller-identity --query Account --output text)" || die "no AWS credentials"
bucket="${CBASE_CODE_BUCKET:-cbase-code-$account-$AWS_REGION}"
version="${CBASE_VM_VERSION:-$(git -C "$repo" rev-parse --short=9 HEAD)}"
key="vm/cbase-serve-$version"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
# The role reads vm/* but cannot list the bucket, so a missing key answers 403, not 404.
if aws s3api get-object --bucket "$bucket" --key "$key.sha256" "$tmp/had" >/dev/null 2>&1; then
  [ "$(cut -d' ' -f1 "$tmp/had")" = "$sum" ] || die "s3://$bucket/$key exists with a different sha256 (set CBASE_VM_VERSION)"
  echo "s3://$bucket/$key is already this binary"
else
  printf '%s  %s\n' "$sum" "$(basename "$key")" > "$tmp/sum"
  aws s3api put-object --bucket "$bucket" --key "$key" --body "$bin" >/dev/null
  aws s3api put-object --bucket "$bucket" --key "$key.sha256" --body "$tmp/sum" >/dev/null
  echo "uploaded s3://$bucket/$key ($sum)"
fi

# ---- 3. the pointer ---------------------------------------------------------------
aws ssm put-parameter --name "$param" --type String --overwrite --value "$bucket $key $sum" >/dev/null
echo "$param -> $key"

# ---- 4 and 5. the swap, and the proof -----------------------------------------------
restart
verify "$sum"
echo "cbase-vm runs $key"
