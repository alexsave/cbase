#!/bin/bash
# cbase_vm_test.sh - the deploy tool's VM side end to end, as an app runs it, through the
# real AWS CLI against the recorded local fake (fake_aws.c) with dummy credentials (lib.sh). No
# call reaches AWS.
#
#   bash tools/cbase.d/test/cbase_vm_test.sh      (cbase's make check runs it: cbase-vm-test)
#
# The cases: group-init vm refused without --yes (nothing made, not even the state bucket), an
# x86 type refused, then made with the state bucket and its state file; group-init static, with
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
# With --yes: the state bucket made, the stack's change set run, the state file written.
m="$(mark)"
run "$t" group-init vm g9 --yes
expect_rc 0 "group-init vm"
since "$m" | grep -q "S3 CreateBucket $BUCKET 200" || fail "group-init did not make the state bucket"
since "$m" | grep -q "S3 PutPublicAccessBlock $BUCKET 200" || fail "group-init did not block public access to the state bucket"
since "$m" | grep -q "CFN CreateChangeSet cbase-group-vm-g9 CREATE ok params=InstanceType=t4g.nano,ImageId=ami-0f4ce1234567890ab,CloudFrontPrefixList=pl-3b927c52,ArtifactBucket=$BUCKET caps=CAPABILITY_IAM" \
  || { since "$m" | grep CreateChangeSet >&2; fail "group-init vm's change set parameters"; }
since "$m" | grep -q "CFN ExecuteChangeSet cbase-group-vm-g9 ok" || fail "group-init vm did not execute"
out_has "vm group g9: i-0g9" "group-init vm finds its group"
printf 'cbase-state 1 vm g9\n' > "$t/want"
state_of vm g9 | diff -u "$t/want" - || fail "group-init vm's state file"
ok
template_of cbase-group-vm-g9 > "$t/tpl"
for want in "Type: AWS::EC2::Instance" "Key: cbase:vm-group" "Value: g9" "SourcePrefixListId: !Ref CloudFrontPrefixList" \
            "FromPort: 8700" "ToPort: 8799" "HttpTokens: required" "policy/AmazonSSMManagedInstanceCore" \
            'Resource: !Sub "arn:${AWS::Partition}:s3:::${ArtifactBucket}/artifacts/*"' "Encrypted: true" "Type: AWS::EC2::EIP"; do
  grep -Fq -- "$want" "$t/tpl" || { cat "$t/tpl" >&2; fail "the vm group's template lacks [$want]"; }
done
[ "$(grep -c 'IpProtocol:' "$t/tpl")" = 1 ] || fail "the vm group's security group opens more than the apps' ports"
grep -Eq 'CidrIp|FromPort: (22|80|443)$' "$t/tpl" && fail "the vm group's security group opens SSH, the web ports or a CIDR"
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
run "$t" group-init vm g9
expect_rc 0 "group-init vm again"
out_has "stack: cbase-group-vm-g9 is up to date (no changes)" "a re-run changes nothing"
since "$m" | grep -q "params=InstanceType=(previous),ImageId=(previous)," || fail "a re-run did not keep the type and the image"
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
ORIGIN_SECRET=hunter4-origin-header-value
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
grep -Fq "aws s3api get-object --region us-east-1 --bucket $BUCKET --key artifacts/shop/$sha" "$t/script" || fail "the script does not fetch the artifact"
grep -Fq "echo \"$sha  \$tmp/serve\" | sha256sum -c" "$t/script" || fail "the script does not check the artifact's sha256"
awk "/^cat > .*<< 'CBASE_UNIT'\$/{f=1; next} /^CBASE_UNIT\$/{f=0} f" "$t/script" > "$t/unit"
for want in "User=cbase-shop" "WorkingDirectory=/srv/cbase/shop" "ExecStart=/srv/cbase/shop/serve serve" \
            "Environment=CBASE_LISTEN=0.0.0.0:8700" "Environment=CBASE_DB=/srv/cbase/shop/data/cbase.sqlite" \
            "Environment=CBASE_ENV_FILE=/srv/cbase/shop/env" "Restart=on-failure" "ReadWritePaths=/srv/cbase/shop/data"; do
  grep -Fxq -- "$want" "$t/unit" || { cat "$t/unit" >&2; fail "the unit lacks [$want]"; }
done
grep -q '^MemoryMax' "$t/unit" && fail "an undeclared app's unit has a MemoryMax"
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
script_of | awk "/<< 'CBASE_UNIT'\$/{f=1; next} /^CBASE_UNIT\$/{f=0} f" > "$t/unit"
grep -Fxq "MemoryMax=64M" "$t/unit" || { cat "$t/unit" >&2; fail "a declared app's unit lacks MemoryMax=64M"; }
grep -Fxq "Environment=CBASE_LISTEN=0.0.0.0:8701" "$t/unit" || fail "blog's unit is not on its port"
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
[ "$1" != is-active ] || [ "${STUB_ACTIVE:-0}" = 1 ]
EOF
cat > "$stubs/ss" << 'EOF'
#!/bin/bash
[ "${STUB_TAKEN:-0}" != 1 ] || echo "LISTEN 0 4096 0.0.0.0:${STUB_PORT} 0.0.0.0:* users:((\"python3\",pid=7,fd=3))"
EOF
cat > "$stubs/aws" << 'EOF'
#!/bin/bash
echo "aws $*" >> "$STUB_LOG"
case "$1 $2" in
  "s3api get-object") cp "$STUB_ELF" "${!#}" ;;
  "ssm get-parameters-by-path")
    case "$*" in
      *"length(Parameters)"*) echo 1 ;;
      *) printf '/cbase/%s/DB_PASSWORD\t%s\n' "$STUB_APP" "$STUB_SECRET" ;;
    esac ;;
esac
EOF
cat > "$stubs/sha256sum" << 'EOF'
#!/bin/bash
read -r want file
[ "$(shasum -a 256 "$file" | cut -d' ' -f1)" = "$want" ]
EOF
printf '#!/bin/bash\n[ "${1:-}" != -u ] || case "${2:-}" in cbase-*) [ -f "$STUB_USERS/$2" ] ;; *) exit 0 ;; esac\n' > "$stubs/id"
printf '#!/bin/bash\necho "useradd $*" >> "$STUB_LOG"; touch "$STUB_USERS/${!#}"\n' > "$stubs/useradd"
printf '#!/bin/bash\necho "userdel $*" >> "$STUB_LOG"; rm -f "$STUB_USERS/$1"\n' > "$stubs/userdel"
printf '#!/bin/bash\necho "chown $*" >> "$STUB_LOG"\n' > "$stubs/chown"
printf '#!/bin/bash\nprintf 200\n' > "$stubs/curl"
printf '#!/bin/bash\n:\n' > "$stubs/journalctl"
chmod +x "$stubs"/*
# on_vm ROOT SCRIPT: the script as SSM runs it, under ROOT; stdout in $t/vm-out, status in $rc.
on_vm() {
  mkdir -p "$1/srv/cbase" "$1/etc/systemd/system" "$1/users"
  : > "$1/calls"
  rc=0
  env PATH="$stubs:$PATH" CBASE_VM_ROOT="$1" STUB_LOG="$1/calls" STUB_USERS="$1/users" STUB_ELF="$t/elf-arm64" \
      STUB_APP=shop STUB_PORT=8700 STUB_SECRET="$SECRET" bash "$2" > "$t/vm-out" 2>&1 < /dev/null || rc=$?
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
[ "$(cat "$vm/srv/cbase/shop/env")" = "DB_PASSWORD='$SECRET'" ] || fail "the env file is not the SSM parameter"
cmp -s "$vm/srv/cbase/shop/serve" "$t/elf-arm64" || fail "the server was not installed"
[ -d "$vm/srv/cbase/shop/data" ] || fail "the data folder was not made"
awk "/<< 'CBASE_UNIT'\$/{f=1; next} /^CBASE_UNIT\$/{f=0} f" "$t/shop-deploy.sh" | diff -u - "$vm/etc/systemd/system/cbase-shop.service" \
  || fail "the installed unit is not the one the deploy rendered"
grep -q "^useradd .*cbase-shop$" "$vm/calls" || fail "the app's user was not made"
grep -Fxq "chown cbase-shop:cbase-shop $vm/srv/cbase/shop/data" "$vm/calls" || fail "the data folder is not the service's"
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
# A redeploy: the unit running holds the port, which is no refusal.
STUB_ACTIVE=1 STUB_TAKEN=1 on_vm "$t/vm1" "$t/shop-deploy.sh"
[ "$rc" = 0 ] || { cat "$t/vm-out" >&2; fail "the deploy script on a redeploy: status $rc"; }
ok
# Undeploy, by default: the unit gone, the data kept. With --delete-data: the app's folder gone.
printf 'precious\n' > "$t/vm1/srv/cbase/shop/data/cbase.sqlite"
m="$(mark)"
run "$shop" undeploy
expect_rc 0 "an undeploy"
script_of > "$t/shop-undeploy.sh"
on_vm "$t/vm1" "$t/shop-undeploy.sh"
[ "$rc" = 0 ] || { cat "$t/vm-out" >&2; fail "the undeploy script: status $rc"; }
grep -Fxq "systemctl disable --now --quiet cbase-shop.service" "$t/vm1/calls" || fail "the undeploy script did not stop and disable the unit"
[ ! -e "$t/vm1/etc/systemd/system/cbase-shop.service" ] || fail "the undeploy script left the unit"
[ "$(cat "$t/vm1/srv/cbase/shop/data/cbase.sqlite")" = precious ] || fail "the undeploy script did not keep the data"
[ ! -e "$t/vm1/srv/cbase/shop/env" ] || fail "the undeploy script left the env file"
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

[ "$(log_count unsigned)" = 0 ] || fail "a call reached the fake without the test's credentials"
echo "cbase_vm_test: ok ($pass checks)"
