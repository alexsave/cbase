#!/bin/bash
# run_aws.sh : launch the calibration boxes (c6g.large and t4g.nano by default), each running runner.sh and uploading
# to its own S3 bucket, then power off by itself. Needs bin/calbench (run ./build.sh first). Makes AWS calls: it is the
# only script here that creates anything.
#
# Required env:
#   CAL_SUBNET   an IPv6-capable subnet (the boxes get one IPv6 address and no IPv4 route)
#   CAL_PROFILE  name of an instance profile whose role may reach SSM; the bucket policy grants that role put/get
# Optional env:
#   AWS_DEFAULT_REGION (default us-east-1), CAL_AMI (default: latest AL2023 arm64 from the public SSM parameter),
#   CAL_ROLES (default "c6g t4g"; "t4g" for the nano alone), CAL_PASSES (5), CAL_GAP_S (900), CAL_SWAP (1: a 1 GB
#   swapfile for dnf; the 2026-10-20 run had none), CAL_GROUPS (default: every group but sqlite; e.g. "sqlite"),
#   CAL_MAX_MIN (185: the box shuts down, and so terminates, after this many minutes whatever the runner is doing;
#   60 is plenty for a sqlite-only run)
# Everything created is named cbase-perf-temp-cal-*; the ids go to state/<run>.env, which collect.sh and cleanup.sh read
# and which they are the only way to name resources by.
set -euo pipefail
D=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
: "${CAL_SUBNET:?set CAL_SUBNET}" "${CAL_PROFILE:?set CAL_PROFILE}"
export AWS_PAGER= AWS_DEFAULT_REGION=${AWS_DEFAULT_REGION:-us-east-1}
REGION=$AWS_DEFAULT_REGION
ROLES=${CAL_ROLES:-"c6g t4g"}
MAXM=${CAL_MAX_MIN:-185}
[[ $MAXM =~ ^[0-9]+$ ]] && [ "$MAXM" -ge 20 ] && [ "$MAXM" -le 185 ] || { echo "CAL_MAX_MIN must be 20..185" >&2; exit 1; }
[[ ${CAL_GROUPS:-} =~ ^[a-z\ ]*$ ]] || { echo "CAL_GROUPS: group names only" >&2; exit 1; }
[ -x "$D/bin/calbench" ] || { echo "no bin/calbench: run ./build.sh first" >&2; exit 1; }
aws sts get-caller-identity --query Arn --output text

SUF=$(openssl rand -hex 4)
RUN=$(date -u +%Y%m%dT%H%M%SZ)-$SUF
mkdir -p "$D/state"
STATE=$D/state/$RUN.env
: > "$STATE"
rec() { echo "$1=$2" >> "$STATE"; }
rec RUN "$RUN"; rec REGION "$REGION"

SUBNET=$CAL_SUBNET
VPC=$(aws ec2 describe-subnets --subnet-ids "$SUBNET" --query 'Subnets[0].VpcId' --output text)
AMI=${CAL_AMI:-$(aws ssm get-parameter --name /aws/service/ami-amazon-linux-latest/al2023-ami-kernel-default-arm64 --query Parameter.Value --output text)}
PROFILE_ROLE_ARN=$(aws iam get-instance-profile --instance-profile-name "$CAL_PROFILE" --query 'InstanceProfile.Roles[0].Arn' --output text)
rec AMI "$AMI"

# bucket: private, only the instance role may put and get
B=cbase-perf-temp-cal-$SUF
aws s3api create-bucket --bucket "$B" $([ "$REGION" = us-east-1 ] || echo --create-bucket-configuration LocationConstraint="$REGION") >/dev/null
rec BUCKET "$B"
aws s3api put-public-access-block --bucket "$B" --public-access-block-configuration BlockPublicAcls=true,IgnorePublicAcls=true,BlockPublicPolicy=true,RestrictPublicBuckets=true
BPOL=$(mktemp); trap 'rm -f "$BPOL" "${UD:-}"' EXIT
echo "{\"Version\":\"2012-10-17\",\"Statement\":[{\"Effect\":\"Allow\",\"Principal\":{\"AWS\":\"$PROFILE_ROLE_ARN\"},\"Action\":[\"s3:PutObject\",\"s3:GetObject\"],\"Resource\":\"arn:aws:s3:::$B/*\"}]}" > "$BPOL"
aws s3api put-bucket-policy --bucket "$B" --policy "file://$BPOL"

# security group: IPv6 egress only, no ingress (the boxes are reached through SSM)
SG=$(aws ec2 create-security-group --group-name "cbase-perf-temp-cal-sg-$SUF" --description "calibration boxes, egress only" --vpc-id "$VPC" --query GroupId --output text)
rec SG "$SG"
aws ec2 authorize-security-group-egress --group-id "$SG" --ip-permissions 'IpProtocol=-1,Ipv6Ranges=[{CidrIpv6=::/0}]' >/dev/null
# a new SG carries an allow-all IPv4 egress rule; the subnet has no IPv4 route, but drop it anyway
aws ec2 revoke-security-group-egress --group-id "$SG" --ip-permissions 'IpProtocol=-1,IpRanges=[{CidrIp=0.0.0.0/0}]' >/dev/null || true
aws ec2 create-tags --resources "$SG" --tags "Key=Name,Value=cbase-perf-temp-cal-sg-$SUF"

# package: the binary and the runner, fetched by the box through a 6 h presigned URL on the dual-stack endpoint
PK=$(mktemp -d)
cp "$D/bin/calbench" "$D/runner.sh" "$PK/"
(cd "$PK" && tar czf pkg.tgz calbench runner.sh)
aws s3 cp --only-show-errors "$PK/pkg.tgz" "s3://$B/perf-temp-cal/pkg.tgz"
rm -rf "$PK"
URL=$(aws s3 presign "s3://$B/perf-temp-cal/pkg.tgz" --expires-in 21600 --endpoint-url "https://s3.dualstack.$REGION.amazonaws.com")

for role in $ROLES; do
  case $role in
    c6g) TYPE=c6g.large; CR=();;
    t4g) TYPE=t4g.nano; CR=(--credit-specification CpuCredits=unlimited);; # unlimited: a throttled box would measure the credit bucket
    *) echo "unknown role $role" >&2; exit 1;;
  esac
  UD=$(mktemp)
  {
    echo '#!/bin/bash'
    # two independent kill switches (the box also terminates on shutdown, and the runner powers off at the end)
    echo "shutdown -h +$MAXM"
    echo "systemd-run --unit cal-killswitch --on-active=$(( (MAXM + 5) * 60 )) /usr/sbin/poweroff"
    if [ "${CAL_SWAP:-1}" = 1 ]; then
      echo 'fallocate -l 1G /swapfile && chmod 600 /swapfile && mkswap /swapfile && swapon /swapfile'
    fi
    # dual-stack SSM endpoints: the subnet is IPv6-only
    echo 'mkdir -p /etc/amazon/ssm'
    echo "echo '{\"Agent\": {\"UseDualStackEndpoint\": true}}' > /etc/amazon/ssm/amazon-ssm-agent.json"
    echo 'systemctl restart amazon-ssm-agent'
    echo 'mkdir -p /var/tmp/cal && cd /var/tmp/cal'
    echo "for i in 1 2 3 4 5 6 7 8 9 10; do curl -fsS -o pkg.tgz \"$URL\" && break; sleep 5; done"
    echo 'tar xzf pkg.tgz'
    echo "systemd-run --unit cal-runner --working-directory=/var/tmp/cal -p LimitNOFILE=65536 -E CAL_PASSES=${CAL_PASSES:-5} -E CAL_GAP_S=${CAL_GAP_S:-900} -E \"CAL_GROUPS=${CAL_GROUPS:-}\" /bin/bash /var/tmp/cal/runner.sh $B $role"
  } > "$UD"
  ID=$(aws ec2 run-instances --image-id "$AMI" --instance-type "$TYPE" --subnet-id "$SUBNET" --security-group-ids "$SG" \
    --ipv6-address-count 1 --iam-instance-profile "Name=$CAL_PROFILE" --instance-initiated-shutdown-behavior terminate "${CR[@]}" \
    --metadata-options HttpTokens=required,HttpEndpoint=enabled,HttpProtocolIpv6=enabled,HttpPutResponseHopLimit=1 \
    --private-dns-name-options HostnameType=resource-name,EnableResourceNameDnsAAAARecord=true \
    --block-device-mappings "DeviceName=/dev/xvda,Ebs={VolumeSize=8,VolumeType=gp3,DeleteOnTermination=true,Encrypted=true}" \
    --tag-specifications "ResourceType=instance,Tags=[{Key=Name,Value=cbase-perf-temp-cal-$role-$SUF}]" "ResourceType=volume,Tags=[{Key=Name,Value=cbase-perf-temp-cal-$role-$SUF}]" \
    --user-data "file://$UD" --query 'Instances[0].InstanceId' --output text)
  rm -f "$UD"
  rec "INSTANCE_$role" "$ID"
  echo "$role $TYPE $ID"
done
echo "state: $STATE"
echo "next: ./collect.sh $STATE   (when runner.log says ALLDONE), then ./cleanup.sh $STATE"
