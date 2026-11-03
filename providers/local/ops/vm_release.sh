#!/bin/bash
# Ships a built `cbase serve` binary to the running cbase-vm instance: the code
# only, never the stack. .github/workflows/c_base.yml runs it on every push to
# main, as the role providers/aws/ops/github_oidc.yaml makes; it runs the same
# from this Mac.
#
#   c_base/providers/local/ops/vm_release.sh BIN   # upload, point at it, restart, verify
#   c_base/providers/local/ops/vm_release.sh --restart   # pin this tree's boot script, run it on the instance (it re-reads SSM, refetches, restarts); verify
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
#      roll the server back. The boot script is pinned the same way: this tree's
#      vm_boot.sh (cbase.service spliced in) goes up as vm/cbase-vm-boot-<sha12>
#      and /cbase-vm/release/boot names it (the key is the content, so an
#      unchanged script uploads the same object). Both modes do this, --restart
#      too, so a tree that is not main's tip cannot be restarted onto the instance;
#   4. runs vm_boot_stub.sh on the instance through SSM: it reads
#      /cbase-vm/release/boot, fetches that script, checks its sha256 and syntax,
#      keeps the installed copy as cbase-vm-boot.prev, installs the new one and
#      runs it (it fetches and checks the binary named by release/serve and
#      restarts cbase). A boot-script change therefore reaches the running
#      instance with no replacement. Any failed check leaves the installed copy
#      as it was. The swap happens only after the uploads, so a failed upload
#      leaves the old binary serving;
#   5. --verify: cbase active, /usr/local/bin/cbase has this sha256, the
#      instance's own /health and /ready answer 200, and <PublicUrl>/health
#      answers "ok cbase". Any of them not so fails the deploy;
#   6. prunes the bucket (c_base/ops/s3_prune_lib.sh): every other
#      vm/cbase-serve-* and its .sha256 goes, except the new release and the one
#      the pointer named before (the one-step rollback); the same for
#      vm/cbase-vm-boot-*, by its own pointer. Only after step 5
#      passed, only when the pointer read back as the new key, and never
#      templates/ or the amazon-ssm rpm. A prune that fails warns and does not
#      fail the deploy. CBASE_PRUNE_DRY_RUN=1 only prints what it would delete.
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

# shellcheck source=c_base/providers/local/ops/vm_boot_lib.sh
. "$ops/vm_boot_lib.sh"   # sha256, boot_assemble, boot_key, stub_build
# shellcheck source=c_base/ops/s3_prune_lib.sh
. "$repo/c_base/ops/s3_prune_lib.sh"   # prune_bucket

mode=release
case "$1" in
  --restart)
    mode=restart
    # shellcheck source=c_base/ops/require_main.sh
    . "$repo/c_base/ops/require_main.sh"
    require_main "$repo"   # --restart ships this tree's boot script
    ;;
  --verify) mode=verify ;;
  -h|--help) sed -n '2,48p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
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
boot_param=/cbase-vm/release/boot

# run_on_vm COMMAND...: the commands as one AWS-RunShellScript on the group's
# instance; prints their output, and fails unless the invocation succeeded.
run_on_vm() {
  local id cmd params status
  # shellcheck disable=SC2016 # the backticks are a JMESPath literal
  id="$(aws autoscaling describe-auto-scaling-groups --auto-scaling-group-names "$stack" \
        --query 'AutoScalingGroups[0].Instances[?LifecycleState==`InService`].InstanceId | [0]' --output text)"
  if [ -z "$id" ] || [ "$id" = None ]; then die "no InService instance in $stack"; fi
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

# The boot step: the stub (the instance's user-data, the same text) runs through
# SSM, so it works on an instance whose installed boot script predates the stub.
# Its output goes to the boot log; a failure shows the log's tail.
restart() {
  local stub
  stub="$(base64 < "$tmp/stub.sh" | tr -d '\n')"
  # From a file with stdin on /dev/null, so the boot script it execs inherits no pipe.
  run_on_vm "f=\$(mktemp); echo $stub | base64 -d > \$f; bash \$f < /dev/null >> /var/log/cbase-vm-boot.log 2>&1 || { rc=\$?; tail -n 20 /var/log/cbase-vm-boot.log; rm -f \$f; exit \$rc; }; rm -f \$f" \
            "systemctl is-active cbase" "tail -n 3 /var/log/cbase-vm-boot.log" \
    || die "the instance's boot failed: its installed boot script is the one it had (cbase-vm-boot.prev is the one before) unless the log says it moved"
}

# pin_boot: uploads this tree's boot script as vm/cbase-vm-boot-<sha12> (the key is
# the content: an existing object is the same bytes) and sets boot_value/boot_key/
# prev_boot_key; point_boot moves the pointer, after the other uploads.
pin_boot() {
  boot_assemble "$ops/vm_boot.sh" "$ops/cbase.service" "$tmp/boot.sh" || die "cannot build the boot script"
  stub_build "$ops/vm_boot_stub.sh" "$tmp/stub.sh" || die "the user-data stub is too big"
  boot_sum="$(sha256 "$tmp/boot.sh")"
  boot_key="$(boot_key "$boot_sum")"
  aws s3api put-object --bucket "$bucket" --key "$boot_key" --body "$tmp/boot.sh" >/dev/null
  echo "boot script: s3://$bucket/$boot_key ($boot_sum)"
  boot_value="$bucket $boot_key $boot_sum"
  prev_boot_key=""
  if old_boot="$(aws ssm get-parameter --name "$boot_param" --query Parameter.Value --output text 2>/dev/null)"; then
    prev_boot_key="$(echo "$old_boot" | awk '{print $2}')"
  fi
}
point_boot() {
  aws ssm put-parameter --name "$boot_param" --type String --overwrite --value "$boot_value" >/dev/null
  echo "$boot_param -> $boot_key"
}
# prune_boot: after the checks passed, keep the new boot object and the one before it.
prune_boot() {
  local live_boot
  live_boot="$(aws ssm get-parameter --name "$boot_param" --query Parameter.Value --output text 2>/dev/null | awk '{print $2}')" || live_boot=""
  if [ -z "$prev_boot_key" ] || [ "$prev_boot_key" = None ]; then
    echo "boot prune skipped: the pointer's previous value could not be read"
  elif [ "$prev_boot_key" = "$boot_key" ]; then
    echo "boot prune skipped: the pointer already named $boot_key (the script it replaced is not known)"
  elif [ "$live_boot" != "$boot_key" ]; then
    echo "boot prune skipped: the pointer reads '$live_boot', not $boot_key"
  else
    prune_bucket boot "$bucket" "$live_boot" "$prev_boot_key" || echo "warning: the boot prune failed; the deploy stands" >&2
  fi
}

if [ "$mode" = verify ]; then verify; exit; fi

# ---- 2. the upload ----------------------------------------------------------------
account="$(aws sts get-caller-identity --query Account --output text)" || die "no AWS credentials"
bucket="${CBASE_CODE_BUCKET:-cbase-code-$account-$AWS_REGION}"
version="${CBASE_VM_VERSION:-$(git -C "$repo" rev-parse --short=9 HEAD)}"
key="vm/cbase-serve-$version"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
pin_boot
if [ "$mode" = restart ]; then
  point_boot
  restart; verify
  prune_boot
  exit
fi
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
# What it named until now is the rollback target the prune keeps. If it cannot be
# read, nothing is pruned (a guess here would delete the wrong release).
prev_key=""
if old_value="$(aws ssm get-parameter --name "$param" --query Parameter.Value --output text 2>/dev/null)"; then
  prev_key="$(echo "$old_value" | awk '{print $2}')"
fi
aws ssm put-parameter --name "$param" --type String --overwrite --value "$bucket $key $sum" >/dev/null
echo "$param -> $key"
point_boot

# ---- 4 and 5. the swap, and the proof -----------------------------------------------
restart
verify "$sum"
echo "cbase-vm runs $key"

# ---- 6. the prune -------------------------------------------------------------------
# The live key is read back from the pointer, the source of truth, never taken
# from this run's variables or the listing's order.
live_key="$(aws ssm get-parameter --name "$param" --query Parameter.Value --output text 2>/dev/null | awk '{print $2}')" || live_key=""
if [ -z "$prev_key" ] || [ "$prev_key" = None ]; then
  echo "prune skipped: the pointer's previous value could not be read"
elif [ "$prev_key" = "$key" ]; then
  echo "prune skipped: the pointer already named $key (the release it replaced is not known)"
elif [ "$live_key" != "$key" ]; then
  echo "prune skipped: the pointer reads '$live_key', not $key"
else
  prune_bucket vm "$bucket" "$live_key" "$prev_key" || echo "warning: the prune failed; the deploy stands" >&2
fi
prune_boot
