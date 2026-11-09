# shellcheck shell=bash
# vm.sh - the VM side of a deploy and an undeploy (README.md "./cbase deploy tool"). Sourced by
# tools/cbase.
#
# DEPLOY (vm_find, vm_build, vm_join, vm_ship), for an app with CBASE_VM not none:
#   1. the group's instance, found by its tag (aws.sh aws_group_find vm; a missing group stops with
#      its group-init command), running, arm64, with its public DNS name (the group's Elastic IP);
#   2. the server built for the instance: `make serve-binary` in the app's server directory (the
#      first of . and server/ that holds a Makefile), which is cbase's own static build
#      (ops/build_static.sh serve: musl, in a pinned arm64 Alpine container, so it needs Docker),
#      into dist/cbase-serve-linux-arm64. CBASE_SERVE_BIN names a binary built elsewhere instead (CI).
#      Either way it must be a 64-bit little-endian ELF for aarch64, or the deploy stops;
#   3. THE MEMORY FIT and the port, in one conditional write of the group's state file (state.sh):
#      every member's mem, an undeclared one counted at 128M and said so, plus the app's, must fit
#      in the instance's memory (describe-instance-types' MemoryInfo) less VM_OS_RESERVE, or the
#      deploy is refused, naming the members and their numbers;
#   4. the artifact to s3://cbase-state-<account>/artifacts/<app>/<sha256> (the instance's role
#      reads artifacts/* and nothing else of the bucket);
#   5. one SSM send-command (AWS-RunShellScript, no SSH) runs vm_remote_deploy's script as root:
#      the app's user cbase-<app> and /srv/cbase/<app>/data (the service's alone, 0700); the port
#      checked FREE before a first start (`ss`), refused with exit 3 when another process holds it,
#      and deploy then releases the claim it just made; the artifact fetched and its sha256 checked;
#      the app's settings from SSM Parameter Store /cbase/<app>/ into /srv/cbase/<app>/env (0640,
#      root:cbase-<app>), which serve reads as CBASE_ENV_FILE; the unit cbase-<app>.service
#      (vm_unit) installed and ONLY it restarted; then its GET /health answering 200 on
#      127.0.0.1:<port>, or its startup line ("cbase: listening ...") in its journal, within 60 s;
#   6. the stack's VmOrigin and VmPort parameters (stack.sh): the instance's public DNS name and the
#      claimed port, which every vm and ws route's behaviour points at.
#
# THE PORT IS THE ORIGIN. The service listens on 0.0.0.0:<port> and CloudFront reaches it there
# directly: the group's security group (group.sh) opens 8700-8799 to CloudFront's origin-facing
# managed prefix list alone, so no reverse proxy sits between them (no shared service to run and
# reconfigure, and each app's port is its own origin). Health is checked from the instance, on
# 127.0.0.1.
#
# SECRETS. A setting's value lives in SSM Parameter Store and goes from there to the env file on the
# instance; it never reaches this script. The command's output is never printed whole: only the
# remote script's own "CBASE: " lines, which name no value, pass (masked again by aws_mask), and a
# failure points at journalctl on the instance rather than printing the journal.
#
# UNDEPLOY (vm_undeploy): the unit stopped, disabled and removed, its binary and env file deleted;
# /srv/cbase/<app>/data and the user stay unless --delete-data.
#
# Every wait is a counter loop over CBASE_POLL_SECONDS (macOS has no `timeout`).

VM_OS_RESERVE=192   # MiB an instance keeps for itself: the kernel, systemd, journald and the SSM
                    # agent (Amazon Linux 2023 idles near 150 on a t4g.nano). Never the apps'.
VM_ARTIFACT_NAME=cbase-serve-linux-arm64
VM_POLLS="${CBASE_VM_POLLS:-180}"   # get-command-invocation reads before the wait gives up

VM_ID=""; VM_DNS=""; VM_TYPE=""; VM_MIB=""
VM_BIN=""; VM_SHA=""; VM_KEY=""
VM_PORT=""; VM_NEW=""

# vm_find GROUP: VM_ID, VM_DNS, VM_TYPE and VM_MIB, the group's running arm64 instance.
vm_find() {
  local group="$1" text state arch
  VM_ID="$(aws_group_find vm "$group")"
  text="$(aws_call ec2 describe-instances --instance-ids "$VM_ID" \
            --query 'Reservations[0].Instances[0].[State.Name,PublicDnsName,InstanceType,Architecture]' --output text)" \
    || aws_fail "reading instance $VM_ID"
  IFS=$'\t' read -r state VM_DNS VM_TYPE arch <<< "$text"
  [ "$state" = running ] || { echo "cbase: vm group $group's instance $VM_ID is $state, not running: start it (aws ec2 start-instances --instance-ids $VM_ID) and run this again" >&2; exit 1; }
  [ "$arch" = arm64 ] || { echo "cbase: vm group $group's instance $VM_ID is $arch, and cbase builds the server for linux arm64 alone (make serve-binary)" >&2; exit 1; }
  { [ -n "$VM_DNS" ] && [ "$VM_DNS" != None ]; } || { echo "cbase: vm group $group's instance $VM_ID has no public DNS name, which CloudFront needs as its origin" >&2; exit 1; }
  VM_MIB="$(vm_type_mib "$VM_TYPE")"
  echo "vm group $group: $VM_ID ($VM_TYPE, $VM_MIB MiB), $VM_DNS"
}

# vm_type_mib TYPE: the instance type's memory in MiB (describe-instance-types).
vm_type_mib() {
  local m
  m="$(aws_call ec2 describe-instance-types --instance-types "$1" --query 'InstanceTypes[0].MemoryInfo.SizeInMiB' --output text)" \
    || aws_fail "reading instance type $1"
  [[ "$m" =~ ^[0-9]+$ ]] || { echo "cbase: instance type $1 has no memory size AWS names" >&2; exit 1; }
  printf '%s' "$m"
}

# vm_fit_set: the fit check's limit for the join (state.sh STATE_FIT_LIMIT): the instance's memory
# less what the OS keeps.
vm_fit_set() {
  STATE_FIT_LIMIT=$((VM_MIB - VM_OS_RESERVE))
  [ "$STATE_FIT_LIMIT" -ge 0 ] || STATE_FIT_LIMIT=0
  STATE_FIT_WHAT="$VM_TYPE: $VM_MIB MiB less $VM_OS_RESERVE for the OS"
}

# vm_mem: the app's mem field: the declared MiB, or ~128, the estimate it is counted at.
vm_mem() {
  # shellcheck disable=SC2153 # conf.sh sets CONF_MEMORY_ESTIMATE
  if [ -n "$CONF_MEMORY_ESTIMATED" ]; then printf '~%s' "$CONF_MEMORY_ESTIMATE"; else printf '%s' "$CONF_MEMORY"; fi
}

# vm_sha256 FILE
vm_sha256() {
  if command -v sha256sum > /dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1
  else shasum -a 256 "$1" | cut -d' ' -f1; fi
}

# vm_build: VM_BIN, the linux-arm64 server, built by cbase's own static build or named by
# CBASE_SERVE_BIN; VM_SHA its sha256.
vm_build() {
  local dir="" d magic
  if [ -n "${CBASE_SERVE_BIN:-}" ]; then
    VM_BIN="$CBASE_SERVE_BIN"
    [ -f "$VM_BIN" ] || { echo "cbase: CBASE_SERVE_BIN names $VM_BIN, which is not a file" >&2; exit 1; }
    echo "server: $VM_BIN (CBASE_SERVE_BIN)"
  else
    for d in . server; do
      if [ -f "$d/Makefile" ]; then dir="$d"; break; fi
    done
    [ -n "$dir" ] || { echo "cbase: no Makefile at . or server/, so there is no server to build: the server's Makefile includes cbase.mk, whose make serve-binary builds it (or set CBASE_SERVE_BIN to a linux-arm64 binary). Nothing was changed." >&2; exit 1; }
    echo "server: make -C $dir serve-binary (linux arm64, static, in cbase's Alpine container)"
    if ! make -C "$dir" serve-binary; then
      echo "cbase: make -C $dir serve-binary failed, so there is no linux-arm64 server to ship. It is cbase.mk's target for this build (ops/build_static.sh serve), and it needs Docker; run it until it passes, or set CBASE_SERVE_BIN to a linux-arm64 serve binary, then deploy again. Nothing was changed." >&2
      exit 1
    fi
    VM_BIN="$dir/dist/$VM_ARTIFACT_NAME"
    [ -f "$VM_BIN" ] || { echo "cbase: make -C $dir serve-binary made no $VM_BIN (cbase.mk's serve-binary writes dist/$VM_ARTIFACT_NAME)" >&2; exit 1; }
  fi
  # The ELF header: \177ELF, class 2 (64-bit), data 1 (little-endian), e_machine 0xb7 (aarch64).
  magic="$(od -An -tx1 -N20 "$VM_BIN" | tr -d ' \n')"
  case "$magic" in
    7f454c460201*) [ "${magic:36:4}" = b700 ] || { echo "cbase: $VM_BIN is not an arm64 (aarch64) Linux binary, and the VM is arm64: build it with make serve-binary" >&2; exit 1; } ;;
    *) echo "cbase: $VM_BIN is not a 64-bit Linux (ELF) binary: build it with make serve-binary" >&2; exit 1 ;;
  esac
  VM_SHA="$(vm_sha256 "$VM_BIN")"
  echo "server: $VM_BIN, sha256 $VM_SHA"
}

# vm_plan_dry GROUP: what a deploy would claim, from the state file, writing nothing.
vm_plan_dry() {
  local group="$1" at="" i line port
  vm_fit_set
  state_read vm "$group"
  for i in "${!ST_APPS[@]}"; do [ "${ST_APPS[$i]}" != "$CONF_APP" ] || at="$i"; done
  if [ -n "$at" ]; then port="${ST_PORTS[$at]}"; else port="$(state_free_port)"; fi
  [ -n "$port" ] || { echo "cbase: vm group $group has no free port: all of $STATE_PORT_MIN-$STATE_PORT_MAX are held" >&2; exit 1; }
  line="app=$CONF_APP port=$port service=cbase-$CONF_APP.service mem=$(vm_mem)"
  if ! state_fit "$CONF_APP" "$line"; then
    printf '%s\n' "$STATE_FIT_REPORT" >&2
    echo "cbase: $CONF_APP would not fit on vm group $group ($STATE_FIT_WHAT)" >&2
    exit 1
  fi
  echo "vm: would join vm group $group as $line"
  printf '%s\n' "$STATE_FIT_REPORT"
  VM_PORT="$port"
}

# vm_join GROUP: the app joins the group's state file, the port claimed and the memory fit checked
# in the one conditional write. VM_PORT, and VM_NEW (1 when the app was not a member).
vm_join() {
  vm_fit_set
  state_change vm "$1" "$CONF_APP" "$(vm_mem)" join
  VM_PORT="$STATE_PORT"
  VM_NEW="$STATE_JOINED_NEW"
}

# vm_unit PORT: the app's systemd unit. MemoryMax only for a declared CBASE_MEMORY: an undeclared
# app has no cap (it is only counted at 128M).
vm_unit() {
  local port="$1" app="$CONF_APP" home="/srv/cbase/$CONF_APP"
  cat << EOF
# cbase-$app.service: the app $app, installed by ./cbase deploy (tools/cbase.d/vm.sh). Edits by hand
# are undone by the next deploy.
[Unit]
Description=cbase app $app
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=cbase-$app
Group=cbase-$app
WorkingDirectory=$home
ExecStart=$home/serve serve
Environment=CBASE_LISTEN=0.0.0.0:$port
Environment=CBASE_DB=$home/data/cbase.sqlite
Environment=CBASE_ENV_FILE=$home/env
EOF
  [ -n "$CONF_MEMORY_ESTIMATED" ] || printf 'MemoryMax=%sM\n' "$CONF_MEMORY"
  cat << EOF
Restart=on-failure
RestartSec=2
NoNewPrivileges=yes
PrivateTmp=yes
ProtectHome=yes
ProtectSystem=strict
ReadWritePaths=$home/data

[Install]
WantedBy=multi-user.target
EOF
}

# vm_remote_deploy PORT BUCKET KEY SHA REGION: the script SSM runs as root on the instance. Every
# value in it is one this tool checked (a name, digits, hex, a bucket and region name), so it is
# written into the script as it is. CBASE_VM_ROOT is unset on the instance; cbase_vm_test.sh runs
# the same script under a scratch root with stand-ins for systemctl, ss, aws and the rest.
vm_remote_deploy() {
  local port="$1" bucket="$2" key="$3" sha="$4" region="$5" app="$CONF_APP"
  cat << EOF
#!/bin/bash
# The deploy of cbase app $app on its VM, rendered by ./cbase deploy (tools/cbase.d/vm.sh) and run
# as root by SSM. It prints "CBASE: " lines alone, none naming a setting's value. Exit 3: the port
# is taken; 4: the service did not come up.
set -euo pipefail
R="\${CBASE_VM_ROOT:-}"
app=$app
port=$port
user=cbase-$app
unit=cbase-$app.service
home="\$R/srv/cbase/$app"
say() { echo "CBASE: \$*"; }
tmp="\$(mktemp -d)"
trap 'rm -rf "\$tmp"' EXIT

# 1. Its user and its folders: the data folder is the service's alone.
id -u "\$user" > /dev/null 2>&1 || useradd --system --user-group --no-create-home --home-dir "/srv/cbase/\$app" --shell /sbin/nologin "\$user"
mkdir -p "\$home/data"
chmod 0755 "\$R/srv/cbase" "\$home"
chown "\$user:\$user" "\$home/data"
chmod 0700 "\$home/data"

# 2. The port, free before a first start: with the unit not running, nothing may listen on it.
if ! systemctl is-active --quiet "\$unit"; then
  if [ -n "\$(ss -Hltn "sport = :\$port")" ]; then
    say "port \$port is in use on the VM, and not by \$unit: refused, nothing was started"
    exit 3
  fi
fi

# 3. The server: fetched, its sha256 checked, then swapped in.
aws s3api get-object --region $region --bucket $bucket --key $key "\$tmp/serve" > /dev/null
echo "$sha  \$tmp/serve" | sha256sum -c --quiet - || { say "the artifact's sha256 is not $sha: refused"; exit 1; }
install -m 0755 "\$tmp/serve" "\$home/serve.new"
mv -f "\$home/serve.new" "\$home/serve"

# 4. The settings: SSM Parameter Store /cbase/$app/<NAME>, as NAME='value' lines of the env file
# serve reads (CBASE_ENV_FILE). Read on the instance and written there; never printed.
aws ssm get-parameters-by-path --region $region --path "/cbase/\$app/" --with-decryption \\
  --query 'Parameters[].[Name,Value]' --output text > "\$tmp/params"
want="\$(aws ssm get-parameters-by-path --region $region --path "/cbase/\$app/" --query 'length(Parameters)' --output text)"
n=0
: > "\$tmp/env"
while IFS=\$'\\t' read -r name value; do
  [ -n "\$name" ] || continue
  k="\${name##*/}"
  [[ "\$k" =~ ^[A-Za-z_][A-Za-z0-9_]*\$ ]] || { say "the SSM parameter \$name does not end in an environment name: refused"; exit 1; }
  case "\$value" in *"'"*) say "the SSM parameter \$name holds a single quote, which its env file line cannot: refused"; exit 1 ;; esac
  printf "%s='%s'\\n" "\$k" "\$value" >> "\$tmp/env"
  n=\$((n + 1))
done < "\$tmp/params"
[ "\$n" = "\$want" ] || { say "an SSM parameter under /cbase/\$app/ holds a newline, which its env file line cannot: refused"; exit 1; }
chown "root:\$user" "\$tmp/env"
chmod 0640 "\$tmp/env"
mv -f "\$tmp/env" "\$home/env"
say "settings: \$n from SSM /cbase/\$app/ (values not shown)"

# 5. The unit, and only it restarted.
cat > "\$R/etc/systemd/system/\$unit.new" << 'CBASE_UNIT'
$(vm_unit "$port")
CBASE_UNIT
mv -f "\$R/etc/systemd/system/\$unit.new" "\$R/etc/systemd/system/\$unit"
systemctl daemon-reload
systemctl enable --quiet "\$unit"
since="\$(date +%s)"
systemctl restart "\$unit"

# 6. Up: GET /health answers 200 on the port, or the startup line is in its journal.
for i in \$(seq 1 60); do
  if [ "\$(curl -s -o /dev/null -w '%{http_code}' --max-time 2 "http://127.0.0.1:\$port/health" || true)" = 200 ]; then
    say "\$unit is up: GET /health answered 200 on 127.0.0.1:\$port"
    exit 0
  fi
  if journalctl -u "\$unit" --since "@\$since" -o cat --no-pager 2> /dev/null | grep -q '^cbase: listening '; then
    say "\$unit is up: it is listening on 0.0.0.0:\$port (no /health route)"
    exit 0
  fi
  if ! systemctl is-active --quiet "\$unit"; then
    say "\$unit stopped after its start: see journalctl -u \$unit on the instance"
    exit 4
  fi
  [ "\$i" = 60 ] || sleep 1
done
say "\$unit did not answer on 127.0.0.1:\$port in 60 s: see journalctl -u \$unit on the instance"
exit 4
EOF
}

# vm_remote_undeploy DELETE_DATA: the script that stops and removes the app's service.
vm_remote_undeploy() {
  local app="$CONF_APP"
  cat << EOF
#!/bin/bash
# The undeploy of cbase app $app on its VM, rendered by ./cbase undeploy (tools/cbase.d/vm.sh).
set -euo pipefail
R="\${CBASE_VM_ROOT:-}"
app=$app
user=cbase-$app
unit=cbase-$app.service
home="\$R/srv/cbase/$app"
say() { echo "CBASE: \$*"; }
if [ -f "\$R/etc/systemd/system/\$unit" ]; then
  systemctl disable --now --quiet "\$unit" || true
  rm -f "\$R/etc/systemd/system/\$unit"
  systemctl daemon-reload
  say "\$unit stopped, disabled and removed"
else
  say "\$unit is not installed"
fi
rm -f "\$home/serve" "\$home/serve.new" "\$home/env"
EOF
  if [ "$1" = 1 ]; then
    cat << 'EOF'
rm -rf "$home"
if id -u "$user" > /dev/null 2>&1; then userdel "$user" || true; fi
say "deleted /srv/cbase/$app and its data (--delete-data)"
EOF
  else
    cat << 'EOF'
if [ -d "$home/data" ]; then say "kept /srv/cbase/$app/data (./cbase undeploy --delete-data deletes it)"; fi
EOF
  fi
}

# vm_ssm_run SCRIPT WHAT: the script run as root on VM_ID through SSM's AWS-RunShellScript, waited
# for with a bounded counter loop. Prints its "CBASE: " lines; sets VM_SSM_STATUS (Success, Failed,
# ...) and VM_SSM_CODE (its exit status); the lines are in $CB_TMP/ssm-lines too.
VM_SSM_STATUS=""
VM_SSM_CODE=""
vm_ssm_run() {
  local script="$1" what="$2" b64 cmd id n text out
  # The script travels base64, one command line with no comma in it (the CLI's shorthand splits a
  # list on commas), decoded to a file on the instance and run: its text needs no quoting.
  b64="$(base64 < "$script" | tr -d '\n')"
  cmd="f=\$(mktemp) && echo $b64 | base64 -d > \$f && bash \$f; rc=\$?; rm -f \$f; exit \$rc"
  id="$(aws_call ssm send-command --instance-ids "$VM_ID" --document-name AWS-RunShellScript \
          --comment "cbase $what" --timeout-seconds 600 --parameters "commands=$cmd" \
          --query Command.CommandId --output text)" || {
    case "$(aws_code)" in
      InvalidInstanceId) echo "cbase: $VM_ID is not an SSM managed instance yet (a new instance takes a few minutes; its role must hold AmazonSSMManagedInstanceCore, as group-init's does)" >&2; exit 1 ;;
    esac
    aws_fail "sending the $what to $VM_ID (ssm send-command)"
  }
  echo "vm: the $what runs on $VM_ID (SSM command $id)"
  VM_SSM_STATUS=""
  for ((n = 0; n <= VM_POLLS; n++)); do
    if text="$(aws_call ssm get-command-invocation --command-id "$id" --instance-id "$VM_ID" \
                  --query '[Status,ResponseCode]' --output text)"; then
      IFS=$'\t' read -r VM_SSM_STATUS VM_SSM_CODE <<< "$text"
      case "$VM_SSM_STATUS" in Pending|InProgress|Delayed) ;; *) break ;; esac
    elif [ "$(aws_code)" != InvocationDoesNotExist ]; then
      aws_fail "reading SSM command $id"
    fi
    VM_SSM_STATUS=""
    [ "$n" = "$VM_POLLS" ] || sleep "$POLL_SECONDS"
  done
  if [ -z "$VM_SSM_STATUS" ]; then
    echo "cbase: the $what on $VM_ID (SSM command $id) did not finish in time; its output may name the app's settings, so cbase does not print it (aws ssm get-command-invocation --command-id $id --instance-id $VM_ID)" >&2
    exit 1
  fi
  # Only the script's own lines, never the rest of what the command wrote.
  out="$(aws_call ssm get-command-invocation --command-id "$id" --instance-id "$VM_ID" \
           --query StandardOutputContent --output text)" || aws_fail "reading SSM command $id's output"
  printf '%s\n' "$out" | { grep '^CBASE: ' || true; } | aws_mask > "$CB_TMP/ssm-lines"
  sed 's/^CBASE: /vm: /' "$CB_TMP/ssm-lines"
}

# vm_ship GROUP: the artifact uploaded and the service (re)started on VM_ID. Status 1 when the
# service was not started (said why): a port taken on the VM releases the claim this deploy made.
vm_ship() {
  local group="$1" bucket region
  bucket="$(state_bucket)"
  VM_KEY="artifacts/$CONF_APP/$VM_SHA"
  region="$(aws_region_required)"
  if aws_call s3api head-object --bucket "$bucket" --key "$VM_KEY" --query ETag --output text > /dev/null; then
    echo "artifact: s3://$bucket/$VM_KEY is already this server"
  else
    case "$(aws_code)" in 404|NoSuchKey|NotFound) ;; *) aws_fail "reading s3://$bucket/$VM_KEY" ;; esac
    aws_call s3api put-object --bucket "$bucket" --key "$VM_KEY" --body "$VM_BIN" --content-type application/octet-stream \
      --query ETag --output text > /dev/null || aws_fail "uploading s3://$bucket/$VM_KEY"
    echo "artifact: s3://$bucket/$VM_KEY"
  fi
  vm_remote_deploy "$VM_PORT" "$bucket" "$VM_KEY" "$VM_SHA" "$region" > "$CB_TMP/vm-deploy.sh"
  vm_ssm_run "$CB_TMP/vm-deploy.sh" "deploy of $CONF_APP"
  [ "$VM_SSM_STATUS" != Success ] || return 0
  if [ "$VM_SSM_CODE" = 3 ]; then
    if [ -n "$VM_NEW" ]; then
      state_change vm "$group" "$CONF_APP" "" leave
      echo "cbase: port $VM_PORT is taken on $VM_ID by another process, so $CONF_APP was not started, and its claim on the port is released. Free the port (or stop what holds it), then deploy again." >&2
    else
      echo "cbase: port $VM_PORT is taken on $VM_ID by another process, so $CONF_APP was not started; it keeps its port (it was a member before this deploy). Free the port, then deploy again." >&2
    fi
    return 1
  fi
  echo "cbase: the deploy of $CONF_APP on $VM_ID ended $VM_SSM_STATUS (exit ${VM_SSM_CODE:-?}), above; the service's own log is on the instance: journalctl -u cbase-$CONF_APP (aws ssm start-session --target $VM_ID)" >&2
  return 1
}

# vm_undeploy GROUP DELETE_DATA: the app's service stopped and removed, then its membership left.
vm_undeploy() {
  local group="$1" del="$2"
  vm_find "$group"
  vm_remote_undeploy "$del" > "$CB_TMP/vm-undeploy.sh"
  vm_ssm_run "$CB_TMP/vm-undeploy.sh" "undeploy of $CONF_APP"
  [ "$VM_SSM_STATUS" = Success ] || { echo "cbase: the undeploy of $CONF_APP on $VM_ID ended $VM_SSM_STATUS (exit ${VM_SSM_CODE:-?}); it still holds its port, and nothing else was changed" >&2; exit 1; }
  state_change vm "$group" "$CONF_APP" "" leave
}
