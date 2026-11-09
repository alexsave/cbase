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
# state bucket; doctor; an unknown command. Then deploy: a static site end to
# end (the stack's create, each file's headers, the hashed rule, the upload order, nothing outside
# the app's folder), a redeploy (drift checked, no changes, the invalidation), prune scoped to the
# folder even over a listing of the whole bucket, a bucket's replacement and removal refused, a
# distribution's replacement allowed and said, drift refused then accepted, DRY_RUN, an origin
# header's and a parameter's value kept out of a drift, a change set and an error, the
# certificate found, requested with a zone, requested without one (the records printed, status 3),
# a certificate read that fails, several hostnames on one distribution and one certificate, a missing
# group and a missing build, the template's routes (ws ones on CBASE_API_HOST's distribution too),
# and the router's code run under node (files, pages, the app shell).
# The VM side, group-init, undeploy and group-destroy are cbase_vm_test.sh's. docs/mutation_checks.md
# records the mutations each was checked against.
set -euo pipefail
# shellcheck source=tools/cbase.d/test/lib.sh
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

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
conf_error "cbase.conf line 3: CBASE_API_HOST is a second hostname, and there is no CBASE_DOMAIN" "CBASE_APP = a" "CBASE_MEMORY = 1G" "CBASE_API_HOST = api.example.com"
conf_error "cbase.conf line 3: CBASE_CERT_ARN is for CBASE_DOMAIN, and there is none" "CBASE_APP = a" "CBASE_VM = none" \
  "CBASE_CERT_ARN = arn:aws:acm:us-east-1:$ACCOUNT:certificate/12345678-abcd-ef01-2345-6789abcdef01"
conf_error "cbase.conf line 2: CBASE_DOMAIN is lower-case hostnames" "CBASE_APP = a" "CBASE_DOMAIN = *.example.com"
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
files      ui/out/ (the default)
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
# The defaults, for each of the three shapes: routes and memory resolved.
shape() {   # shape NAME "WANT LINES" CONF LINES...: config check prints the WANT lines after "ok"
  local name="$1" want="$2" d; shift 2
  d="$(app "shape-$name" "$@")"
  run "$d" config check
  expect_rc 0 "the $name shape"
  printf 'cbase.conf: ok\n%s\n' "$want" > "$t/want"
  diff -u "$t/want" "$t/out" || fail "the $name shape resolved otherwise"
  ok
}
# CBASE_APP alone is a full app: its own VM and bucket, the default routes, memory undeclared and
# counted at the estimate.
shape full "app        shop
vm         shop (shop: memory not declared, counted as 128M)
static     shop
files      ui/out/ (the default)
address    (the distribution's cloudfront.net name)
route      /api/* vm nocache ALL
route      * static cache GET,HEAD" "CBASE_APP = shop"
# Each optional key overrides its default (the good file above sets every one at once).
shape memory "app        shop
vm         shop (memory 2048 MiB)
static     shop
files      ui/out/ (the default)
address    (the distribution's cloudfront.net name)
route      /api/* vm nocache ALL
route      * static cache GET,HEAD" "CBASE_APP = shop" "CBASE_MEMORY = 2G"
shape groups "app        shop
vm         shared (shop: memory not declared, counted as 128M)
static     sites
files      ui/out/ (the default)
address    (the distribution's cloudfront.net name)
route      /api/* vm nocache ALL
route      * static cache GET,HEAD" "CBASE_APP = shop" "CBASE_VM = shared" "CBASE_STATIC = sites"
shape static "app        site
vm         none
static     site
files      build/ (the default)
address    (the distribution's cloudfront.net name)
route      * static cache GET,HEAD" "CBASE_APP = site" "CBASE_VM = none"
shape vm "app        svc
vm         svc (memory 64 MiB)
static     none
address    (the distribution's cloudfront.net name)
route      * vm nocache ALL" "CBASE_APP = svc" "CBASE_STATIC = none" "CBASE_MEMORY = 64M"
# A written route replaces the defaults (no /api/* merged in); CloudFront's "*" is still added.
shape written "app        shop
vm         shop (shop: memory not declared, counted as 128M)
static     shop
files      ui/out/ (the default)
address    (the distribution's cloudfront.net name)
route      /rpc/* vm nocache ALL
route      * static cache GET,HEAD" "CBASE_APP = shop" "CBASE_ROUTE = /rpc/* vm"
# A domain needs no certificate in the file: deploy finds or makes one.
shape domain "app        site
vm         none
static     site
files      build/ (the default)
domain     site.example.com
cert       (found or requested by deploy)
route      * static cache GET,HEAD" "CBASE_APP = site" "CBASE_VM = none" "CBASE_DOMAIN = site.example.com"

# README.md's examples (its ```conf blocks) are files the tool takes.
mkdir -p "$t/apps/readme"
awk '/^```conf$/{f=1; n++; next} /^```$/{f=0} f{print > (dir "/" n ".conf")}' dir="$t/apps/readme" "$root/README.md"
n_examples=0
for f in "$t/apps/readme"/*.conf; do
  [ -f "$f" ] || continue
  run "$t" -f "$f" config check
  expect_rc 0 "README.md's example $(basename "$f")"
  n_examples=$((n_examples + 1))
done
[ "$n_examples" -ge 3 ] || fail "README.md has $n_examples example cbase.conf blocks, not 3 or more"
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
# An app that declares no memory is recorded at the estimate it is counted at.
e="$(app e "CBASE_APP = e" "CBASE_VM = g1" "CBASE_STATIC = none")"
run "$e" state join
expect_rc 0 "e's join"
out_has "e joined vm group g1: app=e port=8702 service=cbase-e.service mem=~128" "an undeclared app's memory"
ok
run "$e" state leave
expect_rc 0 "e's leave"
ok

# ---- apps --------------------------------------------------------------------------------------
run "$b" apps
expect_rc 0 "apps from cbase.conf"
{
  echo "vm group g1 (s3://$BUCKET/vm/g1.txt): 2 apps"
  printf '  %-24s %-5s %-34s %s\n' app port service mem b 8701 cbase-b.service 1024M c 8700 cbase-c.service 64M
  echo "static group g1 (s3://$BUCKET/static/g1.txt): 1 app"
  printf '  %-24s %s\n' app folder b b/
} > "$t/want"
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
out_has "  c                        8700  cbase-c.service" "doctor lists the members"
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
  if [ "$rc1" != 0 ] || [ "$rc2" != 0 ]; then cat "$t/out1" "$t/err1" "$t/out2" "$t/err2" >&2; fail "racing joins $1 and $2: status $rc1 and $rc2"; fi
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

# ---- an unknown command ------------------------------------------------------------------------
run "$b" frobnicate
expect_rc 2 "an unknown command"
ok

# ================================================================================================
# deploy: the static part
# ================================================================================================
export CBASE_POLL_SECONDS=0
SITE_BUCKET="site-bucket"
ST_SITE="cbase:static-group site s3 arn:aws:s3:::$SITE_BUCKET"
CERT_WILD="arn:aws:acm:us-east-1:$ACCOUNT:certificate/11111111-1111-1111-1111-111111111111"
CERT_OTHER="arn:aws:acm:us-east-1:$ACCOUNT:certificate/22222222-2222-2222-2222-222222222222"
CERT_DEEP="arn:aws:acm:us-east-1:$ACCOUNT:certificate/33333333-3333-3333-3333-333333333333"
CERT_NOZONE="arn:aws:acm:us-east-1:$ACCOUNT:certificate/44444444-4444-4444-4444-444444444444"
CERT_NAMED="arn:aws:acm:us-east-1:$ACCOUNT:certificate/66666666-6666-6666-6666-666666666666"
site_fake=(--bucket "$BUCKET" --bucket "$SITE_BUCKET:us-west-2" --resource "$ST_SITE" --resource "$VM_G1"
           --cert "$CERT_OTHER ISSUED other.org" --cert "$CERT_DEEP ISSUED *.www.example.com"
           --cert "$CERT_WILD ISSUED example.com,*.example.com" --zone "Z0EXAMPLE example.com"
           --zone "Z0PRIVATE corp.example.com private" --zone "Z0SHOPORG shop.org"
           --cert "$CERT_NOZONE ISSUED a.nozone.net,b.nozone.net" --cert "$CERT_NAMED ISSUED www.example.com")
head_of() {   # head_of KEY: "<Content-Type>|<Cache-Control>" of s3://site-bucket/KEY
  aws s3api head-object --bucket "$SITE_BUCKET" --key "$1" --query '[ContentType,CacheControl]' --output text | tr '\t' '|'
}
seed() {   # seed KEY: an object of someone else's, put straight into the site bucket
  printf 'theirs\n' > "$t/seed"
  aws s3api put-object --bucket "$SITE_BUCKET" --key "$1" --body "$t/seed" > /dev/null
}
# The site: a static app (CBASE_VM = none) with every kind of file.
site="$(app site "CBASE_APP = site" "CBASE_VM = none" "CBASE_HASHED = assets/*")"
mkdir -p "$site/build/assets" "$site/build/docs" "$site/build/.well-known" "$site/build/.git"
for f in index.html about.html docs/index.html app.wasm style.css data.json font.WOFF2 LICENSE assets/app.1a2b3c4d.js \
         assets/logo.9f8e7d.png assets/page.html .well-known/security.txt .well-known/apple-app-site-association .DS_Store .git/config; do
  printf '%s\n' "$f" > "$site/build/$f"
done

# ---- static-only end to end: the create path, the headers, the hashed rule, the order ---------
start_fake "${site_fake[@]}"
seed other/index.html; seed site-admin/app.js; seed siteish.txt; seed index.html
m="$(mark)"
run "$site" deploy
expect_rc 0 "a static site's first deploy"
out_has "static group site: s3://$SITE_BUCKET (us-west-2), the app's folder site/" "the first deploy finds the group"
out_has "stack: cbase-app-site is new" "the first deploy makes the stack"
out_has "change set cbase-deploy on cbase-app-site (CREATE):" "the change set is printed"
out_has "Add     Distribution     AWS::CloudFront::Distribution" "the change set is printed"
out_has "site joined static group site: app=site folder=site/" "the app joins its static group"
out_has "deployed site: https://d1fake.cloudfront.net/" "the URL is printed"
since "$m" | grep -q "CFN ExecuteChangeSet cbase-app-site ok" || fail "the create's change set was not executed"
since "$m" | grep -q "DetectStackDrift" && fail "drift detected on a stack that did not exist"
since "$m" | grep -q "CF CreateInvalidation" && fail "a new distribution was invalidated"
printf 'cbase-state 1 static site\napp=site folder=site/\n' > "$t/want"
state_of static site | diff -u "$t/want" - || fail "the static group's state file after a deploy"
ok
# The template: the folder as OriginPath, the bucket's regional name, OAC, the router, no VM.
template_of cbase-app-site > "$t/tpl"
for want in "OriginPath: /site" "DomainName: $SITE_BUCKET.s3.us-west-2.amazonaws.com" "OriginAccessControlId: !GetAtt Oac.Id" \
            "FunctionARN: !GetAtt Router.FunctionMetadata.FunctionARN" "CloudFrontDefaultCertificate: true"; do
  grep -Fq -- "$want" "$t/tpl" || { cat "$t/tpl" >&2; fail "the stack's template lacks [$want]"; }
done
grep -Eq 'VmOrigin|TargetOriginId: vm|BucketPolicy|AWS::S3::' "$t/tpl" && fail "a static site's template names the VM, a bucket or a bucket policy"
ok
# Each file's headers.
check_head() {   # check_head KEY "TYPE|CACHE"
  local got
  got="$(head_of "site/$1")"
  [ "$got" = "$2" ] || fail "s3://$SITE_BUCKET/site/$1 has [$got], not [$2]"
}
NC=no-cache
IM="public, max-age=31536000, immutable"
check_head index.html "text/html; charset=utf-8|$NC"
check_head docs/index.html "text/html; charset=utf-8|$NC"
check_head app.wasm "application/wasm|$NC"
check_head style.css "text/css; charset=utf-8|$NC"
check_head data.json "application/json|$NC"
check_head font.WOFF2 "font/woff2|$NC"
check_head LICENSE "application/octet-stream|$NC"
check_head .well-known/security.txt "text/plain; charset=utf-8|$NC"
check_head .well-known/apple-app-site-association "application/json|$NC"
ok
# The hashed rule: a file under CBASE_HASHED is immutable for a year, html never.
check_head assets/app.1a2b3c4d.js "text/javascript; charset=utf-8|$IM"
check_head assets/logo.9f8e7d.png "image/png|$IM"
check_head assets/page.html "text/html; charset=utf-8|$NC"
ok
# Dot files are not uploaded (but .well-known/); nothing lands outside the app's folder.
since "$m" | grep -E '^PUT /'"$SITE_BUCKET"'/' > "$t/puts"
grep -Eq '/\.(DS_Store|git/)' "$t/puts" && fail "a dot file was uploaded"
[ "$(wc -l < "$t/puts" | tr -d ' ')" = 13 ] || { cat "$t/puts" >&2; fail "the deploy uploaded other than its 13 files"; }
grep -v "^PUT /$SITE_BUCKET/site/" "$t/puts" && fail "a deploy wrote outside its folder"
ok
# The order: the hashed files first, then the rest, the html last.
last_hashed="$(grep -n '/site/assets/[a-z]*\.[0-9a-f]*\.\(js\|png\) ' "$t/puts" | tail -1 | cut -d: -f1)"
first_other="$(grep -vn 'ct=text/html\|immutable' "$t/puts" | head -1 | cut -d: -f1)"
last_other="$(grep -vn 'ct=text/html\|immutable' "$t/puts" | tail -1 | cut -d: -f1)"
first_html="$(grep -n 'ct=text/html' "$t/puts" | head -1 | cut -d: -f1)"
{ [ "$last_hashed" -lt "$first_other" ] && [ "$last_other" -lt "$first_html" ]; } \
  || { cat "$t/puts" >&2; fail "the upload order is not hashed, other, html"; }
ok
# Someone else's objects are where they were.
for k in other/index.html site-admin/app.js siteish.txt index.html; do
  aws s3api head-object --bucket "$SITE_BUCKET" --key "$k" > /dev/null 2>&1 || fail "s3://$SITE_BUCKET/$k is gone"
done
ok

# ---- a redeploy: drift checked, no changes, the edge invalidated --------------------------------
m="$(mark)"
run "$site" deploy
expect_rc 0 "a redeploy"
out_has "drift: none" "a redeploy checks drift"
out_has "stack: cbase-app-site is up to date (no changes)" "an unchanged redeploy"
out_has "invalidation I1FAKE of /* on E1FAKEDIST" "a redeploy invalidates"
since "$m" | grep -q "CF CreateInvalidation E1FAKEDIST <Path>/\*</Path>" || fail "the invalidation was not /*"
since "$m" | grep -q "CFN ExecuteChangeSet" && fail "a change set with no changes was executed"
since "$m" | grep -q "^DELETE " && fail "a deploy without --prune deleted"
ok

# ---- prune: the build's leftovers in the app's folder, and nothing else -------------------------
seed site/stale.js; seed site/old/page.html
rm "$site/build/data.json"
m="$(mark)"
run "$site" deploy --prune
expect_rc 0 "a prune"
since "$m" | grep '^DELETE ' | sort > "$t/deleted"
printf 'DELETE /%s/site/data.json 204\nDELETE /%s/site/old/page.html 204\nDELETE /%s/site/stale.js 204\n' \
  "$SITE_BUCKET" "$SITE_BUCKET" "$SITE_BUCKET" > "$t/want"
diff -u "$t/want" "$t/deleted" || fail "the prune deleted other than the folder's three stale files"
out_has "pruned 3 files the build no longer has, under s3://$SITE_BUCKET/site/ alone" "the prune's count"
# The listing is the folder's alone: no other app's key is even read, let alone named.
grep -q "is outside site/" "$t/err" && { cat "$t/err" >&2; fail "the prune listed keys outside the app's folder"; }
ok
# A listing that answers keys outside the folder (as a wrong one might): still nothing outside is
# deleted, and each is named. CBASE_PRUNE=1 is the flag's spelling in the environment.
fake_set list-ignores-prefix=1
seed site/stale2.js
m="$(mark)"
rc=0
(cd "$site" && CBASE_PRUNE=1 "$tool" deploy) > "$t/out" 2> "$t/err" || rc=$?
expect_rc 0 "a prune over a listing of the whole bucket"
since "$m" | grep '^DELETE ' > "$t/deleted" || true
printf 'DELETE /%s/site/stale2.js 204\n' "$SITE_BUCKET" > "$t/want"
diff -u "$t/want" "$t/deleted" || fail "a prune deleted outside the app's folder"
for k in other/index.html site-admin/app.js siteish.txt index.html; do
  err_has "prune: s3://$SITE_BUCKET/$k is outside site/, and is left alone" "the folder guard"
  aws s3api head-object --bucket "$SITE_BUCKET" --key "$k" > /dev/null 2>&1 || fail "s3://$SITE_BUCKET/$k is gone"
done
fake_set list-ignores-prefix=0
ok

# ---- the safety rules --------------------------------------------------------------------------
puts_since() { since "$1" | grep -c "^PUT /$SITE_BUCKET/" || true; }
# A bucket replaced, or removed: refused, the change set deleted, nothing executed or uploaded.
for how in replace-bucket remove-bucket; do
  fake_set "changes=$how"
  m="$(mark)"
  run "$site" deploy
  expect_rc 1 "a change set that would $how"
  if [ "$how" = replace-bucket ]; then err_has "refused: the bucket Bucket would be replaced (Replacement: True)" "$how"
  else err_has "refused: the bucket Bucket would be removed" "$how"; fi
  err_has "refused, and the change set deleted; nothing was executed" "$how"
  since "$m" | grep -q "CFN DeleteChangeSet cbase-app-site ok" || fail "$how: the change set was not deleted"
  since "$m" | grep -q "CFN ExecuteChangeSet" && fail "$how: the change set was executed"
  [ "$(puts_since "$m")" = 0 ] || fail "$how: files were uploaded"
  ok
done
# A distribution replaced: allowed, and said.
fake_set changes=replace-distribution
m="$(mark)"
run "$site" deploy
expect_rc 0 "a distribution replacement"
out_has "Modify  Distribution     AWS::CloudFront::Distribution (replacement: True)" "a distribution replacement is printed"
out_has "note: the distribution Distribution is replaced: a new one with a new cloudfront.net name" "a distribution replacement is explained"
since "$m" | grep -q "CFN ExecuteChangeSet cbase-app-site ok" || fail "a distribution replacement was not executed"
fake_set changes=auto
ok
# Drift: printed and refused, then allowed with --accept-drift (and CBASE_ACCEPT_DRIFT=1).
fake_set drift=1
m="$(mark)"
run "$site" deploy
expect_rc 1 "a drifted stack"
err_has "drift: Distribution (AWS::CloudFront::Distribution) MODIFIED" "drift is printed"
err_has "drift:   Comment NOT_EQUAL: expected cbase app, actual edited by hand" "drift's values are printed"
err_has "the cbase-app-site stack drifted from its template (above)" "drift is refused"
since "$m" | grep -q "CFN CreateChangeSet" && fail "a drifted stack got a change set"
[ "$(puts_since "$m")" = 0 ] || fail "a drifted stack's files were uploaded"
ok
run "$site" deploy --accept-drift
expect_rc 0 "a drifted stack, accepted"
err_has "drift: accepted (--accept-drift)" "accepted drift"
ok
rc=0
(cd "$site" && CBASE_ACCEPT_DRIFT=1 "$tool" deploy) > "$t/out" 2> "$t/err" || rc=$?
expect_rc 0 "a drifted stack, accepted by CBASE_ACCEPT_DRIFT=1"
fake_set drift=0
ok
# DRY_RUN: the change set made, printed and deleted; nothing executed, uploaded or joined.
printf 'CBASE_APP = site\nCBASE_VM = none\nCBASE_HASHED = assets/*\nCBASE_ROUTE = /img/* static nocache\n' > "$site/cbase.conf"
m="$(mark)"
rc=0
(cd "$site" && DRY_RUN=1 "$tool" deploy) > "$t/out" 2> "$t/err" || rc=$?
expect_rc 0 "a dry run"
out_has "Modify  Distribution" "a dry run prints the change set"
out_has "dry run: change set cbase-deploy deleted unexecuted; nothing was uploaded, deleted or changed" "a dry run"
out_has "text/html; charset=utf-8  no-cache" "a dry run prints the upload"
since "$m" | grep -q "CFN DeleteChangeSet cbase-app-site ok" || fail "a dry run left its change set"
since "$m" | grep -qE "CFN ExecuteChangeSet|CF CreateInvalidation|^PUT |^DELETE " && fail "a dry run changed something"
ok
# A dry run of a new app: its empty stack goes too.
dry="$(app dry "CBASE_APP = dry" "CBASE_VM = none" "CBASE_STATIC = site")"
mkdir -p "$dry/build" && printf 'x\n' > "$dry/build/index.html"
m="$(mark)"
run "$dry" deploy --dry-run
expect_rc 0 "a new app's dry run"
since "$m" | grep -q "CFN DeleteStack cbase-app-dry" || fail "a new app's dry run left its empty stack"
[ "$(template_of cbase-app-dry)" = "(none)" ] || fail "a new app's dry run left a stack"
since "$m" | grep -qE "^PUT |CFN ExecuteChangeSet" && fail "a new app's dry run changed something"
ok

# ---- secrets in a change set, a drift and an error: never printed -------------------------------
# A value that looks like nothing (no long random run) so only the masking can keep it out.
SECRET=hunter2-origin-value
SECRET2=hunter3-origin-value
no_secret() {   # no_secret WHAT
  if grep -Fq -e "$SECRET" -e "$SECRET2" "$t/out" "$t/err"; then
    grep -F -e "$SECRET" -e "$SECRET2" "$t/out" "$t/err" >&2
    fail "$1: an origin header's value was printed"
  fi
}
fake_set "secret=$SECRET&secret2=$SECRET2&drift=1"
run "$site" deploy
expect_rc 1 "a drift holding an origin header"
err_has "drift:   origin header Origins/0/OriginCustomHeaders/0/HeaderValue changed (its value is not shown)" "the drift's header"
err_has "drift:   origin header Origins/0 changed (its value is not shown)" "the drift's whole origin"
err_has "drift:   Comment NOT_EQUAL: expected cbase app, actual edited by hand" "a plain drift is still shown"
no_secret "the drift refusal"
ok
run "$site" deploy --accept-drift
expect_rc 0 "a change set holding an origin header"
out_has "Comment: cbase app before -> cbase app after" "a plain change is shown"
out_has "origin header Origins/1/OriginCustomHeaders/0/HeaderValue changed (its value is not shown)" "the change set's header"
out_has "parameter OriginSecret changed Origins/1 (its value is not shown)" "the change set's parameter"
no_secret "the change set"
ok
fake_set "fail-execute=1&drift=0"
run "$site" deploy
expect_rc 1 "an execute that fails with the value in its message"
no_secret "the failed execute"
err_has "executing change set cbase-deploy on cbase-app-site failed:" "the failed execute"
err_has "(a line naming an origin header or a parameter value, not shown)" "the failed execute's message"
fake_set "fail-execute=0&secret=&secret2="
ok

# ---- certificates ------------------------------------------------------------------------------
# Found: an issued wildcard covers www.example.com (one of three, the deeper wildcard does not), and
# the zone gets the alias.
www="$(app www "CBASE_APP = www" "CBASE_VM = none" "CBASE_STATIC = site" "CBASE_DOMAIN = www.example.com")"
mkdir -p "$www/build" && printf 'x\n' > "$www/build/index.html"
m="$(mark)"
run "$www" deploy
expect_rc 0 "a domain with an issued certificate"
out_has "dns: www.example.com is in Route 53 zone Z0EXAMPLE" "the zone is found"
out_has "certificate: $CERT_WILD, issued, covers www.example.com" "the issued certificate is found"
since "$m" | grep -q "ACM RequestCertificate" && fail "a certificate was requested when one was issued"
template_of cbase-app-www > "$t/tpl"
grep -Fq "AcmCertificateArn: $CERT_WILD" "$t/tpl" || fail "the template lacks the certificate"
grep -Fq "Aliases: [www.example.com]" "$t/tpl" || fail "the template lacks the alias"
since "$m" | grep -q "R53 UPSERT Z0EXAMPLE www.example.com A alias=d" || fail "the A alias was not written"
since "$m" | grep -q "R53 UPSERT Z0EXAMPLE www.example.com AAAA alias=d" || fail "the AAAA alias was not written"
out_has "deployed www: https://www.example.com/" "the domain's URL"
ok
# A certificate that cannot be read stops the deploy: it is never taken for "none covers", which
# would request another.
fake_set acm-deny=1
m="$(mark)"
run "$www" deploy
expect_rc 1 "a certificate that cannot be read"
err_has "AWS refused it (AccessDenied)" "a certificate that cannot be read"
since "$m" | grep -qE "ACM RequestCertificate|CFN CreateChangeSet" && fail "a certificate read that failed went on"
fake_set acm-deny=0
ok
# Requested, with a zone: a.corp.example.com is under no wildcard (one label each), so a request;
# its validation CNAME goes in the longest public zone (example.com: corp.example.com is private),
# ACM issues it, and the alias follows.
deep="$(app deep "CBASE_APP = deep" "CBASE_VM = none" "CBASE_STATIC = site" "CBASE_DOMAIN = a.corp.example.com")"
mkdir -p "$deep/build" && printf 'x\n' > "$deep/build/index.html"
m="$(mark)"
run "$deep" deploy
expect_rc 0 "a domain whose certificate is requested, with a zone"
since "$m" | grep -q "ACM RequestCertificate a.corp.example.com" || fail "no certificate was requested for a.corp.example.com"
since "$m" | grep -qE "R53 UPSERT Z0EXAMPLE _v[0-9]+\.a\.corp\.example\.com\. CNAME _a[0-9]+\.acm-validations\.aws\." \
  || { since "$m" >&2; fail "the validation record was not written"; }
since "$m" | grep -q "ACM issued" || fail "the requested certificate was not issued"
since "$m" | grep -q "R53 UPSERT Z0EXAMPLE a.corp.example.com A alias=" || fail "the alias was not written"
since "$m" | grep -q "Z0PRIVATE" && fail "a record was written in a private zone"
out_has "deployed deep: https://a.corp.example.com/" "the requested certificate's URL"
ok
# Requested, no zone: the records printed, a clean stop (status 3), nothing else changed; the next
# deploy reuses the pending request.
other="$(app other "CBASE_APP = other" "CBASE_VM = none" "CBASE_STATIC = site" "CBASE_DOMAIN = shop.other.net")"
mkdir -p "$other/build" && printf 'x\n' > "$other/build/index.html"
m="$(mark)"
run "$other" deploy
expect_rc 3 "a domain with no zone"
out_has "dns: no Route 53 hosted zone in this account covers shop.other.net" "no zone"
grep -Eq "^  _v[0-9]+\.shop\.other\.net\.  CNAME  _a[0-9]+\.acm-validations\.aws\.   \(proves the certificate for shop\.other\.net\)$" "$t/out" \
  || { cat "$t/out" >&2; fail "the validation record was not printed"; }
out_has "then run ./cbase deploy again" "the clean stop says what next"
since "$m" | grep -qE "R53 UPSERT|CFN CreateChangeSet|^PUT " && fail "a deploy waiting on DNS changed something"
ok
m="$(mark)"
run "$other" deploy
expect_rc 3 "a domain with no zone, again"
out_has "requested before, still pending validation" "the pending certificate is reused"
since "$m" | grep -q "ACM RequestCertificate" && fail "a second certificate was requested"
ok

# ---- a named certificate: used as it is, never looked for or replaced ---------------------------
# CERT_WILD would be found first (by ARN); CBASE_CERT_ARN names CERT_NAMED, which is served.
named="$(app named "CBASE_APP = named" "CBASE_VM = none" "CBASE_STATIC = site" "CBASE_DOMAIN = www.example.com" "CBASE_CERT_ARN = $CERT_NAMED")"
mkdir -p "$named/build" && printf 'x\n' > "$named/build/index.html"
m="$(mark)"
run "$named" deploy
expect_rc 0 "a named certificate"
out_has "certificate: $CERT_NAMED (CBASE_CERT_ARN, used as it is)" "the named certificate is used"
since "$m" | grep -qE "ACM (ListCertificates|RequestCertificate)" && fail "a certificate was looked for or requested with CBASE_CERT_ARN set"
template_of cbase-app-named | grep -Fq "AcmCertificateArn: $CERT_NAMED" || fail "the template does not serve the named certificate"
ok
# One that does not cover the hostname stops the deploy (it is never swapped for another).
printf 'CBASE_APP = named\nCBASE_VM = none\nCBASE_STATIC = site\nCBASE_DOMAIN = example.com\nCBASE_CERT_ARN = %s\n' "$CERT_NAMED" > "$named/cbase.conf"
m="$(mark)"
run "$named" deploy
expect_rc 1 "a named certificate that does not cover the hostname"
err_has "CBASE_CERT_ARN $CERT_NAMED does not cover example.com" "a named certificate that does not cover the hostname"
since "$m" | grep -qE "ACM (ListCertificates|RequestCertificate)|CFN CreateChangeSet" && fail "a named certificate that does not cover went on"
ok
conf_error "cbase.conf line 4: CBASE_API_CERT_ARN is for CBASE_API_HOST, and there is none" "CBASE_APP = a" "CBASE_MEMORY = 1G" "CBASE_DOMAIN = a.example.com" \
  "CBASE_API_CERT_ARN = arn:aws:acm:us-east-1:$ACCOUNT:certificate/12345678-abcd-ef01-2345-6789abcdef01"

# ---- several hostnames: one distribution, one certificate, a record each -------------------------
# example.com and www.example.com: the issued example.com,*.example.com covers both, and each is an
# alias of the one distribution.
two="$(app two "CBASE_APP = two" "CBASE_VM = none" "CBASE_STATIC = site" "CBASE_DOMAIN = example.com www.example.com")"
mkdir -p "$two/build" && printf 'x\n' > "$two/build/index.html"
m="$(mark)"
run "$two" deploy
expect_rc 0 "two hostnames with an issued certificate"
out_has "certificate: $CERT_WILD, issued, covers example.com www.example.com" "one certificate covers both hostnames"
since "$m" | grep -q "ACM RequestCertificate" && fail "a certificate was requested when one covered both hostnames"
template_of cbase-app-two > "$t/tpl"
grep -Fq "Aliases: [example.com, www.example.com]" "$t/tpl" || { cat "$t/tpl" >&2; fail "the template lacks both aliases"; }
[ "$(grep -c "Aliases:" "$t/tpl")" = 1 ] || fail "two hostnames made more than one distribution's aliases"
for h in example.com www.example.com; do
  for ty in A AAAA; do
    since "$m" | grep -q "R53 UPSERT Z0EXAMPLE $h $ty alias=d" || fail "the $ty alias of $h was not written"
  done
done
out_has "deployed two: https://example.com/" "the first hostname is the URL"
ok
# A hostname no certificate covers (shop.org, in its own zone): a new request naming every
# hostname, its records written in each one's zone, and an alias in each.
printf 'CBASE_APP = two\nCBASE_VM = none\nCBASE_STATIC = site\nCBASE_DOMAIN = example.com www.example.com shop.org\n' > "$two/cbase.conf"
m="$(mark)"
run "$two" deploy
expect_rc 0 "three hostnames, one of them uncovered"
since "$m" | grep -q "ACM RequestCertificate example.com,www.example.com,shop.org" || { since "$m" | grep ACM >&2; fail "the request did not name every hostname"; }
since "$m" | grep -qE "R53 UPSERT Z0SHOPORG _v[0-9]+\.shop\.org\. CNAME" || fail "shop.org's validation record was not written in its zone"
since "$m" | grep -qE "R53 UPSERT Z0EXAMPLE _v[0-9]+\.www\.example\.com\. CNAME" || fail "www.example.com's validation record was not written"
since "$m" | grep -q "ACM issued" || fail "the certificate for three hostnames was not issued"
since "$m" | grep -q "R53 UPSERT Z0SHOPORG shop.org A alias=d" || fail "shop.org's alias was not written in its zone"
since "$m" | grep -q "R53 UPSERT Z0EXAMPLE www.example.com AAAA alias=d" || fail "www.example.com's alias was not written"
template_of cbase-app-two | grep -Fq "Aliases: [example.com, www.example.com, shop.org]" || fail "the template lacks the three aliases"
ok
# No zone for either hostname: the record to add is printed for each.
nz="$(app nz "CBASE_APP = nz" "CBASE_VM = none" "CBASE_STATIC = site" "CBASE_DOMAIN = a.nozone.net b.nozone.net")"
mkdir -p "$nz/build" && printf 'x\n' > "$nz/build/index.html"
run "$nz" deploy
expect_rc 0 "two hostnames with no zone"
out_has "certificate: $CERT_NOZONE, issued, covers a.nozone.net b.nozone.net" "the no-zone certificate"
for h in a.nozone.net b.nozone.net; do
  grep -Eq "^dns: point $h at d[0-9]+fake\.cloudfront\.net where its DNS is served" "$t/out" || { cat "$t/out" >&2; fail "no record printed for $h"; }
done
ok
conf_error "cbase.conf line 2: CBASE_DOMAIN names www.example.com twice" "CBASE_APP = a" "CBASE_DOMAIN = www.example.com www.example.com"
conf_error "cbase.conf line 2: CBASE_DOMAIN is lower-case hostnames, blank-separated" "CBASE_APP = a" "CBASE_DOMAIN = example.com WWW.example.com"
conf_error "cbase.conf line 3: CBASE_API_HOST api.example.com is one of CBASE_DOMAIN's hostnames" "CBASE_APP = a" "CBASE_DOMAIN = example.com api.example.com" "CBASE_API_HOST = api.example.com"
shape domains "app        site
vm         none
static     site
files      build/ (the default)
domain     example.com www.example.com
cert       (found or requested by deploy)
route      * static cache GET,HEAD" "CBASE_APP = site" "CBASE_VM = none" "CBASE_DOMAIN =  example.com   www.example.com"

# ---- what deploy refuses -----------------------------------------------------------------------
# A static group that does not exist: the group-init command, nothing made.
ghost="$(app ghost "CBASE_APP = ghost" "CBASE_VM = none" "CBASE_STATIC = nosuch")"
m="$(mark)"
run "$ghost" deploy
expect_rc 1 "a missing static group"
err_has "Create it with: ./cbase group-init static nosuch" "a missing static group"
since "$m" | grep -qE "^CFN|^PUT " && fail "a deploy to a missing group made something"
ok
# A static dir that is not a build.
mkdir -p "$ghost/build"
printf 'CBASE_APP = ghost\nCBASE_VM = none\nCBASE_STATIC = site\n' > "$ghost/cbase.conf"
run "$ghost" deploy
expect_rc 1 "a build with no index.html"
err_has "build has no index.html, so it is not a built site" "a build with no index.html"
printf 'CBASE_APP = ghost\nCBASE_VM = none\nCBASE_STATIC = site\nCBASE_STATIC_DIR = dist\n' > "$ghost/cbase.conf"
run "$ghost" deploy
expect_rc 1 "a missing static dir"
err_has "the static dir dist does not exist" "a missing static dir"
run "$ghost" deploy --force
expect_rc 2 "an unknown deploy flag"
ok
conf_error "cbase.conf line 2: CBASE_STATIC_DIR is a directory inside the repository" "CBASE_APP = a" "CBASE_STATIC_DIR = ../site"
conf_error "cbase.conf line 2: CBASE_STATIC_DIR is a directory inside the repository" "CBASE_APP = a" "CBASE_STATIC_DIR = /srv/www"
conf_error "cbase.conf line 3: CBASE_STATIC_DIR is the static files' directory, and CBASE_STATIC is none" "CBASE_APP = a" "CBASE_STATIC = none" "CBASE_STATIC_DIR = dist"
conf_error "cbase.conf line 2: CBASE_HASHED is blank-separated globs" "CBASE_APP = a" "CBASE_HASHED = /assets/*"

# ---- the template: routes on the VM only for an app with one -----------------------------------
full="$(app full "CBASE_APP = shop" "CBASE_ROUTE = /api/* vm" "CBASE_ROUTE = /live ws" "CBASE_ROUTE = /img/* static nocache GET,OPTIONS" \
          "CBASE_ROUTE = /form vm POST" "CBASE_ROUTE = /read vm cache GET")"
run "$full" template
expect_rc 0 "a full app's template"
behaviour() {   # behaviour PATTERN: the lines of that route's behaviour in $t/out
  awk -v p="          - PathPattern: \"$1\"" '$0 == p {f=1; print; next} f && /^          - / {f=0} f && /^[^ ]/ {f=0} f' "$t/out"
}
behaviour /api/* | grep -q "TargetOriginId: vm" || fail "/api/* is not on the VM"
behaviour /api/* | grep -q "AllowedMethods: \[GET, HEAD, OPTIONS, PUT, PATCH, POST, DELETE\]" || fail "/api/* has not every method"
behaviour /api/* | grep -q "CachePolicyId: $CF_DISABLED" || fail "/api/* is cached"
behaviour /live | grep -q "TargetOriginId: ws" || fail "/live is not on the ws origin"
behaviour /live | grep -q "Compress: false" || fail "/live is compressed"
behaviour /img/* | grep -q "AllowedMethods: \[GET, HEAD, OPTIONS\]" || fail "/img/* is not GET,HEAD,OPTIONS"
behaviour /img/* | grep -q "FunctionARN: !GetAtt Router" || fail "/img/* has no router"
behaviour /form | grep -q "AllowedMethods: \[GET, HEAD, OPTIONS, PUT, PATCH, POST, DELETE\]" || fail "POST alone is not all seven"
behaviour /read | grep -q "AllowedMethods: \[GET, HEAD\]" || fail "/read is not GET,HEAD"
behaviour /read | grep -q "CachePolicyId: $CF_OPTIMIZED" || fail "/read is not cached"
awk '/- Id: ws/{f=1} f' "$t/out" | grep -q "OriginReadTimeout: 60" || fail "the ws origin's read timeout"
awk '/- Id: ws/{f=1} f' "$t/out" | grep -q "OriginKeepaliveTimeout: 60" || fail "the ws origin's keep-alive"
out_has "  VmOrigin:" "a full app's template has the VM origin parameter"
ok
# CBASE_API_HOST's distribution: everything to the VM, and a ws route on the ws origin there too.
apiws="$(app apiws "CBASE_APP = shop" "CBASE_ROUTE = /mqtt* ws" "CBASE_ROUTE = /ws* ws" "CBASE_ROUTE = /api/* vm" \
           "CBASE_DOMAIN = shop.example.com" "CBASE_API_HOST = api.example.com")"
run "$apiws" template
expect_rc 0 "a template with CBASE_API_HOST and ws routes"
awk '/^  ApiDistribution:$/{f=1} f' "$t/out" > "$t/api"
api_behaviour() {   # api_behaviour PATTERN: that route's behaviour on the ApiDistribution
  awk -v p="          - PathPattern: \"$1\"" '$0 == p {f=1; print; next} f && /^          - / {f=0} f && /^[^ ]/ {f=0} f' "$t/api"
}
for p in /mqtt* /ws*; do
  api_behaviour "$p" | grep -q "TargetOriginId: ws" || { cat "$t/api" >&2; fail "$p is not on the API host's ws origin"; }
  api_behaviour "$p" | grep -q "AllowedMethods: \[GET, HEAD, OPTIONS, PUT, PATCH, POST, DELETE\]" || fail "$p on the API host has not every method"
  api_behaviour "$p" | grep -q "CachePolicyId: $CF_DISABLED" || fail "$p on the API host is cached"
done
awk '/- Id: ws/{f=1} f' "$t/api" | grep -q "OriginReadTimeout: 60" || fail "the API host's ws origin's read timeout"
awk '/- Id: ws/{f=1} f' "$t/api" | grep -q "OriginKeepaliveTimeout: 60" || fail "the API host's ws origin's keep-alive"
[ -z "$(api_behaviour /api/*)" ] || fail "a vm route is a behaviour of its own on the API host (its default is the VM)"
ok
run "$site" template
expect_rc 0 "a static site's template"
grep -Eq "VmOrigin|TargetOriginId: (vm|ws)" "$t/out" && fail "a static site's template names the VM"
ok

# ---- the router, run: its code from the template, driven under node ---------------------------
# router_of: the Router's code in the template $t/out, into $t/router.js.
router_of() {
  awk '/^      FunctionCode: \|$/ {f=1; next} f && /^        / {sub(/^        /, ""); print; next} f {exit}' "$t/out" > "$t/router.js"
  [ -s "$t/router.js" ] || { cat "$t/out" >&2; fail "the template has no Router code"; }
}
# routes URI...: each URI through the router's handler, a line each: "URI -> <uri the origin is
# asked for>".
routes() {
  node -e '
    const handler = new Function(require("fs").readFileSync(process.argv[1], "utf8") + "\nreturn handler;")();
    for (const uri of process.argv.slice(2))
      console.log(uri + " -> " + handler({ request: { method: "GET", uri, querystring: {}, headers: {}, cookies: {} } }).uri);
  ' "$t/router.js" "$@"
}
if ! command -v node > /dev/null 2>&1; then
  # CI's runners have node; a machine without it skips the router's run, and says so.
  [ -z "${CI:-}" ] || fail "node is not on PATH, and CI runs the router's code under it"
  echo "cbase_test: node is not on PATH: the router's code was not run" >&2
else
  spa="$(app spa "CBASE_APP = spa" "CBASE_VM = none")"
  mkdir -p "$spa/build/docs" "$spa/build/blog" "$spa/build/.well-known" "$spa/build/assets"
  for f in index.html privacy.html _shell.html docs/index.html blog/post.html assets/app.js .well-known/apple-app-site-association; do
    printf 'x\n' > "$spa/build/$f"
  done
  run "$spa" template
  expect_rc 0 "an SPA's template"
  out_has "var PAGES = ' _shell blog/post docs/index index privacy ';" "the router's page list is the build's"
  out_has "var SHELL = '/index.html';" "the shell defaults to /index.html"
  router_of
  # (a) files as they are, (b) clean URLs of real pages, (c) anything else the shell; a missing
  # asset (an extension) is passed to S3, which answers 404, never the shell.
  routes /assets/app.js /privacy.html /.well-known/apple-app-site-association \
         / /privacy /docs/ /blog/post \
         /abc123 /game/7/replay /docs /privacy/ /blog/ \
         /assets/missing.js > "$t/got"
  printf '%s\n' "/assets/app.js -> /assets/app.js" "/privacy.html -> /privacy.html" \
    "/.well-known/apple-app-site-association -> /.well-known/apple-app-site-association" \
    "/ -> /index.html" "/privacy -> /privacy.html" "/docs/ -> /docs/index.html" "/blog/post -> /blog/post.html" \
    "/abc123 -> /index.html" "/game/7/replay -> /index.html" "/docs -> /index.html" "/privacy/ -> /index.html" "/blog/ -> /index.html" \
    "/assets/missing.js -> /assets/missing.js" > "$t/want"
  diff -u "$t/want" "$t/got" || fail "the router mapped a path otherwise"
  ok
  # CBASE_SHELL overrides the shell's file; pages and files are as before.
  printf 'CBASE_APP = spa\nCBASE_VM = none\nCBASE_SHELL = /_shell.html\n' > "$spa/cbase.conf"
  run "$spa" template
  expect_rc 0 "an SPA's template with CBASE_SHELL"
  router_of
  routes /abc123 /a/b / /privacy /assets/app.js > "$t/got"
  printf '%s\n' "/abc123 -> /_shell.html" "/a/b -> /_shell.html" "/ -> /index.html" "/privacy -> /privacy.html" "/assets/app.js -> /assets/app.js" > "$t/want"
  diff -u "$t/want" "$t/got" || fail "the router with CBASE_SHELL mapped a path otherwise"
  ok
  # A shell the build does not hold stops the render.
  printf 'CBASE_APP = spa\nCBASE_VM = none\nCBASE_SHELL = /app.html\n' > "$spa/cbase.conf"
  run "$spa" template
  expect_rc 1 "a shell the build lacks"
  err_has "the app shell is /app.html (CBASE_SHELL), and the static dir has no app.html" "a shell the build lacks"
  ok
fi
conf_error "cbase.conf line 2: CBASE_SHELL is a page of the build, an absolute path ending in .html" "CBASE_APP = a" "CBASE_SHELL = _shell.html"
conf_error "cbase.conf line 2: CBASE_SHELL is a page of the build, an absolute path ending in .html" "CBASE_APP = a" "CBASE_SHELL = /../x.html"
conf_error "cbase.conf line 3: CBASE_SHELL is a page of the static files, and CBASE_STATIC is none" "CBASE_APP = a" "CBASE_STATIC = none" "CBASE_SHELL = /_shell.html"

[ "$(log_count unsigned)" = 0 ] || fail "a call reached the fake without the test's credentials"
echo "cbase_test: ok ($pass checks)"
