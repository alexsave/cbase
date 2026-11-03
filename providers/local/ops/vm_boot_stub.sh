#!/bin/bash
# vm_boot_stub.sh: the cbase-vm instance's user-data, and nothing else. It is
# all a CloudFormation parameter (4096 bytes) and a boot cannot do without: it
# reads which boot script is pinned, fetches it, checks it and runs it.
#
#   1. /cbase-vm/release/boot ("<bucket> <key> <sha256>", an SSM String made by
#      vm_release.sh / vm_deploy.sh) names the full boot script, vm_boot.sh with
#      cbase.service spliced in, uploaded as vm/cbase-vm-boot-<sha12>;
#   2. it is fetched to cbase-vm-boot.new, which must match the sha256 and parse
#      (bash -n) before anything is replaced; a failed check leaves the installed
#      copy exactly as it was and exits non-zero;
#   3. the installed copy moves to cbase-vm-boot.prev, the new one takes its
#      place, and it runs (exec).
#
# The same text runs at first boot (user-data) and on every `vm_release.sh
# --restart` (sent over SSM), so a boot-script change reaches the running
# instance with no replacement. Credentials, the signing and the fetch are the
# boot script's own (IMDSv2 over IPv6, curl --aws-sigv4, no aws CLI, no dnf).
# vm_boot_lib.sh strips these comments and refuses a stub over 1024 bytes.
# VM_ROOT is a root prefix for the tests (empty on the VM).
# Unquoted on purpose, for bytes: every value is a region, a credential or a key
# from IMDS or from our own SSM parameter, none holds a space or a glob.
# shellcheck disable=SC2086,SC2046
set -eu
I='http://[fd00:ec2::254]'
c() { curl -sSf --retry 5 --retry-all-errors "$@"; }
J() { python3 -c "import json,sys;d=json.load(sys.stdin);print($1)"; }
T=$(c -X PUT $I/latest/api/token -H X-aws-ec2-metadata-token-ttl-seconds:60)
M() { c -H "X-aws-ec2-metadata-token: $T" $I/latest/meta-data/$1; }
R=$(M placement/region)
P=iam/security-credentials/
read -r A Z Y < <(M $P$(M $P) | J 'd["AccessKeyId"],d["SecretAccessKey"],d["Token"]')
S() { c --aws-sigv4 aws:amz:$R:$1 --user $A:$Z -H "x-amz-security-token: $Y" "${@:2}"; }
read -r B K H < <(S ssm https://ssm.$R.api.aws/ -H Content-Type:application/x-amz-json-1.1 -H X-Amz-Target:AmazonSSM.GetParameter -d '{"Name":"/cbase-vm/release/boot"}' | J 'd["Parameter"]["Value"]')
cd ${VM_ROOT:-}/usr/local/sbin
D=cbase-vm-boot
trap 'rm -f $D.new' EXIT
S s3 https://$B.s3.dualstack.$R.amazonaws.com/$K -o $D.new
sha256sum -c --quiet <<<"$H  $D.new"
bash -n $D.new
chmod 700 $D.new
[ ! -e $D ] || cp -p $D $D.prev
mv -f $D.new $D
exec ./$D
