#!/bin/bash
# vm_boot.sh: the cbase-vm instance's user-data (vm_deploy.sh gzips it into the
# launch template) and its upgrade command (it installs itself as
# /usr/local/sbin/cbase-vm-boot; `vm_deploy.sh --restart` runs it again).
#
# No dnf: everything comes from S3 and SSM over IPv6, signed by curl
# (--aws-sigv4) with the instance role's credentials from IMDSv2.
#   1. reads every parameter under /cbase-vm/ (one GetParametersByPath):
#        release/serve, release/agent   "<bucket> <key> <sha256>" (vm.yaml)
#        env/CBASE_*                    the server's environment (secrets are
#                                       SecureStrings, made by vm_deploy.sh)
#   2. installs the SSM agent from its pinned rpm (minimal AL2023 has none),
#      dual-stack endpoints on, IMDS over IPv6;
#   3. fetches the static `cbase serve` binary, checks its sha256, writes the
#      0600 EnvironmentFile and the systemd unit, and (re)starts cbase.
# It never prints a parameter value.
set -euo pipefail
exec > >(tee -a /var/log/cbase-vm-boot.log /dev/console | logger -t cbase-vm-boot) 2>&1
trap 'echo "cbase-vm-boot: failed at line $LINENO"' ERR
umask 077

IMDS='http://[fd00:ec2::254]'
self=/usr/local/sbin/cbase-vm-boot
[ "$(readlink -f "$0")" = "$self" ] || install -m 0700 "$0" "$self"

tok="$(curl -sSf -X PUT "$IMDS/latest/api/token" -H 'X-aws-ec2-metadata-token-ttl-seconds: 300')"
md() { curl -sSf -H "X-aws-ec2-metadata-token: $tok" "$IMDS/latest/$1"; }
region="$(md meta-data/placement/region)"
role="$(md meta-data/iam/security-credentials/)"
creds="$(md "meta-data/iam/security-credentials/$role")"
field() { python3 -c 'import json,sys; print(json.load(sys.stdin)[sys.argv[1]])' "$1"; }
ak="$(field AccessKeyId <<<"$creds")"
sk="$(field SecretAccessKey <<<"$creds")"
st="$(field Token <<<"$creds")"

aws_curl() {   # aws_curl SERVICE URL [curl args...]
  local svc="$1" url="$2"; shift 2
  curl -sSf --retry 5 --retry-all-errors --aws-sigv4 "aws:amz:$region:$svc" --user "$ak:$sk" \
    -H "x-amz-security-token: $st" "$@" "$url"
}

# 1. the parameters, into a private directory: one file per name (never echoed).
P="$(mktemp -d)"
trap 'rm -rf "$P"' EXIT
aws_curl ssm "https://ssm.$region.api.aws/" \
  -H 'Content-Type: application/x-amz-json-1.1' -H 'X-Amz-Target: AmazonSSM.GetParametersByPath' \
  --data '{"Path":"/cbase-vm/","Recursive":true,"WithDecryption":true,"MaxResults":10}' |
  python3 -c '
import json, os, sys
d = json.load(sys.stdin)
if d.get("NextToken"): sys.exit("more than 10 parameters under /cbase-vm/: page them")
for p in d["Parameters"]:
    n = p["Name"][len("/cbase-vm/"):]
    if "/" not in n or ".." in n: continue
    os.makedirs(os.path.join(sys.argv[1], os.path.dirname(n)), exist_ok=True)
    with open(os.path.join(sys.argv[1], n), "w") as f: f.write(p["Value"])
' "$P"

fetch() {   # fetch RELEASE_PARAM DEST: the pinned object, sha256-checked, moved into place
  local bucket key sum
  read -r bucket key sum < "$P/release/$1"
  aws_curl s3 "https://$bucket.s3.dualstack.$region.amazonaws.com/$key" -o "$2.new"
  echo "$sum  $2.new" | sha256sum -c --quiet || { rm -f "$2.new"; echo "sha256 mismatch: $key"; exit 1; }
  echo "fetched $key"
}

# 1b. the fixed address (/cbase-vm/net/origin_ipv6) on this instance's interface.
# A replacement may boot before EC2 has freed it from the old one: retry.
want="$(cat "$P/net/origin_ipv6")"
mac="$(md meta-data/mac)"
eni="$(md "meta-data/network/interfaces/macs/$mac/interface-id")"
have() { md "meta-data/network/interfaces/macs/$mac/ipv6s" | python3 -c '
import ipaddress, sys
w = ipaddress.ip_address(sys.argv[1]); sys.exit(0 if any(ipaddress.ip_address(l.strip()) == w for l in sys.stdin if l.strip()) else 1)' "$want"; }
for _ in $(seq 1 30); do
  have && break
  aws_curl ec2 "https://ec2.$region.api.aws/" --retry 0 -o /dev/null \
    --data "Action=AssignIpv6Addresses&Version=2016-11-15&NetworkInterfaceId=$eni&Ipv6Addresses.1=$want" || true
  sleep 10
done
have || { echo "could not claim $want"; exit 1; }
dev="$(ip -o link | awk -F': ' -v m="$mac" '$0 ~ m { print $2; exit }')"
ip -6 addr replace "$want/128" dev "$dev"
echo "address $want on $dev"

# 2. the SSM agent, once per instance.
if ! systemctl is-active --quiet amazon-ssm-agent; then
  fetch agent /root/amazon-ssm-agent.rpm
  rpm -i --nodigest --nosignature /root/amazon-ssm-agent.rpm.new 2>/dev/null || rpm -q amazon-ssm-agent
  rm -f /root/amazon-ssm-agent.rpm.new
  cat > /etc/amazon/ssm/amazon-ssm-agent.json <<EOF
{"Agent":{"Region":"$region","UseDualStackEndpoint":true}}
EOF
  mkdir -p /etc/systemd/system/amazon-ssm-agent.service.d
  printf '[Service]\nEnvironment=AWS_EC2_METADATA_SERVICE_ENDPOINT_MODE=IPv6\nEnvironment=AWS_USE_DUALSTACK_ENDPOINT=true\n' \
    > /etc/systemd/system/amazon-ssm-agent.service.d/ipv6.conf
  systemctl daemon-reload
  systemctl enable --now amazon-ssm-agent
fi

# 3. the server.
getent passwd cbase >/dev/null || useradd --system --no-create-home --home-dir /var/lib/cbase --shell /sbin/nologin cbase
fetch serve /usr/local/bin/cbase
chmod 0755 /usr/local/bin/cbase.new
mv -f /usr/local/bin/cbase.new /usr/local/bin/cbase
install -d -m 0700 /etc/cbase
: > /etc/cbase/cbase.env.new
for f in "$P"/env/CBASE_*; do
  printf '%s=%s\n' "$(basename "$f")" "$(cat "$f")" >> /etc/cbase/cbase.env.new
done
mv -f /etc/cbase/cbase.env.new /etc/cbase/cbase.env
cat > /etc/systemd/system/cbase.service <<'EOF'
@CBASE_SERVICE@
EOF
systemctl daemon-reload
systemctl enable cbase
systemctl restart cbase
echo "cbase: $(systemctl is-active cbase)"
