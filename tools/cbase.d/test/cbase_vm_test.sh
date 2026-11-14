#!/bin/bash
# cbase_vm_test.sh - the deploy tool's VM side end to end, as an app runs it, through the
# real AWS CLI against the recorded local fake (fake_aws.c) with dummy credentials (lib.sh). No
# call reaches AWS.
#
#   bash tools/cbase.d/test/cbase_vm_test.sh      (cbase's make check runs it: cbase-vm-test)
#
# The cases: group-init vm refused without --yes (nothing made, not even the state bucket), an
# x86 type refused; IPv6-only by default (no Elastic IP, no public IPv4, CloudFront's IPv6 list, the
# boot's claim of ::10), --ipv4's public IPv4 and its cost; made with the state bucket and its
# state file; group-init static, with
# the bucket policy on aws:SourceAccount; a re-run up to date, keeping the image and the type;
# re-runs whose change set would replace the bucket or the instance, refused. A full app's
# deploy: the port claimed, the memory fit with an undeclared app counted at 128M and said so, the
# artifact uploaded, the SSM command and the unit it installs (the port, MemoryMax only when
# declared, only that unit restarted), the stack with VmOrigin and VmPort; a VM-only app; the
# memory fit refused, naming the members; the port taken on the VM, refused with the claim
# released and the change set deleted; a failed build and a binary for another machine, refused
# before any change; a dry run; two racing deploys getting distinct ports; apps' table. The scripts
# SSM would run, run here under a scratch root with stand-ins for systemctl, ss and aws: the port
# check, the env file from SSM, data kept and deleted. Undeploy keeping files and data, and
# --delete-files and --delete-data doing what they say; group-destroy refused while members remain
# and while the instance holds data, then destroying. No secret in any output.
# docs/mutation_checks.md records the mutations each was checked against.
# shellcheck disable=SC2016 # the expected text and the stand-in scripts hold a literal $
set -euo pipefail
# shellcheck source=tools/cbase.d/test/lib.sh
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
# wanted SECTION, for this file: CBASE_VM_TEST_ONLY (names separated by spaces or commas) picks the
# sections a run covers, unset runs them all. lib.sh's wanted reads cbase_test.sh's CBASE_TEST_ONLY,
# which cbase-cli-test sets for that file alone. The names are the `if wanted` blocks below, and a
# section that uses state an earlier one made needs that section in the subset too.
wanted() {
  [ -z "${CBASE_VM_TEST_ONLY:-}" ] && return 0
  case " ${CBASE_VM_TEST_ONLY//,/ } " in *" $1 "*) return 0 ;; esac
  return 1
}
export CBASE_POLL_SECONDS=0
SECRET=hunter2-db-value
SECRET2=hunter3-origin-value
no_secret() {   # no_secret WHAT: neither planted secret in the last run's output
  if grep -Fq -e "$SECRET" -e "$SECRET2" "$t/out" "$t/err"; then
    grep -F -e "$SECRET" -e "$SECRET2" "$t/out" "$t/err" >&2
    fail "$1: a secret was printed"
  fi
}
# script_of: the last SSM command's script, decoded from the fake's log.
script_of() {
  grep '^SSM SendCommand ' "$t/log" | tail -n 1 \
    | sed -n 's/.*echo \([A-Za-z0-9+/=]*\) | base64 -d.*/\1/p' | base64 -d
}
puts_to() { since "$1" | grep -c "^PUT /$2" || true; }

# The server binaries a build makes: a 64-bit little-endian ELF for aarch64 (e_machine 0xb7), and
# one for x86-64 (0x3e).
printf '\177ELF\002\001\001\000\000\000\000\000\000\000\000\000\002\000\267\000\001\000\000\000cbase serve (test)\n' > "$t/elf-arm64"
printf '\177ELF\002\001\001\000\000\000\000\000\000\000\000\000\002\000\076\000\001\000\000\000cbase serve (x86)\n' > "$t/elf-x86"
# vm_app NAME LINES...: an app with a server whose Makefile's serve-binary "builds" the arm64 ELF.
vm_app() {
  local d
  d="$(app "$@")"
  printf 'serve-binary:\n\tmkdir -p dist && cp %s dist/cbase-serve-linux-arm64\n' "$t/elf-arm64" > "$d/Makefile"
  mkdir -p "$d/ui/out" && printf '<p>%s</p>\n' "$1" > "$d/ui/out/index.html"
  printf '%s' "$d"
}

if wanted group-init; then
# ================================================================================================
# group-init
# ================================================================================================
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
if wanted deploy; then
# ================================================================================================
# deploy: a full app's VM side
# ================================================================================================
# shop: the VM and the bucket of the groups g9, its memory undeclared (counted at 128M).
shop="$(vm_app shop "CBASE_APP = shop" "CBASE_VM = g9" "CBASE_STATIC = g9")"
# Its origin secret missing from SSM: refused, with the one command that makes it.
fake_set origin-secret=
m="$(mark)"
run "$shop" deploy
expect_rc 1 "a VM app with no origin secret"
err_has "there is no SSM parameter /cbase/shop/CBASE_ORIGIN_SECRET" "a VM app with no origin secret"
err_has "aws ssm put-parameter --type SecureString --name /cbase/shop/CBASE_ORIGIN_SECRET" "the command that makes the origin secret"
since "$m" | grep -qE "^PUT |^CFN CreateChangeSet|^SSM SendCommand" && fail "a deploy with no origin secret changed something"
ok
ORIGIN_SECRET=hunter4-origin-header-value-for-the-edge
fake_set "origin-secret=$ORIGIN_SECRET"
m="$(mark)"
run "$shop" deploy
expect_rc 0 "a full app's deploy"
out_has "vm group g9: i-0g9 (t4g.nano, 512 MiB), ec2-i-0g9.compute-1.amazonaws.com" "the deploy finds the instance"
out_has "shop joined vm group g9: app=shop port=8700 service=cbase-shop.service mem=~128" "the port is claimed"
out_has "  shop: memory not declared, counted as 128M" "an undeclared app is counted at 128M, and said so"
out_has "  memory: shop ~128 = 128 MiB of the 320 MiB for apps (t4g.nano: 512 MiB less 192 for the OS)" "the memory fit"
out_has "vm: done (the fake's answer)" "the command's own lines are printed"
sha="$(shasum -a 256 "$t/elf-arm64" | cut -d' ' -f1)"
since "$m" | grep -q "^PUT /$BUCKET/artifacts/shop/$sha 200" || fail "the artifact was not uploaded to artifacts/shop/<sha256>"
since "$m" | grep -q "SSM GetCommandInvocation c0ffee00-[0-9a-f-]* InProgress" || fail "the command was not waited for"
since "$m" | grep -q "CFN CreateChangeSet cbase-app-shop CREATE ok params=VmOrigin=ec2-i-0g9.compute-1.amazonaws.com,VmPort=8700,OriginSecret=(the ssm value) " \
  || { since "$m" | grep CreateChangeSet >&2; fail "the app's stack lacks VmOrigin and VmPort"; }
out_has "shop joined static group g9: app=shop folder=shop/" "the app joins its static group"
out_has "deployed shop: https://" "the URL is printed"
out_has "service: cbase-shop.service: the deploy did not say whether it restarted" "a VM app's summary says the service line is missing when the script printed none"
out_has "origin secret: /cbase/shop/CBASE_ORIGIN_SECRET (X-Cbase-Origin; its value is not shown)" "the origin secret is read"
grep -Fq -- "$ORIGIN_SECRET" "$t/out" "$t/err" && fail "the origin secret's value was printed"
template_of cbase-app-shop > "$t/tpl"
[ "$(grep -c "HeaderName: X-Cbase-Origin" "$t/tpl")" = "$(grep -c "DomainName: !Ref VmOrigin" "$t/tpl")" ] || fail "a VM origin lacks X-Cbase-Origin"
grep -A1 "HeaderName: X-Cbase-Origin" "$t/tpl" | grep -q "HeaderValue: !Ref OriginSecret" || fail "X-Cbase-Origin is not the OriginSecret parameter"
grep -A2 "^  OriginSecret:" "$t/tpl" | grep -q "NoEcho: true" || fail "OriginSecret is not NoEcho"
aws s3api head-object --bucket g9-group-bucket --key shop/index.html > /dev/null 2>&1 || fail "the files were not uploaded"
ok
# The order: the service before the files, the files before the distribution.
since "$m" | grep -nE "^SSM SendCommand|^PUT /g9-group-bucket/shop/|^CFN ExecuteChangeSet cbase-app-shop" | cut -d: -f2 | cut -c1-14 | uniq > "$t/order"
printf 'SSM SendComman\nPUT /g9-group-\nCFN ExecuteCha\n' | diff -u - "$t/order" || fail "the deploy's order is not service, files, distribution"
ok
# The command: the deploy script, the unit it installs.
script_of > "$t/script"
{ grep -Fxq "key=artifacts/shop/$sha" "$t/script" && grep -Fxq "bucket=$BUCKET" "$t/script"; } || fail "the script does not fetch the artifact"
grep -Fq 'aws_curl s3 "https://$bucket.s3.dualstack.$region.amazonaws.com/$key"' "$t/script" || fail "the script does not fetch over S3's dual-stack endpoint"
{ grep -Fxq "sha=$sha" "$t/script" && grep -Fq 'echo "$sha  $tmp/serve" | sha256sum -c' "$t/script"; } || fail "the script does not check the artifact's sha256"
awk "/^cat > .*<< 'EOF'\$/{f=1; next} /^EOF\$/{f=0} f" "$t/script" > "$t/unit"
for want in "User=cbase-shop" "WorkingDirectory=/srv/cbase/shop/data" "ExecStart=/srv/cbase/shop/serve serve" \
            "Environment=CBASE_LISTEN=:::8700" "Environment=CBASE_DB=/srv/cbase/shop/data/cbase.sqlite" \
            "EnvironmentFile=/etc/cbase/shop.env" "Restart=always" "ReadWritePaths=/srv/cbase/shop/data" \
            "Environment=CBASE_BACKUP_BUCKET=$BUCKET" "Environment=CBASE_BACKUP_KEY=backups/shop/cbase.sqlite" \
            "Environment=CBASE_BACKUP_URL=s3://$BUCKET/backups/shop/cbase.sqlite" "Environment=CBASE_BACKUP_REGION=us-east-1"; do
  grep -Fxq -- "$want" "$t/unit" || { cat "$t/unit" >&2; fail "the unit lacks [$want]"; }
done
grep -q '^MemoryMax' "$t/unit" && fail "an undeclared app's unit has a MemoryMax"
grep -Fxq "Requires=cbase-shop.socket" "$t/unit" || fail "the unit does not take its port from cbase-shop.socket"
awk "/^cat > .*<< 'SOCKET'\$/{f=1; next} /^SOCKET\$/{f=0} f" "$t/script" > "$t/socket"
for want in "ListenStream=[::]:8700" "BindIPv6Only=both" "Backlog=4096"; do
  grep -Fxq -- "$want" "$t/socket" || { cat "$t/socket" >&2; fail "the socket unit lacks [$want]"; }
done
if [ "$(grep -c 'systemctl restart' "$t/script")" != 1 ] || ! grep -Fq 'systemctl restart "$unit"' "$t/script"; then
  fail "the script restarts other than the app's own unit"
fi
grep -Fq 'unit=cbase-shop.service' "$t/script" || fail "the script's unit is not cbase-shop.service"
ok
# An AWS error that holds the origin secret's bare value (no keyword on its line): masked by value.
fake_set "changes=replace-distribution&fail-execute=2"
run "$shop" deploy --allow-downtime
expect_rc 1 "an execute whose error holds the origin secret"
err_has "(a line naming an origin header or a parameter value, not shown)" "the masked error"
grep -Fq -- "$ORIGIN_SECRET" "$t/out" "$t/err" && fail "an AWS error printed the origin secret's value"
fake_set "changes=auto&fail-execute=0"
ok
# blog: VM-only, its memory declared: the next port, and a real cap.
blog="$(vm_app blog "CBASE_APP = blog" "CBASE_VM = g9" "CBASE_STATIC = none" "CBASE_MEMORY = 64M")"
m="$(mark)"
run "$blog" deploy
expect_rc 0 "a VM-only app's deploy"
out_has "blog joined vm group g9: app=blog port=8701 service=cbase-blog.service mem=64" "the next port"
out_has "  shop: memory not declared, counted as 128M" "a member's undeclared memory is said"
out_has "  memory: blog 64, shop ~128 = 192 MiB of the 320 MiB for apps" "two members fit"
since "$m" | grep -q "params=VmOrigin=ec2-i-0g9.compute-1.amazonaws.com,VmPort=8701,OriginSecret=(the ssm value) " || fail "blog's stack lacks its port"
since "$m" | grep -q "^PUT /g9-group-bucket/" && fail "a VM-only app uploaded files"
script_of | awk "/^cat > .*<< 'EOF'\$/{f=1; next} /^EOF\$/{f=0} f" > "$t/unit"
grep -Fxq "MemoryMax=64M" "$t/unit" || { cat "$t/unit" >&2; fail "a declared app's unit lacks MemoryMax=64M"; }
grep -Fxq "Environment=CBASE_LISTEN=:::8701" "$t/unit" || fail "blog's unit is not on its port"
template_of cbase-app-blog | grep -q "TargetOriginId: vm" || fail "blog's distribution does not route to the VM"
ok
# A redeploy keeps the port, and the artifact already there is not sent again.
m="$(mark)"
run "$blog" deploy
expect_rc 0 "a VM app's redeploy"
out_has "blog is in vm group g9: app=blog port=8701" "a redeploy keeps its port"
out_has "artifact: s3://$BUCKET/artifacts/blog/$sha is already this server" "an artifact is not sent twice"
[ "$(puts_to "$m" "$BUCKET/artifacts/")" = 0 ] || fail "a redeploy uploaded the same artifact again"
ok

fi
if wanted prune-binaries; then
# ================================================================================================
# prune-binaries: the old server binaries of a VM app (on by default), after the deploy has succeeded
# ================================================================================================
# blog's artifacts folder holds sha256-named binaries. The fake dates an object by its put, so the order
# of the puts below is the order of LastModified, and it is NOT the order of the names.
sha="$(shasum -a 256 "$t/elf-arm64" | cut -d' ' -f1)"
A=artifacts/blog
cur="$A/$sha"
hexs() { printf '%064d' 0 | tr 0 "$1"; }   # hexs C: 64 times the hex digit C
put_obj() { printf 'theirs\n' > "$t/seed"; aws s3api put-object --bucket "$BUCKET" --key "$1" --body "$t/seed" > /dev/null; }
has_obj() { aws s3api head-object --bucket "$BUCKET" --key "$1" > /dev/null 2>&1; }
deleted() { since "$1" | grep '^DELETE ' | sed 's/ 204$//; s|^DELETE /[^/]*/||' | sort; }   # the keys deleted since MARK
aws s3api head-object --bucket "$BUCKET" --key "$cur" > /dev/null || fail "blog's current binary is not in its folder"
put_obj "$A/$(hexs c)"; put_obj "$A/$(hexs a)"; put_obj "$A/$(hexs d)"; put_obj "$A/$(hexs b)"   # oldest to newest: c a d b
# --no-prune and CBASE_PRUNE=0: nothing is deleted.
m="$(mark)"
run "$blog" deploy --no-prune
expect_rc 0 "a VM deploy with --no-prune"
[ -z "$(deleted "$m")" ] || fail "--no-prune deleted a binary"
grep -q "server binar" "$t/out" && fail "--no-prune printed a server prune"
since "$m" | grep -q "LIST prefix=$A/" && fail "--no-prune listed the artifacts folder"
m="$(mark)"
rc=0
(cd "$blog" && CBASE_PRUNE=0 "$tool" deploy) > "$t/out" 2> "$t/err" || rc=$?
expect_rc 0 "a VM deploy with CBASE_PRUNE=0"
[ -z "$(deleted "$m")" ] || fail "CBASE_PRUNE=0 deleted a binary"
for k in a b c d; do has_obj "$A/$(hexs $k)" || fail "a binary is gone without a prune"; done
ok
# --dry-run: the plan, no deletion, no put. A dry run makes no build, so the binary it would ship is unknown:
# the newest one is the previous, and the current one is listed as prunable too (the summary says why).
m="$(mark)"
run "$blog" deploy --dry-run
expect_rc 0 "a dry run with the default prune"
for k in "$cur" "$A/$(hexs c)" "$A/$(hexs a)" "$A/$(hexs d)"; do out_has "would prune s3://$BUCKET/$k" "a dry run's plan"; done
out_has "would prune 4 old server binaries under s3://$BUCKET/$A/ (keeping the build's own binary, not made in a dry run, and $A/$(hexs b))" "a dry run's summary"
grep -q "would prune s3://$BUCKET/$A/$(hexs b)" "$t/out" && fail "a dry run would prune the newest binary"
since "$m" | grep -qE '^DELETE |^PUT ' && fail "a dry run deleted or put"
for k in a b c d; do has_obj "$A/$(hexs $k)" || fail "a dry run deleted a binary"; done
m="$(mark)"
run "$blog" deploy --dry-run --no-prune
expect_rc 0 "a dry run with --no-prune"
grep -q "would prune" "$t/out" && fail "a dry run with --no-prune printed a plan"
ok
# The default deploy: the current binary (older than all of these) and the newest other (b) stay, the three
# oldest others (c, a, d) go: not the largest names, not the smallest.
m="$(mark)"
run "$blog" deploy
expect_rc 0 "a VM deploy that prunes"
printf '%s\n%s\n%s\n' "$A/$(hexs a)" "$A/$(hexs c)" "$A/$(hexs d)" > "$t/want"
deleted "$m" | diff -u "$t/want" - || fail "the prune deleted other than the three oldest others"
for k in c a d; do out_has "pruned s3://$BUCKET/$A/$(hexs $k)" "a pruned binary is named"; done
out_has "pruned 3 old server binaries under s3://$BUCKET/$A/ (kept $cur and $A/$(hexs b))" "the prune's summary"
has_obj "$cur" || fail "the prune deleted the current binary"
has_obj "$A/$(hexs b)" || fail "the prune deleted the newest other binary (the rollback target)"
ok
# A rerun has nothing to delete: the summary says 0.
m="$(mark)"
run "$blog" deploy
expect_rc 0 "a VM deploy with nothing to prune"
out_has "pruned 0 old server binaries under s3://$BUCKET/$A/ (kept $cur and $A/$(hexs b))" "a prune of nothing"
[ -z "$(deleted "$m")" ] || fail "a prune of nothing deleted"
ok
# --prune is still accepted (it is the default), and CBASE_PRUNE=yes is not "0": still on.
put_obj "$A/$(hexs e)"
m="$(mark)"
run "$blog" deploy --prune
expect_rc 0 "a VM deploy with --prune"
printf '%s\n' "$A/$(hexs b)" | diff -u - <(deleted "$m") || fail "--prune did not prune (the newest other is e now, b goes)"
put_obj "$A/$(hexs f)"
m="$(mark)"
rc=0
(cd "$blog" && CBASE_PRUNE=yes "$tool" deploy) > "$t/out" 2> "$t/err" || rc=$?
expect_rc 0 "a VM deploy with CBASE_PRUNE=yes"
printf '%s\n' "$A/$(hexs e)" | diff -u - <(deleted "$m") || fail "CBASE_PRUNE=yes turned the prune off"
ok
# Nothing that is not a binary of the app's folder is ever deleted: not a sidecar, not a name of another length or
# case, not a nested key, not another prefix or app, even when they are the newest objects there (a filter dropped
# would make one the kept "previous" and delete a real binary). A listing that answers the whole bucket is named.
put_obj "$A/$(hexs 9)"
put_obj "$A/$(hexs f).sha256"; put_obj "$A/notes.txt"; put_obj "$A/$(hexs 1 | cut -c2-)"; put_obj "$A/$(hexs 1)0"
put_obj "$A/$(hexs F)"; put_obj "$A/sub/$(hexs 2)"; put_obj "artifacts/blogger/$(hexs 3)"; put_obj "artifacts/shop/$(hexs 4)"
put_obj "other/$(hexs 5)"
fake_set list-ignores-prefix=1
m="$(mark)"
run "$blog" deploy
expect_rc 0 "a prune over a listing of the whole bucket"
fake_set list-ignores-prefix=0
printf '%s\n' "$A/$(hexs f)" | diff -u - <(deleted "$m") || fail "the prune deleted other than the one old binary"
for k in "artifacts/blogger/$(hexs 3)" "artifacts/shop/$(hexs 4)" "other/$(hexs 5)"; do
  err_has "prune: s3://$BUCKET/$k is outside $A/, and is left alone" "the folder guard"
done
for k in "$A/$(hexs f).sha256" "$A/notes.txt" "$A/$(hexs 1 | cut -c2-)" "$A/$(hexs 1)0" "$A/$(hexs F)" "$A/sub/$(hexs 2)" \
         "artifacts/blogger/$(hexs 3)" "artifacts/shop/$(hexs 4)" "other/$(hexs 5)" "$A/$(hexs 9)" "$cur"; do
  has_obj "$k" || fail "the prune deleted s3://$BUCKET/$k"
done
ok
# A deploy that does not get to the end deletes nothing, with the prune on: the binary it leaves is the one a
# rollback needs. A change set that fails; a service that does not start; an execute that fails.
put_obj "$A/$(hexs 6)"; put_obj "$A/$(hexs 7)"
gone_check() { [ -z "$(deleted "$m")" ] || fail "$1 deleted a binary"; { has_obj "$A/$(hexs 9)" && has_obj "$A/$(hexs 6)"; } || fail "$1 left a binary gone"; }
fake_set "cs-fail=Template format error: Unresolved resource dependencies Nope in the Resources block of the template"
m="$(mark)"
run "$blog" deploy
expect_rc 1 "a change set that fails, with the prune on"
gone_check "a failed change set"
fake_set cs-fail=
fake_set "ssm-status=Failed&ssm-code=1&ssm-out=CBASE: done (the fake's answer)%0A"
m="$(mark)"
run "$blog" deploy
expect_rc 1 "a service that does not start, with the prune on"
gone_check "a service that did not start"
fake_set "ssm-status=Success&ssm-code=0&ssm-out=CBASE: done (the fake's answer)%0A"
fake_set "changes=replace-distribution&fail-execute=1"
m="$(mark)"
run "$blog" deploy --allow-downtime
expect_rc 1 "an execute that fails, with the prune on"
since "$m" | grep -q "CFN ExecuteChangeSet cbase-app-blog" || fail "the failing deploy never reached the execute"
gone_check "a failed execute"
fake_set "changes=auto&fail-execute=0"
ok
# A deploy that executes the stack prunes only after it: the first delete follows the execute in the log.
m="$(mark)"
fake_set "changes=replace-distribution"
run "$blog" deploy --allow-downtime
expect_rc 0 "a deploy that executes the stack and prunes"
fake_set "changes=auto"
since "$m" | grep -nE "^CFN ExecuteChangeSet cbase-app-blog|^DELETE " | cut -c1-40 > "$t/order"
{ [ "$(grep -c 'ExecuteChangeSet' "$t/order")" = 1 ] && [ "$(grep -c DELETE "$t/order")" -ge 1 ]; } || { cat "$t/order" >&2; fail "the executing deploy lacks the execute or the prune"; }
head -n 1 "$t/order" | grep -q ExecuteChangeSet || { cat "$t/order" >&2; fail "the prune deleted before the stack was executed"; }
printf '%s\n' "$A/$(hexs 6)" "$A/$(hexs 9)" | sort | diff -u - <(deleted "$m") || fail "the executing deploy pruned other than the old binaries (7 is the newest other)"
ok

fi
if wanted apps; then
# ---- apps: the port, the service and the memory ------------------------------------------------
run "$t" apps g9
expect_rc 0 "apps g9"
{
  echo "vm group g9 (s3://$BUCKET/vm/g9.txt): 2 apps"
  printf '  %-24s %-5s %-34s %s\n' app port service mem blog 8701 cbase-blog.service 64M shop 8700 cbase-shop.service "~128 counted"
  echo "static group g9 (s3://$BUCKET/static/g9.txt): 1 app"
  printf '  %-24s %s\n' app folder shop shop/
} > "$t/want"
diff -u "$t/want" "$t/out" || fail "apps g9 printed another table"
ok

fi
if wanted memory; then
# ---- the memory fit, refused -----------------------------------------------------------------
# t4g.nano leaves 320 MiB for apps; shop (~128) and blog (64) hold 192, so 256 more does not fit.
big="$(vm_app big "CBASE_APP = big" "CBASE_VM = g9" "CBASE_STATIC = none" "CBASE_MEMORY = 256M")"
m="$(mark)"
run "$big" deploy
expect_rc 1 "an app that does not fit"
err_has "  shop: memory not declared, counted as 128M" "the refusal says shop is counted at 128M"
err_has "  memory: big 256, blog 64, shop ~128 = 448 MiB of the 320 MiB for apps (t4g.nano: 512 MiB less 192 for the OS)" "the refusal names the members and their numbers"
err_has "cbase: big does not fit on vm group g9" "the fit refusal"
since "$m" | grep -qE "^PUT /$BUCKET/(vm/|artifacts/)|^SSM SendCommand|^CFN CreateChangeSet" && fail "a deploy that does not fit changed something"
state_of vm g9 | grep -q "app=big" && fail "a deploy that does not fit joined"
ok
# A bigger instance has room: the same app fits on a t4g.small (2048 MiB).
fake_set instance-type=t4g.small
rm -rf "$big/dist"
m="$(mark)"
run "$big" deploy --dry-run
expect_rc 0 "an app that fits a bigger instance (dry run)"
out_has "  memory: big 256, blog 64, shop ~128 = 448 MiB of the 1856 MiB for apps (t4g.small: 2048 MiB less 192 for the OS)" "the fit on a t4g.small"
out_has "vm: would join vm group g9 as app=big port=8702 service=cbase-big.service mem=256" "a dry run says the port it would claim"
[ ! -e "$big/dist" ] || fail "a dry run built the server"
state_of vm g9 | grep -q "app=big" && fail "a dry run joined"
# Strictly read-only but for its own change set: no S3 write (no state file, no artifact), no SSM
# command, no executed change set; the change set it made is deleted.
since "$m" | grep -E "^PUT /|^DELETE /|^SSM SendCommand|^CFN ExecuteChangeSet|^CF CreateInvalidation" && fail "a VM dry run changed something"
since "$m" | grep -q "^CFN DeleteChangeSet cbase-app-big ok" || fail "a VM dry run left its change set"
fake_set instance-type=t4g.nano
ok
# Exactly the room left fits: shop (~128) and blog (64) leave 128 of the 320 MiB.
exact="$(vm_app exact "CBASE_APP = exact" "CBASE_VM = g9" "CBASE_STATIC = none" "CBASE_MEMORY = 128M")"
run "$exact" deploy --dry-run
expect_rc 0 "an app that fills the instance exactly (dry run)"
out_has "  memory: blog 64, exact 128, shop ~128 = 320 MiB of the 320 MiB for apps" "an exact fit fits"
ok

fi
if wanted taken; then
# ---- the port taken on the VM: refused, the claim released ------------------------------------
taken="$(vm_app taken "CBASE_APP = taken" "CBASE_VM = g9" "CBASE_STATIC = none" "CBASE_MEMORY = 16M")"
fake_set "ssm-status=Failed&ssm-code=3&ssm-out=CBASE: port 8702 is in use on the VM%2C and not by cbase-taken.service: refused%2C nothing was started%0A"
m="$(mark)"
run "$taken" deploy
expect_rc 1 "a port taken on the VM"
out_has "vm: port 8702 is in use on the VM, and not by cbase-taken.service" "the VM's refusal is printed"
out_has "taken left vm group g9 (it held: app=taken port=8702" "the claim is released"
err_has "port 8702 is taken on i-0g9 by another process, so taken was not started, and its claim on the port is released" "a port taken on the VM"
state_of vm g9 | grep -q "app=taken" && fail "a refused port is still claimed"
since "$m" | grep -q "CFN DeleteStack cbase-app-taken" || fail "the refused deploy left its change set's empty stack"
since "$m" | grep -q "CFN ExecuteChangeSet" && fail "a refused deploy executed its change set"
fake_set "ssm-status=Success&ssm-code=0&ssm-out=CBASE: done (the fake's answer)%0A"
ok
# The port taken on the VM for an app that was a member before this deploy: refused, its claim kept.
fake_set "ssm-status=Failed&ssm-code=3&ssm-out=CBASE: port 8701 is in use on the VM%0A"
run "$blog" deploy
expect_rc 1 "a member's port taken on the VM"
err_has "port 8701 is taken on i-0g9 by another process, so blog was not started; it keeps its port (it was a member before this deploy)" "a member keeps its port"
state_of vm g9 | grep -q "^app=blog port=8701 " || fail "a member's port was released"
ok
# An undeploy whose command fails: it says so and keeps the app's membership (its port).
fake_set "ssm-status=Failed&ssm-code=1&ssm-out=CBASE: done (the fake's answer)%0A"
run "$blog" undeploy
expect_rc 1 "an undeploy whose command fails"
err_has "the undeploy of blog on i-0g9 ended Failed (exit 1); it still holds its port, and nothing else was changed" "a failed undeploy"
state_of vm g9 | grep -q "^app=blog port=8701 " || fail "a failed undeploy left the group"
fake_set "ssm-status=Success&ssm-code=0"
ok

fi
if wanted service; then
# ---- the summary's service line: the script's last line about the app's own unit ---------------
svc="$(vm_app svcsay "CBASE_APP = svcsay" "CBASE_VM = g9" "CBASE_STATIC = none" "CBASE_MEMORY = 16M")"
for case_ in "restarted (server changed)" "started (it was not active)" "unchanged: not restarted"; do
  fake_set "ssm-out=CBASE: cbase-svcsay.service restarted (units changed)%0ACBASE: cbase-svcsay.service $case_%0ACBASE: cbase-svcsaz.service restarted (settings changed)%0ACBASE: cbase-svcsay.service is up: GET /health answered 200 on 127.0.0.1:8703%0A"
  run "$svc" deploy
  expect_rc 0 "a deploy whose script says [$case_]"
  out_has "service: cbase-svcsay.service $case_" "the summary's service line is the script's last line about the unit [$case_]"
done
run "$svc" undeploy
expect_rc 0 "svcsay's undeploy"
fake_set "ssm-status=Success&ssm-code=0&ssm-out=CBASE: done (the fake's answer)%0A"
ok
fi
if wanted refused; then
# ---- refused before anything changes: no group, a failed build, a binary for another machine ---
ghost="$(vm_app ghost "CBASE_APP = ghost" "CBASE_VM = nosuch" "CBASE_STATIC = none")"
run "$ghost" deploy
expect_rc 1 "a missing vm group"
err_has "there is no vm group named nosuch (nothing tagged cbase:vm-group=nosuch in us-east-1). Create it with: ./cbase group-init vm nosuch" "a missing vm group"
ok
broken="$(app broken "CBASE_APP = broken" "CBASE_VM = g9" "CBASE_STATIC = none")"
printf 'serve-binary:\n\t@echo "make bootstrap: needs Docker (an arm64 Alpine container builds the static binary)." >&2; exit 1\n' > "$broken/Makefile"
m="$(mark)"
run "$broken" deploy
expect_rc 1 "a failed build"
err_has "make -C . serve-binary failed, so there is no linux-arm64 server to ship. It is cbase.mk's target for this build (ops/build_static.sh serve), and it needs Docker" "a failed build"
since "$m" | grep -qE "^PUT |^SSM SendCommand|^CFN CreateChangeSet" && fail "a failed build changed something"
ok
mkdir -p "$broken/server" && mv "$broken/Makefile" "$broken/server/Makefile"
printf 'serve-binary:\n\tmkdir -p dist && cp %s dist/cbase-serve-linux-arm64\n' "$t/elf-x86" > "$broken/server/Makefile"
m="$(mark)"
run "$broken" deploy
expect_rc 1 "an x86 server"
out_has "server: make -C server serve-binary" "the server's Makefile is found in server/"
err_has "server/dist/cbase-serve-linux-arm64 is not an arm64 (aarch64) Linux binary" "an x86 server"
since "$m" | grep -qE "^PUT |^SSM SendCommand" && fail "an x86 server changed something"
ok
rc=0
(cd "$broken" && CBASE_SERVE_BIN="$t/elf-arm64" "$tool" deploy --dry-run) < /dev/null > "$t/out" 2> "$t/err" || rc=$?
expect_rc 0 "CBASE_SERVE_BIN names the server"
ok
# An instance that is not arm64 (the fake's t3.nano is x86_64): refused before anything is built.
fake_set instance-type=t3.nano
m="$(mark)"
run "$blog" deploy
expect_rc 1 "an x86 instance"
err_has "vm group g9's instance i-0g9 is x86_64, and cbase builds the server for linux arm64 alone" "an x86 instance"
since "$m" | grep -qE "^PUT |^SSM SendCommand|^CFN CreateChangeSet" && fail "a deploy to an x86 instance changed something"
fake_set instance-type=t4g.nano
ok

fi
if wanted race; then
# ---- two racing VM deploys: distinct ports -----------------------------------------------------
r1="$(vm_app r1 "CBASE_APP = r1" "CBASE_VM = g9" "CBASE_STATIC = none" "CBASE_MEMORY = 16M")"
r2="$(vm_app r2 "CBASE_APP = r2" "CBASE_VM = g9" "CBASE_STATIC = none" "CBASE_MEMORY = 16M")"
curl -fsS -X POST "$AWS_ENDPOINT_URL/_fake/gather?n=2" > /dev/null
before412="$(log_count "PUT /$BUCKET/vm/g9.txt 412")"
rc1=0; rc2=0
(cd "$r1" && "$tool" deploy) < /dev/null > "$t/out1" 2> "$t/err1" & p1=$!
(cd "$r2" && "$tool" deploy) < /dev/null > "$t/out2" 2> "$t/err2" & p2=$!
wait "$p1" || rc1=$?
wait "$p2" || rc2=$?
if [ "$rc1" != 0 ] || [ "$rc2" != 0 ]; then cat "$t/out1" "$t/err1" "$t/out2" "$t/err2" >&2; fail "racing deploys: status $rc1 and $rc2"; fi
grep -q "GATHER timed out" "$t/log" && fail "the racing deploys did not read the state file at once"
[ "$(( $(log_count "PUT /$BUCKET/vm/g9.txt 412") - before412 ))" = 1 ] || fail "racing deploys: not exactly one 412"
p_1="$(sed -n 's/^r1 joined vm group g9: app=r1 port=\([0-9]*\) .*/\1/p' "$t/out1")"
p_2="$(sed -n 's/^r2 joined vm group g9: app=r2 port=\([0-9]*\) .*/\1/p' "$t/out2")"
{ [ -n "$p_1" ] && [ -n "$p_2" ] && [ "$p_1" != "$p_2" ]; } || fail "racing deploys got ports [$p_1] and [$p_2]"
[ "$(printf '%s\n' "$p_1" "$p_2" | sort | tr '\n' ' ')" = "8702 8703 " ] || fail "racing deploys did not get the two lowest free ports"
{ grep -q "VmPort=$p_1" "$t/log" && grep -q "VmPort=$p_2" "$t/log"; } || fail "the racing deploys' stacks lack their ports"
ok

fi
if wanted secrets; then
# ---- no secret in any output -------------------------------------------------------------------
# The command's output and its stderr carry a setting's value (as an app's own output could): only
# the script's CBASE lines are printed.
fake_set "secret=$SECRET&secret2=$SECRET2"
run "$blog" deploy
expect_rc 0 "a deploy whose command output holds secrets"
no_secret "a deploy's command output"
fake_set "ssm-status=Failed&ssm-code=4"
run "$blog" deploy
expect_rc 1 "a deploy that fails with secrets in its output"
err_has "the service's own log is on the instance: journalctl -u cbase-blog" "a failed start points at the journal"
no_secret "a failed deploy's command output"
fake_set "ssm-status=Success&ssm-code=0&secret=&secret2="
ok

fi
if wanted scripts; then
# ================================================================================================
# the scripts SSM runs, run here under a scratch root
# ================================================================================================
# Stand-ins for what the instance has: systemctl and useradd log their calls; ss answers a listener
# on the port when STUB_TAKEN=1; systemctl is-active answers yes when STUB_ACTIVE=1; aws hands over
# the artifact and one SSM parameter holding a secret; curl answers /health with 200.
stubs="$t/stubs"
mkdir -p "$stubs"
cat > "$stubs/systemctl" << 'EOF'
#!/bin/bash
echo "systemctl $*" >> "$STUB_LOG"
# show: systemd's loaded units are the files (NeedDaemonReload no), and no InvocationID.
if [ "$1" = show ]; then case "$*" in *NeedDaemonReload*) echo no ;; esac; exit 0; fi
[ "$1" != is-active ] || [ "${STUB_ACTIVE:-0}" = 1 ] || { [ -n "${STUB_ACTIVE_UNIT:-}" ] && [ "${!#}" = "$STUB_ACTIVE_UNIT" ]; }
EOF
cat > "$stubs/ss" << 'EOF'
#!/bin/bash
[ "${STUB_TAKEN:-0}" != 1 ] || echo "LISTEN 0 4096 0.0.0.0:${STUB_PORT} 0.0.0.0:* users:((\"python3\",pid=7,fd=3))"
EOF
cat > "$stubs/curl" << 'EOF'
#!/bin/bash
# The instance's curl: IMDSv2 (IPv6 unless STUB_NO_IMDS6=1), the artifact from S3, one SSM
# parameter holding a secret (GetParametersByPath, in two pages), and GET /health 200.
out=""; url=""; w=""; data=""
while [ $# -gt 0 ]; do
  case "$1" in
    -o) out="$2"; shift ;; -w) w="$2"; shift ;; --data) data="$2"; shift ;;
    -H|-X|--user|--aws-sigv4|--max-time|--retry) shift ;;
    -*) ;;
    *) url="$1" ;;
  esac
  shift
done
echo "curl $url" >> "$STUB_LOG"
case "$url" in
  'http://[fd00:ec2::254]'*) [ "${STUB_NO_IMDS6:-0}" != 1 ] || exit 7 ;;
esac
case "$url" in
  */latest/api/token) echo tok ;;
  */iam/security-credentials/) echo role ;;
  */iam/security-credentials/role) echo '{"AccessKeyId":"AKIASTUBSTUBSTUB0000","SecretAccessKey":"stub","Token":"stub"}' ;;
  *.s3.dualstack.*) cp "$STUB_ELF" "$out" ;;
  https://ssm.*.api.aws/)
    case "$data" in
      *NextToken*) printf '{"Parameters":[{"Name":"%sDB_PASSWORD","Value":"%s"}]}\n' "$STUB_SETTINGS" "$STUB_SECRET" ;;
      *) printf '{"Parameters":[],"NextToken":"p2"}\n' ;;
    esac ;;
  */health) [ -z "$w" ] || printf 200 ;;
  *) exit 22 ;;
esac
EOF
cat > "$stubs/sha256sum" << 'EOF'
#!/bin/bash
[ "${1:-}" = -c ] || exec shasum -a 256 "$@"
read -r want file
[ "$(shasum -a 256 "$file" | cut -d' ' -f1)" = "$want" ]
EOF
printf '#!/bin/bash\n[ "${1:-}" != -u ] || case "${2:-}" in cbase-*) [ -f "$STUB_USERS/$2" ] ;; *) exit 0 ;; esac\n' > "$stubs/id"
printf '#!/bin/bash\necho "useradd $*" >> "$STUB_LOG"; touch "$STUB_USERS/${!#}"\n' > "$stubs/useradd"
printf '#!/bin/bash\necho "userdel $*" >> "$STUB_LOG"; rm -f "$STUB_USERS/$1"\n' > "$stubs/userdel"
printf '#!/bin/bash\necho "chown $*" >> "$STUB_LOG"\n' > "$stubs/chown"
printf '#!/bin/bash\n:\n' > "$stubs/journalctl"
chmod +x "$stubs"/*
# on_vm ROOT SCRIPT: the script as SSM runs it, under ROOT; stdout in $t/vm-out, status in $rc.
on_vm() {
  mkdir -p "$1/srv/cbase" "$1/etc/systemd/system" "$1/users"
  : > "$1/calls"
  rc=0
  env PATH="$stubs:$PATH" CBASE_VM_ROOT="$1" STUB_LOG="$1/calls" STUB_USERS="$1/users" STUB_ELF="$t/elf-arm64" \
      STUB_APP=shop STUB_PORT="${STUB_PORT:-8700}" STUB_SETTINGS="${STUB_SETTINGS:-/cbase/shop/}" STUB_SECRET="${STUB_SECRET:-$SECRET}" bash "$2" > "$t/vm-out" 2>&1 < /dev/null || rc=$?
}
grep '^SSM SendCommand .*\[cbase deploy of shop\]' "$t/log" | tail -1 | sed -n 's/.*echo \([A-Za-z0-9+/=]*\) | base64 -d.*/\1/p' | base64 -d > "$t/shop-deploy.sh"
[ -s "$t/shop-deploy.sh" ] || fail "no deploy script of shop's in the fake's log"
# The first start, the port free: the user, the folders, the binary, the env file, the unit, and
# that one unit restarted.
vm="$t/vm1"
on_vm "$vm" "$t/shop-deploy.sh"
[ "$rc" = 0 ] || { cat "$t/vm-out" >&2; fail "the deploy script on a free port: status $rc"; }
grep -Fq "CBASE: cbase-shop.service is up: GET /health answered 200 on 127.0.0.1:8700" "$t/vm-out" || { cat "$t/vm-out" >&2; fail "the deploy script's health line"; }
grep -Fq "CBASE: settings: 1 from SSM /cbase/shop/ (values not shown)" "$t/vm-out" || fail "the deploy script's settings line"
grep -Fq "$SECRET" "$t/vm-out" && fail "the deploy script printed a setting's value"
[ "$(cat "$vm/etc/cbase/shop.env")" = "DB_PASSWORD=$SECRET" ] || fail "the env file is not the SSM parameter"
cmp -s "$vm/srv/cbase/shop/serve" "$t/elf-arm64" || fail "the server was not installed"
[ -d "$vm/srv/cbase/shop/data" ] || fail "the data folder was not made"
awk "/^cat > .*<< 'EOF'\$/{f=1; next} /^EOF\$/{f=0} f" "$t/shop-deploy.sh" | diff -u - "$vm/etc/systemd/system/cbase-shop.service" \
  || fail "the installed unit is not the one the deploy rendered"
awk "/^cat > .*<< 'SOCKET'\$/{f=1; next} /^SOCKET\$/{f=0} f" "$t/shop-deploy.sh" | diff -u - "$vm/etc/systemd/system/cbase-shop.socket" \
  || fail "the installed socket unit is not the one the deploy rendered"
grep -Fxq "systemctl start cbase-shop.socket" "$vm/calls" || fail "a first deploy did not start the socket"
grep -q "^useradd .*cbase-shop$" "$vm/calls" || fail "the app's user was not made"
grep -Fxq "chown -R cbase-shop:cbase-shop $vm/srv/cbase/shop/data" "$vm/calls" || fail "the data folder is not the service's"
if [ "$(grep -c '^systemctl restart' "$vm/calls")" != 1 ] || ! grep -Fxq "systemctl restart cbase-shop.service" "$vm/calls"; then
  cat "$vm/calls" >&2
  fail "the deploy script restarted other than cbase-shop.service"
fi
ok
# The port taken by another process before a first start: exit 3, nothing installed or started.
vm="$t/vm2"
STUB_TAKEN=1 on_vm "$vm" "$t/shop-deploy.sh"
[ "$rc" = 3 ] || { cat "$t/vm-out" >&2; fail "the deploy script on a taken port: status $rc, not 3"; }
grep -Fq "CBASE: port 8700 is in use on the VM, and not by cbase-shop.service: refused, nothing was started" "$t/vm-out" || fail "the taken port's line"
grep -q "^systemctl restart" "$vm/calls" && fail "the deploy script started a service on a taken port"
[ ! -e "$vm/etc/systemd/system/cbase-shop.service" ] || fail "the deploy script installed a unit on a taken port"
ok
# RESTART ONLY FOR A CHANGE. A redeploy with nothing changed, the service active (the unit running
# holds the port, which is no refusal): nothing restarted, reloaded or rewritten, the wait still run.
vm="$t/vm1"
files() { ls -i "$vm/srv/cbase/shop/serve" "$vm/etc/cbase/shop.env" "$vm/etc/systemd/system/cbase-shop.service" "$vm/etc/systemd/system/cbase-shop.socket"; }
files > "$t/inodes"
STUB_ACTIVE=1 STUB_TAKEN=1 on_vm "$vm" "$t/shop-deploy.sh"
[ "$rc" = 0 ] || { cat "$t/vm-out" >&2; fail "the deploy script on an unchanged redeploy: status $rc"; }
grep -Eq '^systemctl (start|stop|restart|daemon-reload)' "$vm/calls" && { cat "$vm/calls" >&2; fail "an unchanged redeploy started, stopped, restarted or reloaded"; }
for want in "CBASE: server unchanged, units unchanged, settings unchanged" "CBASE: cbase-shop.service unchanged: not restarted" \
            "CBASE: cbase-shop.service is up: GET /health answered 200 on 127.0.0.1:8700"; do
  grep -Fxq "$want" "$t/vm-out" || { cat "$t/vm-out" >&2; fail "an unchanged redeploy lacks [$want]"; }
done
files | diff -u "$t/inodes" - || fail "an unchanged redeploy rewrote a file"
ok
# Each of what serve has loaded, changed alone, restarts the service. The installed server is not
# the artifact:
printf 'old\n' >> "$vm/srv/cbase/shop/serve"
STUB_ACTIVE=1 STUB_TAKEN=1 on_vm "$vm" "$t/shop-deploy.sh"
[ "$rc" = 0 ] || { cat "$t/vm-out" >&2; fail "a redeploy of a new server: status $rc"; }
grep -E '^systemctl (start|stop|restart)' "$vm/calls" | diff -u <(echo "systemctl restart cbase-shop.service") - \
  || fail "a new server did other than restart the service alone (the socket holds the port)"
grep -Fxq "CBASE: cbase-shop.service restarted (server changed)" "$t/vm-out" || { cat "$t/vm-out" >&2; fail "a new server's restart line"; }
cmp -s "$vm/srv/cbase/shop/serve" "$t/elf-arm64" || fail "a new server was not installed"
ok
# A setting's value changed in SSM: restarted, and neither value printed.
STUB_SECRET="$SECRET-rotated" STUB_ACTIVE=1 STUB_TAKEN=1 on_vm "$vm" "$t/shop-deploy.sh"
[ "$rc" = 0 ] || { cat "$t/vm-out" >&2; fail "a redeploy of a changed setting: status $rc"; }
grep -Fxq "systemctl restart cbase-shop.service" "$vm/calls" || fail "a changed setting did not restart the service"
grep -Fxq "CBASE: cbase-shop.service restarted (settings changed)" "$t/vm-out" || { cat "$t/vm-out" >&2; fail "a changed setting's restart line"; }
grep -Fq "$SECRET" "$t/vm-out" && fail "a changed setting's redeploy printed a value"
[ "$(cat "$vm/etc/cbase/shop.env")" = "DB_PASSWORD=$SECRET-rotated" ] || fail "a changed setting was not written"
ok
# The installed unit is not the one this deploy renders: reloaded, then restarted.
printf '# edited\n' >> "$vm/etc/systemd/system/cbase-shop.service"
STUB_SECRET="$SECRET-rotated" STUB_ACTIVE=1 STUB_TAKEN=1 on_vm "$vm" "$t/shop-deploy.sh"
[ "$rc" = 0 ] || { cat "$t/vm-out" >&2; fail "a redeploy of a changed unit: status $rc"; }
[ "$(grep -E '^systemctl (daemon-reload|restart)' "$vm/calls" | tr '\n' '|')" = "systemctl daemon-reload|systemctl restart cbase-shop.service|" ] \
  || { cat "$vm/calls" >&2; fail "a changed unit was not reloaded, then restarted"; }
grep -Fxq "CBASE: cbase-shop.service restarted (units changed)" "$t/vm-out" || { cat "$t/vm-out" >&2; fail "a changed unit's restart line"; }
ok
# Nothing changed, the service not active (failed or stopped): started.
STUB_SECRET="$SECRET-rotated" on_vm "$vm" "$t/shop-deploy.sh"
[ "$rc" = 0 ] || { cat "$t/vm-out" >&2; fail "a redeploy of an inactive service: status $rc"; }
grep -Fxq "systemctl restart cbase-shop.service" "$vm/calls" || fail "an inactive service was not started"
grep -Fxq "CBASE: cbase-shop.service started (it was not active)" "$t/vm-out" || { cat "$t/vm-out" >&2; fail "an inactive service's start line"; }
ok
fi
if wanted undeploy; then
# Undeploy, by default: the unit gone, the data kept. With --delete-data: the app's folder gone.
printf 'precious\n' > "$t/vm1/srv/cbase/shop/data/cbase.sqlite"
m="$(mark)"
run "$shop" undeploy
expect_rc 0 "an undeploy"
script_of > "$t/shop-undeploy.sh"
mkdir -p "$t/vm1/etc/cbase" && printf 'server abc\n' > "$t/vm1/etc/cbase/shop.loaded"
on_vm "$t/vm1" "$t/shop-undeploy.sh"
[ "$rc" = 0 ] || { cat "$t/vm-out" >&2; fail "the undeploy script: status $rc"; }
[ ! -e "$t/vm1/etc/cbase/shop.loaded" ] || fail "the undeploy script left the stamp of what serve loaded"
grep -Fxq "systemctl disable --now --quiet cbase-shop.service" "$t/vm1/calls" || fail "the undeploy script did not stop and disable the unit"
grep -Fxq "systemctl disable --now --quiet cbase-shop.socket" "$t/vm1/calls" || fail "the undeploy script did not stop and disable the socket"
[ ! -e "$t/vm1/etc/systemd/system/cbase-shop.service" ] || fail "the undeploy script left the unit"
[ ! -e "$t/vm1/etc/systemd/system/cbase-shop.socket" ] || fail "the undeploy script left the socket unit"
[ "$(cat "$t/vm1/srv/cbase/shop/data/cbase.sqlite")" = precious ] || fail "the undeploy script did not keep the data"
[ ! -e "$t/vm1/etc/cbase/shop.env" ] || fail "the undeploy script left the env file"
grep -Fq "CBASE: kept /srv/cbase/shop/data (./cbase undeploy --delete-data deletes it)" "$t/vm-out" || fail "the undeploy script does not say the data is kept"
ok
out_has "undeploy: kept s3://g9-group-bucket/shop/ (./cbase undeploy --delete-files deletes it)" "undeploy keeps the files"
out_has "shop left vm group g9 (it held: app=shop port=8700" "undeploy releases the port"
out_has "shop left static group g9" "undeploy leaves the static group"
since "$m" | grep -q "CFN DeleteStack cbase-app-shop" || fail "undeploy did not delete the app's stack"
since "$m" | grep -q "^DELETE " && fail "undeploy without --delete-files deleted files"
aws s3api head-object --bucket g9-group-bucket --key shop/index.html > /dev/null 2>&1 || fail "undeploy deleted the app's files"
state_of vm g9 | grep -q "app=shop" && fail "undeploy left shop in the vm group"
state_of static g9 | grep -q "app=shop" && fail "undeploy left shop in the static group"
[ "$(template_of cbase-app-shop)" = "(none)" ] || fail "undeploy left the app's stack"
ok
# --delete-files and --delete-data: the app's folder in the bucket, and its data on the VM.
gone="$(vm_app gone "CBASE_APP = gone" "CBASE_VM = g9" "CBASE_STATIC = g9" "CBASE_MEMORY = 32M")"
run "$gone" deploy
expect_rc 0 "gone's deploy"
m="$(mark)"
run "$gone" undeploy --delete-files --delete-data
expect_rc 0 "an undeploy --delete-files --delete-data"
since "$m" | grep '^DELETE ' > "$t/deleted" || true
printf 'DELETE /g9-group-bucket/gone/index.html 204\n' | diff -u - "$t/deleted" || fail "--delete-files deleted other than gone's folder"
aws s3api head-object --bucket g9-group-bucket --key shop/index.html > /dev/null 2>&1 || fail "--delete-files deleted another app's file"
script_of > "$t/gone-undeploy.sh"
grep -Fq 'rm -rf "$home"' "$t/gone-undeploy.sh" || fail "the --delete-data script does not delete the app's folder"
mkdir -p "$t/vm3/srv/cbase/gone/data" && printf 'x\n' > "$t/vm3/srv/cbase/gone/data/f"
on_vm "$t/vm3" "$t/gone-undeploy.sh"
{ [ "$rc" = 0 ] && [ ! -e "$t/vm3/srv/cbase/gone" ]; } || fail "the --delete-data script left /srv/cbase/gone"
ok
# --delete-data for an app with no VM: nothing to delete, and it says so.
stonly="$(app stonly "CBASE_APP = stonly" "CBASE_VM = none" "CBASE_STATIC = g9")"
run "$stonly" undeploy --delete-data
expect_rc 0 "--delete-data of a static-only app"
out_has "undeploy: --delete-data: stonly has no VM, so it has no data folder" "--delete-data of a static-only app"
ok

fi
if wanted apih; then
# ---- CBASE_API_HOST: its own distribution, and its own alias -----------------------------------
apih="$(vm_app apih "CBASE_APP = apih" "CBASE_VM = g9" "CBASE_STATIC = none" "CBASE_MEMORY = 16M" \
          "CBASE_DOMAIN = apih.example.com" "CBASE_API_HOST = api.example.com")"
m="$(mark)"
run "$apih" deploy
expect_rc 0 "a deploy with CBASE_API_HOST"
out_has "certificate: $CERT_APIH, issued, covers apih.example.com api.example.com" "one certificate covers the domain and the API host"
since "$m" | grep -q "R53 UPSERT Z0EXAMPLE apih.example.com A alias=d" || fail "the domain's alias was not written"
since "$m" | grep -q "R53 UPSERT Z0EXAMPLE api.example.com A alias=api-d" || { since "$m" | grep R53 >&2; fail "the API host's alias is not its own distribution's"; }
ok
# CBASE_API_CERT_ARN: the API host's distribution serves it as it is; the main one's is still found.
printf 'CBASE_API_CERT_ARN = %s\n' "$CERT_API2" >> "$apih/cbase.conf"
run "$apih" deploy
expect_rc 0 "a deploy with CBASE_API_CERT_ARN"
out_has "certificate: $CERT_API2 (CBASE_API_CERT_ARN, used as it is)" "the API certificate is used as named"
out_has "certificate: $CERT_APIH, issued, covers apih.example.com" "the main certificate is found for the domain alone"
template_of cbase-app-apih > "$t/tpl"
awk '/^  ApiDistribution:$/{f=1} f' "$t/tpl" | grep -Fq "AcmCertificateArn: $CERT_API2" || fail "the API distribution does not serve CBASE_API_CERT_ARN"
awk '/^  Distribution:$/{f=1} /^  ApiDistribution:$/{f=0} f' "$t/tpl" | grep -Fq "AcmCertificateArn: $CERT_APIH" || fail "the main distribution does not serve the found certificate"
ok
run "$apih" undeploy
expect_rc 0 "the API host app's undeploy"
out_has "undeploy: the DNS records of apih.example.com api.example.com still point at the deleted distribution" "undeploy names every hostname"
ok

fi
if wanted game; then
# ---- a grandfathered member: its backup key, and the unit it replaces --------------------------
game="$(vm_app game "CBASE_APP = game" "CBASE_VM = g9" "CBASE_STATIC = none" "CBASE_MEMORY = 16M" \
          "CBASE_VM_PORT = 8080" "CBASE_VM_DATA = /var/lib/game" "CBASE_VM_REPLACES = old.service" \
          "CBASE_BACKUP_KEY = cbase/latest.sqlite")"
run "$game" deploy
expect_rc 0 "a grandfathered member's deploy"
script_of > "$t/game-deploy.sh"
# CBASE_BACKUP_KEY: the unit's key and its URL are the conf's (shop's above are the default).
for want in "Environment=CBASE_BACKUP_KEY=cbase/latest.sqlite" "Environment=CBASE_BACKUP_URL=s3://$BUCKET/cbase/latest.sqlite"; do
  grep -Fxq -- "$want" "$t/game-deploy.sh" || fail "the grandfathered unit lacks [$want]"
done
ok
# CBASE_VM_REPLACES: the port the replaced unit holds is no refusal; that one unit alone is
# stopped, disabled and masked (its unit file moved aside first), before the app's own starts.
vm="$t/vm4"
mkdir -p "$vm/etc/systemd/system"; echo "[Unit]" > "$vm/etc/systemd/system/old.service"
STUB_PORT=8080 STUB_TAKEN=1 STUB_ACTIVE_UNIT=old.service on_vm "$vm" "$t/game-deploy.sh"
[ "$rc" = 0 ] || { cat "$t/vm-out" >&2; fail "the deploy that replaces old.service: status $rc"; }
grep -E '^systemctl (stop|disable|kill|mask)' "$vm/calls" > "$t/stops" || true
printf 'systemctl disable --now --quiet old.service\nsystemctl mask --quiet old.service\n' | diff -u - "$t/stops" || fail "the deploy stopped and masked other than old.service alone"
{ [ -f "$vm/etc/systemd/system/old.service.cbase-replaced" ] && [ ! -f "$vm/etc/systemd/system/old.service" ]; } || fail "old.service's unit file was not moved aside before the mask"
[ "$(grep -nE '^systemctl (disable --now --quiet old.service|mask --quiet old.service|start cbase-game.socket|restart cbase-game.service)' "$vm/calls" | cut -d: -f2- | tr '\n' '|')" \
  = "systemctl disable --now --quiet old.service|systemctl mask --quiet old.service|systemctl start cbase-game.socket|systemctl restart cbase-game.service|" ] || { cat "$vm/calls" >&2; fail "old.service was not stopped before cbase-game.service started"; }
ok
run "$game" undeploy
expect_rc 0 "the grandfathered member's undeploy"
ok

fi
if wanted group-destroy; then
# ================================================================================================
# group-destroy
# ================================================================================================
m="$(mark)"
run "$t" group-destroy vm g9 --yes --delete-data
expect_rc 1 "group-destroy of a group with members"
err_has "vm group g9 still has 3 members: blog r1 r2. Undeploy each first" "group-destroy names the members"
since "$m" | grep -qE "CFN DeleteStack|^SSM SendCommand" && fail "group-destroy of a group with members changed something"
ok
for a in "$blog" "$r1" "$r2"; do run "$a" undeploy; expect_rc 0 "an undeploy"; done
# The instance holds shop's kept data: refused without --delete-data.
fake_set "ssm-out=CBASE: data shop%0A"
m="$(mark)"
run "$t" group-destroy vm g9 --yes
expect_rc 1 "group-destroy of an instance holding data"
err_has "vm group g9's instance i-0g9 holds the data of shop (/srv/cbase/<app>/data, kept by undeploy), which goes with the instance: refused" "group-destroy keeps data"
since "$m" | grep -q "CFN DeleteStack" && fail "group-destroy deleted an instance holding data"
ok
# A data check that does not end Success: whether the instance holds data is not known, so refused.
fake_set "ssm-status=Failed&ssm-code=1&ssm-out="
m="$(mark)"
run "$t" group-destroy vm g9 --yes
expect_rc 1 "group-destroy whose data check fails"
err_has "the data check on i-0g9 ended Failed, so whether the instance holds data is not known: nothing was changed (--delete-data destroys it anyway)" "a failed data check"
since "$m" | grep -q "CFN DeleteStack" && fail "group-destroy went on past a failed data check"
fake_set "ssm-status=Success&ssm-code=0&ssm-out=CBASE: data shop%0A"
ok
run "$t" group-destroy vm g9 --delete-data
expect_rc 2 "group-destroy without --yes"
ok
m="$(mark)"
run "$t" group-destroy vm g9 --delete-data --yes
expect_rc 0 "group-destroy --delete-data --yes"
since "$m" | grep -q "CFN DeleteStack cbase-group-vm-g9" || fail "group-destroy did not delete the group's stack"
[ "$(state_of vm g9)" = "(none)" ] || fail "group-destroy left the state file"
fake_set "ssm-out=CBASE: done (the fake's answer)%0A"
run "$blog" deploy
expect_rc 1 "a deploy to a destroyed group"
err_has "Create it with: ./cbase group-init vm g9" "a destroyed group is gone"
ok
# The static group: its bucket kept, with its files, its tag taken off.
m="$(mark)"
run "$t" group-destroy static g9 --yes
expect_rc 0 "group-destroy static"
since "$m" | grep -q "S3 DeleteBucketTagging g9-group-bucket 204" || fail "group-destroy static did not take the bucket's tag off"
aws s3api head-object --bucket g9-group-bucket --key shop/index.html > /dev/null 2>&1 || fail "group-destroy static deleted files"
out_has "bucket: s3://g9-group-bucket is kept with its files" "group-destroy static says the bucket is kept"
ok

fi
if wanted adopted; then
# ================================================================================================
# an adopted group (CBASE_VM_STACK): its derived paths; a dry run with no state bucket yet and no
# origin secret still reaches the memory fit, the port and the change set, then lists the problem
# ================================================================================================
start_fake --resource "aws:cloudformation:stack-name cbase-vm ec2:instance arn:aws:ec2:us-east-1:$ACCOUNT:instance/i-0adopted"
fake_set origin-secret=
amgi="$(vm_app amgi "CBASE_APP = amgi" "CBASE_VM = veteran" "CBASE_VM_STACK = cbase-vm" "CBASE_STATIC = none")"
m="$(mark)"
run "$amgi" deploy --dry-run
expect_rc 1 "an adopted group's dry run with no origin secret"
out_has "vm group veteran: i-0adopted (t4g.nano, 512 MiB), ec2-i-0adopted.compute-1.amazonaws.com, adopted from the stack cbase-vm" "the adopted stack's instance"
out_has "vm paths: settings SSM /cbase-vm/amgi/, servers s3://cbase-code-$ACCOUNT-us-east-1/vm/amgi/" "the paths derived from the adopted group"
out_has "dry run: would create state bucket s3://$BUCKET (us-east-1; versioned, public access blocked) and seed vm/veteran.txt with amgi" "the state bucket a deploy would make"
err_has "there is no SSM parameter /cbase-vm/amgi/CBASE_ORIGIN_SECRET" "the missing secret, under the derived path"
out_has "dry run: a deploy would stop here, on no SSM parameter /cbase-vm/amgi/CBASE_ORIGIN_SECRET (the origin secret); this dry run goes on" "the dry run goes on"
out_has "  memory: amgi ~128 = 128 MiB of the 320 MiB for apps (t4g.nano: 512 MiB less 192 for the OS)" "the fit is still checked"
out_has "vm: would join vm group veteran as app=amgi port=8700 service=cbase-amgi.service mem=~128" "the port it would claim"
out_has "dry run: change set cbase-deploy deleted unexecuted" "the dry run reaches the change set"
err_has "cbase: dry run: a deploy would stop on 2 problems:" "the problems listed at the end"
err_has "  vm group veteran has no origin recorded and amgi names none: set CBASE_VM_ORIGIN (the VM's AAAA hostname) once" "no origin recorded or named"
err_has "  no SSM parameter /cbase-vm/amgi/CBASE_ORIGIN_SECRET (the origin secret)" "the problem listed"
since "$m" | grep -E "^PUT /|^DELETE /|CreateBucket|^SSM SendCommand|^CFN ExecuteChangeSet" && fail "an adopted group's dry run changed something"
ok
# A grandfathered member: seeded with its own port and data, as they are.
veteran="$(vm_app veteran "CBASE_APP = veteran" "CBASE_VM = veteran" "CBASE_VM_STACK = cbase-vm" "CBASE_STATIC = none" \
             "CBASE_VM_PORT = 8080" "CBASE_VM_DATA = /var/lib/cbase" "CBASE_VM_SETTINGS = /cbase-vm/env/" \
             "CBASE_VM_ORIGIN = vm-origin.example.net")"
fake_set "origin-secret=$ORIGIN_SECRET"
run "$veteran" deploy --dry-run
expect_rc 0 "a grandfathered member's dry run"
out_has "vm paths: settings SSM /cbase-vm/env/, servers s3://cbase-code-$ACCOUNT-us-east-1/vm/veteran/" "CBASE_VM_SETTINGS overrides the derived path"
out_has "and seed vm/veteran.txt with veteran (grandfathered: port 8080, data /var/lib/cbase)" "the seed's grandfathered member"
out_has "vm: would join vm group veteran as app=veteran port=8080 service=cbase-veteran.service mem=~128 data=/var/lib/cbase legacy=1" "the grandfathered claim"
ok
# THE GROUP'S ORIGIN: the first member's CBASE_VM_ORIGIN is recorded in the group's state file; a
# second app naming none gets that host, IpAddressType ipv6 (never the instance's own DNS name);
# an app naming another is refused, one VM having one origin.
aws s3api create-bucket --bucket "cbase-code-$ACCOUNT-us-east-1" > /dev/null || fail "the adopted group's code bucket"
run "$veteran" deploy
expect_rc 0 "the first member's deploy into the adopted group"
state_of vm veteran | head -1 | grep -Fxq "cbase-state 1 vm veteran origin=vm-origin.example.net addr=ipv6" || { state_of vm veteran >&2; fail "the first member's origin was not recorded"; }
m="$(mark)"
run "$amgi" deploy
expect_rc 0 "a second app joining the group with an origin recorded"
grep -Fxq "vm origin: vm-origin.example.net (ipv6), the group's (its state file)" "$t/out" || { cat "$t/out" >&2; fail "the origin's line is not the state file's"; }
since "$m" | grep -q "CFN CreateChangeSet cbase-app-amgi CREATE ok params=VmOrigin=vm-origin.example.net," || { since "$m" | grep CreateChangeSet >&2; fail "the second app did not get the group's origin"; }
template_of cbase-app-amgi | grep -A1 "OriginKeepaliveTimeout" | grep -q "IpAddressType: ipv6" || fail "the second app's origin is not IpAddressType ipv6"
other="$(vm_app other "CBASE_APP = other" "CBASE_VM = veteran" "CBASE_VM_STACK = cbase-vm" "CBASE_STATIC = none" "CBASE_VM_ORIGIN = vm2.example.net")"
m="$(mark)"
run "$other" deploy
expect_rc 1 "an app naming another origin"
err_has "one VM can't have two origins" "an app naming another origin"
since "$m" | grep -qE "^PUT /|^SSM SendCommand|^CFN CreateChangeSet" && fail "an app naming another origin changed something"
ok

fi
[ "$(log_count unsigned)" = 0 ] || fail "a call reached the fake without the test's credentials"
CBASE_TEST_END=1
echo "cbase_vm_test: ok ($pass checks)"
