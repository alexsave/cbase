#!/bin/bash
# site_test.sh - the static-site deploy (providers/aws/ops/site) with no AWS call: a fake `aws`
# on PATH records its argv and answers from scripted files, and the template is rendered offline.
#
#   bash providers/aws/ops/site/test/site_test.sh      (cbase's make check runs it: site-test)
#
# The cases:
#   * upload: each file's Content-Type and Cache-Control from the one table (html revalidated,
#     content-hashed files immutable, .wasm application/wasm, the product's rule first), the
#     hashed files before the rest and the html last, the page list between them;
#   * the router's page list: rewritten on the previous template with the other parameters kept,
#     and left alone when unchanged;
#   * the invalidation: one, of /*, on the stack's distribution;
#   * deletes: a file the build dropped goes, a stale content-hashed file stays without --prune
#     and goes with it, a live one never goes; a key prefix keeps every key under it;
#   * the stack: drift refuses and executes nothing (printing the drifted property);
#     --accept-drift proceeds; no drift proceeds; a change set replacing the distribution or the
#     bucket is refused and deleted; --dry-run deletes the change set and executes nothing;
#     no changes executes nothing; a new stack skips drift detection;
#   * render: the routes' behaviours and origins in order, with their policies; a route to an
#     unknown origin, an origin no route uses and an unknown option are refused;
#   * the router's rules, run in node when node is on PATH;
#   * site.mk alone (no CBASE, no cbase.mk): site-template, PRUNE=1, DRY_RUN=1, ACCEPT_DRIFT=1;
#   * the OIDC role grants every call site_deploy.sh makes (role_covers_release.sh).
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
t="$(mktemp -d)"
trap 'rm -rf "$t"' EXIT
fail() { echo "site_test: FAIL: $*" >&2; exit 1; }
LOG="$t/log"
has() { grep -Fq -- "$1" "$LOG" || fail "$2: the log lacks [$1]"; }
hasnt() { if grep -Fq -- "$1" "$LOG"; then fail "$2: the log holds [$1]"; fi; }
REVAL='public, max-age=0, s-maxage=86400, must-revalidate'
IMM='public, max-age=31536000, immutable'

# ---- the fake aws ---------------------------------------------------------------------------
mkdir -p "$t/bin" "$t/fake"
cat > "$t/bin/aws" <<'S'
#!/bin/bash
printf 'AWS %s\n' "$*" >> "$FAKE_LOG"
q=""; ct=""; cc=""; prev=""
for a in "$@"; do
  case "$prev" in --query) q="$a" ;; --content-type) ct="$a" ;; --cache-control) cc="$a" ;; esac
  prev="$a"
done
F="$FAKE_DIR"
case "$1 $2" in
  "cloudformation describe-stacks")
    if [ -f "$F/no_stack" ]; then
      echo "An error occurred (ValidationError) when calling the DescribeStacks operation: Stack with id x does not exist" >&2; exit 254
    fi
    case "$q" in
      *"OutputKey=='BucketName'"*) echo site-bucket ;;
      *"OutputKey=='DistributionId'"*) echo EDIST1 ;;
      *"ParameterKey=='Pages'"*) cat "$F/pages" ;;
      *"Parameters[].ParameterKey"*) printf 'Pages\tOriginSecretVm\n' ;;
      *StackStatus*) echo UPDATE_COMPLETE ;;
      *) echo "(outputs)" ;;
    esac ;;
  "s3 sync"|"s3 cp")
    (cd "$3" && find . -type f | sed 's|^\./||' | LC_ALL=C sort) | while IFS= read -r f; do
      printf 'PUT %s/%s|%s|%s\n' "$4" "$f" "$ct" "$cc" >> "$FAKE_LOG"
    done ;;
  "s3api list-objects-v2") cat "$F/keys" ;;
  "cloudfront create-invalidation") echo I1 ;;
  "cloudformation detect-stack-drift") echo DD1 ;;
  "cloudformation describe-stack-drift-detection-status")
    if [ ! -f "$F/polled" ]; then touch "$F/polled"; printf 'DETECTION_IN_PROGRESS\tNone\tNone\n'
    else printf 'DETECTION_COMPLETE\t%s\tNone\n' "$(cat "$F/drift_status")"; fi ;;
  "cloudformation describe-stack-resource-drifts")
    case "$q" in
      *PropertyDifferences*) printf 'NOT_EQUAL\t/DistributionConfig/Origins/1/DomainName\n' ;;
      *) cat "$F/drifts" ;;
    esac ;;
  "cloudformation execute-change-set") rm -f "$F/no_stack" ;;
  "cloudformation create-change-set") echo arn:aws:cloudformation:us-east-1:000000000000:changeSet/cs/1 ;;
  "cloudformation wait")
    if [ "$3" = change-set-create-complete ] && [ -f "$F/no_changes" ]; then exit 255; fi ;;
  "cloudformation describe-change-set")
    case "$q" in
      *"Replacement=='True'"*) cat "$F/replacing" ;;
      StatusReason) echo "The submitted information didn't contain changes. Submit different information to create a change set." ;;
      *) echo "(change set table)" ;;
    esac ;;
esac
exit 0
S
chmod +x "$t/bin/aws"
export PATH="$t/bin:$PATH" FAKE_LOG="$LOG" FAKE_DIR="$t/fake" SITE_POLL_SECONDS=0 AWS_REGION=us-east-1

# Each run starts from a clean log and fake state, and only the knobs it sets.
reset() {
  rm -rf "$t/fake"; mkdir -p "$t/fake"; : > "$LOG"
  echo None > "$t/fake/pages"; : > "$t/fake/keys"; : > "$t/fake/drifts"; : > "$t/fake/replacing"
  echo IN_SYNC > "$t/fake/drift_status"
  unset SITE_PREFIX SITE_SHELL SITE_HASHED SITE_RULES SITE_SECRET_vm
}

# ---- upload ---------------------------------------------------------------------------------
site="$t/site"
mkdir -p "$site/assets" "$site/docs" "$site/.well-known"
for f in index.html about.html docs/intro.html _shell.html 404.html assets/app.3f2a.js assets/app.3f2a.css \
         assets/page.html logo.svg robots.txt mod.wasm .well-known/apple-app-site-association data.json; do
  echo "$f" > "$site/$f"
done
printf '.well-known/apple-app-site-association\tapplication/json\trevalidate\n' > "$t/rules.tsv"
upload() {
  SITE_STACK=s SITE_DIR="$site" SITE_HASHED='assets/*' SITE_RULES="$t/rules.tsv" SITE_SHELL=/_shell.html \
    bash "$here/site_deploy.sh" "$@" > "$t/out" 2>&1 || { cat "$t/out"; fail "site_deploy.sh $* failed"; }
}

reset
printf 'index.html\told.html\tassets/app.3f2a.js\tassets/old.1111.js\n' > "$t/fake/keys"
upload
D=s3://site-bucket
has "PUT $D/index.html|text/html; charset=utf-8|$REVAL" "html is revalidated (no-cache)"
has "PUT $D/docs/intro.html|text/html; charset=utf-8|$REVAL" "nested html"
has "PUT $D/assets/app.3f2a.js|text/javascript; charset=utf-8|$IMM" "a hashed file is immutable"
has "PUT $D/assets/app.3f2a.css|text/css; charset=utf-8|$IMM" "a hashed stylesheet is immutable"
has "PUT $D/assets/page.html|text/html; charset=utf-8|$REVAL" "html under a hashed glob is still revalidated"
has "PUT $D/logo.svg|image/svg+xml|$REVAL" "an unhashed asset is revalidated"
has "PUT $D/mod.wasm|application/wasm|$REVAL" ".wasm is application/wasm"
has "PUT $D/robots.txt|text/plain; charset=utf-8|$REVAL" "txt"
has "PUT $D/.well-known/apple-app-site-association|application/json|$REVAL" "the product's rule wins"
[ "$(grep -c '^PUT ' "$LOG")" = 13 ] || fail "13 files, $(grep -c '^PUT ' "$LOG") uploads"
order="$(grep -E '^PUT |update-stack' "$LOG" | cut -c1-60 | tr '\n' ';')"
lastimm="$(grep -n '^PUT ' "$LOG" | grep -F "|$IMM" | tail -1 | cut -d: -f1)"
firstother="$(grep -n '^PUT ' "$LOG" | grep -vF "|$IMM" | head -1 | cut -d: -f1)"
[ "$lastimm" -lt "$firstother" ] || fail "hashed files are not all before the rest: $order"
pages_at="$(grep -n 'update-stack' "$LOG" | cut -d: -f1)"
firsthtml="$(grep -n '^PUT ' "$LOG" | grep -F 'text/html' | head -1 | cut -d: -f1)"
lastnonhtml="$(grep -n '^PUT ' "$LOG" | grep -vF 'text/html' | tail -1 | cut -d: -f1)"
[ "$lastnonhtml" -lt "$pages_at" ] && [ "$pages_at" -lt "$firsthtml" ] || fail "the order is not assets, page list, html: $order"
has "update-stack --stack-name s --use-previous-template --parameters ParameterKey=Pages,ParameterValue=404 _shell about docs/intro index ParameterKey=OriginSecretVm,UsePreviousValue=true" "the page list is rewritten on the previous template"
has "cloudformation wait stack-update-complete --stack-name s" "the page list update is waited for"
has "cloudfront create-invalidation --distribution-id EDIST1 --paths /* " "one invalidation of /*"
has "cloudfront wait invalidation-completed --distribution-id EDIST1 --id I1" "the invalidation is waited for"
[ "$(grep -c 'create-invalidation' "$LOG")" = 1 ] || fail "more than one invalidation"
has "s3 rm s3://site-bucket/old.html" "a page the build dropped is deleted"
hasnt "s3://site-bucket/assets/old.1111.js" "a stale hashed file stays without --prune"
hasnt "s3 rm s3://site-bucket/index.html" "a live page is never deleted"

# unchanged pages: no stack update.
reset
printf '404 _shell about docs/intro index\n' > "$t/fake/pages"
upload
hasnt "update-stack" "an unchanged page list does not update the stack"

# --prune: the stale hashed file goes, the live one stays.
reset
printf 'index.html\tassets/app.3f2a.js\tassets/old.1111.js\nassets/old.2222.css\n' > "$t/fake/keys"
upload --prune
has "s3 rm s3://site-bucket/assets/old.1111.js" "--prune deletes a stale hashed file"
has "s3 rm s3://site-bucket/assets/old.2222.css" "--prune reads every page of the listing"
hasnt "s3 rm s3://site-bucket/assets/app.3f2a.js" "--prune keeps a live hashed file"
[ "$(grep -c 's3 rm' "$LOG")" = 2 ] || fail "--prune deleted $(grep -c 's3 rm' "$LOG") keys, not 2"
inv_at="$(grep -n 'create-invalidation' "$LOG" | cut -d: -f1)"
prune_at="$(grep -n 's3 rm' "$LOG" | head -1 | cut -d: -f1)"
[ "$inv_at" -lt "$prune_at" ] || fail "the prune ran before the invalidation"

# --force uploads with cp, not sync.
reset
upload --force
hasnt "AWS s3 sync" "--force uploads with cp"
has "AWS s3 cp " "--force uploads with cp"

# A prefix: keys under it, and nothing outside it is touched.
reset
printf 'www/index.html\twww/gone.html\n' > "$t/fake/keys"
SITE_PREFIX=www upload
has "PUT s3://site-bucket/www/index.html|" "a prefix puts keys under it"
has "list-objects-v2 --bucket site-bucket --prefix www/" "the listing is of the prefix"
has "s3 rm s3://site-bucket/www/gone.html" "a dropped file under the prefix is deleted"

# No index.html: nothing uploaded, nothing deleted.
reset
mkdir -p "$t/empty"
if SITE_STACK=s SITE_DIR="$t/empty" bash "$here/site_deploy.sh" > "$t/out" 2>&1; then fail "a directory with no index.html deployed"; fi
hasnt "s3 " "a directory with no index.html touches nothing"

# ---- render ---------------------------------------------------------------------------------
export SITE_STACK=sample
render() { bash "$here/site_stack.sh" render; }
reset
SITE_ORIGINS="vm=vm.example.com,proto=http-only,http=8080,read=60,keepalive=60,ip=ipv6,header=X-Origin-Secret api=api.example.com" \
SITE_ROUTES="/m/*=vm,request=AllViewerExceptHostHeader /api/*=api,cache=CachingDisabled,request=AllViewer,methods=all /.well-known/*=s3,compress=no" \
  render > "$t/r.yaml"
behaviours="$(grep -E '^          - PathPattern:|^            TargetOriginId:|^            CachePolicyId:|^            OriginRequestPolicyId:|^            Compress:' "$t/r.yaml" | tr -s ' ' | tr '\n' ';')"
want=" - PathPattern: /m/*; TargetOriginId: vm; Compress: true; CachePolicyId: 658327ea-f89d-4fab-a63d-7e88639e58f6; OriginRequestPolicyId: b689b0a8-53d0-40ab-baf2-68738e2966ac;"
want="$want - PathPattern: /api/*; TargetOriginId: api; Compress: true; CachePolicyId: 4135ea2d-6df8-44a3-9df3-4b5a84be39ad; OriginRequestPolicyId: 216adef6-5c7f-47e4-b989-5492eafa07d3;"
want="$want - PathPattern: /.well-known/*; TargetOriginId: s3; Compress: false; CachePolicyId: 658327ea-f89d-4fab-a63d-7e88639e58f6;"
[ "$behaviours" = "$want" ] || fail "render's behaviours: got [$behaviours], want [$want]"
origins="$(sed -n '/^        Origins:/,/^        DefaultCacheBehavior:/p' "$t/r.yaml" | grep -E '^          - Id:' | tr -s ' ' | tr '\n' ';')"
[ "$origins" = " - Id: s3; - Id: vm; - Id: api;" ] || fail "render's origins: [$origins]"
grep -q 'AllowedMethods: \[GET, HEAD, OPTIONS, PUT, PATCH, POST, DELETE\]' "$t/r.yaml" || fail "methods=all"
grep -q 'HeaderValue: !Ref OriginSecretVm' "$t/r.yaml" || fail "the header's value is the NoEcho parameter"
grep -q '^  OriginSecretVm:' "$t/r.yaml" || fail "no OriginSecretVm parameter"
for want in 'OriginProtocolPolicy: http-only' 'HTTPPort: 8080' 'IpAddressType: ipv6' 'OriginReadTimeout: 60'; do
  grep -q "$want" "$t/r.yaml" || fail "the vm origin lacks $want"
done
[ "$(grep -c 'FunctionARN: !GetAtt Router' "$t/r.yaml")" = 2 ] || fail "the router runs on the default and the S3 route only"
if grep -q '@@\|#@' "$t/r.yaml"; then fail "a marker survived the render"; fi
render_fails() { if "$@" bash "$here/site_stack.sh" render > /dev/null 2>&1; then fail "render accepted $*"; fi; }
render_fails env SITE_ROUTES="/m/*=nope"
render_fails env SITE_ORIGINS="vm=vm.example.com"
render_fails env SITE_ORIGINS="vm=vm.example.com,colour=red" SITE_ROUTES="/m/*=vm"
render_fails env SITE_ROUTES="/m/*=s3,methods=all"
render_fails env SITE_ALIASES="www.example.com"
render_fails env SITE_PREFIX="/lead"
# The S3-only render (no routes) has no CacheBehaviors.
render > "$t/plain.yaml"
if grep -q CacheBehaviors "$t/plain.yaml"; then fail "an S3-only site has behaviours"; fi
# cfn-lint, when it is installed (CI's ops job runs it on these renders too).
if command -v cfn-lint >/dev/null 2>&1; then cfn-lint --regions us-east-1 -- "$t/r.yaml" "$t/plain.yaml" "$here/site_role.yaml"; fi

# ---- the router's rules ---------------------------------------------------------------------
if command -v node >/dev/null 2>&1; then
  SITE_SHELL=/_shell.html SITE_PREFIX=www SITE_REDIRECTS="/old=/new" render > "$t/router.yaml"
  awk '/FunctionCode: !Sub \|/ { on = 1; next } on && NF && !/^        / { on = 0 } on && NF { print }' "$t/router.yaml" \
    | sed 's/^        //' | sed "s/\${Pages}/about docs\/intro index/" > "$t/router.js"
  cat >> "$t/router.js" <<'J'
var cases = [
  ['/', '/www/index.html'], ['/about', '/www/about.html'], ['/about/', '/www/about.html'],
  ['/docs/intro', '/www/docs/intro.html'], ['/abc123', '/www/_shell.html'], ['/a/b', '/www/404.html'],
  ['/app.js', '/www/app.js'], ['/.well-known/thing', '/www/.well-known/thing']];
for (var i = 0; i < cases.length; i++) {
  var r = handler({ request: { uri: cases[i][0], querystring: {} } });
  if (r.uri !== cases[i][1]) { console.log('router: ' + cases[i][0] + ' -> ' + r.uri + ', want ' + cases[i][1]); process.exit(1); }
}
var red = handler({ request: { uri: '/old', querystring: { a: { value: '1' } } } });
if (red.statusCode !== 302 || red.headers.location.value !== '/new?a=1') { console.log('router: the redirect'); process.exit(1); }
J
  node "$t/router.js" || fail "the router's rules"
  # With no shell, every extensionless path gets .html.
  render > "$t/router2.yaml"
  awk '/FunctionCode: !Sub \|/ { on = 1; next } on && NF && !/^        / { on = 0 } on && NF { print }' "$t/router2.yaml" \
    | sed 's/^        //' | sed "s/\${Pages}//" > "$t/router2.js"
  echo "var r = handler({ request: { uri: '/a/b', querystring: {} } }); if (r.uri !== '/a/b.html') { console.log(r.uri); process.exit(1); }" >> "$t/router2.js"
  node "$t/router2.js" || fail "the router with no shell"
else
  echo "site_test: node is not on PATH: the router's rules were not run"
fi

# ---- the stack ------------------------------------------------------------------------------
stack() { bash "$here/site_stack.sh" deploy "$@" > "$t/out" 2>&1; }
export SITE_ORIGINS="vm=vm.example.com" SITE_ROUTES="/m/*=vm"

# Drift: refused, the property printed, no change set made, nothing executed.
reset
echo DRIFTED > "$t/fake/drift_status"
printf 'Distribution\tAWS::CloudFront::Distribution\tMODIFIED\n' > "$t/fake/drifts"
if stack; then cat "$t/out"; fail "a drifted stack was updated"; fi
grep -q '/DistributionConfig/Origins/1/DomainName' "$t/out" || fail "the drifted property is not printed"
has "detect-stack-drift --stack-name sample" "drift detection runs"
hasnt "create-change-set" "a drifted stack gets no change set"
hasnt "execute-change-set" "a drifted stack executes nothing"

# --accept-drift proceeds.
reset
echo DRIFTED > "$t/fake/drift_status"
printf 'Distribution\tAWS::CloudFront::Distribution\tMODIFIED\n' > "$t/fake/drifts"
stack --accept-drift || { cat "$t/out"; fail "--accept-drift did not proceed"; }
has "execute-change-set" "--accept-drift executes"

# No drift: the change set is made with Pages kept, printed and executed.
reset
stack || { cat "$t/out"; fail "a clean update failed"; }
has "create-change-set --stack-name sample" "a change set"
has "--change-set-type UPDATE" "an update"
has "ParameterKey=Pages,UsePreviousValue=true" "Pages is kept"
has "execute-change-set --change-set-name arn:aws:cloudformation:us-east-1:000000000000:changeSet/cs/1" "executed"
has "wait stack-update-complete --stack-name sample" "waited for"
grep -q '(change set table)' "$t/out" || fail "the change set is not printed"

# A replacement of the distribution or the bucket: refused, deleted, nothing executed.
for r in 'Distribution	AWS::CloudFront::Distribution	Modify	True' 'Bucket	AWS::S3::Bucket	Modify	Conditional'; do
  reset
  printf '%s\n' "$r" > "$t/fake/replacing"
  if stack; then fail "a change set replacing ${r%%	*} was executed"; fi
  hasnt "execute-change-set" "a replacing change set executes nothing"
  has "delete-change-set" "a refused change set is deleted"
  grep -q "refused: ${r%%	*}" "$t/out" || fail "the refusal does not name ${r%%	*}"
done

# --dry-run: printed, deleted, nothing executed.
reset
stack --dry-run || { cat "$t/out"; fail "--dry-run failed"; }
has "delete-change-set" "--dry-run deletes the change set"
hasnt "execute-change-set" "--dry-run executes nothing"
grep -q '(change set table)' "$t/out" || fail "--dry-run does not print the change set"

# No changes: nothing executed, success.
reset
touch "$t/fake/no_changes"
stack || { cat "$t/out"; fail "an unchanged stack failed"; }
hasnt "execute-change-set" "no changes executes nothing"
grep -q 'no changes' "$t/out" || fail "no changes is not said"

# A new stack: no drift detection, a CREATE change set; with --dry-run the empty stack goes too.
reset
touch "$t/fake/no_stack"
stack --dry-run || { cat "$t/out"; fail "a new stack's dry run failed"; }
hasnt "detect-stack-drift" "a new stack has no drift to detect"
has "--change-set-type CREATE" "a create"
has "delete-stack --stack-name sample" "a dry run's empty stack is deleted"

# A secret origin header on a new stack needs SITE_SECRET_<id>.
reset
touch "$t/fake/no_stack"
if SITE_ORIGINS="vm=vm.example.com,header=X-Origin-Secret" stack; then fail "a new stack took no header value"; fi
grep -q 'set SITE_SECRET_vm' "$t/out" || fail "the missing secret is not named"
reset
touch "$t/fake/no_stack"
SITE_SECRET_vm=0123456789abcdef0123 SITE_ORIGINS="vm=vm.example.com,header=X-Origin-Secret" stack || { cat "$t/out"; fail "a new stack with its secret failed"; }
has "ParameterKey=OriginSecretVm,ParameterValue=0123456789abcdef0123" "the secret is passed"
if grep -q 0123456789abcdef0123 "$t/out"; then fail "the secret was printed"; fi

# ---- site.mk, alone: no CBASE, no cbase.mk, no CBASE_PROVIDERS -----------------------------
mkdir -p "$t/product"
cat > "$t/product/Makefile" <<M
SITE_STACK := mk
SITE_DIR := $site
SITE_HASHED := assets/*
SITE_ORIGINS := vm=vm.example.com
SITE_ROUTES := /m/*=vm,request=AllViewerExceptHostHeader
SITE_COMMENT := two words
include $here/site.mk
M
env -u SITE_ORIGINS -u SITE_ROUTES -u SITE_STACK make -s -C "$t/product" site-template > "$t/mk.yaml" || fail "make site-template"
grep -q 'PathPattern: /m/\*' "$t/mk.yaml" || fail "site.mk does not pass SITE_ROUTES"
grep -q 'Comment: "two words"' "$t/mk.yaml" || fail "site.mk does not pass SITE_COMMENT"
# A quote in a knob reaches the script whole (and is refused there), never breaking the recipe.
env -u SITE_ORIGINS -u SITE_ROUTES -u SITE_STACK make -s -C "$t/product" site-template "SITE_COMMENT=it's" > "$t/out" 2>&1 && fail "a quote in SITE_COMMENT was taken"
grep -q "SITE_COMMENT: 'it's'" "$t/out" || { cat "$t/out"; fail "the quoted knob did not reach the script whole"; }
reset
printf 'index.html\tassets/old.1111.js\n' > "$t/fake/keys"
env -u SITE_ORIGINS -u SITE_ROUTES -u SITE_STACK make -s -C "$t/product" deploy-web PRUNE=1 > "$t/out" 2>&1 || true
has "s3 rm s3://site-bucket/assets/old.1111.js" "make deploy-web PRUNE=1 prunes"
reset
env -u SITE_ORIGINS -u SITE_ROUTES -u SITE_STACK make -s -C "$t/product" deploy-web-stack DRY_RUN=1 > "$t/out" 2>&1 || { cat "$t/out"; fail "make deploy-web-stack DRY_RUN=1"; }
hasnt "execute-change-set" "make DRY_RUN=1 executes nothing"
reset
echo DRIFTED > "$t/fake/drift_status"
printf 'Distribution\tAWS::CloudFront::Distribution\tMODIFIED\n' > "$t/fake/drifts"
env -u SITE_ORIGINS -u SITE_ROUTES -u SITE_STACK make -s -C "$t/product" deploy-web-stack ACCEPT_DRIFT=1 > "$t/out" 2>&1 || { cat "$t/out"; fail "make deploy-web-stack ACCEPT_DRIFT=1"; }
has "execute-change-set" "make ACCEPT_DRIFT=1 proceeds"

# ---- the role ---------------------------------------------------------------------------------
bash "$here/../role_covers_release.sh" "$here/site_deploy.sh" "$here/site_role.yaml" > "$t/out" 2>&1 \
  || { cat "$t/out"; fail "site_role.yaml does not grant every call site_deploy.sh makes"; }

echo "site_test: ok"
