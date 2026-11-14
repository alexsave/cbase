#!/bin/bash
# cbase_group_test.sh - group-init, group-destroy and group-template, the commands of the deploy tool's
# group side, end to end through the real AWS CLI against the recorded local fake (fake_aws.c) with
# dummy credentials (lib.sh). No call reaches AWS. CBASE_TOOL names the tool under test (tools/cbase, the
# launcher of the Python deploy tool, by default; nothing is built).
#
#   bash tools/cbase.d/test/cbase_group_test.sh
#   CBASE_TOOL=/path/to/other/cbase bash tools/cbase.d/test/cbase_group_test.sh
#
# The VM group's cases are cbase_vm_test.sh's group-init block, copied (that file runs the deploy
# after them, which this file does not); the rest are the argument errors, a group-template compared
# with its golden text under golden/group-template/, and group-destroy's refusals and deletions that
# need no deploy.
# CBASE_TEST_ONLY=group-init|group-template|group-destroy runs one part (lib.sh's wanted).
# shellcheck disable=SC2016 # the expected text holds a literal $
set -euo pipefail
# shellcheck source=tools/cbase.d/test/lib.sh
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
export CBASE_POLL_SECONDS=0

# ----
# group-init
# ----
if wanted group-init; then
CERT_APIH="arn:aws:acm:us-east-1:$ACCOUNT:certificate/55555555-5555-5555-5555-555555555555"
CERT_API2="arn:aws:acm:us-east-1:$ACCOUNT:certificate/77777777-7777-7777-7777-777777777777"
start_fake --cert "$CERT_APIH ISSUED apih.example.com,api.example.com" --cert "$CERT_API2 ISSUED api.example.com" --zone "Z0EXAMPLE example.com"
# A VM costs money: without --yes (and no terminal to type yes at) the change set is shown and
# deleted, and nothing is made, not even the state bucket.
m="$(mark)"
run "$t" group-init vm g9
expect_rc 2 "group-init vm without --yes"
out_has 'instance: a t4g.nano runs about $3/month' "group-init prints the cost class"
out_has "Add     Instance         AWS::EC2::Instance" "group-init prints the change set"
err_has "group-init vm runs its change set only with --yes" "group-init vm without --yes"
since "$m" | grep -q "CFN ExecuteChangeSet" && fail "group-init without --yes executed its change set"
since "$m" | grep -q "CFN DeleteStack cbase-group-vm-g9" || fail "group-init without --yes left its empty stack"
since "$m" | grep -q "S3 CreateBucket" && fail "group-init without --yes made the state bucket"
ok
# An x86 type: the server is built for arm64 alone.
run "$t" group-init vm g9 --type t3.nano --yes
expect_rc 1 "group-init vm on an x86 type"
err_has "t3.nano is x86_64, and cbase builds the server for linux arm64 alone" "group-init vm on an x86 type"
ok
run "$t" group-init vm Bad_Name --yes
expect_rc 2 "group-init of a bad name"
ok
# With --yes (and --ipv4, the group the deploys below share): the state bucket made, the stack's
# change set run, the state file written with the instance's public DNS name as its origin.
m="$(mark)"
run "$t" group-init vm g9 --ipv4 --yes
expect_rc 0 "group-init vm"
since "$m" | grep -q "S3 CreateBucket $BUCKET 200" || fail "group-init did not make the state bucket"
since "$m" | grep -q "S3 PutPublicAccessBlock $BUCKET 200" || fail "group-init did not block public access to the state bucket"
since "$m" | grep -q "CFN CreateChangeSet cbase-group-vm-g9 CREATE ok params=InstanceType=t4g.nano,ImageId=ami-0f4ce1234567890ab,AvailabilityZone=us-east-1a,CloudFrontPrefixList=pl-0a6b6e1c,CloudFrontPrefixListIpv4=pl-3b927c52,ArtifactBucket=$BUCKET caps=CAPABILITY_IAM" \
  || { since "$m" | grep CreateChangeSet >&2; fail "group-init vm's change set parameters"; }
since "$m" | grep -q "CFN ExecuteChangeSet cbase-group-vm-g9 ok" || fail "group-init vm did not execute"
out_has "vm group g9: i-0g9" "group-init vm finds its group"
printf 'cbase-state 1 vm g9 origin=ec2-i-0g9.compute-1.amazonaws.com addr=dualstack\n' > "$t/want"
state_of vm g9 | diff -u "$t/want" - || fail "group-init vm's state file"
ok
# The state bucket is versioned (a state file overwritten by mistake can be read back).
since "$m" | grep -q "S3 PutBucketVersioning $BUCKET 200 status=Enabled" || { since "$m" | grep "^S3 " >&2; fail "group-init did not turn on the state bucket's versioning"; }
ok
template_of cbase-group-vm-g9 > "$t/tpl"
for want in "Type: AWS::EC2::Instance" "Key: cbase:vm-group" "Value: g9" "SourcePrefixListId: !Ref CloudFrontPrefixList" \
            "FromPort: 8700" "ToPort: 8799" "HttpTokens: required" "policy/AmazonSSMManagedInstanceCore" \
            'Resource: !Sub "arn:${AWS::Partition}:s3:::${ArtifactBucket}/artifacts/*"' "Encrypted: true" "Type: AWS::EC2::EIP"; do
  grep -Fq -- "$want" "$t/tpl" || { cat "$t/tpl" >&2; fail "the vm group's template lacks [$want]"; }
done
awk '/SecurityGroupIngress:/{f=1; next} /SecurityGroupEgress:|^  [A-Za-z]/{f=0} f' "$t/tpl" > "$t/ingress"
[ "$(grep -c 'FromPort:' "$t/ingress")" = "$(grep -c 'FromPort: 8700$' "$t/ingress")" ] || fail "the vm group's security groups open more than the apps' ports"
grep -Eq 'CidrIp|FromPort: (22|80|443)$' "$t/ingress" && fail "the vm group's security groups open SSH, the web ports or a CIDR"
ok
# IPv6-only by default: the group's own VPC and IPv6-only subnet, no Elastic IP and no public IPv4,
# the app ports open to CloudFront's IPv6 list, the boot claiming the fixed ::10 and putting the
# SSM agent on the dual-stack endpoints; no origin recorded yet (the first app's CBASE_VM_ORIGIN).
m="$(mark)"
run "$t" group-init vm g6 --yes
expect_rc 0 "group-init vm, IPv6-only"
since "$m" | grep -q "CFN CreateChangeSet cbase-group-vm-g6 CREATE ok params=InstanceType=t4g.nano,ImageId=ami-0f4ce1234567890ab,AvailabilityZone=us-east-1a,CloudFrontPrefixList=pl-0a6b6e1c,ArtifactBucket=$BUCKET caps=CAPABILITY_IAM" \
  || { since "$m" | grep CreateChangeSet >&2; fail "the IPv6-only group's change set parameters"; }
out_has "address: IPv6-only, no public IPv4" "the IPv6-only group says so"
out_has "origin: the instance claims 2600:1f18:4a3:6f00::10 at boot" "the IPv6-only group prints its fixed address"
grep -Fq 'a public IPv4 is about' "$t/out" && fail "an IPv6-only group was billed a public IPv4"
template_of cbase-group-vm-g6 > "$t/tpl"
for want in "Ipv6Native: true" "AmazonProvidedIpv6CidrBlock: true" "DestinationIpv6CidrBlock: ::/0" "SourcePrefixListId: !Ref CloudFrontPrefixList" \
            "AssociatePublicIpAddress: false" "HttpProtocolIpv6: enabled" "Action: ec2:AssignIpv6Addresses" \
            "Action=AssignIpv6Addresses&Version=2016-11-15&NetworkInterfaceId=\$eni&Ipv6Addresses.1=\$want" "want='\${OriginIpv6}'" \
            '"Ssm":{"Endpoint":"ssm.%s.api.aws"}' "Value: !Select [0, !Split [\"/\", !Select [1, !Cidr [!Select [0, !GetAtt Subnet.Ipv6CidrBlocks], 2, 4]]]]"; do
  grep -Fq -- "$want" "$t/tpl" || { cat "$t/tpl" >&2; fail "the IPv6-only group's template lacks [$want]"; }
done
grep -Eq 'AWS::EC2::EIP|CidrBlock: 10\.0\.0\.0/24$|MapPublicIpOnLaunch|DestinationCidrBlock|CloudFrontPrefixListIpv4|AssociatePublicIpAddress: true' "$t/tpl" \
  && fail "the IPv6-only group's template has a public IPv4 or an IPv4 subnet"
printf 'cbase-state 1 vm g6\n' > "$t/want"
state_of vm g6 | diff -u "$t/want" - || fail "the IPv6-only group's state file"
ok
# --ipv4: dual-stack with an Elastic IP, its cost said before the --yes refusal; the IPv4 list in a
# second security group.
run "$t" group-init vm g4 --ipv4
expect_rc 2 "group-init vm --ipv4 without --yes"
out_has 'address: dual-stack (--ipv4): a public IPv4 is about $3.60 a month' "--ipv4 prints its cost"
err_has "group-init vm runs its change set only with --yes" "--ipv4 asks first"
run "$t" group-template vm g4 --ipv4
expect_rc 0 "group-template vm --ipv4"
for want in "Type: AWS::EC2::EIP" "CidrBlock: 10.0.0.0/24" "DestinationCidrBlock: 0.0.0.0/0" "SourcePrefixListId: !Ref CloudFrontPrefixListIpv4" \
            "GroupSet: [!GetAtt SecurityGroup.GroupId, !GetAtt SecurityGroupIpv4.GroupId]"; do
  grep -Fq -- "$want" "$t/out" || { cat "$t/out" >&2; fail "the --ipv4 group's template lacks [$want]"; }
done
ok
# group-init static: the state bucket is there now (no second create), the bucket's one policy.
m="$(mark)"
run "$t" group-init static g9
expect_rc 0 "group-init static"
since "$m" | grep -q "S3 HeadBucket $BUCKET 200" || fail "group-init static did not find the state bucket"
since "$m" | grep -q "S3 CreateBucket" && fail "group-init static made a second state bucket"
out_has "static group g9: g9-group-bucket" "group-init static finds its group"
template_of cbase-group-static-g9 > "$t/tpl"
for want in "Type: AWS::S3::Bucket" "DeletionPolicy: Retain" "Status: Enabled" "BlockPublicAcls: true" "RestrictPublicBuckets: true" \
            "Key: cbase:static-group" "Type: AWS::S3::BucketPolicy" "Service: cloudfront.amazonaws.com" \
            "Action: [s3:GetObject, s3:ListBucket]" "aws:SourceAccount: !Ref AWS::AccountId"; do
  grep -Fq -- "$want" "$t/tpl" || { cat "$t/tpl" >&2; fail "the static group's template lacks [$want]"; }
done
ok
# A re-run: up to date, the image and the type kept as they are.
m="$(mark)"
run "$t" group-init vm g9 --ipv4
expect_rc 0 "group-init vm again"
out_has "stack: cbase-group-vm-g9 is up to date (no changes)" "a re-run changes nothing"
since "$m" | grep -q "params=InstanceType=(previous),ImageId=(previous),AvailabilityZone=(previous)," || fail "a re-run did not keep the type, the image and the zone"
since "$m" | grep -q "SSM GetParameter" && fail "a re-run read the newest image (it would replace the instance)"
ok
# THE ONE HARD STOP, for a group: a change set replacing the bucket or the instance is refused.
for how in replace-bucket remove-bucket replace-instance; do
  fake_set "changes=$how"
  kind=static; [ "$how" != replace-instance ] || kind=vm
  m="$(mark)"
  run "$t" group-init "$kind" g9 --yes
  expect_rc 1 "a group-init whose change set would $how"
  case "$how" in
    replace-bucket) err_has "refused: the bucket Bucket would be replaced (Replacement: True)" "$how" ;;
    remove-bucket) err_has "refused: the bucket Bucket would be removed" "$how" ;;
    replace-instance) err_has "refused: the instance Instance would be replaced (Replacement: True)" "$how" ;;
  esac
  err_has "refused, and the change set deleted; nothing was executed" "$how"
  since "$m" | grep -q "CFN ExecuteChangeSet" && fail "$how: the change set was executed"
  since "$m" | grep -q "CFN DeleteChangeSet cbase-group-$kind-g9 ok" || fail "$how: the change set was not deleted"
  ok
done
fake_set changes=auto
fi

# ----
# the arguments: refused before anything is made (no AWS call: the usage errors come first)
# ----
expect_err() {   # expect_err LINE WHAT ARGS...: exit 2 with exactly LINE on stderr, and no AWS call
  local line="$1" what="$2"; shift 2
  local m; m="$(mark)"
  run "$t" "$@"
  expect_rc 2 "$what"
  [ "$(cat "$t/err")" = "$line" ] || { cat "$t/err" >&2; fail "$what: stderr is not [$line]"; }
  [ "$(since "$m" | wc -l | tr -d ' ')" = 0 ] || fail "$what: an AWS call was made before the usage error"
  ok
}
if wanted group-args; then
start_fake
expect_err "cbase: group-init takes --yes, --type TYPE and --ipv4, not --bogus" "group-init: an unknown flag" group-init vm g9 --bogus
expect_err "cbase: --type wants an instance type (t4g.small)" "group-init: --type with no type" group-init vm g9 --type
expect_err "cbase: bad is not an instance type (t4g.small)" "group-init: --type not a type" group-init vm g9 --type bad
expect_err "cbase: --ipv4 is group-init vm's alone" "group-init static: --ipv4" group-init static g9 --ipv4
expect_err "cbase: group-init takes no --delete-data" "group-init: --delete-data" group-init vm g9 --delete-data
expect_err "cbase: group-destroy takes no --type" "group-destroy: --type" group-destroy vm g9 --type t4g.small
expect_err "cbase: --ipv4 is group-init vm's alone" "group-destroy: --ipv4" group-destroy vm g9 --ipv4
expect_err "cbase: group-destroy takes --yes and --delete-data, not --bogus" "group-destroy: an unknown flag" group-destroy vm g9 --bogus
expect_err "cbase: group-template vm wants one group name, and --ipv4 or nothing" "group-template: no name" group-template vm
expect_err "cbase: group-template static wants one group name" "group-template: static with --ipv4" group-template static g9 --ipv4
expect_err "cbase: group-init vm GROUP or group-init static GROUP" "group-init: no kind" group-init
fi

# ----
# group-template: no AWS; the output is byte for byte its golden text, recorded from the shell tool
# before that tool was retired (golden/group-template/, named by the arguments with spaces as _)
# ----
if wanted group-template; then
start_fake
for want in "vm g4" "vm g4 --ipv4" "static g4"; do
  cp "$root/tools/cbase.d/test/golden/group-template/$(printf '%s' "$want" | tr ' ' '_')" "$t/want"
  m="$(mark)"
  # shellcheck disable=SC2086 # the arguments are words: "vm g4 --ipv4" is three of them
  run "$t" group-template $want
  expect_rc 0 "group-template $want"
  diff -u "$t/want" "$t/out" > /dev/null || { diff -u "$t/want" "$t/out" >&2; fail "group-template $want differs from its golden text"; }
  [ "$(since "$m" | wc -l | tr -d ' ')" = 0 ] || fail "group-template $want made an AWS call"
  ok
done
run "$t" group-template vm g4 --ipv4
out_has "Type: AWS::EC2::EIP" "group-template vm --ipv4 has its Elastic IP"
ok
fi

# ----
# group-destroy: the members refusal, the data refusal, the confirmation, and the deletions
# ----
if wanted group-destroy; then
start_fake
printf 'cbase-state 1 vm g9 origin=x.example.com addr=dualstack\napp=blog port=8700 service=cbase-blog.service mem=128\napp=r1 port=8701 service=cbase-r1.service mem=128\n' > "$t/seed"
aws s3api create-bucket --bucket "$BUCKET" > /dev/null
aws s3api put-object --bucket "$BUCKET" --key vm/g9.txt --body "$t/seed" > /dev/null
m="$(mark)"
run "$t" group-destroy vm g9 --yes
expect_rc 1 "group-destroy of a group with members"
err_has "vm group g9 still has 2 members: blog r1. Undeploy each first (./cbase undeploy in its repository); nothing was changed." "group-destroy names the members"
since "$m" | grep -qE "CFN DeleteStack|S3 DeleteObject" && fail "group-destroy of a group with members changed something"
ok
aws s3api delete-object --bucket "$BUCKET" --key vm/g9.txt > /dev/null
m="$(mark)"
run "$t" group-destroy vm g9
expect_rc 2 "group-destroy without --yes, no stack"
err_has "group-destroy deletes the group's stack only with --yes" "group-destroy asks first"
since "$m" | grep -q "CFN DeleteStack" && fail "group-destroy without --yes deleted a stack"
ok
run "$t" group-destroy static g9 --yes
expect_rc 0 "group-destroy static with no stack"
out_has "static group g9 is destroyed" "group-destroy static with no stack says so"
ok
run "$t" group-destroy vm g9 --yes
expect_rc 0 "group-destroy vm with no stack"
out_has "vm group g9 is destroyed" "group-destroy vm with no stack says so"
ok
# A stack that exists: a static group's bucket is kept with its files, its tag taken off.
run "$t" group-init static g9
expect_rc 0 "group-init static for the destroy"
m="$(mark)"
run "$t" group-destroy static g9 --yes
expect_rc 0 "group-destroy static"
since "$m" | grep -q "S3 DeleteBucketTagging g9-group-bucket 204" || fail "group-destroy static did not take the bucket's tag off"
out_has "bucket: s3://g9-group-bucket is kept with its files" "group-destroy static says the bucket is kept"
ok
fi

CBASE_TEST_END=1
echo "cbase_group_test: ok ($pass checks)"
