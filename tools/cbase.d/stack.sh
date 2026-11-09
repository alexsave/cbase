# shellcheck shell=bash
# stack.sh - an app's own CloudFormation stack, cbase-app-<app> in us-east-1 (CloudFront's region):
# its distribution and the routes (README.md "./cbase deploy tool"). Sourced by tools/cbase.
#
# THE TEMPLATE (stack_render), plain CloudFormation with no transform, rendered from cbase.conf:
#   Oac           the Origin Access Control the distribution signs its bucket reads with. The
#                 bucket's policy, which lets it, is the static group's and is set once per bucket,
#                 on aws:SourceAccount, by group-init (group.sh): deploy never edits a bucket policy.
#   Router        a viewer-request CloudFront Function on every static behaviour, an SPA's routing
#                 (stack_router): a file passes as it is, a page's clean URL is its .html ("/about"
#                 /about.html, "/docs/" /docs/index.html), and any other path is the app shell.
#   Distribution  the default behaviour is the "*" route; every other route is a behaviour, in
#                 cbase.conf's order. The static origin is the group's bucket with OriginPath
#                 /<app>, so the app reads its own folder and nothing else. A VM route (only when
#                 CBASE_VM is not none) goes to the VmOrigin and VmPort parameters: the group VM's
#                 public DNS name and the app's claimed port, which the VM deploy (vm.sh) supplies.
#                 A ws route is its own origin, for its long timeouts (CloudFront sets them per
#                 origin): read 60 s, keep-alive 60 s, uncached, every method.
#   ApiDistribution  with CBASE_API_HOST: that hostname's every request to the VM, a ws route's
#                 on the ws origin as on the Distribution.
# Caching uses AWS-managed policies alone (CachingOptimized, CachingDisabled; the VM's requests
# AllViewerExceptHostHeader), so a flat-rate CloudFront plan can hold the distribution.
# A route's methods are the smallest of CloudFront's three sets that covers them: GET,HEAD;
# GET,HEAD,OPTIONS; or all seven.
#
# THE SAFETY RULES (stack_plan), in order:
#   1. an existing stack's drift is detected first; drifted resources are printed and the deploy
#      stops, unless CBASE_ACCEPT_DRIFT=1 (--accept-drift): the update would undo a change made by hand;
#   2. a change set is made and described, always;
#   3. a change that would replace or remove a BUCKET (AWS::S3::Bucket) is REFUSED, the change set
#      deleted, nothing executed: a bucket replaced is every app's files lost. The one hard stop.
#      The same guard (stack_keep_guard) holds a VM group's INSTANCE (AWS::EC2::Instance), whose
#      disk holds every member's /srv/cbase/<app>/data: group-init's stacks (group.sh) run through
#      stack_changeset too, and an app's own stack holds neither;
#   4. a distribution replaced (a new cloudfront.net name, a brief outage) is allowed, and printed;
#   5. with DRY_RUN=1 (--dry-run) the change set is deleted unexecuted (and the empty stack a first
#      change set leaves).
#
# MASKING. A change set's or a drift's values are printed (before and after, expected and actual),
# but never one that could be a secret: a property under an origin's custom headers (where an origin
# secret lives), or one a parameter set that is not known to be plain. Those print as "origin header
# <where> changed" or "parameter <name> changed". The values are left out by the CLI's own --query
# filter, so they never reach this script, let alone its output; aws.sh aws_mask masks any line of
# an AWS error that names one, the other way a value could be printed. Never run under set -x.

STACK_REGION=us-east-1   # an app's stack; group.sh sets a group stack's (the CLI's region)
STACK_NAME=""
STACK_STATUS=""     # before this deploy: NONE, or the stack's status
STACK_TYPE=""       # CREATE or UPDATE
STACK_CS=""         # the change set's name
STACK_CHANGES=""    # 1 when the change set holds changes
STACK_TEMPLATE=""   # the rendered template's file
STACK_CERT=""       # the certificate the distribution serves the hostnames with, or ""
STACK_API_CERT=""   # the certificate CBASE_API_HOST's distribution serves, or ""
STACK_BUCKET=""     # the static group's bucket and its region
STACK_BUCKET_REGION=""
POLL_SECONDS="${CBASE_POLL_SECONDS:-5}"
# The template's parameters that are plain (their values may be printed); any other one's are not.
STACK_PLAIN_PARAMS="VmOrigin VmPort InstanceType ImageId CloudFrontPrefixList ArtifactBucket"
STACK_TAGS=()   # the stack's tags, "Key=K,Value=V" each
STACK_CAPS=()   # --capabilities, for a template that makes an IAM role (a VM group's)
# The stack's parameters, "ParameterKey=K,ParameterValue=V" each: the VM deploy (vm.sh) sets VmOrigin
# and VmPort. Empty for a static site, whose template has none.
STACK_PARAMS=()

# The AWS-managed policies, by id.
CF_CACHING_OPTIMIZED=658327ea-f89d-4fab-a63d-7e88639e58f6
CF_CACHING_DISABLED=4135ea2d-6df8-44a3-9df3-4b5a84be39ad
CF_ALL_VIEWER_EXCEPT_HOST=b689b0a8-53d0-40ab-baf2-68738e2966ac
# shellcheck disable=SC2034 # cert.sh's alias records read it
CF_ZONE_ID=Z2FDTNDATAQYW2   # every CloudFront distribution's hosted zone, for a Route 53 alias

stack_name() { printf 'cbase-app-%s' "$CONF_APP"; }

# stack_methods METHODS: CloudFront's allowed-method set covering METHODS (ALL or a comma list).
stack_methods() {
  case ",$1," in
    *,ALL,*|*,PUT,*|*,POST,*|*,PATCH,*|*,DELETE,*) echo "[GET, HEAD, OPTIONS, PUT, PATCH, POST, DELETE]" ;;
    *,OPTIONS,*) echo "[GET, HEAD, OPTIONS]" ;;
    *) echo "[GET, HEAD]" ;;
  esac
}

# stack_behaviour PATTERN ORIGIN CACHE METHODS: one behaviour's lines: DefaultCacheBehavior's
# fields for the "*" route, else an item of CacheBehaviors.
stack_behaviour() {
  local pattern="$1" origin="$2" cache="$3" methods="$4" pad="          " policy
  if [ "$pattern" != "*" ]; then
    printf '%s- PathPattern: "%s"\n' "$pad" "$pattern"
    pad="$pad  "
  fi
  printf '%sTargetOriginId: %s\n' "$pad" "$origin"
  printf '%sViewerProtocolPolicy: redirect-to-https\n' "$pad"
  printf '%sAllowedMethods: %s\n' "$pad" "$(stack_methods "$methods")"
  printf '%sCachedMethods: [GET, HEAD]\n' "$pad"
  if [ "$origin" = ws ]; then printf '%sCompress: false\n' "$pad"; else printf '%sCompress: true\n' "$pad"; fi
  if [ "$cache" = cache ]; then policy="$CF_CACHING_OPTIMIZED   # Managed-CachingOptimized"
  else policy="$CF_CACHING_DISABLED   # Managed-CachingDisabled"; fi
  printf '%sCachePolicyId: %s\n' "$pad" "$policy"
  if [ "$origin" != static ]; then
    printf '%sOriginRequestPolicyId: %s   # Managed-AllViewerExceptHostHeader\n' "$pad" "$CF_ALL_VIEWER_EXCEPT_HOST"
  else
    printf '%sFunctionAssociations:\n' "$pad"
    printf '%s  - EventType: viewer-request\n' "$pad"
    printf '%s    FunctionARN: !GetAtt Router.FunctionMetadata.FunctionARN\n' "$pad"
  fi
}

# stack_vm_origin ID READ KEEPALIVE: a custom origin on the VM.
stack_vm_origin() {
  cat << EOF
          - Id: $1
            DomainName: !Ref VmOrigin
            CustomOriginConfig:
              HTTPPort: !Ref VmPort
              OriginProtocolPolicy: http-only
              OriginReadTimeout: $2
              OriginKeepaliveTimeout: $3
EOF
}

# stack_viewer_cert ALIAS...: the Aliases and ViewerCertificate lines.
stack_viewer_cert() {
  if [ -n "$STACK_CERT" ] && [ $# -gt 0 ]; then
    printf '        Aliases: [%s]\n' "$(printf '%s, ' "$@" | sed 's/, $//')"
    cat << EOF
        ViewerCertificate:
          AcmCertificateArn: $STACK_CERT
          SslSupportMethod: sni-only
          MinimumProtocolVersion: TLSv1.2_2021
EOF
  else
    cat << EOF
        ViewerCertificate:
          CloudFrontDefaultCertificate: true
EOF
  fi
}

# stack_pages: the build's pages, as the router's PAGES names them: each html file of ST_FILES (the
# static dir, read by static_scan) less its ".html", blank-separated. A name of other characters
# than a clean URL's is left out (no quote or backslash reaches the code); its file is still served
# by its own path. With no build read (the template command away from one) the list is empty.
stack_pages() {
  { [ -n "$ST_FILES" ] && [ -f "$ST_FILES" ]; } || return 0
  { grep -E '^[A-Za-z0-9_.~/-]+\.html$' "$ST_FILES" || true; } | { grep -v '^\.well-known/' || true; } \
    | sed 's/\.html$//' | tr '\n' ' ' | sed 's/ $//'
}

# CloudFront's limit on a function's code, in bytes.
STACK_FUNCTION_MAX=10240

# stack_router: the Router resource. Its code is the one rule a static path is mapped by, an SPA's:
#   (a) a file: a path whose last part has a dot, or under /.well-known/, passes as it is. One the
#       bucket lacks is S3's 404 (the group's policy grants s3:ListBucket), never the shell, so a
#       broken asset link fails loudly;
#   (b) a page: "/x" is /x.html, and "/x/" /x/index.html ("/" /index.html), when the build has
#       that page (PAGES, the build's html files, rendered in by deploy);
#   (c) anything else is the app shell, CBASE_SHELL (default /index.html), with S3's 200: the app
#       routes it client-side and shows its own 404.
# cbase_test.sh runs this code under node.
stack_router() {
  local pages code shell="${CONF_SHELL:-/index.html}"
  pages="$(stack_pages)"
  if [ -n "$ST_FILES" ] && [ -f "$ST_FILES" ] && ! grep -Fxq -- "${shell#/}" "$ST_FILES"; then
    echo "cbase: the app shell is $shell (CBASE_SHELL), and the static dir has no ${shell#/}: nothing was changed" >&2
    exit 1
  fi
  code="$(cat << EOF
// cbase app router (tools/cbase.d/stack.sh): the distribution's OriginPath is the app's folder.
var SHELL = '$shell';
var PAGES = ' $pages ';
function handler(event) {
  var request = event.request;
  var uri = request.uri;
  if (uri.indexOf('/.well-known/') === 0 || uri.substring(uri.lastIndexOf('/')).indexOf('.') >= 0) {
    return request;
  }
  var page = uri.substring(1);
  if (page === '' || page.charAt(page.length - 1) === '/') page = page + 'index';
  request.uri = PAGES.indexOf(' ' + page + ' ') >= 0 ? '/' + page + '.html' : SHELL;
  return request;
}
EOF
)"
  if [ "${#code}" -gt "$STACK_FUNCTION_MAX" ]; then
    echo "cbase: the router's code is ${#code} bytes with the build's page list, over CloudFront's $STACK_FUNCTION_MAX: the build has too many pages; nothing was changed" >&2
    exit 1
  fi
  cat << EOF
  Router:
    Type: AWS::CloudFront::Function
    Properties:
      Name: cbase-$CONF_APP-router
      AutoPublish: true
      FunctionConfig:
        Comment: clean URLs for the app $CONF_APP
        Runtime: cloudfront-js-2.0
      FunctionCode: |
EOF
  printf '%s\n' "$code" | sed 's/^/        /'
}

# stack_render FILE: the template, from CONF_* and STACK_BUCKET, STACK_BUCKET_REGION, STACK_CERT.
stack_render() {
  local out="$1" r pattern origin cache methods has_static="" has_vm="" has_ws="" def="" i
  for r in "${CONF_ROUTES[@]}"; do
    origin="$(echo "$r" | cut -d' ' -f2)"
    case "$origin" in static) has_static=1 ;; vm) has_vm=1 ;; ws) has_ws=1 ;; esac
  done
  [ "$CONF_VM" != none ] || { has_vm=""; has_ws=""; }
  [ -z "$CONF_API_HOST" ] || has_vm=1
  {
    cat << EOF
AWSTemplateFormatVersion: "2010-09-09"
# The app $CONF_APP's distribution, rendered by ./cbase deploy (tools/cbase.d/stack.sh) from its
# cbase.conf. Edits by hand are drift: the next deploy stops on them.
Description: "cbase app $CONF_APP: its CloudFront distribution and routes"
EOF
    if [ -n "$has_vm$has_ws" ]; then
      cat << EOF

Parameters:
  VmOrigin:
    Type: String
    Description: the VM group's instance public DNS name (set by ./cbase deploy)
    AllowedPattern: "[a-z0-9.-]+"
  VmPort:
    Type: Number
    Description: the port the app claimed in its VM group's state file (set by ./cbase deploy)
    MinValue: 8700
    MaxValue: 8799
EOF
    fi
    printf '\nResources:\n'
    if [ -n "$has_static" ]; then
      cat << EOF
  Oac:
    Type: AWS::CloudFront::OriginAccessControl
    Properties:
      OriginAccessControlConfig:
        Name: cbase-$CONF_APP
        Description: the app $CONF_APP's reads of its folder in the static group's bucket
        OriginAccessControlOriginType: s3
        SigningBehavior: always
        SigningProtocol: sigv4

EOF
      stack_router
      echo
    fi
    cat << EOF
  Distribution:
    Type: AWS::CloudFront::Distribution
    Properties:
      DistributionConfig:
        Comment: cbase app $CONF_APP
        Enabled: true
EOF
    stack_viewer_cert ${CONF_DOMAINS[@]+"${CONF_DOMAINS[@]}"}
    cat << EOF
        HttpVersion: http2and3
        IPV6Enabled: true
        PriceClass: PriceClass_100
        Origins:
EOF
    if [ -n "$has_static" ]; then
      cat << EOF
          - Id: static
            DomainName: $STACK_BUCKET.s3.$STACK_BUCKET_REGION.amazonaws.com
            OriginPath: /$CONF_APP
            OriginAccessControlId: !GetAtt Oac.Id
            S3OriginConfig:
              OriginAccessIdentity: ""
EOF
    fi
    [ -z "$has_vm" ] || stack_vm_origin vm 30 5
    [ -z "$has_ws" ] || stack_vm_origin ws 60 60
    for r in "${CONF_ROUTES[@]}"; do
      read -r pattern origin cache methods <<< "$r"
      [ "$pattern" = "*" ] || continue
      def="$r"
      printf '        DefaultCacheBehavior:\n'
      stack_behaviour "$pattern" "$origin" "$cache" "$methods"
    done
    i=0
    for r in "${CONF_ROUTES[@]}"; do
      read -r pattern origin cache methods <<< "$r"
      [ "$pattern" != "*" ] || continue
      if [ "$origin" != static ] && [ "$CONF_VM" = none ]; then continue; fi
      [ "$i" -gt 0 ] || printf '        CacheBehaviors:\n'
      i=$((i + 1))
      stack_behaviour "$pattern" "$origin" "$cache" "$methods"
    done
    [ -n "$def" ] || { echo "cbase: no default route (conf.sh always adds one)" >&2; exit 1; }
    if [ -n "$CONF_API_HOST" ]; then
      cat << EOF

  ApiDistribution:
    Type: AWS::CloudFront::Distribution
    Properties:
      DistributionConfig:
        Comment: cbase app $CONF_APP, $CONF_API_HOST to the VM
        Enabled: true
EOF
      STACK_CERT="$STACK_API_CERT" stack_viewer_cert "$CONF_API_HOST"
      cat << EOF
        HttpVersion: http2and3
        IPV6Enabled: true
        PriceClass: PriceClass_100
        Origins:
EOF
      stack_vm_origin vm 30 5
      [ -z "$has_ws" ] || stack_vm_origin ws 60 60
      printf '        DefaultCacheBehavior:\n'
      stack_behaviour "*" vm nocache ALL
      # The ws routes keep their behaviour on the API host too: the ws origin's long timeouts.
      i=0
      for r in "${CONF_ROUTES[@]}"; do
        read -r pattern origin cache methods <<< "$r"
        [ "$origin" = ws ] || continue
        [ "$i" -gt 0 ] || printf '        CacheBehaviors:\n'
        i=$((i + 1))
        stack_behaviour "$pattern" ws "$cache" "$methods"
      done
    fi
    cat << EOF

Outputs:
  DistributionId:
    Value: !Ref Distribution
  DistributionDomain:
    Description: the app's cloudfront.net name, where its hostname points
    Value: !GetAtt Distribution.DomainName
EOF
    if [ -n "$CONF_API_HOST" ]; then
      cat << EOF
  ApiDistributionDomain:
    Description: where CBASE_API_HOST points
    Value: !GetAtt ApiDistribution.DomainName
EOF
    fi
  } > "$out"
}

# stack_print_template FILE: the template, any origin header's value masked (there is none in a
# template deploy renders: a secret would come in a NoEcho parameter, never a literal).
stack_print_template() {
  sed -E 's/^([[:blank:]]*(- )?HeaderValue:).*/\1 (masked)/' "$1"
}

# cfn ARGS...: one CloudFormation call in the stack's region.
cfn() { aws_call cloudformation --region "$STACK_REGION" "$@"; }

# stack_status: STACK_STATUS, NONE when there is no stack.
stack_status() {
  local st
  if st="$(cfn describe-stacks --stack-name "$STACK_NAME" --query 'Stacks[0].StackStatus' --output text)"; then
    STACK_STATUS="$st"
    return 0
  fi
  if [ "$(aws_code)" = ValidationError ] && grep -q 'does not exist' "$AWS_ERR"; then STACK_STATUS=NONE; return 0; fi
  aws_fail "reading the $STACK_NAME stack"
}

# The JMESPath filters that keep a secret's value out of the CLI's answer. A detail (a change set's
# Target, a drift's property difference) is secret when its path or either value names an origin's
# custom headers, or (a change set) a parameter caused it that is not one of STACK_PLAIN_PARAMS.
# Each query takes its one resource with a pipe ("Changes[?...] | [0].Details[?filter]"): a filter
# inside a projection's projection reads the wrong node in the CLI's JMESPath, every field null, and
# "not secret" would hold for everything. The plain query also asks for a path that is there, so a
# filter reading the wrong node prints nothing rather than everything.
stack_secret_filter() {   # stack_secret_filter PATH BEFORE AFTER [CAUSE]
  local p="$1" b="$2" a="$3" f="" w
  for w in OriginCustomHeaders HeaderValue; do
    f="$f${f:+ || }contains($p || '', '$w') || contains($b || '', '$w') || contains($a || '', '$w')"
  done
  if [ -n "${4:-}" ]; then
    local plain="" q
    for q in $STACK_PLAIN_PARAMS; do plain="$plain && CausingEntity != '$q'"; done
    f="$f || (ChangeSource == 'ParameterReference'$plain)"
  fi
  printf '%s' "$f"
}

# stack_where PATH: a property path's place, as "origin header" lines name it.
stack_where() {
  local p="$1"
  p="${p#/Properties}"
  p="${p#/DistributionConfig/}"
  printf '%s' "${p:-the property}"
}

# stack_value TEXT: a value on one line, at most 160 characters.
stack_value() {
  local v
  v="$(printf '%s' "$1" | tr '\n\t' '  ')"
  [ "${#v}" -le 160 ] || v="${v:0:157}..."
  printf '%s' "$v"
}

# stack_drift_guard: stops on a drifted stack unless CBASE_ACCEPT_DRIFT=1.
stack_drift_guard() {
  local id st ds sds n=0 drifted lid type status filter p b a kind text paths=()
  echo "drift: detecting on $STACK_NAME"
  id="$(cfn detect-stack-drift --stack-name "$STACK_NAME" --query StackDriftDetectionId --output text)" \
    || aws_fail "detecting drift on $STACK_NAME"
  while :; do
    st="$(cfn describe-stack-drift-detection-status --stack-drift-detection-id "$id" \
            --query '[DetectionStatus,StackDriftStatus]' --output text)" || aws_fail "reading drift detection $id"
    IFS=$'\t' read -r ds sds <<< "$st"
    [ "$ds" = DETECTION_IN_PROGRESS ] || break
    n=$((n + 1))
    [ "$n" -le 120 ] || { echo "cbase: drift detection $id on $STACK_NAME did not finish" >&2; exit 1; }
    sleep "$POLL_SECONDS"
  done
  drifted="$(cfn describe-stack-resource-drifts --stack-name "$STACK_NAME" \
               --stack-resource-drift-status-filters MODIFIED DELETED \
               --query 'StackResourceDrifts[].[LogicalResourceId,ResourceType,StackResourceDriftStatus]' --output text)" \
    || aws_fail "reading $STACK_NAME's drift"
  [ "$drifted" != None ] || drifted=""
  if [ "$ds" = DETECTION_COMPLETE ] && [ "$sds" = IN_SYNC ] && [ -z "$drifted" ]; then
    echo "drift: none"
    return 0
  fi
  [ "$ds" = DETECTION_COMPLETE ] || echo "drift: detection ended $ds, so the stack could not be shown to be in sync" >&2
  filter="$(stack_secret_filter PropertyPath ExpectedValue ActualValue)"
  while IFS=$'\t' read -r lid type status; do
    [ -n "$lid" ] || continue
    echo "drift: $lid ($type) $status" >&2
    [ "$status" = MODIFIED ] || continue
    # The plain differences, with their values; then the secret ones, by their path alone.
    text="$(cfn describe-stack-resource-drifts --stack-name "$STACK_NAME" --stack-resource-drift-status-filters MODIFIED \
              --query "StackResourceDrifts[?LogicalResourceId=='$lid'] | [0].PropertyDifferences[?PropertyPath && !($filter)].[PropertyPath,DifferenceType,ExpectedValue || '',ActualValue || '']" \
              --output text)" || aws_fail "reading $lid's drift"
    while IFS=$'\t' read -r p kind b a; do
      [ -n "$p" ] || continue
      echo "drift:   $(stack_where "$p") $kind: expected $(stack_value "$b"), actual $(stack_value "$a")" >&2
    done <<< "$text"
    text="$(cfn describe-stack-resource-drifts --stack-name "$STACK_NAME" --stack-resource-drift-status-filters MODIFIED \
              --query "StackResourceDrifts[?LogicalResourceId=='$lid'] | [0].PropertyDifferences[?$filter].PropertyPath" \
              --output text)" || aws_fail "reading $lid's drift"
    read -r -a paths <<< "$text"
    for p in ${paths[@]+"${paths[@]}"}; do
      [ "$p" != None ] || continue
      echo "drift:   origin header $(stack_where "$p") changed (its value is not shown)" >&2
    done
  done <<< "$drifted"
  if [ "${CBASE_ACCEPT_DRIFT:-}" = 1 ]; then
    echo "drift: accepted (--accept-drift): this deploy makes the stack's resources match the template again" >&2
    return 0
  fi
  echo "cbase: the $STACK_NAME stack drifted from its template (above): a deploy would undo those changes. Put the change in cbase.conf, or run ./cbase deploy --accept-drift (CBASE_ACCEPT_DRIFT=1) to overwrite it. Nothing was changed." >&2
  exit 1
}

# stack_discard: deletes the change set, and the empty stack a first change set made.
stack_discard() {
  cfn delete-change-set --stack-name "$STACK_NAME" --change-set-name "$STACK_CS" > /dev/null \
    || aws_fail "deleting change set $STACK_CS"
  if [ "$STACK_TYPE" = CREATE ] && [ "$STACK_STATUS" = NONE ]; then
    cfn delete-stack --stack-name "$STACK_NAME" > /dev/null || aws_fail "deleting the empty $STACK_NAME stack"
  fi
}

# stack_print_changes: the change set, resource by resource, a secret's value never shown.
stack_print_changes() {
  local lines action lid type repl filter p b a cause text
  lines="$(cfn describe-change-set --stack-name "$STACK_NAME" --change-set-name "$STACK_CS" \
             --query 'Changes[].ResourceChange.[Action,LogicalResourceId,ResourceType,Replacement]' --output text)" \
    || aws_fail "describing change set $STACK_CS"
  echo "change set $STACK_CS on $STACK_NAME ($STACK_TYPE):"
  filter="$(stack_secret_filter Target.Path Target.BeforeValue Target.AfterValue cause)"
  while IFS=$'\t' read -r action lid type repl; do
    [ -n "$action" ] || continue
    if [ "$action" = Modify ]; then
      printf '  %-7s %-16s %s (replacement: %s)\n' "$action" "$lid" "$type" "$repl"
    else
      printf '  %-7s %-16s %s\n' "$action" "$lid" "$type"
    fi
    [ "$action" = Modify ] || continue
    text="$(cfn describe-change-set --stack-name "$STACK_NAME" --change-set-name "$STACK_CS" --include-property-values \
              --query "Changes[?ResourceChange.LogicalResourceId=='$lid'] | [0].ResourceChange.Details[?Target.Name && !($filter)].[Target.Path || Target.Name,Target.BeforeValue || '',Target.AfterValue || '']" \
              --output text)" || aws_fail "describing change set $STACK_CS"
    while IFS=$'\t' read -r p b a; do
      [ -n "$p" ] || continue
      if [ -z "$b$a" ]; then echo "      $(stack_where "$p") changes"
      else echo "      $(stack_where "$p"): $(stack_value "$b") -> $(stack_value "$a")"; fi
    done <<< "$text"
    text="$(cfn describe-change-set --stack-name "$STACK_NAME" --change-set-name "$STACK_CS" --include-property-values \
              --query "Changes[?ResourceChange.LogicalResourceId=='$lid'] | [0].ResourceChange.Details[?$filter].[Target.Path || Target.Name || '',CausingEntity || '']" \
              --output text)" || aws_fail "describing change set $STACK_CS"
    while IFS=$'\t' read -r p cause; do
      [ -n "$p" ] || continue
      if [ "$cause" != None ] && [ -n "$cause" ] && [[ "$p" != *OriginCustomHeaders* && "$p" != *HeaderValue* ]]; then
        echo "      parameter $cause changed $(stack_where "$p") (its value is not shown)"
      else
        echo "      origin header $(stack_where "$p") changed (its value is not shown)"
      fi
    done <<< "$text"
  done <<< "$lines"
}

# stack_plan: the app's template, then stack_changeset. Sets STACK_CHANGES.
stack_plan() {
  STACK_NAME="$(stack_name)"
  STACK_TEMPLATE="$CB_TMP/stack.yaml"
  STACK_REGION=us-east-1
  STACK_TAGS=("Key=cbase:app,Value=$CONF_APP")
  STACK_CAPS=()
  stack_render "$STACK_TEMPLATE"
  stack_changeset
}

# stack_changeset: STACK_NAME's change set from STACK_TEMPLATE (with STACK_PARAMS, STACK_TAGS,
# STACK_CAPS, in STACK_REGION): the drift guard, the change set printed, its gates. Sets
# STACK_CHANGES.
stack_changeset() {
  STACK_CS="cbase-deploy"
  stack_status
  case "$STACK_STATUS" in
    NONE|REVIEW_IN_PROGRESS) STACK_TYPE=CREATE; echo "stack: $STACK_NAME is new" ;;
    ROLLBACK_COMPLETE)
      echo "cbase: the $STACK_NAME stack's first create failed and rolled back (ROLLBACK_COMPLETE), and CloudFormation updates no such stack: delete it (aws cloudformation delete-stack --region $STACK_REGION --stack-name $STACK_NAME; it holds nothing), then deploy again" >&2
      exit 1 ;;
    *_IN_PROGRESS)
      echo "cbase: the $STACK_NAME stack is $STACK_STATUS: another deploy is running, or CloudFormation is still working; wait for it, then deploy again" >&2
      exit 1 ;;
    *) STACK_TYPE=UPDATE; echo "stack: $STACK_NAME is $STACK_STATUS"; stack_drift_guard ;;
  esac
  # A change set of this name left by a run that stopped: gone before the new one.
  if [ "$STACK_STATUS" != NONE ]; then
    if ! cfn delete-change-set --stack-name "$STACK_NAME" --change-set-name "$STACK_CS" > /dev/null; then
      [ "$(aws_code)" = ChangeSetNotFound ] || aws_fail "clearing change set $STACK_CS"
    fi
  fi
  local why
  cfn create-change-set --stack-name "$STACK_NAME" --change-set-name "$STACK_CS" --change-set-type "$STACK_TYPE" \
    --template-body "file://$STACK_TEMPLATE" --tags "${STACK_TAGS[@]}" ${STACK_CAPS[@]+--capabilities "${STACK_CAPS[@]}"} \
    ${STACK_PARAMS[@]+--parameters "${STACK_PARAMS[@]}"} --query Id --output text > /dev/null \
    || aws_fail "creating a change set on $STACK_NAME"
  if ! cfn wait change-set-create-complete --stack-name "$STACK_NAME" --change-set-name "$STACK_CS"; then
    why="$(cfn describe-change-set --stack-name "$STACK_NAME" --change-set-name "$STACK_CS" --query StatusReason --output text)" \
      || aws_fail "describing change set $STACK_CS"
    case "$why" in
      *"didn't contain changes"*|*"No updates are to be performed"*)
        echo "stack: $STACK_NAME is up to date (no changes)"
        STACK_CHANGES=""
        cfn delete-change-set --stack-name "$STACK_NAME" --change-set-name "$STACK_CS" > /dev/null || aws_fail "deleting change set $STACK_CS"
        return 0 ;;
    esac
    printf '%s\n' "$why" > "$AWS_ERR"
    echo "cbase: change set $STACK_CS on $STACK_NAME failed:" >&2
    aws_mask < "$AWS_ERR" | sed 's/^/  /' | head -5 >&2
    stack_discard
    exit 1
  fi
  # shellcheck disable=SC2034 # tools/cbase reads it
  STACK_CHANGES=1
  stack_print_changes

  stack_keep_guard
  local replaced lid
  replaced="$(cfn describe-change-set --stack-name "$STACK_NAME" --change-set-name "$STACK_CS" \
      --query "Changes[?ResourceChange.ResourceType=='AWS::CloudFront::Distribution' && (ResourceChange.Action=='Remove' || ResourceChange.Replacement=='True' || ResourceChange.Replacement=='Conditional')].ResourceChange.LogicalResourceId" \
      --output text)" || aws_fail "describing change set $STACK_CS"
  [ "$replaced" != None ] || replaced=""
  for lid in $replaced; do
    echo "note: the distribution $lid is replaced: a new one with a new cloudfront.net name is made and the old one deleted, so the app is briefly unreachable and its hostname's record moves to the new name (deploy rewrites a Route 53 record itself)"
  done
}

# stack_keep_guard: THE ONE HARD STOP. A change set that would replace or remove a bucket (every
# file in it lost) or a VM group's instance (every member's data on its disk lost) is refused,
# deleted, and nothing runs. Deploy's and group-init's change sets both pass through it.
stack_keep_guard() {
  local refused lid type a r what
  refused="$(cfn describe-change-set --stack-name "$STACK_NAME" --change-set-name "$STACK_CS" \
      --query "Changes[?(ResourceChange.ResourceType=='AWS::S3::Bucket' || ResourceChange.ResourceType=='AWS::EC2::Instance') && (ResourceChange.Action=='Remove' || ResourceChange.Replacement=='True' || ResourceChange.Replacement=='Conditional')].ResourceChange.[LogicalResourceId,ResourceType,Action,Replacement]" \
      --output text)" || aws_fail "describing change set $STACK_CS"
  [ "$refused" != None ] || refused=""
  [ -n "$refused" ] || return 0
  while IFS=$'\t' read -r lid type a r; do
    [ -n "$lid" ] || continue
    if [ "$type" = AWS::S3::Bucket ]; then what=bucket; else what=instance; fi
    echo "refused: the $what $lid would be $([ "$a" = Remove ] && echo removed || echo "replaced (Replacement: $r)")" >&2
  done <<< "$refused"
  stack_discard
  echo "cbase: the change set would replace or remove a bucket (and every file in it) or a VM group's instance (and every app's data on its disk): refused, and the change set deleted; nothing was executed" >&2
  exit 1
}

# stack_delete: deletes STACK_NAME (in STACK_REGION) and waits; nothing when there is none.
stack_delete() {
  stack_status
  if [ "$STACK_STATUS" = NONE ]; then
    echo "stack: there is no $STACK_NAME stack: nothing to delete"
    return 0
  fi
  cfn delete-stack --stack-name "$STACK_NAME" > /dev/null || aws_fail "deleting the $STACK_NAME stack"
  echo "stack: deleting $STACK_NAME (a distribution takes some minutes to go)"
  if ! cfn wait stack-delete-complete --stack-name "$STACK_NAME"; then
    echo "cbase: the $STACK_NAME stack was not deleted (stack-delete-complete). Its failed resources:" >&2
    cfn describe-stack-events --stack-name "$STACK_NAME" \
      --query "StackEvents[?ends_with(ResourceStatus, 'FAILED')].[LogicalResourceId,ResourceStatusReason]" --output text \
      > "$AWS_ERR" 2> /dev/null || true
    aws_mask < "$AWS_ERR" | head -10 | sed 's/^/  /' >&2
    exit 1
  fi
  echo "stack: $STACK_NAME is deleted"
}

# stack_execute: runs the change set and waits.
stack_execute() {
  cfn execute-change-set --stack-name "$STACK_NAME" --change-set-name "$STACK_CS" > /dev/null \
    || aws_fail "executing change set $STACK_CS on $STACK_NAME"
  echo "stack: executing $STACK_CS (a new distribution takes some minutes)"
  local w=stack-update-complete
  [ "$STACK_TYPE" = UPDATE ] || w=stack-create-complete
  if ! cfn wait "$w" --stack-name "$STACK_NAME"; then
    echo "cbase: the $STACK_NAME stack did not finish ($w). Its failed resources:" >&2
    cfn describe-stack-events --stack-name "$STACK_NAME" \
      --query "StackEvents[?ends_with(ResourceStatus, 'FAILED')].[LogicalResourceId,ResourceStatusReason]" --output text \
      > "$AWS_ERR" 2> /dev/null || true
    aws_mask < "$AWS_ERR" | head -10 | sed 's/^/  /' >&2
    exit 1
  fi
  echo "stack: $STACK_NAME is ${w%-complete} complete"
}

# stack_output KEY: one of the stack's outputs.
stack_output() {
  local v
  v="$(cfn describe-stacks --stack-name "$STACK_NAME" --query "Stacks[0].Outputs[?OutputKey=='$1'].OutputValue" --output text)" \
    || aws_fail "reading $STACK_NAME's outputs"
  [ "$v" != None ] || v=""
  printf '%s' "$v"
}
