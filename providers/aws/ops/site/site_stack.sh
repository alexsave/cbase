#!/bin/bash
# site_stack.sh - the site's stack (site.yaml) from a product's knobs (site.mk's header).
#
#   site_stack.sh render                           the template, on stdout; no AWS call
#   site_stack.sh deploy [--accept-drift] [--dry-run]
#                                                  create or update the stack
#
# deploy, in order:
#   1. renders the template;
#   2. on an existing stack, runs CloudFormation drift detection and REFUSES when any resource
#      drifted (a change made by hand, such as an origin added in the console, which the update
#      would silently undo), printing each drifted property; --accept-drift is the only override;
#   3. makes a change set and prints it;
#   4. REFUSES, deleting the change set, when a change would replace or remove the distribution or
#      the bucket (a new distribution is a new domain name and an hours-long DNS move; a new bucket
#      is an empty site);
#   5. with --dry-run, deletes the change set (and the empty stack a create left), executes nothing;
#   6. else executes it and waits.
# Upload-only deploys (site_deploy.sh) never come here.
#
# WHY A RENDERER, not the AWS::LanguageExtensions transform's Fn::ForEach: ForEach writes map keys
# (resources, outputs, conditions, an object's properties), never items of a list, and a
# distribution's Origins and CacheBehaviors are lists; and a transform is expanded only by the
# CloudFormation service, so the template could not be checked offline. The rendered template is
# plain CloudFormation: cfn-lint reads it, and the change set shows exactly what it holds.
#
# Settings, from the environment: the SITE_* knobs (site.mk's header), SITE_SECRET_<origin> for an
# origin's header value (never in a file: on an update an unset one keeps the stack's value),
# AWS_REGION (us-east-1).
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
die() { echo "site_stack.sh: $*" >&2; exit 1; }
usage() { sed -n '2,8p' "$0" | sed 's/^# \{0,1\}//' >&2; exit 2; }
export AWS_REGION="${AWS_REGION:-${AWS_DEFAULT_REGION:-us-east-1}}"

# match VALUE REGEX WHAT: VALUE matches the extended REGEX, or the run stops naming WHAT.
match() { printf '%s' "$1" | grep -Eq "^($2)\$" || die "$3: '$1' is not $4"; }

# The AWS-managed policies by name (a raw policy id is taken as is).
cache_policy() {
  case "$1" in
    CachingOptimized) echo 658327ea-f89d-4fab-a63d-7e88639e58f6 ;;
    CachingDisabled) echo 4135ea2d-6df8-44a3-9df3-4b5a84be39ad ;;
    *) match "$1" '[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}' "a cache policy" \
         "CachingOptimized, CachingDisabled or a policy id"; echo "$1" ;;
  esac
}
request_policy() {
  case "$1" in
    AllViewer) echo 216adef6-5c7f-47e4-b989-5492eafa07d3 ;;
    AllViewerExceptHostHeader) echo b689b0a8-53d0-40ab-baf2-68738e2966ac ;;
    *) match "$1" '[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}' "an origin request policy" \
         "AllViewer, AllViewerExceptHostHeader or a policy id"; echo "$1" ;;
  esac
}
upper1() { printf '%s%s' "$(printf '%s' "${1:0:1}" | tr '[:lower:]' '[:upper:]')" "${1:1}"; }

# ---- render ---------------------------------------------------------------------------------
# Sets the blocks below from the knobs, each a run of lines already indented for its marker.
ORIGIN_IDS=(); SECRET_PARAMS=()
render_blocks() {
  stack="${SITE_STACK:-}"
  [ -n "$stack" ] || die "set SITE_STACK (the site's stack name)"
  match "$stack" '[A-Za-z][A-Za-z0-9-]{0,100}' SITE_STACK "a stack name"
  name="${SITE_NAME:-$stack}"
  match "$name" '[A-Za-z0-9_-]{1,57}' SITE_NAME "a name of at most 57 of A-Z a-z 0-9 _ -"
  prefix="${SITE_PREFIX:-}"
  [ -z "$prefix" ] || match "$prefix" '[A-Za-z0-9][A-Za-z0-9._-]*(/[A-Za-z0-9._-]+)*' SITE_PREFIX "a key prefix (no leading or trailing /)"
  shell="${SITE_SHELL:-}"
  [ -z "$shell" ] || match "$shell" '/[A-Za-z0-9._/-]+[.]html' SITE_SHELL "a path such as /_shell.html"
  comment="${SITE_COMMENT:-cbase site $stack}"
  match "$comment" '[A-Za-z0-9 ._:/-]{1,128}' SITE_COMMENT "a comment of A-Z a-z 0-9 space . _ : / -"
  price="${SITE_PRICE_CLASS:-PriceClass_100}"
  match "$price" 'PriceClass_100|PriceClass_200|PriceClass_All' SITE_PRICE_CLASS "a price class"

  local r from to
  redirects=""
  for r in ${SITE_REDIRECTS:-}; do
    from="${r%%=*}"; to="${r#*=}"
    [ "$from" != "$r" ] || die "SITE_REDIRECTS: '$r' is not from=to"
    match "$from" '/[A-Za-z0-9._/-]*' SITE_REDIRECTS "a path"
    match "$to" '(https://[A-Za-z0-9.-]+)?/[A-Za-z0-9._/-]*' SITE_REDIRECTS "a path or https URL"
    redirects="$redirects${redirects:+, }'$from': '$to'"
  done

  BUCKET_NAME=""
  if [ -n "${SITE_BUCKET:-}" ]; then
    # shellcheck disable=SC2016 # ${AWS::AccountId} is CloudFormation's, kept literal
    match "$SITE_BUCKET" '([a-z0-9.-]|\$\{AWS::(AccountId|Region)\})+' SITE_BUCKET 'a bucket name (it may hold ${AWS::AccountId} and ${AWS::Region})'
    BUCKET_NAME="      BucketName: !Sub \"$SITE_BUCKET\""
  fi

  ALIASES=""
  local a list=""
  for a in ${SITE_ALIASES:-}; do
    match "$a" '[a-z0-9*][a-z0-9.-]*' SITE_ALIASES "a domain name"
    list="$list${list:+, }$a"
  done
  [ -z "$list" ] || ALIASES="        Aliases: [$list]"
  VIEWER_CERTIFICATE="        ViewerCertificate:
          CloudFrontDefaultCertificate: true"
  if [ -n "${SITE_CERT_ARN:-}" ]; then
    match "$SITE_CERT_ARN" 'arn:aws:acm:us-east-1:[0-9]{12}:certificate/[0-9a-f-]+' SITE_CERT_ARN "an ACM certificate ARN in us-east-1"
    [ -n "$list" ] || die "SITE_CERT_ARN is set with no SITE_ALIASES: the certificate is for the site's names"
    VIEWER_CERTIFICATE="        ViewerCertificate:
          AcmCertificateArn: $SITE_CERT_ARN
          SslSupportMethod: sni-only
          MinimumProtocolVersion: TLSv1.2_2021"
  elif [ -n "$list" ]; then
    die "SITE_ALIASES is set with no SITE_CERT_ARN: CloudFront serves a custom name only with its certificate"
  fi

  S3_FUNCTIONS="            - { EventType: viewer-request, FunctionARN: !GetAtt Router.FunctionMetadata.FunctionARN }"
  ISOLATION_FUNCTION=""
  case "${SITE_ISOLATION:-no}" in
    no) ;;
    yes)
      S3_FUNCTIONS="$S3_FUNCTIONS
            - { EventType: viewer-response, FunctionARN: !GetAtt Isolation.FunctionMetadata.FunctionARN }"
      ISOLATION_FUNCTION="  Isolation:
    Type: AWS::CloudFront::Function
    Properties:
      Name: \"$name-isolation\"
      AutoPublish: true
      FunctionConfig:
        Comment: COOP same-origin, COEP credentialless
        Runtime: cloudfront-js-2.0
      FunctionCode: |
        function handler(event) {
          var h = event.response.headers;
          h['cross-origin-opener-policy'] = { value: 'same-origin' };
          h['cross-origin-embedder-policy'] = { value: 'credentialless' };
          return event.response;
        }" ;;
    *) die "SITE_ISOLATION: '$SITE_ISOLATION' is not yes or no" ;;
  esac

  # The origins: id=domain,option=value,...
  ORIGINS=""; SECRET_PARAMETERS=""; ORIGIN_IDS=(); SECRET_PARAMS=()
  local o id domain opt k v proto http https readt keep ip header
  local -a parts origins_in routes_in
  read -r -a origins_in <<<"${SITE_ORIGINS:-}"
  for o in ${origins_in[@]+"${origins_in[@]}"}; do
    IFS=, read -r -a parts <<<"$o"
    id="${parts[0]%%=*}"; domain="${parts[0]#*=}"
    [ "$id" != "${parts[0]}" ] || die "SITE_ORIGINS: '$o' does not start id=domain"
    match "$id" '[A-Za-z][A-Za-z0-9]{0,30}' SITE_ORIGINS "an origin id of A-Z a-z 0-9"
    [ "$id" != s3 ] || die "SITE_ORIGINS: s3 is the bucket's origin id, taken"
    match "$domain" '[A-Za-z0-9][A-Za-z0-9.-]*' SITE_ORIGINS "a domain name"
    for k in ${ORIGIN_IDS[@]+"${ORIGIN_IDS[@]}"}; do [ "$k" != "$id" ] || die "SITE_ORIGINS: $id is named twice"; done
    proto="https-only"; http=""; https=""; readt=""; keep=""; ip=""; header=""
    for opt in "${parts[@]:1}"; do
      k="${opt%%=*}"; v="${opt#*=}"
      case "$k" in
        proto) match "$v" 'http-only|https-only|match-viewer' "SITE_ORIGINS $id proto" "http-only, https-only or match-viewer"; proto="$v" ;;
        http) match "$v" '[1-9][0-9]{0,4}' "SITE_ORIGINS $id http" "a port"; http="$v" ;;
        https) match "$v" '[1-9][0-9]{0,4}' "SITE_ORIGINS $id https" "a port"; https="$v" ;;
        read) match "$v" '[1-9][0-9]{0,2}' "SITE_ORIGINS $id read" "seconds"; readt="$v" ;;
        keepalive) match "$v" '[1-9][0-9]{0,2}' "SITE_ORIGINS $id keepalive" "seconds"; keep="$v" ;;
        ip) match "$v" 'ipv4|ipv6|dualstack' "SITE_ORIGINS $id ip" "ipv4, ipv6 or dualstack"; ip="$v" ;;
        header) match "$v" '[A-Za-z0-9-]{1,64}' "SITE_ORIGINS $id header" "a header name"; header="$v" ;;
        *) die "SITE_ORIGINS $id: unknown option '$opt' (proto, http, https, read, keepalive, ip, header)" ;;
      esac
    done
    ORIGIN_IDS+=("$id")
    ORIGINS="$ORIGINS
          - Id: $id
            DomainName: $domain
            CustomOriginConfig:
              OriginProtocolPolicy: $proto"
    [ -z "$http" ] || ORIGINS="$ORIGINS
              HTTPPort: $http"
    [ -z "$https" ] || ORIGINS="$ORIGINS
              HTTPSPort: $https"
    [ "$proto" = http-only ] || ORIGINS="$ORIGINS
              OriginSSLProtocols: [TLSv1.2]"
    [ -z "$readt" ] || ORIGINS="$ORIGINS
              OriginReadTimeout: $readt"
    [ -z "$keep" ] || ORIGINS="$ORIGINS
              OriginKeepaliveTimeout: $keep"
    [ -z "$ip" ] || ORIGINS="$ORIGINS
              IpAddressType: $ip"
    if [ -n "$header" ]; then
      k="OriginSecret$(upper1 "$id")"
      SECRET_PARAMS+=("$k=$id")
      ORIGINS="$ORIGINS
            OriginCustomHeaders:
              - HeaderName: $header
                HeaderValue: !Ref $k"
      SECRET_PARAMETERS="$SECRET_PARAMETERS
  $k:
    Type: String
    NoEcho: true
    MinLength: 1
    Description: the $header header's value on every request to the $id origin, which lets the origin refuse anything but this distribution (from SITE_SECRET_$id)"
    fi
  done

  # The routes, in order: pattern=origin,option=value,...
  CACHE_BEHAVIORS=""
  local route path origin cache request methods compress seen="" used=" "
  read -r -a routes_in <<<"${SITE_ROUTES:-}"
  for route in ${routes_in[@]+"${routes_in[@]}"}; do
    IFS=, read -r -a parts <<<"$route"
    path="${parts[0]%%=*}"; origin="${parts[0]#*=}"
    [ "$path" != "${parts[0]}" ] || die "SITE_ROUTES: '$route' does not start pattern=origin"
    match "$path" '/[A-Za-z0-9._/*?-]*' SITE_ROUTES "a path pattern"
    case "$seen" in *" $path "*) die "SITE_ROUTES: $path is routed twice" ;; esac
    seen="$seen $path "
    if [ "$origin" != s3 ]; then
      local known=no
      for k in ${ORIGIN_IDS[@]+"${ORIGIN_IDS[@]}"}; do [ "$k" != "$origin" ] || known=yes; done
      [ "$known" = yes ] || die "SITE_ROUTES $path: no origin '$origin' in SITE_ORIGINS (or s3, the bucket)"
    fi
    used="$used$origin "
    cache=CachingOptimized; request=""; methods="read"; compress=yes
    for opt in "${parts[@]:1}"; do
      k="${opt%%=*}"; v="${opt#*=}"
      case "$k" in
        cache) cache="$v" ;;
        request) request="$v" ;;
        methods) match "$v" 'read|all' "SITE_ROUTES $path methods" "read or all"; methods="$v" ;;
        compress) match "$v" 'yes|no' "SITE_ROUTES $path compress" "yes or no"; compress="$v" ;;
        *) die "SITE_ROUTES $path: unknown option '$opt' (cache, request, methods, compress)" ;;
      esac
    done
    [ "$origin" != s3 ] || [ "$methods" = read ] || die "SITE_ROUTES $path: the bucket takes reads only"
    cache="$(cache_policy "$cache")"
    [ -z "$request" ] || request="$(request_policy "$request")"
    CACHE_BEHAVIORS="$CACHE_BEHAVIORS
          - PathPattern: $path
            TargetOriginId: $origin
            ViewerProtocolPolicy: redirect-to-https"
    if [ "$methods" = all ]; then
      CACHE_BEHAVIORS="$CACHE_BEHAVIORS
            AllowedMethods: [GET, HEAD, OPTIONS, PUT, PATCH, POST, DELETE]"
    else
      CACHE_BEHAVIORS="$CACHE_BEHAVIORS
            AllowedMethods: [GET, HEAD]"
    fi
    CACHE_BEHAVIORS="$CACHE_BEHAVIORS
            CachedMethods: [GET, HEAD]
            Compress: $([ "$compress" = yes ] && echo true || echo false)
            CachePolicyId: $cache"
    [ -z "$request" ] || CACHE_BEHAVIORS="$CACHE_BEHAVIORS
            OriginRequestPolicyId: $request"
    # Every S3 behaviour runs the router: it is what puts the prefix and .html on a key.
    [ "$origin" != s3 ] || CACHE_BEHAVIORS="$CACHE_BEHAVIORS
            FunctionAssociations:
$(printf '%s\n' "$S3_FUNCTIONS" | sed 's/^/  /')"
  done
  [ -z "$CACHE_BEHAVIORS" ] || CACHE_BEHAVIORS="        CacheBehaviors:$CACHE_BEHAVIORS"
  # An origin no route uses is a dead origin: refused, so none lingers in the distribution.
  for k in ${ORIGIN_IDS[@]+"${ORIGIN_IDS[@]}"}; do
    case "$used" in *" $k "*) ;; *) die "SITE_ORIGINS: no route uses the origin $k; remove it or route to it" ;; esac
  done
  ORIGINS="${ORIGINS#$'\n'}"
  SECRET_PARAMETERS="${SECRET_PARAMETERS#$'\n'}"
}

render() {
  render_blocks
  local line block trimmed
  while IFS= read -r line || [ -n "$line" ]; do
    trimmed="${line#"${line%%[! ]*}"}"
    case "$trimmed" in
      '#@'*)
        case "${trimmed#\#@}" in
          SECRET_PARAMETERS) block="$SECRET_PARAMETERS" ;;
          BUCKET_NAME) block="$BUCKET_NAME" ;;
          ISOLATION_FUNCTION) block="$ISOLATION_FUNCTION" ;;
          ALIASES) block="$ALIASES" ;;
          VIEWER_CERTIFICATE) block="$VIEWER_CERTIFICATE" ;;
          ORIGINS) block="$ORIGINS" ;;
          S3_FUNCTIONS) block="$S3_FUNCTIONS" ;;
          CACHE_BEHAVIORS) block="$CACHE_BEHAVIORS" ;;
          *) die "site.yaml: unknown block marker: $line" ;;
        esac
        [ -z "$block" ] || printf '%s\n' "$block"
        continue ;;
    esac
    line="${line//@@NAME@@/$name}"
    line="${line//@@SHELL@@/$shell}"
    line="${line//@@PREFIX@@/${prefix:+/$prefix}}"
    line="${line//@@REDIRECTS@@/$redirects}"
    line="${line//@@COMMENT@@/$comment}"
    line="${line//@@PRICE_CLASS@@/$price}"
    case "$line" in *@@*) die "site.yaml: an unknown @@ value: $line" ;; esac
    printf '%s\n' "$line"
  done < "$here/site.yaml"
}

# ---- deploy ---------------------------------------------------------------------------------
POLL="${SITE_POLL_SECONDS:-5}"

# drift_guard ACCEPT: 0 when the stack has not drifted (or ACCEPT is yes), else prints the drift
# and stops the run.
drift_guard() {
  local accept="$1" id st ds sds reason n=0 drifted lid type status
  echo "drift: detecting on $stack"
  id="$(aws cloudformation detect-stack-drift --stack-name "$stack" --query StackDriftDetectionId --output text)"
  while :; do
    st="$(aws cloudformation describe-stack-drift-detection-status --stack-drift-detection-id "$id" \
          --query '[DetectionStatus,StackDriftStatus,DetectionStatusReason]' --output text)"
    IFS=$'\t' read -r ds sds reason <<<"$st"
    [ "$ds" = DETECTION_IN_PROGRESS ] || break
    n=$((n + 1)); [ "$n" -le 120 ] || die "drift detection $id did not finish"
    sleep "$POLL"
  done
  drifted="$(aws cloudformation describe-stack-resource-drifts --stack-name "$stack" \
             --stack-resource-drift-status-filters MODIFIED DELETED \
             --query 'StackResourceDrifts[].[LogicalResourceId,ResourceType,StackResourceDriftStatus]' --output text)"
  [ "$drifted" != None ] || drifted=""
  if [ "$ds" = DETECTION_COMPLETE ] && [ "$sds" = IN_SYNC ] && [ -z "$drifted" ]; then
    echo "drift: none"
    return 0
  fi
  [ "$ds" = DETECTION_COMPLETE ] || echo "drift: detection ended $ds: $reason" >&2
  while IFS=$'\t' read -r lid type status; do
    [ -n "$lid" ] || continue
    echo "drift: $lid ($type) $status" >&2
    [ "$status" = MODIFIED ] || continue
    aws cloudformation describe-stack-resource-drifts --stack-name "$stack" \
      --stack-resource-drift-status-filters MODIFIED \
      --query "StackResourceDrifts[?LogicalResourceId=='$lid'].PropertyDifferences[].[DifferenceType,PropertyPath]" \
      --output text | while IFS=$'\t' read -r k p; do echo "drift:   $k $p" >&2; done
  done <<<"$drifted"
  if [ "$accept" = yes ]; then
    echo "drift: accepted (--accept-drift): the update makes the stack's resources match the template again" >&2
    return 0
  fi
  die "the $stack stack drifted from its template (above): an update would undo those changes. Put them in the knobs (site.mk), or pass --accept-drift (make ACCEPT_DRIFT=1) to overwrite them"
}

deploy() {
  local accept=no dry=no
  while [ $# -gt 0 ]; do
    case "$1" in --accept-drift) accept=yes ;; --dry-run) dry=yes ;; *) usage ;; esac
    shift
  done
  command -v aws >/dev/null 2>&1 || die "the AWS CLI is not installed"
  local t
  t="$(mktemp -d)"
  # shellcheck disable=SC2064 # t is fixed now
  trap "rm -rf '$t'" EXIT
  render > "$t/site.yaml"

  local status err type existing
  if status="$(aws cloudformation describe-stacks --stack-name "$stack" --query 'Stacks[0].StackStatus' --output text 2>"$t/err")"; then
    :
  elif grep -q 'does not exist' "$t/err"; then
    status=NONE
  else
    err="$(cat "$t/err")"; die "cannot read the $stack stack: $err"
  fi
  case "$status" in
    NONE|REVIEW_IN_PROGRESS) type=CREATE; echo "stack: $stack is new" ;;
    *_IN_PROGRESS) die "the $stack stack is $status: wait for it" ;;
    *) type=UPDATE; echo "stack: $stack is $status"; drift_guard "$accept" ;;
  esac

  # The parameters: Pages keeps its value (site_deploy.sh owns it); a secret comes from
  # SITE_SECRET_<origin>, or keeps the stack's value on an update.
  local params=() p key oid var
  existing=" "
  [ "$type" = CREATE ] || existing=" $(aws cloudformation describe-stacks --stack-name "$stack" --query 'Stacks[0].Parameters[].ParameterKey' --output text | tr '\t' ' ') "
  case "$existing" in *" Pages "*) params+=("ParameterKey=Pages,UsePreviousValue=true") ;; esac
  for p in ${SECRET_PARAMS[@]+"${SECRET_PARAMS[@]}"}; do
    key="${p%%=*}"; oid="${p#*=}"; var="SITE_SECRET_$oid"
    if [ -n "${!var:-}" ]; then
      # Checked without printing it: the value is a secret.
      { [ "$(printf '%s' "${!var}" | wc -c)" -le 256 ] && printf '%s' "${!var}" | grep -Eq '^[A-Za-z0-9_.~+/=-]{16,}$'; } \
        || die "$var must be 16 to 256 of A-Z a-z 0-9 _ . ~ + / = -"
      params+=("ParameterKey=$key,ParameterValue=${!var}")
    else
      case "$existing" in *" $key "*) params+=("ParameterKey=$key,UsePreviousValue=true") ;;
        *) die "set $var: the $oid origin's header value, which the stack does not hold yet" ;; esac
    fi
  done

  local cs csid
  cs="cbase-site-$(date -u +%Y%m%d%H%M%S)"
  csid="$(aws cloudformation create-change-set --stack-name "$stack" --change-set-name "$cs" \
          --change-set-type "$type" --template-body "file://$t/site.yaml" \
          ${params[@]+--parameters "${params[@]}"} --query Id --output text)"
  # cleanup: the change set, and the empty stack a create change set made.
  cleanup() {
    aws cloudformation delete-change-set --change-set-name "$csid" >/dev/null
    if [ "$type" = CREATE ] && [ "$status" = NONE ]; then aws cloudformation delete-stack --stack-name "$stack"; fi
  }
  if ! aws cloudformation wait change-set-create-complete --change-set-name "$csid" 2>/dev/null; then
    local why
    why="$(aws cloudformation describe-change-set --change-set-name "$csid" --query 'StatusReason' --output text)"
    case "$why" in
      *"didn't contain changes"*|*"No updates are to be performed"*)
        echo "stack: no changes"; cleanup; return 0 ;;
    esac
    cleanup
    die "the change set failed: $why"
  fi

  echo "change set $cs:"
  # shellcheck disable=SC2016 # a JMESPath literal
  aws cloudformation describe-change-set --change-set-name "$csid" \
    --query 'Changes[].ResourceChange.[Action,LogicalResourceId,ResourceType,Replacement,join(`,`,Scope)]' --output table

  # THE NO-REPLACEMENT GATE: the distribution and the bucket are never recreated or removed.
  local replacing
  replacing="$(aws cloudformation describe-change-set --change-set-name "$csid" \
    --query "Changes[?(ResourceChange.Replacement=='True' || ResourceChange.Replacement=='Conditional' || ResourceChange.Action=='Remove') && (ResourceChange.ResourceType=='AWS::CloudFront::Distribution' || ResourceChange.ResourceType=='AWS::S3::Bucket')].ResourceChange.[LogicalResourceId,ResourceType,Action,Replacement]" \
    --output text)"
  [ "$replacing" != None ] || replacing=""
  if [ -n "$replacing" ]; then
    printf 'refused: %s\n' "$replacing" | tr '\t' ' ' >&2
    cleanup
    die "the change set would replace or remove the distribution or the bucket (above); nothing was executed"
  fi

  if [ "$dry" = yes ]; then
    cleanup
    echo "dry run: change set $cs deleted, nothing executed"
    return 0
  fi
  aws cloudformation execute-change-set --change-set-name "$csid"
  if [ "$type" = CREATE ]; then aws cloudformation wait stack-create-complete --stack-name "$stack"
  else aws cloudformation wait stack-update-complete --stack-name "$stack"; fi
  aws cloudformation describe-stacks --stack-name "$stack" \
    --query 'Stacks[0].Outputs[].[OutputKey,OutputValue]' --output table
}

case "${1:-}" in
  render) render ;;
  deploy) shift; deploy "$@" ;;
  *) usage ;;
esac
