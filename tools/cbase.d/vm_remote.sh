#!/bin/bash
# vm_remote.sh: the deploy of one app on its group's VM, run as root by SSM (AWS-RunShellScript).
# vm.sh (vm_remote_deploy) sends it with the app's values set above this text and its unit
# (vm_unit.service, filled in) spliced in at @CBASE_SERVICE@. It grew out of the boot script of the
# VM cbase was carved out of, which ran it in production: the same credentials, signing, fetch and
# install, for an app of a group (its own user, port, data and unit) instead of the one server.
#
# No dnf and no aws CLI: everything comes from S3 and SSM over the dual-stack endpoints, signed by
# curl (--aws-sigv4) with the instance role's credentials from IMDSv2 (over IPv6, else IPv4), so it
# runs on Amazon Linux 2023 minimal (an IPv6-only VM's image) as on the standard one.
#   1. the app's user and its data directory (the service's alone);
#   2. the port, free before a first start (exit 3 when another process holds it);
#   3. the instance role's credentials;
#   4. fetches the static `serve` binary, checks its sha256, swaps it in;
#   5. reads every parameter under the app's settings path (GetParametersByPath, page by page)
#      into the 0600 EnvironmentFile;
#   6. writes the socket unit and the service unit, starts the socket when it is not listening yet
#      (the one switch onto it), and restarts the service, and only it (vm.sh ZERO DOWNTIME);
#   7. waits for GET /health (200) on the port, or its startup line (exit 4 when it does not come).
# A new VM's own setup (its fixed address, the SSM agent's dual-stack configuration, the journal in
# RAM) is the group's boot, not an app's deploy, and is not here.
# It prints "CBASE: " lines alone (vm.sh prints those), and never a parameter value.
# CBASE_VM_ROOT is unset on the instance; cbase_vm_test.sh runs it under a scratch root with
# stand-ins for systemctl, ss, curl and the rest.
# replaces (CBASE_VM_REPLACES, a grandfathered member's alone) names the unit that served the app
# before cbase: the port it holds is no refusal, its data is chowned only once it is stopped, and
# it is stopped, disabled and masked right before the app's unit starts, in this one command.
# shellcheck disable=SC2154 # app, port, user, unit, sock, home, data, bucket, key, sha, region, settings, replaces: vm.sh sets them
set -eEuo pipefail
trap 'echo "CBASE: the deploy of $app failed at line $LINENO"' ERR
umask 077
R="${CBASE_VM_ROOT:-}"
say() { echo "CBASE: $*"; }
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
envfile="$R/etc/cbase/$app.env"

# 1. Its user and its folders: the data folder is the service's alone.
id -u "$user" > /dev/null 2>&1 || useradd --system --user-group --no-create-home --home-dir "/srv/cbase/$app" --shell /sbin/nologin "$user"
mkdir -p "$home" "$R$data"
chmod 0755 "$R/srv/cbase" "$home"
own_data() { chown -R "$user:$user" "$R$data"; chmod 0700 "$R$data"; }
# The data of a unit being replaced stays its own while it runs.
if [ -z "$replaces" ]; then own_data; fi

# 2. The port, free before a first start: with neither the socket nor the unit running, nothing
# may listen on it.
if ! systemctl is-active --quiet "$sock" && ! systemctl is-active --quiet "$unit" &&
   { [ -z "$replaces" ] || ! systemctl is-active --quiet "$replaces"; }; then
  if [ -n "$(ss -Hltn "sport = :$port")" ]; then
    say "port $port is in use on the VM, and not by $unit: refused, nothing was started"
    exit 3
  fi
fi

# 3. The instance role's credentials.
imds=""
for a in 'http://[fd00:ec2::254]' http://169.254.169.254; do
  if tok="$(curl -sSf --max-time 2 -X PUT "$a/latest/api/token" -H 'X-aws-ec2-metadata-token-ttl-seconds: 300' 2> /dev/null)"; then imds="$a"; break; fi
done
[ -n "$imds" ] || { say "no instance metadata service answered (IMDSv2, over IPv6 or IPv4): refused"; exit 1; }
md() { curl -sSf -H "X-aws-ec2-metadata-token: $tok" "$imds/latest/$1"; }
role="$(md meta-data/iam/security-credentials/)"
creds="$(md "meta-data/iam/security-credentials/$role")"
field() { python3 -c 'import json,sys; print(json.load(sys.stdin)[sys.argv[1]])' "$1"; }
ak="$(field AccessKeyId <<< "$creds")"
sk="$(field SecretAccessKey <<< "$creds")"
st="$(field Token <<< "$creds")"

aws_curl() {   # aws_curl SERVICE URL [curl args...]
  local svc="$1" url="$2"; shift 2
  curl -sSf --retry 5 --retry-all-errors --aws-sigv4 "aws:amz:$region:$svc" --user "$ak:$sk" \
    -H "x-amz-security-token: $st" "$@" "$url"
}

# 4. The server: fetched, its sha256 checked, then swapped in.
aws_curl s3 "https://$bucket.s3.dualstack.$region.amazonaws.com/$key" -o "$tmp/serve" \
  || { say "the artifact s3://$bucket/$key could not be fetched: refused"; exit 1; }
echo "$sha  $tmp/serve" | sha256sum -c --quiet - || { say "the artifact's sha256 is not $sha: refused"; exit 1; }
install -m 0755 "$tmp/serve" "$home/serve.new"
mv -f "$home/serve.new" "$home/serve"

# 5. The settings, into a private file: one NAME=value line per parameter (never echoed).
# GetParametersByPath answers at most 10 per page: follow NextToken until there is none.
install -d -m 0700 "$R/etc/cbase"
: > "$tmp/env"
next=""
for _ in $(seq 1 50); do
  body="$(python3 -c '
import json, sys
b = {"Path": sys.argv[1], "WithDecryption": True, "MaxResults": 10}
if sys.argv[2]: b["NextToken"] = sys.argv[2]
print(json.dumps(b))' "$settings" "$next")"
  if ! next="$(aws_curl ssm "https://ssm.$region.api.aws/" \
                 -H 'Content-Type: application/x-amz-json-1.1' -H 'X-Amz-Target: AmazonSSM.GetParametersByPath' \
                 --data "$body" |
               python3 -c '
import json, re, sys
d = json.load(sys.stdin)
with open(sys.argv[1], "a") as f:
    for p in d["Parameters"]:
        k, v = p["Name"].rsplit("/", 1)[-1], p["Value"]
        if not re.fullmatch("[A-Za-z_][A-Za-z0-9_]*", k): sys.exit("the SSM parameter " + p["Name"] + " does not end in an environment name")
        if chr(10) in v: sys.exit("the SSM parameter " + p["Name"] + " holds a newline, which its env file line cannot")
        f.write(k + "=" + v + chr(10))
print(d.get("NextToken", ""))
' "$tmp/env" 2> "$tmp/why")"; then
    say "settings: $(tail -1 "$tmp/why" | cut -c1-200): refused"
    exit 1
  fi
  [ -n "$next" ] || break
done
[ -z "$next" ] || { say "more than 50 pages of parameters under $settings: refused"; exit 1; }
n="$(wc -l < "$tmp/env" | tr -d ' ')"
chmod 0600 "$tmp/env"
mv -f "$tmp/env" "$envfile"
say "settings: $n from SSM $settings (values not shown)"

# 6. The units, and only the service restarted: the socket holds the port across the restart, so
# the connections that arrive meanwhile wait for the new serve (vm.sh ZERO DOWNTIME).
cat > "$R/etc/systemd/system/$sock.new" << 'SOCKET'
@CBASE_SOCKET@
SOCKET
cat > "$R/etc/systemd/system/$unit.new" << 'EOF'
@CBASE_SERVICE@
EOF
mv -f "$R/etc/systemd/system/$sock.new" "$R/etc/systemd/system/$sock"
mv -f "$R/etc/systemd/system/$unit.new" "$R/etc/systemd/system/$unit"
systemctl daemon-reload
systemctl enable --quiet "$sock" "$unit"
if [ -n "$replaces" ]; then
  # The unit the app ran as before cbase: stopped, disabled and masked, so nothing (a script, a
  # boot, a human) starts it again onto the socket's port, seconds before the app's own starts on
  # its port and its data. mask refuses over a real unit file, so the file is moved aside first,
  # kept as <unit>.cbase-replaced (README.md: the rollback).
  systemctl disable --now --quiet "$replaces" 2> /dev/null || true
  if [ -f "$R/etc/systemd/system/$replaces" ] && [ ! -L "$R/etc/systemd/system/$replaces" ]; then
    mv -f "$R/etc/systemd/system/$replaces" "$R/etc/systemd/system/$replaces.cbase-replaced"
  fi
  systemctl mask --quiet "$replaces"
  systemctl daemon-reload
  say "$replaces stopped, disabled and masked (its unit file kept as /etc/systemd/system/$replaces.cbase-replaced): $unit takes port $port and $data"
  own_data
fi
if ! systemctl is-active --quiet "$sock"; then
  # THE ONE SWITCH onto the socket: the app's service from before socket activation binds the port
  # itself, so it stops before the socket can listen. This deploy's gap is the last.
  if systemctl is-active --quiet "$unit"; then
    systemctl stop "$unit"
    say "$unit stopped to hand port $port to $sock (the switch to socket activation: later deploys queue connections, not refuse them)"
  fi
  systemctl start "$sock"
fi
since="$(date +%s)"
systemctl restart "$unit"

# 7. Up: GET /health answers 200 on the port, or the startup line is in its journal.
for i in $(seq 1 60); do
  if [ "$(curl -s -o /dev/null -w '%{http_code}' --max-time 2 "http://127.0.0.1:$port/health" || true)" = 200 ]; then
    say "$unit is up: GET /health answered 200 on 127.0.0.1:$port"
    exit 0
  fi
  if journalctl -u "$unit" --since "@$since" -o cat --no-pager 2> /dev/null | grep -q '^cbase: listening '; then
    say "$unit is up: it is listening on port $port (no /health route)"
    exit 0
  fi
  if ! systemctl is-active --quiet "$unit"; then
    say "$unit stopped after its start: see journalctl -u $unit on the instance"
    exit 4
  fi
  [ "$i" = 60 ] || sleep 1
done
say "$unit did not answer on 127.0.0.1:$port in 60 s: see journalctl -u $unit on the instance"
exit 4
