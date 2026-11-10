# shellcheck shell=bash
# vm.sh - the VM side of a deploy and an undeploy (README.md "./cbase deploy tool"). Sourced by
# tools/cbase.
#
# DEPLOY (README.md "Deploying a full app"): vm_find the group's running arm64 instance; vm_build
# the linux-arm64 server (make serve-binary, or CBASE_SERVE_BIN), checked to be an aarch64 ELF;
# vm_join the port and THE MEMORY FIT in one conditional write of the group's state file
# (state.sh): every member's mem plus the app's within the instance's memory less VM_OS_RESERVE;
# vm_ship the artifact to its sha256's key (vm_artifacts) and one SSM send-command (no SSH) that
# runs vm_remote.sh as root: the app's user and data folder, the port checked free before a first
# start (exit 3, and deploy releases the claim it just made), the artifact fetched and checked, the
# settings from SSM (vm_settings) into its env file, the units installed and ONLY the service
# restarted, then its GET /health or its startup line waited for. The stack's VmOrigin and VmPort
# are the origin's hostname and that port.
#
# THE PORT IS THE ORIGIN. The service listens on 0.0.0.0:<port> and CloudFront reaches it there
# directly: the group's security group (group.sh) opens 8700-8799 to CloudFront's origin-facing
# managed prefix list alone, so no reverse proxy sits between them (no shared service to run and
# reconfigure, and each app's port is its own origin). Health is checked from the instance, on
# 127.0.0.1.
#
# ZERO DOWNTIME. After an app's first deploy no deploy refuses a connection. The port is the socket
# unit's: systemd listens on it and hands it to serve as fd 3 (socket activation; net/server.c THE
# INHERITED LISTENER), and keeps it while the service restarts, so a code deploy's restart queues
# the connections that arrive (Backlog=, vm_unit.socket) for the new serve: a short delay, never a
# refusal. The old serve's drain answers what it had begun; its WebSockets get 1001 and reconnect to
# the new serve, which has only what the store holds. One serving process at a time, always (an
# app's in-memory state is never split between two). A deploy never restarts the socket: only the
# first deploy onto socket activation starts it, after stopping what held the port before (the
# app's own service from before, or CBASE_VM_REPLACES's unit), and that one switch is a gap. So
# an edit of vm_unit.socket reaches a running app at its VM's next boot, not at a deploy.
#
# SECRETS. A setting's value lives in SSM Parameter Store and goes from there to the env file on the
# instance; it never reaches this script but the origin secret, which is masked. The command's
# output is never printed whole: only the remote script's own "CBASE: " lines, which name no value,
# pass (masked again by aws_mask), and a failure points at journalctl on the instance.
#
# UNDEPLOY (vm_undeploy): the socket and the unit stopped, disabled and removed, its binary and env
# file deleted; the data folder and the user stay unless --delete-data.

VM_OS_RESERVE=192   # MiB an instance keeps for itself: the kernel, systemd, journald and the SSM
                    # agent (Amazon Linux 2023 idles near 150 on a t4g.nano). Never the apps'.
VM_ARTIFACT_NAME=cbase-serve-linux-arm64
VM_POLLS="${CBASE_VM_POLLS:-180}"   # get-command-invocation reads before the wait gives up

VM_ID=""; VM_DNS=""; VM_TYPE=""; VM_MIB=""
VM_BIN=""; VM_SHA=""; VM_KEY=""
VM_PORT=""; VM_NEW=""
VM_ORIGIN_ADDR=""   # the origin's IpAddressType, ipv6 or dualstack: the group's (state.sh ST_ORIGIN_ADDR)

# vm_stack_instance STACK: the running instance an adopted stack holds: its AWS::EC2::Instance, or
# the in-service instance of its auto-scaling group of one.
vm_stack_instance() {
  local stack="$1" text type id ids=()
  text="$(aws_call cloudformation describe-stack-resources --stack-name "$stack" \
            --query "StackResources[?ResourceType=='AWS::EC2::Instance' || ResourceType=='AWS::AutoScaling::AutoScalingGroup'].[ResourceType,PhysicalResourceId]" \
            --output text)" || aws_fail "reading the stack $stack (CBASE_VM_STACK)"
  read -r type id <<< "$(printf '%s\n' "$text" | head -1)"
  case "$type" in
    AWS::EC2::Instance) printf '%s' "$id" ;;
    AWS::AutoScaling::AutoScalingGroup)
      text="$(aws_call autoscaling describe-auto-scaling-groups --auto-scaling-group-names "$id" \
                --query "AutoScalingGroups[0].Instances[?LifecycleState=='InService'].InstanceId" --output text)" \
        || aws_fail "reading the auto-scaling group $id of $stack"
      read -r -a ids <<< "$text"
      { [ "${#ids[@]}" -eq 1 ] && [ "${ids[0]}" != None ]; } \
        || { echo "cbase: the auto-scaling group $id of $stack (CBASE_VM_STACK) has ${#ids[@]} instances in service, not one: a group is one VM" >&2; exit 1; }
      printf '%s' "${ids[0]}" ;;
    *) echo "cbase: the stack $stack (CBASE_VM_STACK) holds no EC2 instance or auto-scaling group" >&2; exit 1 ;;
  esac
}

# vm_find GROUP: VM_ID, VM_DNS (the instance's public DNS name, until vm_origin), VM_TYPE and
# VM_MIB, the group's running arm64 instance: found by CBASE_VM_STACK (adopted), or by its group tag.
vm_find() {
  local group="$1" text state arch
  if [ -n "$CONF_VM_STACK" ]; then VM_ID="$(vm_stack_instance "$CONF_VM_STACK")"
  else VM_ID="$(aws_group_find vm "$group")"; fi
  text="$(aws_call ec2 describe-instances --instance-ids "$VM_ID" \
            --query "Reservations[0].Instances[0].[State.Name,PublicDnsName,InstanceType,Architecture]" --output text)" \
    || aws_fail "reading instance $VM_ID"
  IFS=$'\t' read -r state VM_DNS VM_TYPE arch <<< "$text"
  [ "$state" = running ] || { echo "cbase: vm group $group's instance $VM_ID is $state, not running: start it (aws ec2 start-instances --instance-ids $VM_ID) and run this again" >&2; exit 1; }
  [ "$arch" = arm64 ] || { echo "cbase: vm group $group's instance $VM_ID is $arch, and cbase builds the server for linux arm64 alone (make serve-binary)" >&2; exit 1; }
  [ "$VM_DNS" != None ] || VM_DNS=""
  VM_MIB="$(vm_type_mib "$VM_TYPE")"
  echo "vm group $group: $VM_ID ($VM_TYPE, $VM_MIB MiB), ${VM_DNS:-no public DNS name}${CONF_VM_STACK:+, adopted from the stack $CONF_VM_STACK}"
}

# vm_origin GROUP: VM_DNS and VM_ORIGIN_ADDR, the group's origin (state.sh state_origin), read
# before anything is changed: every app in a group shares one VM, so the origin is the group's.
# None recorded and none named stops a deploy; a dry run lists it and goes on with the instance's
# public DNS name.
vm_origin() {
  local group="$1" held
  STATE_ORIGIN="$CONF_VM_ORIGIN"
  # An adopted group's first deploy makes the state bucket (group.sh), after this check.
  if [ -n "$CONF_VM_STACK" ] && ! group_state_bucket_found; then
    # shellcheck disable=SC2034 # state.sh reads it
    STATE_NO_BUCKET=1
    state_read vm "$group"
    # shellcheck disable=SC2034
    STATE_NO_BUCKET=""
  else state_read vm "$group"; fi
  held="$ST_ORIGIN"
  if state_origin "$group"; then
    VM_DNS="$ST_ORIGIN"
    # shellcheck disable=SC2153 # state.sh sets ST_ORIGIN_ADDR
    VM_ORIGIN_ADDR="$ST_ORIGIN_ADDR"
    if [ -n "$held" ]; then echo "vm origin: $VM_DNS ($VM_ORIGIN_ADDR), the group's (its state file)"
    else echo "vm origin: $VM_DNS ($VM_ORIGIN_ADDR), the group's: CBASE_VM_ORIGIN, recorded in its state file by this deploy"; fi
    return 0
  fi
  local what="vm group $group has no origin recorded and $CONF_APP names none: set CBASE_VM_ORIGIN (the VM's AAAA hostname) once"
  dry_stop "$what" "cbase: $what, in this app's cbase.conf. Nothing was changed."
  VM_DNS="${VM_DNS:-unrecorded-origin.invalid}"   # the change set's placeholder, deleted unexecuted
  VM_ORIGIN_ADDR=""
}

# vm_origin_secret: VM_ORIGIN_SECRET, the app's origin secret, SSM /cbase/<app>/CBASE_ORIGIN_SECRET
# (a SecureString), which the stack's VM origins send as X-Cbase-Origin and serve reads from its env
# file as CBASE_ORIGIN_SECRET. CloudFormation resolves no ssm-secure reference in a distribution's
# origin headers, so it goes in as the NoEcho parameter OriginSecret. Its value is never printed:
# aws_mask masks any line that holds it, and a bad one is refused without it.
VM_ORIGIN_SECRET=""
VM_DRY_SECRET=cbase-dry-run-stand-in-for-the-missing-origin-secret   # a dry run's change set's, never a deploy's
vm_origin_secret() {
  local name
  name="$(vm_settings)CBASE_ORIGIN_SECRET"
  if ! VM_ORIGIN_SECRET="$(aws_call ssm get-parameter --name "$name" --with-decryption --query Parameter.Value --output text)"; then
    VM_ORIGIN_SECRET=""
    if [ "$(aws_code)" = ParameterNotFound ]; then
      echo "cbase: there is no SSM parameter $name, the origin secret CloudFront sends the app as X-Cbase-Origin (and serve checks). Make it once, then deploy again; nothing was changed:" >&2
      echo "  aws ssm put-parameter --type SecureString --name $name --value \"\$(openssl rand -hex 32)\"" >&2
      dry_stop "no SSM parameter $name (the origin secret)"
      VM_ORIGIN_SECRET="$VM_DRY_SECRET"
      return 0
    fi
    aws_fail "reading the SSM parameter $name"
  fi
  if ! [[ "$VM_ORIGIN_SECRET" =~ ^[A-Za-z0-9_-]{32,128}$ ]]; then
    VM_ORIGIN_SECRET=""
    echo "cbase: the SSM parameter $name is not 32 to 128 letters, digits, _ and - (its value is not shown): openssl rand -hex 32 makes one" >&2
    dry_stop "the SSM parameter $name is not an origin secret"
    VM_ORIGIN_SECRET="$VM_DRY_SECRET"
    return 0
  fi
  AWS_MASK_VALUES+=("$VM_ORIGIN_SECRET")
  echo "origin secret: $name (X-Cbase-Origin; its value is not shown)"
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
  local group="$1" line
  vm_fit_set
  state_read vm "$group"
  state_at "$CONF_APP"
  state_port "$group" "$CONF_VM_PORT"
  line="$(state_vm_line "$CONF_APP" "$STATE_PORT" "$(conf_mem)" "$CONF_VM_DATA")"
  if ! state_fit "$CONF_APP" "$line"; then
    printf '%s\n' "$STATE_FIT_REPORT" >&2
    echo "cbase: $CONF_APP would not fit on vm group $group ($STATE_FIT_WHAT)" >&2
    exit 1
  fi
  echo "vm: would join vm group $group as $line"
  printf '%s\n' "$STATE_FIT_REPORT"
  VM_PORT="$STATE_PORT"
}

# vm_join GROUP: the app joins the group's state file, the port claimed and the memory fit checked
# in the one conditional write. VM_PORT, and VM_NEW (1 when the app was not a member).
vm_join() {
  vm_fit_set
  # shellcheck disable=SC2034 # state.sh reads them
  STATE_LEGACY_PORT="$CONF_VM_PORT"
  # shellcheck disable=SC2034
  STATE_LEGACY_DATA="$CONF_VM_DATA"
  # shellcheck disable=SC2034 # state.sh reads them
  STATE_ORIGIN="$CONF_VM_ORIGIN"
  # shellcheck disable=SC2034
  STATE_ORIGIN_NEED=1
  state_change vm "$1" "$CONF_APP" "$(conf_mem)" join
  VM_PORT="$STATE_PORT"
  VM_NEW="$STATE_JOINED_NEW"
}

# vm_settings: the SSM path of the app's settings, ending in /: CBASE_VM_SETTINGS, else, in an
# adopted group (CBASE_VM_STACK), /<stack>/<app>/, under the stack's own SSM namespace /<stack>/*
# that its instance role reads (cbase-vm's reads /cbase-vm/*), else /cbase/<app>/.
vm_settings() {
  if [ -n "$CONF_VM_SETTINGS" ]; then printf '%s' "$CONF_VM_SETTINGS"
  elif [ -n "$CONF_VM_STACK" ]; then printf '/%s/%s/' "$CONF_VM_STACK" "$CONF_APP"
  else printf '/cbase/%s/' "$CONF_APP"; fi
}
# vm_artifacts: "<bucket> <prefix>" the app's servers go to and the instance fetches from, the
# prefix ending in /: CBASE_VM_ARTIFACTS, else, in an adopted group, the account's code bucket's
# vm/<app>/ (s3://cbase-code-<account>-<region>/vm/<app>/, under the vm/* its instance role reads),
# else the state bucket's artifacts/<app>/.
vm_artifacts() {
  local a="$CONF_VM_ARTIFACTS"
  if [ -z "$a" ] && [ -n "$CONF_VM_STACK" ]; then a="s3://cbase-code-$AWS_ACCOUNT-$(vm_region)/vm/$CONF_APP/"
  elif [ -z "$a" ]; then a="s3://$(state_bucket)/artifacts/$CONF_APP/"; fi
  a="${a#s3://}"
  printf '%s %s' "${a%%/*}" "${a#*/}"
}

# vm_data_dir: the app's data directory on the VM: a grandfathered member's own (CBASE_VM_DATA), or
# /srv/cbase/<app>/data.
vm_data_dir() { printf '%s' "${CONF_VM_DATA:-/srv/cbase/$CONF_APP/data}"; }
# vm_backup_bucket: where serve puts the app's backups (CBASE_BACKUP_BUCKET, or the state bucket).
vm_backup_bucket() { printf '%s' "${CONF_BACKUP_BUCKET:-$(state_bucket)}"; }
# vm_region: the group's region (the CLI's), which the instance's calls and its backups use.
vm_region() { aws_region_required; }

# vm_unit PORT: the app's systemd unit, vm_unit.service filled in. MemoryMax only for a declared
# CBASE_MEMORY: an undeclared app has no cap (it is only counted at 128M).
vm_unit() {
  local port="$1" mem="/^@MEMORY_MAX@\$/d"
  [ -n "$CONF_MEMORY_ESTIMATED" ] || mem="s|^@MEMORY_MAX@\$|MemoryMax=${CONF_MEMORY}M|"
  # Every value is one this tool checked (a name, digits, a path, a bucket and region name), none
  # holding a '|' or a '&', so each is a literal sed replacement.
  # shellcheck disable=SC2154 # here is tools/cbase's directory
  sed -e "s|@APP@|$CONF_APP|g" -e "s|@USER@|cbase-$CONF_APP|g" -e "s|@PORT@|$port|g" \
      -e "s|@DATA@|$(vm_data_dir)|g" -e "s|@HOME@|/srv/cbase/$CONF_APP|g" -e "s|@REGION@|$(vm_region)|g" \
      -e "s|@BACKUP_BUCKET@|$(vm_backup_bucket)|g" -e "s|@BACKUP_KEY@|${CONF_BACKUP_KEY:-backups/$CONF_APP/cbase.sqlite}|g" \
      -e "$mem" "$here/cbase.d/vm_unit.service"
}

# vm_socket PORT: the app's socket unit, vm_unit.socket filled in: the port systemd holds across
# the service's restarts.
vm_socket() { sed -e "s|@APP@|$CONF_APP|g" -e "s|@PORT@|$1|g" "$here/cbase.d/vm_unit.socket"; }

# vm_remote_deploy PORT BUCKET KEY SHA REGION: the script SSM runs as root on the instance. Every
# value in it is one this tool checked (a name, digits, hex, a bucket and region name), so it is
# written into the script as it is. CBASE_VM_ROOT is unset on the instance; cbase_vm_test.sh runs
# the same script under a scratch root with stand-ins for systemctl, ss, aws and the rest.
vm_remote_deploy() {
  local port="$1" bucket="$2" key="$3" sha="$4" region="$5" app="$CONF_APP"
  cat << EOF
#!/bin/bash
# The deploy of cbase app $app on its VM, rendered by ./cbase deploy (tools/cbase.d/vm.sh) from
# vm_remote.sh, and run as root by SSM.
app=$app
port=$port
user=cbase-$app
unit=cbase-$app.service
sock=cbase-$app.socket
home="\${CBASE_VM_ROOT:-}/srv/cbase/$app"
data=$(vm_data_dir)
bucket=$bucket
key=$key
sha=$sha
region=$region
settings=$(vm_settings)
replaces=$CONF_VM_REPLACES
EOF
  vm_unit "$port" > "$CB_TMP/vm-unit"
  vm_socket "$port" > "$CB_TMP/vm-socket"
  # The units spliced in at @CBASE_SERVICE@ and @CBASE_SOCKET@ (the production boot script's own
  # way), and the shebang once, above.
  awk -v unit="$CB_TMP/vm-unit" -v sock="$CB_TMP/vm-socket" 'NR == 1 { next }
    $0 == "@CBASE_SERVICE@" { while ((getline l < unit) > 0) print l; next }
    $0 == "@CBASE_SOCKET@" { while ((getline l < sock) > 0) print l; next } { print }' \
    "$here/cbase.d/vm_remote.sh"
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
unit=cbase-$app.service
sock=cbase-$app.socket
home="\$R/srv/cbase/$app"
say() { echo "CBASE: \$*"; }
if [ -f "\$R/etc/systemd/system/\$unit" ]; then
  # The socket first: with it listening, a connection would start the service again.
  systemctl disable --now --quiet "\$sock" 2> /dev/null || true
  systemctl disable --now --quiet "\$unit" || true
  rm -f "\$R/etc/systemd/system/\$unit" "\$R/etc/systemd/system/\$sock"
  systemctl daemon-reload
  say "\$unit and \$sock stopped, disabled and removed"
else
  say "\$unit is not installed"
fi
rm -f "\$home/serve" "\$home/serve.new" "\$R/etc/cbase/\$app.env"
EOF
  if [ "$1" = 1 ]; then
    cat << 'EOF'
rm -rf "$home"
user="cbase-$app"
if id -u "$user" > /dev/null 2>&1; then userdel "$user" || true; fi
say "deleted /srv/cbase/$app and its data (--delete-data)"
EOF
  else
    cat << 'EOF'
if [ -d "$home/data" ]; then say "kept /srv/cbase/$app/data (./cbase undeploy --delete-data deletes it)"; fi
EOF
  fi
}

# vm_remote_data: the script that names each app whose data is on the instance (group-destroy's).
vm_remote_data() {
  cat << 'EOF'
#!/bin/bash
set -euo pipefail
R="${CBASE_VM_ROOT:-}"
for d in "$R"/srv/cbase/*/data; do
  [ -d "$d" ] || continue
  a="${d%/data}"
  echo "CBASE: data ${a##*/}"
done
EOF
}

# vm_ssm_run SCRIPT WHAT: the script run as root on VM_ID through SSM's AWS-RunShellScript, waited
# for with a bounded counter loop. Prints its "CBASE: " lines; sets VM_SSM_STATUS (Success, Failed,
# ...) and VM_SSM_CODE (its exit status); the lines are in $CB_TMP/ssm-lines too.
VM_SSM_STATUS=""
VM_SSM_CODE=""
VM_SSM_ID=""
vm_ssm_read() {   # one read of command VM_SSM_ID; true once it has ended
  local text
  if text="$(aws_call ssm get-command-invocation --command-id "$VM_SSM_ID" --instance-id "$VM_ID" \
                --query '[Status,ResponseCode]' --output text)"; then
    IFS=$'\t' read -r VM_SSM_STATUS VM_SSM_CODE <<< "$text"
    case "$VM_SSM_STATUS" in Pending|InProgress|Delayed) ;; *) return 0 ;; esac
  elif [ "$(aws_code)" != InvocationDoesNotExist ]; then
    aws_fail "reading SSM command $VM_SSM_ID"
  fi
  VM_SSM_STATUS=""
  return 1
}
vm_ssm_run() {
  local script="$1" what="$2" b64 cmd id out
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
  VM_SSM_ID="$id"
  if ! aws_poll $((VM_POLLS + 1)) vm_ssm_read; then
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
  local group="$1" bucket region prefix
  read -r bucket prefix <<< "$(vm_artifacts)"
  VM_KEY="$prefix$VM_SHA"
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
