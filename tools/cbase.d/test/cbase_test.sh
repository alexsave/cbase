#!/bin/bash
# cbase_test.sh - the deploy tool (tools/cbase) end to end, as an app runs it, against a recorded
# local fake of AWS (fake_aws.c, built here) reached by the real AWS CLI through
# AWS_ENDPOINT_URL, with dummy credentials the CLI signs with locally. No call reaches AWS: the
# endpoint is the fake for every service, and the credentials are a key AWS has never issued.
#
#   bash tools/cbase.d/test/cbase_test.sh      (cbase's make check runs it: cbase-tool-test)
#
# The cases: cbase.conf's errors, each with its line number; the secret refusal (and no secret
# echoed); the values that look random but are not secrets; no credentials; a group found and not
# found; README.md's example; join, rejoin, leave and the port claim; two racing joins on a new file and on an
# existing one (distinct ports, one 412 and its retry); apps; a corrupt state file; a missing
# state bucket; doctor; the commands not built yet. docs/mutation_checks.md records the
# mutations each was checked against.
set -euo pipefail
export LC_ALL=C
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
tool="$root/tools/cbase"
t="$(mktemp -d "${TMPDIR:-/tmp}/cbase-test.XXXXXX")"
FAKE_PID=""
stop_fake() {
  if [ -n "$FAKE_PID" ]; then kill "$FAKE_PID" 2> /dev/null || true; wait "$FAKE_PID" 2> /dev/null || true; fi
  FAKE_PID=""
}
trap 'stop_fake; rm -rf "$t"' EXIT
fail() { echo "cbase_test: FAIL: $*" >&2; exit 1; }
pass=0
ok() { pass=$((pass + 1)); }

command -v aws > /dev/null 2>&1 || fail "the AWS CLI (aws) is not on PATH: the tests drive the real CLI against a local fake"
${CC:-cc} -std=gnu11 -D_GNU_SOURCE -O2 -Wall -Wextra -Werror -pthread "$root/tools/cbase.d/test/fake_aws.c" -o "$t/fake_aws"

ACCOUNT=111122223333
BUCKET="cbase-state-$ACCOUNT"
# The test's AWS: nothing of the person's (no profile, no config, no metadata service), dummy keys.
unset AWS_PROFILE AWS_DEFAULT_PROFILE AWS_SESSION_TOKEN AWS_ENDPOINT_URL_S3 AWS_ENDPOINT_URL_STS \
      AWS_ENDPOINT_URL_RESOURCE_GROUPS_TAGGING_API AWS_DEFAULT_REGION AWS_CA_BUNDLE
export AWS_ACCESS_KEY_ID=AKIACBASEFAKE0000000 AWS_SECRET_ACCESS_KEY=cbase-test-dummy-secret-key \
       AWS_REGION=us-east-1 AWS_CONFIG_FILE="$t/no-aws-config" AWS_SHARED_CREDENTIALS_FILE="$t/no-aws-credentials" \
       AWS_EC2_METADATA_DISABLED=true AWS_PAGER="" AWS_MAX_ATTEMPTS=1 AWS_ENDPOINT_URL=http://127.0.0.1:1

# start_fake ARGS...: a fresh fake (no objects), its log at $t/log, AWS_ENDPOINT_URL at it.
start_fake() {
  stop_fake
  # Every call the last fake answered was signed with the dummy key: none was refused as unsigned
  # (a call that had found other credentials).
  if [ -f "$t/log" ] && grep -q unsigned "$t/log"; then fail "a call reached the fake without the test's credentials"; fi
  rm -f "$t/port" "$t/log"
  "$t/fake_aws" --port-file "$t/port" --log "$t/log" --account "$ACCOUNT" "$@" &
  FAKE_PID=$!
  local n=0
  until [ -s "$t/port" ] || [ $n -gt 100 ]; do sleep 0.05; n=$((n + 1)); done
  [ -s "$t/port" ] || fail "the fake did not start"
  AWS_ENDPOINT_URL="http://127.0.0.1:$(cat "$t/port")"
  export AWS_ENDPOINT_URL
}
VM_G1="cbase:vm-group g1 ec2:instance arn:aws:ec2:us-east-1:$ACCOUNT:instance/i-0g1"
VM_G2="cbase:vm-group g2 ec2:instance arn:aws:ec2:us-east-1:$ACCOUNT:instance/i-0g2"
ST_G1="cbase:static-group g1 s3 arn:aws:s3:::g1-site-bucket"
everything=(--bucket "$BUCKET" --resource "$VM_G1" --resource "$VM_G2" --resource "$ST_G1")

# run DIR ARGS...: the tool from DIR (an app's repository), its stdout in $t/out, stderr in
# $t/err, status in $rc.
rc=0
run() {
  local dir="$1"; shift
  rc=0
  (cd "$dir" && "$tool" "$@") > "$t/out" 2> "$t/err" || rc=$?
}
expect_rc() { [ "$rc" = "$1" ] || { cat "$t/out" "$t/err" >&2; fail "$2: status $rc, not $1"; }; }
err_has() { grep -Fq -- "$1" "$t/err" || { cat "$t/err" >&2; fail "$2: stderr lacks [$1]"; }; }
out_has() { grep -Fq -- "$1" "$t/out" || { cat "$t/out" >&2; fail "$2: stdout lacks [$1]"; }; }
log_count() { grep -cF -- "$1" "$t/log" || true; }
app() {   # app NAME LINES...: an app repository with cbase.conf of LINES
  local d="$t/apps/$1"; shift
  mkdir -p "$d"
  printf '%s\n' "$@" > "$d/cbase.conf"
  printf '%s' "$d"
}
state_of() {   # state_of KIND GROUP: the state file as the fake holds it
  aws s3api get-object --bucket "$BUCKET" --key "$1/$2.txt" "$t/state" > /dev/null 2>&1 || { echo "(none)"; return; }
  cat "$t/state"
}

# ---- cbase.conf: errors, each with its line number -------------------------------------------
conf_error() {   # conf_error "EXPECTED" LINES...
  local want="$1"; shift
  local d
  d="$(app conf "$@")"
  run "$d" config check
  expect_rc 1 "a bad cbase.conf ($want)"
  err_has "$want" "a bad cbase.conf"
  ok
}
conf_error "cbase.conf line 2: is not KEY = value (no '=')" "CBASE_APP = a" "CBASE_STATIC none"
conf_error "cbase.conf line 3: CBASE_FOO is not a cbase.conf key" "CBASE_APP = a" "# c" "CBASE_FOO = 1"
conf_error "cbase.conf line 3: sets CBASE_APP again (line 1 set it)" "CBASE_APP = a" "CBASE_VM = none" "CBASE_APP = b"
conf_error "cbase.conf line 1: CBASE_APP is a name" "CBASE_APP = My_App"
conf_error "cbase.conf line 1: CBASE_APP is a name" "CBASE_APP = none"
conf_error "cbase.conf line 2: CBASE_MEMORY is <n>M or <n>G" "CBASE_APP = a" "CBASE_MEMORY = lots"
conf_error "cbase.conf line 2: CBASE_MEMORY is from 16M to 64G" "CBASE_APP = a" "CBASE_MEMORY = 8M"
conf_error "cbase.conf has no CBASE_MEMORY line, and the app has a VM" "CBASE_APP = a"
conf_error "cbase.conf line 2: CBASE_MEMORY is for an app with a VM" "CBASE_APP = a" "CBASE_MEMORY = 64M" "CBASE_VM = none"
conf_error "cbase.conf has no CBASE_APP line" "CBASE_VM = none"
conf_error "cbase.conf line 3: CBASE_VM and CBASE_STATIC are both none" "CBASE_APP = a" "CBASE_VM = none" "CBASE_STATIC = none"
conf_error "cbase.conf line 3: CBASE_ROUTE's origin is vm, static or ws" "CBASE_APP = a" "CBASE_MEMORY = 1G" "CBASE_ROUTE = /x lambda"
conf_error "cbase.conf line 3: CBASE_ROUTE's pattern is" "CBASE_APP = a" "CBASE_MEMORY = 1G" "CBASE_ROUTE = api vm"
conf_error "cbase.conf line 3: a static route takes only GET, HEAD and OPTIONS" "CBASE_APP = a" "CBASE_MEMORY = 1G" "CBASE_ROUTE = /f/* static GET,POST"
conf_error "cbase.conf line 3: a ws route takes no cache or methods" "CBASE_APP = a" "CBASE_MEMORY = 1G" "CBASE_ROUTE = /ws ws cache"
conf_error "cbase.conf line 3: FETCH is not a method CloudFront passes" "CBASE_APP = a" "CBASE_MEMORY = 1G" "CBASE_ROUTE = /x vm GET,FETCH"
conf_error "cbase.conf line 4: CBASE_ROUTE /x is routed twice" "CBASE_APP = a" "CBASE_MEMORY = 1G" "CBASE_ROUTE = /x vm" "CBASE_ROUTE = /x static"
conf_error "cbase.conf line 3: routes to the VM (vm), and CBASE_VM is none" "CBASE_APP = a" "CBASE_VM = none" "CBASE_ROUTE = /api/* vm"
conf_error "cbase.conf line 4: routes to static, and CBASE_STATIC is none" "CBASE_APP = a" "CBASE_STATIC = none" "CBASE_MEMORY = 1G" "CBASE_ROUTE = /f/* static"
conf_error "cbase.conf line 2: has a '#' in its value" "CBASE_APP = a" "CBASE_VM = none # static only"
conf_error "cbase.conf line 2: has a quote in its value" "CBASE_APP = a" "CBASE_VM = \"none\""
conf_error "cbase.conf line 2: CBASE_VM has no value" "CBASE_APP = a" "CBASE_VM ="
conf_error "cbase.conf line 3: CBASE_CERT_ARN is in eu-west-1, and CloudFront takes a certificate from us-east-1 alone" \
  "CBASE_APP = a" "CBASE_VM = none" "CBASE_CERT_ARN = arn:aws:acm:eu-west-1:$ACCOUNT:certificate/12345678-abcd-ef01-2345-6789abcdef01"
conf_error "cbase.conf line 2: CBASE_DOMAIN needs CBASE_CERT_ARN" "CBASE_APP = a" "CBASE_DOMAIN = www.example.com" "CBASE_VM = none"
conf_error "cbase.conf line 2: CBASE_DOMAIN is a lower-case hostname" "CBASE_APP = a" "CBASE_DOMAIN = *.example.com"
conf_error "cbase.conf line 2: is not KEY = value (a key is upper-case" "CBASE_APP = a" "cbase_vm = none"

# A good one, defaults filled in, routes normalized, the "*" route last.
d="$(app good "# the app" "CBASE_APP = shop" "CBASE_MEMORY=512M" "CBASE_ROUTE = * static" \
       "CBASE_ROUTE = /api/* vm POST,GET" "CBASE_ROUTE = /live ws" "  CBASE_ROUTE   =   /img/*   static   nocache  " \
       "CBASE_DOMAIN = shop.example.com" "CBASE_API_HOST = api.example.com" "CBASE_ZONE = Z0123456789ABCDEFGHIJ" \
       "CBASE_CERT_ARN = arn:aws:acm:us-east-1:$ACCOUNT:certificate/12345678-abcd-ef01-2345-6789abcdef01")"
printf 'CBASE_STATIC = shop\r\n' >> "$d/cbase.conf"
run "$d" config check
expect_rc 0 "a good cbase.conf"
cat > "$t/want" << EOF
cbase.conf: ok
app        shop
vm         shop (memory 512 MiB)
static     shop
domain     shop.example.com
api-host   api.example.com
cert       arn:aws:acm:us-east-1:$ACCOUNT:certificate/12345678-abcd-ef01-2345-6789abcdef01
zone       Z0123456789ABCDEFGHIJ
route      /api/* vm nocache GET,POST
route      /live ws nocache ALL
route      /img/* static nocache GET,HEAD
route      * static cache GET,HEAD
EOF
diff -u "$t/want" "$t/out" || fail "config check printed another config"
ok
# README.md's example cbase.conf is one the tool takes.
mkdir -p "$t/apps/readme"
awk '/^# required: \[a-z\]/{f=1} f&&/^```/{exit} f' "$root/README.md" > "$t/apps/readme/cbase.conf"
[ -s "$t/apps/readme/cbase.conf" ] || fail "README.md has no example cbase.conf"
run "$t/apps/readme" config check
expect_rc 0 "README.md's example cbase.conf"
out_has "route      /live ws nocache ALL" "README.md's example cbase.conf"
ok
run "$d" -f missing.conf config check
expect_rc 1 "a missing config"
err_has "missing.conf does not exist" "a missing config"
ok

# ---- secrets: refused by line, never echoed ----------------------------------------------------
secret() {   # secret "EXPECTED" "SECRET TEXT" LINE: the LINE (holding the text) is refused at line 2
  local want="$1" text="$2" line="$3" d
  d="$(app secret "CBASE_APP = a" "$line" "CBASE_VM = none")"
  run "$d" config check
  expect_rc 1 "a secret ($want)"
  err_has "cbase.conf line 2: $want" "a secret"
  if grep -Fq -- "$text" "$t/out" "$t/err"; then fail "the refusal echoed the secret [$text]"; fi
  ok
}
secret "the value of CBASE_ZONE looks like a secret: an AWS access key id" AKIAIOSFODNN7EXAMPLE "CBASE_ZONE = AKIAIOSFODNN7EXAMPLE"
secret "the value of CBASE_DOMAIN looks like a secret: an AWS access key id" ASIAY34FZKBOKMUTVV7A "CBASE_DOMAIN = ASIAY34FZKBOKMUTVV7A"
secret "the value of CBASE_APP2 looks like a secret: a long random-looking string" "wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY" "CBASE_APP2 = wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY"
secret "the value of CBASE_ROUTE looks like a secret: a long hex string" "9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08" "CBASE_ROUTE = /x vm 9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08"
secret "the value of CBASE_DOMAIN looks like a secret: a long random-looking string" "ghpabcdefghijklmnopqrstuvwxyzabcdefghijklmn" "CBASE_DOMAIN = ghpabcdefghijklmnopqrstuvwxyzabcdefghijklmn"
secret "this line looks like a secret: a PEM block" "-----BEGIN RSA PRIVATE KEY-----" "-----BEGIN RSA PRIVATE KEY-----"
secret "this line looks like a secret: a long random-looking string" "MIIEvQIBADANBgkqhkiG9w0BAQEFAASCBKcwggSj" "MIIEvQIBADANBgkqhkiG9w0BAQEFAASCBKcwggSj=="
secret "CBASE_DB_PASSWORD is named like a secret" "hunter2" "CBASE_DB_PASSWORD = hunter2"
secret "CBASE_API_TOKEN is named like a secret" "abc" "CBASE_API_TOKEN = abc"
secret "CBASE_SECRET is named like a secret" "x" "CBASE_SECRET = x"
secret "STRIPE_PRIVATE_KEY is named like a secret" "sk" "STRIPE_PRIVATE_KEY = sk"
# Even a line that is not KEY = value is refused as a secret first, and not echoed.
secret "this line looks like a secret: an AWS access key id" AKIAIOSFODNN7EXAMPLE "export AKIAIOSFODNN7EXAMPLE"

# ---- no credentials: the CLI's own chain finds none --------------------------------------------
start_fake "${everything[@]}"
d="$(app a "CBASE_APP = a" "CBASE_VM = g1" "CBASE_STATIC = g1" "CBASE_MEMORY = 128M")"
rc=0
(cd "$d" && env -u AWS_ACCESS_KEY_ID -u AWS_SECRET_ACCESS_KEY "$tool" apps) > "$t/out" 2> "$t/err" || rc=$?
expect_rc 1 "no credentials"
err_has "no usable AWS credentials. Run aws login (or aws sso login), or set AWS_PROFILE." "no credentials"
[ "$(log_count 'POST STS')" = 0 ] || fail "a call without credentials reached the fake"
ok
# A profile that exists names where the CLI got its keys: ~/.aws's credentials file, as a person has.
printf '[deploy]\naws_access_key_id = AKIACBASEFAKE0000000\naws_secret_access_key = cbase-test-dummy-secret-key\n' > "$t/creds"
rc=0
(cd "$d" && env -u AWS_ACCESS_KEY_ID -u AWS_SECRET_ACCESS_KEY AWS_PROFILE=deploy AWS_SHARED_CREDENTIALS_FILE="$t/creds" \
   "$tool" state join) > "$t/out" 2> "$t/err" || rc=$?
expect_rc 0 "credentials from a profile"
out_has "a joined vm group g1: app=a port=8700 service=cbase-a.service mem=128" "credentials from a profile"
ok

# ---- groups: found by tag, and not found -------------------------------------------------------
start_fake "${everything[@]}"
d="$(app nope "CBASE_APP = nope" "CBASE_VM = ghost" "CBASE_MEMORY = 64M" "CBASE_STATIC = none")"
run "$d" state join
expect_rc 1 "a missing vm group"
err_has "there is no vm group named ghost (nothing tagged cbase:vm-group=ghost in us-east-1). Create it with: ./cbase group-init vm ghost" "a missing vm group"
[ "$(log_count 'PUT ')" = 0 ] || fail "a join to a missing group wrote"
ok
d="$(app nope2 "CBASE_APP = nope" "CBASE_VM = none" "CBASE_STATIC = ghost")"
run "$d" state join
expect_rc 1 "a missing static group"
err_has "Create it with: ./cbase group-init static ghost" "a missing static group"
ok
# A tag on an instance of another group's is not this group's: g2's tag does not find g1.
start_fake --bucket "$BUCKET" --resource "$VM_G2"
d="$(app g1only "CBASE_APP = a" "CBASE_VM = g1" "CBASE_MEMORY = 64M" "CBASE_STATIC = none")"
run "$d" state join
expect_rc 1 "a group tagged under another name"
err_has "there is no vm group named g1" "a group tagged under another name"
ok
# Two instances under one tag: a VM group is one instance.
start_fake --bucket "$BUCKET" --resource "$VM_G1" --resource "cbase:vm-group g1 ec2:instance arn:aws:ec2:us-east-1:$ACCOUNT:instance/i-0dup"
run "$d" state join
expect_rc 1 "two instances in one group"
err_has "2 resources are tagged cbase:vm-group=g1, and a vm group is one instance" "two instances in one group"
ok

# ---- join, rejoin, leave, the port claim -------------------------------------------------------
start_fake "${everything[@]}"
a="$(app a "CBASE_APP = a" "CBASE_VM = g1" "CBASE_STATIC = g1" "CBASE_MEMORY = 128M")"
b="$(app b "CBASE_APP = b" "CBASE_VM = g1" "CBASE_STATIC = g1" "CBASE_MEMORY = 1G")"
c="$(app c "CBASE_APP = c" "CBASE_VM = g1" "CBASE_STATIC = none" "CBASE_MEMORY = 64M")"
run "$a" state join
expect_rc 0 "a's join"
out_has "a joined vm group g1: app=a port=8700 service=cbase-a.service mem=128" "a's join"
out_has "a joined static group g1: app=a folder=a/" "a's join"
grep -Fq "PUT /$BUCKET/vm/g1.txt 200 if-none-match" "$t/log" || fail "a new state file is written If-None-Match: *"
ok
run "$b" state join
expect_rc 0 "b's join"
out_has "b joined vm group g1: app=b port=8701 service=cbase-b.service mem=1024" "b claims the next port"
grep -Fq "PUT /$BUCKET/vm/g1.txt 200 if-match" "$t/log" || fail "an existing state file is written If-Match"
ok
printf 'cbase-state 1 vm g1\napp=a port=8700 service=cbase-a.service mem=128\napp=b port=8701 service=cbase-b.service mem=1024\n' > "$t/want"
state_of vm g1 | diff -u "$t/want" - || fail "the vm state file after two joins"
printf 'cbase-state 1 static g1\napp=a folder=a/\napp=b folder=b/\n' > "$t/want"
state_of static g1 | diff -u "$t/want" - || fail "the static state file after two joins"
ok
puts="$(log_count 'PUT ')"
run "$a" state join
expect_rc 0 "a's rejoin"
out_has "a is in vm group g1: app=a port=8700" "a rejoin keeps its port"
[ "$(log_count 'PUT ')" = "$puts" ] || fail "a rejoin that changes nothing wrote"
ok
# A changed declaration rewrites the app's line and keeps its port.
printf 'CBASE_APP = a\nCBASE_VM = g1\nCBASE_STATIC = g1\nCBASE_MEMORY = 256M\n' > "$a/cbase.conf"
run "$a" state join
expect_rc 0 "a's rejoin with more memory"
out_has "a joined vm group g1: app=a port=8700 service=cbase-a.service mem=256" "a's rejoin keeps its port"
ok
run "$a" state leave
expect_rc 0 "a's leave"
out_has "a left vm group g1 (it held: app=a port=8700 service=cbase-a.service mem=256)" "a's leave"
out_has "a left static group g1" "a's leave"
printf 'cbase-state 1 vm g1\napp=b port=8701 service=cbase-b.service mem=1024\n' > "$t/want"
state_of vm g1 | diff -u "$t/want" - || fail "the vm state file after a leave"
ok
run "$a" state leave
expect_rc 0 "a second leave"
out_has "a is not in vm group g1: nothing to leave" "a second leave"
ok
run "$c" state join
expect_rc 0 "c's join"
out_has "c joined vm group g1: app=c port=8700" "the lowest free port is claimed again"
ok

# ---- apps --------------------------------------------------------------------------------------
run "$b" apps
expect_rc 0 "apps from cbase.conf"
cat > "$t/want" << EOF
vm group g1 (s3://$BUCKET/vm/g1.txt): 2 apps
  app=b port=8701 service=cbase-b.service mem=1024
  app=c port=8700 service=cbase-c.service mem=64
static group g1 (s3://$BUCKET/static/g1.txt): 1 app
  app=b folder=b/
EOF
diff -u "$t/want" "$t/out" || fail "apps listed another membership"
ok
run "$t" apps g1
expect_rc 0 "apps g1 with no cbase.conf"
diff -u "$t/want" "$t/out" || fail "apps g1 listed another membership"
ok
run "$t" apps g9
expect_rc 1 "apps of a group with no state"
err_has "no state file for vm g9 group, static g9 group: no app has joined it yet" "apps of a group with no state"
ok
run "$t" apps Not_A_Group
expect_rc 2 "apps of a bad name"
ok

# ---- doctor ------------------------------------------------------------------------------------
run "$b" doctor
expect_rc 0 "doctor"
out_has "account   $ACCOUNT (us-east-1)" "doctor names the account"
out_has "vm group g1: i-0g1" "doctor names the instance"
out_has "static group g1: g1-site-bucket" "doctor names the bucket"
out_has "  app=c port=8700" "doctor lists the members"
ok
run "$(app doc2 "CBASE_APP = z" "CBASE_VM = ghost" "CBASE_MEMORY = 64M" "CBASE_STATIC = g1")" doctor
expect_rc 1 "doctor with a missing group"
err_has "./cbase group-init vm ghost" "doctor with a missing group"
out_has "static group g1: g1-site-bucket" "doctor goes on past a missing group"
ok

# ---- a corrupt state file, a missing state bucket ----------------------------------------------
bad_state() {   # bad_state "EXPECTED" CONTENT
  printf '%b' "$2" > "$t/bad"
  aws s3api put-object --bucket "$BUCKET" --key vm/g1.txt --body "$t/bad" > /dev/null
  run "$b" apps
  expect_rc 1 "a corrupt state file ($1)"
  err_has "state file s3://$BUCKET/vm/g1.txt $1" "a corrupt state file"
  ok
}
bad_state "line 1 is not \"cbase-state 1 vm g1\"" 'cbase-state 2 vm g1\n'
bad_state "line 1 is not \"cbase-state 1 vm g1\"" 'cbase-state 1 vm g2\n'
bad_state "line 2 is not \"app=<name> port=<port>" 'cbase-state 1 vm g1\napp=b port=8701\n'
bad_state "line 3 gives port 8701 to c, and b holds it" 'cbase-state 1 vm g1\napp=b port=8701 service=cbase-b.service mem=1\napp=c port=8701 service=cbase-c.service mem=1\n'
bad_state "line 3 lists b again" 'cbase-state 1 vm g1\napp=b port=8701 service=cbase-b.service mem=1\napp=b port=8702 service=cbase-b.service mem=1\n'
bad_state "line 2 holds port 9000, outside 8700-8799" 'cbase-state 1 vm g1\napp=b port=9000 service=cbase-b.service mem=1\n'
bad_state "line 2 names service cbase-x.service for app b" 'cbase-state 1 vm g1\napp=b port=8701 service=cbase-x.service mem=1\n'
start_fake --resource "$VM_G1"
run "$b" apps
expect_rc 1 "no state bucket"
err_has "there is no state bucket s3://$BUCKET in this account" "no state bucket"
ok

# ---- two racing joins --------------------------------------------------------------------------
# The fake holds the first two reads until both are in, so both runs read the same file (none, then
# one with two members) before either writes: exactly one write wins, and the other gets 412, reads
# again and claims the next port.
race() {   # race APP1 APP2 PORTS...: the two join at once; the ports they end up with
  local d1 d2 rc1=0 rc2=0
  d1="$(app "$1" "CBASE_APP = $1" "CBASE_VM = g2" "CBASE_STATIC = none" "CBASE_MEMORY = 64M")"
  d2="$(app "$2" "CBASE_APP = $2" "CBASE_VM = g2" "CBASE_STATIC = none" "CBASE_MEMORY = 64M")"
  curl -fsS -X POST "$AWS_ENDPOINT_URL/_fake/gather?n=2" > /dev/null
  local before412
  before412="$(log_count "PUT /$BUCKET/vm/g2.txt 412")"
  (cd "$d1" && "$tool" state join) > "$t/out1" 2> "$t/err1" & local p1=$!
  (cd "$d2" && "$tool" state join) > "$t/out2" 2> "$t/err2" & local p2=$!
  wait "$p1" || rc1=$?
  wait "$p2" || rc2=$?
  [ "$rc1" = 0 ] && [ "$rc2" = 0 ] || { cat "$t/out1" "$t/err1" "$t/out2" "$t/err2" >&2; fail "racing joins $1 and $2: status $rc1 and $rc2"; }
  grep -q "GATHER timed out" "$t/log" && fail "racing joins $1 and $2 did not read at once"
  [ "$(( $(log_count "PUT /$BUCKET/vm/g2.txt 412") - before412 ))" = 1 ] || { cat "$t/log" >&2; fail "racing joins $1 and $2: not exactly one 412"; }
  grep -hq "changed while .* was being written (attempt 1 of 8): reading it again" "$t/err1" "$t/err2" \
    || fail "racing joins $1 and $2: neither retried"
  local p
  for p in "${@:3}"; do
    grep -hqE "joined vm group g2: app=($1|$2) port=$p " "$t/out1" "$t/out2" || { cat "$t/out1" "$t/out2" >&2; fail "racing joins $1 and $2: nobody holds port $p"; }
  done
  ok
}
start_fake "${everything[@]}"
race x y 8700 8701                    # the file is new: both write If-None-Match: *
race z w 8702 8703                    # the file exists: both write If-Match the same ETag
state_of vm g2 > "$t/g2"
[ "$(sed -n 1p "$t/g2")" = "cbase-state 1 vm g2" ] || fail "the racing joins' state file header"
[ "$(sed 1d "$t/g2" | cut -d' ' -f1 | tr '\n' ' ')" = "app=w app=x app=y app=z " ] || { cat "$t/g2" >&2; fail "the racing joins' state file holds every app once"; }
[ "$(grep -o 'port=[0-9]*' "$t/g2" | sort -u | wc -l | tr -d ' ')" = 4 ] || fail "four apps, four ports"
ok

# ---- not yet -----------------------------------------------------------------------------------
for c in deploy undeploy "group-init vm g1"; do
  # shellcheck disable=SC2086 # the command's words
  run "$b" $c
  expect_rc 2 "$c"
  err_has "is not built yet" "$c"
  ok
done
run "$b" frobnicate
expect_rc 2 "an unknown command"
ok

[ "$(log_count unsigned)" = 0 ] || fail "a call reached the fake without the test's credentials"
echo "cbase_test: ok ($pass checks)"
