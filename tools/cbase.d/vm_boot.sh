#!/bin/bash
# vm_boot.sh: a vm group's boot, the instance's user-data (group.sh renders it into the group's
# template, with the group's fixed address at @ORIGIN_IPV6@). It is steps 1b and 2 of the boot
# script of the IPv6-only VM cbase was carved out of, as they ran there in production, for the
# standard Amazon Linux 2023 image (which ships the SSM agent) and any group:
#   1. claims the fixed address, the subnet's ::10 (the stack's OriginIpv6, which the origin's AAAA
#      record names), on the instance's own interface. EC2 launches it with a random address of the
#      subnet; a replacement may boot before EC2 has freed ::10 from the old instance, so it retries.
#   2. points the SSM agent at the dual-stack endpoints (ssm and ssmmessages on api.aws, S3's
#      dualstack name) with IMDS over IPv6, so Run Command works with no IPv4 address at all.
# It copies itself to cloud-init's per-boot scripts, so every boot puts the address back on the
# interface. No dnf and no aws CLI: the one EC2 call is signed by curl (--aws-sigv4) with the
# instance role's credentials from IMDSv2, as vm_remote.sh signs its own. A --ipv4 group runs it
# as well: its instance is dual-stack, and the same address and agent settings hold.
set -eEuo pipefail
exec > >(tee -a /var/log/cbase-vm-boot.log | logger -t cbase-vm-boot) 2>&1
trap 'echo "cbase-vm-boot: failed at line $LINENO"' ERR
umask 077
self=/var/lib/cloud/scripts/per-boot/cbase-vm-boot
[ "$0" = "$self" ] || install -D -m 0700 "$0" "$self"
want='@ORIGIN_IPV6@'

imds=""
for a in 'http://[fd00:ec2::254]' http://169.254.169.254; do
  if tok="$(curl -sSf --max-time 2 -X PUT "$a/latest/api/token" -H 'X-aws-ec2-metadata-token-ttl-seconds: 300' 2> /dev/null)"; then imds="$a"; break; fi
done
[ -n "$imds" ] || { echo "cbase-vm-boot: no instance metadata service answered (IMDSv2, over IPv6 or IPv4)"; exit 1; }
md() { curl -sSf -H "X-aws-ec2-metadata-token: $tok" "$imds/latest/$1"; }
region="$(md meta-data/placement/region)"
role="$(md meta-data/iam/security-credentials/)"
creds="$(md "meta-data/iam/security-credentials/$role")"
field() { python3 -c 'import json,sys; print(json.load(sys.stdin)[sys.argv[1]])' "$1"; }
ak="$(field AccessKeyId <<< "$creds")"
sk="$(field SecretAccessKey <<< "$creds")"
st="$(field Token <<< "$creds")"

# 1. The fixed address on this instance's interface.
mac="$(md meta-data/mac)"
eni="$(md "meta-data/network/interfaces/macs/$mac/interface-id")"
have() { md "meta-data/network/interfaces/macs/$mac/ipv6s" | python3 -c '
import ipaddress, sys
w = ipaddress.ip_address(sys.argv[1]); sys.exit(0 if any(ipaddress.ip_address(l.strip()) == w for l in sys.stdin if l.strip()) else 1)' "$want"; }
for _ in $(seq 1 30); do
  have && break
  curl -sSf --aws-sigv4 "aws:amz:$region:ec2" --user "$ak:$sk" -H "x-amz-security-token: $st" -o /dev/null \
    --data "Action=AssignIpv6Addresses&Version=2016-11-15&NetworkInterfaceId=$eni&Ipv6Addresses.1=$want" \
    "https://ec2.$region.api.aws/" || true
  sleep 10
done
have || { echo "cbase-vm-boot: could not claim $want"; exit 1; }
dev="$(ip -o link | awk -F': ' -v m="$mac" '$0 ~ m { print $2; exit }')"
ip -6 addr replace "$want/128" dev "$dev"
echo "cbase-vm-boot: address $want on $dev"

# 2. The SSM agent on the dual-stack endpoints: UseDualStackEndpoint alone left its credential
# check on ssm.<region>.amazonaws.com (IPv4 alone), so the config names the api.aws endpoints.
conf=/etc/amazon/ssm/amazon-ssm-agent.json
drop=/etc/systemd/system/amazon-ssm-agent.service.d/ipv6.conf
mkdir -p /etc/amazon/ssm "$(dirname "$drop")"
printf '{"Agent":{"Region":"%s","UseDualStackEndpoint":true},"Ssm":{"Endpoint":"ssm.%s.api.aws"},"Mgs":{"Region":"%s","Endpoint":"ssmmessages.%s.api.aws"},"S3":{"Region":"%s","Endpoint":"s3.dualstack.%s.amazonaws.com"}}\n' \
  "$region" "$region" "$region" "$region" "$region" "$region" > "$conf.new"
printf '[Service]\nEnvironment=AWS_EC2_METADATA_SERVICE_ENDPOINT_MODE=IPv6\nEnvironment=AWS_USE_DUALSTACK_ENDPOINT=true\n' > "$drop.new"
if cmp -s "$conf.new" "$conf" && cmp -s "$drop.new" "$drop"; then
  rm -f "$conf.new" "$drop.new"
else
  chmod 0644 "$conf.new" "$drop.new"
  mv -f "$conf.new" "$conf"
  mv -f "$drop.new" "$drop"
  systemctl daemon-reload
  systemctl enable amazon-ssm-agent
  systemctl restart amazon-ssm-agent
  echo "cbase-vm-boot: the SSM agent is on the dual-stack endpoints"
fi
