# shellcheck shell=bash
# stack.sh - the tool's CloudFormation stacks, and the one change-set flow every one of them goes
# through (README.md "./cbase deploy tool"). Sourced by tools/cbase.
#
# THE TEMPLATES are the files in templates/, plain CloudFormation with no transform: app.yaml (an
# app's own stack, cbase-app-<app> in us-east-1, CloudFront's region), group-vm.yaml and
# group-static.yaml (group.sh's, in the CLI's region). template_render fills in a file's __NAME__
# values and keeps its "#if FLAG" blocks for the shape at hand: an app's static origin, its VM
# parameters and its API distribution exist only for an app that has them, so a static site's
# template names no VM at all (a CloudFormation Condition cannot do that: a conditional resource's
# parameters are still declared and must still be given). stack_routes writes the one list that
# varies in length, the route behaviours. In app.yaml:
#   Oac           the Origin Access Control the distribution signs its bucket reads with. The
#                 bucket's policy, which lets it, is the static group's and is set once per bucket,
#                 on aws:SourceAccount, by group-init (group.sh): deploy never edits a bucket policy.
#   Router        a viewer-request CloudFront Function on every static behaviour, an SPA's routing:
#                 (a) a file, a path whose last part has a dot or one under /.well-known/, passes as
#                 it is (one the bucket lacks is S3's 404, never the shell, so a broken asset link
#                 fails loudly); (b) a page, "/x" /x.html and "/x/" /x/index.html ("/" /index.html),
#                 when the build has it (PAGES, its html files: stack_pages), a "/x/" that is no
#                 directory's index tried as "/x"; (c) anything else is the app shell, CBASE_SHELL
#                 (default /index.html), with S3's 200. cbase_test.sh runs it under node.
#   Distribution  the default behaviour is the "*" route; every other route is a behaviour, in
#                 cbase.conf's order. The static origin is the bucket with OriginPath /<folder>, so
#                 the app reads its own folder and nothing else. A vm route goes to the VmOrigin and
#                 VmPort parameters, the VM's origin hostname and the app's port (vm.sh); a ws
#                 route is its own origin, for its long timeouts (CloudFront sets them per origin):
#                 read 60 s, keep-alive 60 s, uncached, every method. Each VM origin sends
#                 X-Cbase-Origin, the NoEcho parameter OriginSecret, which serve checks.
#   ApiDistribution  with CBASE_API_HOST: that hostname's every request to the VM, its own routes
#                 and the ws routes on the ws origin, and CloudFront's Server-Timing.
# Caching uses AWS-managed policies alone (CachingOptimized, CachingDisabled; the VM's requests
# AllViewerExceptHostHeader), so a flat-rate CloudFront plan can hold the distribution. A route's
# methods are the smallest of CloudFront's three sets that covers them: GET,HEAD; GET,HEAD,OPTIONS;
# or all seven.
#
# THE CHANGE-SET FLOW (stack_changeset), deploy's and group-init's alike, in order:
#   1. an existing stack's drift is detected first; drifted resources are printed and the run stops,
#      unless CBASE_ACCEPT_DRIFT=1 (--accept-drift): the update would undo a change made by hand;
#   2. a change set is made and printed, always, a secret's value never shown (aws.sh THE MASK:
#      the values are left out by the CLI's own --query filter, aws_secret_filter);
#   3. THE ONE HARD STOP (stack_keep_guard): a change that would replace or remove a BUCKET (every
#      app's files) or a VM group's INSTANCE (every member's /srv/cbase/<app>/data on its disk) is
#      refused, the change set deleted, nothing executed;
#   4. NO DOWNTIME after an app's first deploy (stack_downtime_guard): on an app's stack that
#      exists, a change set that replaces or removes a distribution, or takes a hostname off one
#      (an alias removed, or moved to the other distribution), is refused and deleted unless
#      CBASE_ALLOW_DOWNTIME=1 (--allow-downtime). A first deploy (the stack's create) is not held;
#   5. with DRY_RUN=1 (--dry-run) an app's change set is deleted unexecuted (and the empty stack a
#      first change set leaves); otherwise the caller executes it (stack_execute) or deletes it
#      (stack_discard): a refused confirmation, a service that did not start.

STACK_NAME=""       # the stack, its region, tags and capabilities (stack_use)
STACK_REGION=""
STACK_TAGS=()       # "Key=K,Value=V" each
STACK_CAPS=()       # --capabilities, for a template that makes an IAM role (a VM group's)
STACK_APP=""        # 1: an app's stack, whose redeploys the downtime guard holds
STACK_PARAMS=()     # "ParameterKey=K,ParameterValue=V" each (vm.sh's VmOrigin, VmPort, OriginSecret)
STACK_TEMPLATE=""   # the rendered template's file
STACK_STATUS=""     # before this run: NONE, or the stack's status
STACK_TYPE=""       # CREATE or UPDATE
STACK_CS=cbase-deploy   # the change set's name
STACK_CHANGES=""    # 1 when the change set holds changes
STACK_CERT=""       # the certificate the distribution serves the hostnames with, or ""
STACK_API_CERT=""   # the certificate CBASE_API_HOST's distribution serves, or ""
STACK_BUCKET=""     # the app's bucket, its region and its folder (static.sh static_bucket_find)
STACK_BUCKET_REGION=""
STACK_PREFIX=""

# The AWS-managed policies, by id.
CF_CACHING_OPTIMIZED=658327ea-f89d-4fab-a63d-7e88639e58f6
CF_CACHING_DISABLED=4135ea2d-6df8-44a3-9df3-4b5a84be39ad
CF_ALL_VIEWER_EXCEPT_HOST=b689b0a8-53d0-40ab-baf2-68738e2966ac
CF_USE_ORIGIN_CC_QS=4cc15a8a-d715-48a4-82b8-cc0b614638fe   # the cache mode origin
# shellcheck disable=SC2034 # cert.sh's alias records read it
CF_ZONE_ID=Z2FDTNDATAQYW2   # every CloudFront distribution's hosted zone, for a Route 53 alias
STACK_FUNCTION_MAX=10240    # CloudFront's limit on a function's code, in bytes

# stack_use NAME REGION TAG: the stack the next change set or delete is on, with no parameters yet.
stack_use() {
  STACK_NAME="$1"
  STACK_REGION="$2"
  STACK_TAGS=("$3")
  STACK_CAPS=()
  STACK_PARAMS=()
  STACK_APP=""
  STACK_TEMPLATE="$CB_TMP/stack.yaml"
}

# stack_app_use: the app's own stack.
stack_app_use() {
  stack_use "cbase-app-$CONF_APP" us-east-1 "Key=cbase:app,Value=$CONF_APP"
  STACK_APP=1
}

# template_render FILE FLAGS: templates/FILE with each "#if FLAG" ... "#end" block (they nest; "#if
# !FLAG" is its opposite) kept only when FLAG is one of the blank-separated FLAGS, the marker lines
# dropped, and each __NAME__ replaced by the value of T_NAME, as text (a value is never read again).
template_render() {
  # shellcheck disable=SC2154 # here is tools/cbase's directory
  local file="$here/cbase.d/templates/$1" line out skip=0 depth=0 f name
  while IFS= read -r line || [ -n "$line" ]; do
    case "$line" in
      '#if '*)
        depth=$((depth + 1))
        f="${line#\#if }"
        if [ "$skip" -eq 0 ]; then
          if [ "${f#!}" != "$f" ]; then [[ " $2 " != *" ${f#!} "* ]] || skip="$depth"
          else [[ " $2 " == *" $f "* ]] || skip="$depth"; fi
        fi
        continue ;;
      '#end')
        [ "$skip" -ne "$depth" ] || skip=0
        depth=$((depth - 1))
        continue ;;
    esac
    [ "$skip" -eq 0 ] || continue
    out=""
    while [[ "$line" =~ __([A-Z0-9]+)__ ]]; do
      name="T_${BASH_REMATCH[1]}"
      out="$out${line%%"${BASH_REMATCH[0]}"*}${!name}"
      line="${line#*"${BASH_REMATCH[0]}"}"
    done
    printf '%s\n' "$out$line"
  done < "$file"
}

# stack_methods METHODS: CloudFront's allowed-method set covering METHODS (ALL or a comma list).
stack_methods() {
  case ",$1," in
    *,ALL,*|*,PUT,*|*,POST,*|*,PATCH,*|*,DELETE,*) echo "[GET, HEAD, OPTIONS, PUT, PATCH, POST, DELETE]" ;;
    *,OPTIONS,*) echo "[GET, HEAD, OPTIONS]" ;;
    *) echo "[GET, HEAD]" ;;
  esac
}

# stack_behaviour ROUTE: one route's behaviour (ROUTE is conf.sh's "<pattern> <origin> <cache>
# <methods>"): DefaultCacheBehavior's fields for the "*" route, else an item of CacheBehaviors.
# STACK_RHP names the response headers policy of a VM behaviour, if any.
stack_behaviour() {
  local pattern origin cache methods pad="          " policy
  read -r pattern origin cache methods <<< "$1"
  if [ "$pattern" != "*" ]; then
    printf '%s- PathPattern: "%s"\n' "$pad" "$pattern"
    pad="$pad  "
  fi
  printf '%sTargetOriginId: %s\n' "$pad" "$origin"
  printf '%sViewerProtocolPolicy: redirect-to-https\n' "$pad"
  printf '%sAllowedMethods: %s\n' "$pad" "$(stack_methods "$methods")"
  printf '%sCachedMethods: [GET, HEAD]\n' "$pad"
  if [ "$origin" = ws ]; then printf '%sCompress: false\n' "$pad"; else printf '%sCompress: true\n' "$pad"; fi
  case "$cache" in
    cache) policy="$CF_CACHING_OPTIMIZED   # Managed-CachingOptimized" ;;
    origin) policy="$CF_USE_ORIGIN_CC_QS   # Managed-UseOriginCacheControlHeaders-QueryStrings" ;;
    *) policy="$CF_CACHING_DISABLED   # Managed-CachingDisabled" ;;
  esac
  printf '%sCachePolicyId: %s\n' "$pad" "$policy"
  if [ "$origin" != static ]; then
    printf '%sOriginRequestPolicyId: %s   # Managed-AllViewerExceptHostHeader\n' "$pad" "$CF_ALL_VIEWER_EXCEPT_HOST"
    [ -z "${STACK_RHP:-}" ] || printf '%sResponseHeadersPolicyId: !Ref %s\n' "$pad" "$STACK_RHP"
  else
    printf '%sFunctionAssociations:\n' "$pad"
    printf '%s  - EventType: viewer-request\n' "$pad"
    printf '%s    FunctionARN: !GetAtt Router.FunctionMetadata.FunctionARN\n' "$pad"
  fi
}

# stack_routes DEFAULT [ROUTE...]: a distribution's behaviours, DEFAULT its default.
stack_routes() {
  local r
  printf '        DefaultCacheBehavior:\n'
  stack_behaviour "$1"
  shift
  [ $# -eq 0 ] || printf '        CacheBehaviors:\n'
  for r in "$@"; do stack_behaviour "$r"; done
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

# stack_render FILE: the app's template, from CONF_* and STACK_BUCKET, STACK_BUCKET_REGION,
# STACK_PREFIX, STACK_CERT and STACK_API_CERT.
stack_render() {
  local r origin flags="" static="" vm="" ws="" apiws="" last=$((${#CONF_ROUTES[@]} - 1)) api_rs=() seen=" " n
  for r in "${CONF_ROUTES[@]}"; do
    read -r _ origin _ <<< "$r"
    case "$origin" in static) static=1 ;; vm) vm=1 ;; ws) ws=1 ;; esac
  done
  [ -z "$CONF_API_HOST" ] || vm=1
  apiws="$ws"
  for r in ${CONF_API_ROUTES[@]+"${CONF_API_ROUTES[@]}"}; do [ "$(echo "$r" | cut -d' ' -f2)" != ws ] || apiws=1; done
  [ -z "$static" ] || flags="$flags static"
  [ -z "$vm$ws" ] || flags="$flags vm"
  [ -z "$vm" ] || flags="$flags vmroute"
  [ -z "$ws" ] || flags="$flags ws"
  [ -z "${VM_ORIGIN_ADDR:-}" ] || flags="$flags addr"
  [ -z "$STACK_CERT" ] || [ -z "$CONF_DOMAIN" ] || flags="$flags cert"
  [ -z "$CONF_API_HOST" ] || flags="$flags api"
  [ -z "$apiws" ] || flags="$flags apiws"
  [ -z "$STACK_API_CERT" ] || flags="$flags apicert"
  # CBASE_ALIASES off (a staged attach's first step): the certificate and no alias, since
  # CloudFront moves an alias only onto a distribution whose certificate covers it (cert_attach).
  [ "${CONF_ALIASES:-on}" = off ] || flags="$flags aliases"
  local shell="${CONF_SHELL:-/index.html}" pages="" aliases routes apiroutes
  if [ -n "$static" ]; then
    if [ -n "$ST_FILES" ] && [ -f "$ST_FILES" ] && ! grep -Fxq -- "${shell#/}" "$ST_FILES"; then
      echo "cbase: the app shell is $shell (CBASE_SHELL), and the static dir has no ${shell#/}: nothing was changed" >&2
      exit 1
    fi
    pages="$(stack_pages)"
  fi
  # shellcheck disable=SC2153 # conf.sh sets CONF_DOMAINS
  aliases="$(printf '%s, ' ${CONF_DOMAINS[@]+"${CONF_DOMAINS[@]}"} | sed 's/, $//')"
  routes="$(stack_routes "${CONF_ROUTES[last]}" "${CONF_ROUTES[@]:0:last}")"
  # The API host's: CBASE_API_ROUTE's lines, then the ws routes (CBASE_ROUTE's) it does not name.
  for r in ${CONF_API_ROUTES[@]+"${CONF_API_ROUTES[@]}"}; do api_rs+=("$r"); seen="$seen${r%% *} "; done
  for r in "${CONF_ROUTES[@]}"; do
    [ "$(echo "$r" | cut -d' ' -f2)" != ws ] || [[ "$seen" == *" ${r%% *} "* ]] || api_rs+=("$r")
  done
  apiroutes="$(STACK_RHP=ServerTimingPolicy; stack_routes "* vm nocache ALL" ${api_rs[@]+"${api_rs[@]}"})"
  # shellcheck disable=SC2034 # template_render reads the T_ values
  local T_APP="$CONF_APP" T_APIHOST="$CONF_API_HOST" T_CERT="$STACK_CERT" T_APICERT="$STACK_API_CERT" T_ALIASES="$aliases" \
    T_BUCKET="$STACK_BUCKET" T_BUCKETREGION="$STACK_BUCKET_REGION" T_PREFIX="$STACK_PREFIX" T_SHELL="$shell" T_PAGES="$pages" \
    T_ROUTES="$routes" T_APIROUTES="$apiroutes" T_ADDR="${VM_ORIGIN_ADDR:-}"
  template_render app.yaml "$flags" > "$1"
  # The router's code, as CloudFront counts it: its lines, less the template's indent.
  n="$(awk '/^      FunctionCode: \|$/ { f = 1; next } f && /^$/ { exit } f { n += length($0) - 8 + (c++ ? 1 : 0) } END { print n + 0 }' "$1")"
  if [ "$n" -gt "$STACK_FUNCTION_MAX" ]; then
    echo "cbase: the router's code is $n bytes with the build's page list, over CloudFront's $STACK_FUNCTION_MAX: the build has too many pages; nothing was changed" >&2
    exit 1
  fi
}

# cfn ARGS...: one CloudFormation call in the stack's region; cs ARGS...: the change set described.
cfn() { aws_call cloudformation --region "$STACK_REGION" "$@"; }
cs() { cfn describe-change-set --stack-name "$STACK_NAME" --change-set-name "$STACK_CS" "$@"; }

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

# stack_drift_read: one read of drift detection STACK_DRIFT_ID; true once it is no longer running.
STACK_DRIFT_ID=""
STACK_DRIFT_DS=""
STACK_DRIFT_SDS=""
stack_drift_read() {
  local st
  st="$(cfn describe-stack-drift-detection-status --stack-drift-detection-id "$STACK_DRIFT_ID" \
          --query '[DetectionStatus,StackDriftStatus]' --output text)" || aws_fail "reading drift detection $STACK_DRIFT_ID"
  IFS=$'\t' read -r STACK_DRIFT_DS STACK_DRIFT_SDS <<< "$st"
  [ "$STACK_DRIFT_DS" != DETECTION_IN_PROGRESS ]
}

# stack_drifts LID QUERY: the property differences of the drifted resource LID that QUERY selects.
stack_drifts() {
  cfn describe-stack-resource-drifts --stack-name "$STACK_NAME" --stack-resource-drift-status-filters MODIFIED \
    --query "StackResourceDrifts[?LogicalResourceId=='$1'] | [0].PropertyDifferences[$2" --output text
}

# stack_drift_guard: stops on a drifted stack unless CBASE_ACCEPT_DRIFT=1.
stack_drift_guard() {
  local drifted lid type status filter p b a kind text paths=()
  echo "drift: detecting on $STACK_NAME"
  STACK_DRIFT_ID="$(cfn detect-stack-drift --stack-name "$STACK_NAME" --query StackDriftDetectionId --output text)" \
    || aws_fail "detecting drift on $STACK_NAME"
  aws_poll 121 stack_drift_read || { echo "cbase: drift detection $STACK_DRIFT_ID on $STACK_NAME did not finish" >&2; exit 1; }
  drifted="$(cfn describe-stack-resource-drifts --stack-name "$STACK_NAME" \
               --stack-resource-drift-status-filters MODIFIED DELETED \
               --query 'StackResourceDrifts[].[LogicalResourceId,ResourceType,StackResourceDriftStatus]' --output text)" \
    || aws_fail "reading $STACK_NAME's drift"
  [ "$drifted" != None ] || drifted=""
  if [ "$STACK_DRIFT_DS" = DETECTION_COMPLETE ] && [ "$STACK_DRIFT_SDS" = IN_SYNC ] && [ -z "$drifted" ]; then
    echo "drift: none"
    return 0
  fi
  [ "$STACK_DRIFT_DS" = DETECTION_COMPLETE ] || echo "drift: detection ended $STACK_DRIFT_DS, so the stack could not be shown to be in sync" >&2
  filter="$(aws_secret_filter PropertyPath ExpectedValue ActualValue)"
  while IFS=$'\t' read -r lid type status; do
    [ -n "$lid" ] || continue
    echo "drift: $lid ($type) $status" >&2
    [ "$status" = MODIFIED ] || continue
    # The plain differences, with their values; then the secret ones, by their path alone.
    text="$(stack_drifts "$lid" "?PropertyPath && !($filter)].[PropertyPath,DifferenceType,ExpectedValue || '',ActualValue || '']")" || aws_fail "reading $lid's drift"
    while IFS=$'\t' read -r p kind b a; do
      [ -n "$p" ] || continue
      echo "drift:   $(stack_where "$p") $kind: expected $(stack_value "$b"), actual $(stack_value "$a")" >&2
    done <<< "$text"
    text="$(stack_drifts "$lid" "?$filter].PropertyPath")" || aws_fail "reading $lid's drift"
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

# stack_details LID QUERY: the change set's details of the resource LID that QUERY selects.
stack_details() {
  cs --include-property-values \
    --query "Changes[?ResourceChange.LogicalResourceId=='$1'] | [0].ResourceChange.Details[$2" --output text
}

# stack_print_changes: the change set, resource by resource, a secret's value never shown.
stack_print_changes() {
  local lines action lid type repl filter p b a cause text
  lines="$(cs --query 'Changes[].ResourceChange.[Action,LogicalResourceId,ResourceType,Replacement]' --output text)" \
    || aws_fail "describing change set $STACK_CS"
  echo "change set $STACK_CS on $STACK_NAME ($STACK_TYPE):"
  filter="$(aws_secret_filter Target.Path Target.BeforeValue Target.AfterValue cause)"
  while IFS=$'\t' read -r action lid type repl; do
    [ -n "$action" ] || continue
    if [ "$action" != Modify ]; then
      printf '  %-7s %-16s %s\n' "$action" "$lid" "$type"
      continue
    fi
    printf '  %-7s %-16s %s (replacement: %s)\n' "$action" "$lid" "$type" "$repl"
    text="$(stack_details "$lid" "?Target.Name && !($filter)].[Target.Path || Target.Name,Target.BeforeValue || '',Target.AfterValue || '']")" || aws_fail "describing change set $STACK_CS"
    while IFS=$'\t' read -r p b a; do
      [ -n "$p" ] || continue
      if [ -z "$b$a" ]; then echo "      $(stack_where "$p") changes"
      else echo "      $(stack_where "$p"): $(stack_value "$b") -> $(stack_value "$a")"; fi
    done <<< "$text"
    text="$(stack_details "$lid" "?$filter].[Target.Path || Target.Name || '',CausingEntity || '']")" || aws_fail "describing change set $STACK_CS"
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

# stack_plan: the app's template, then its change set (stack_changeset).
stack_plan() {
  stack_render "$STACK_TEMPLATE"
  stack_changeset
}

# stack_changeset: THE CHANGE-SET FLOW (above) for STACK_NAME, from STACK_TEMPLATE. Sets
# STACK_CHANGES; a dry run's change set is deleted here.
stack_changeset() {
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
  if [ "$STACK_STATUS" != NONE ] && ! cfn delete-change-set --stack-name "$STACK_NAME" --change-set-name "$STACK_CS" > /dev/null; then
    [ "$(aws_code)" = ChangeSetNotFound ] || aws_fail "clearing change set $STACK_CS"
  fi
  local why
  cfn create-change-set --stack-name "$STACK_NAME" --change-set-name "$STACK_CS" --change-set-type "$STACK_TYPE" \
    --template-body "file://$STACK_TEMPLATE" --tags "${STACK_TAGS[@]}" ${STACK_CAPS[@]+--capabilities "${STACK_CAPS[@]}"} \
    ${STACK_PARAMS[@]+--parameters "${STACK_PARAMS[@]}"} --query Id --output text > /dev/null \
    || aws_fail "creating a change set on $STACK_NAME"
  if ! cfn wait change-set-create-complete --stack-name "$STACK_NAME" --change-set-name "$STACK_CS"; then
    why="$(cs --query StatusReason --output text)" || aws_fail "describing change set $STACK_CS"
    case "$why" in
      *"didn't contain changes"*|*"No updates are to be performed"*)
        echo "stack: $STACK_NAME is up to date (no changes)"
        STACK_CHANGES=""
        cfn delete-change-set --stack-name "$STACK_NAME" --change-set-name "$STACK_CS" > /dev/null || aws_fail "deleting change set $STACK_CS"
        return 0 ;;
    esac
    printf '%s\n' "$why" > "$AWS_ERR"
    echo "cbase: change set $STACK_CS on $STACK_NAME failed:" >&2
    aws_show 5
    stack_discard
    exit 1
  fi
  # shellcheck disable=SC2034 # tools/cbase reads it
  STACK_CHANGES=1
  stack_print_changes
  stack_keep_guard
  [ "$STACK_APP" != 1 ] || [ "$STACK_TYPE" != UPDATE ] || stack_downtime_guard
  [ "$STACK_APP" != 1 ] || [ "${DRY_RUN:-}" != 1 ] || stack_discard
}

# stack_refusals TYPES: the change set's removals and replacements of the resources whose type is
# one of TYPES (a JMESPath condition) in STACK_REFUSED, "<logical id> <type> <action>
# <replacement>" a line.
STACK_REFUSED=""
stack_refusals() {
  STACK_REFUSED="$(cs --query "Changes[?($1) && (ResourceChange.Action=='Remove' || ResourceChange.Replacement=='True' || ResourceChange.Replacement=='Conditional')].ResourceChange.[LogicalResourceId,ResourceType,Action,Replacement]" \
                     --output text)" || aws_fail "describing change set $STACK_CS"
  [ "$STACK_REFUSED" != None ] || STACK_REFUSED=""
}

# stack_aliases FILE: each distribution's hostnames in a template, "<logical id> <host>" a line.
stack_aliases() {
  awk '/^  [A-Za-z]+:$/ {lid = substr($1, 1, length($1) - 1)}
       /^        Aliases: \[/ {s = $0; sub(/^ *Aliases: \[/, "", s); sub(/\]$/, "", s); n = split(s, a, ", "); for (i = 1; i <= n; i++) print lid, a[i]}' "$1"
}

# stack_downtime_guard: NO DOWNTIME after the first deploy. On an app's stack that exists, a change
# set that replaces or removes a distribution (a new cloudfront.net name: the old hostname record
# points at nothing until DNS moves), or takes a hostname off the distribution that serves it (an
# alias removed, or moved to the other distribution: CloudFront serves it nowhere meanwhile), is
# refused and deleted, unless CBASE_ALLOW_DOWNTIME=1 (--allow-downtime). The stack's current
# template is the "before" of the aliases.
stack_downtime_guard() {
  local lid a w why=() old="$CB_TMP/stack-old.yaml"
  stack_refusals "ResourceChange.ResourceType=='AWS::CloudFront::Distribution'"
  while IFS=$'\t' read -r lid _ a _; do
    [ -n "$lid" ] || continue
    if [ "$a" = Remove ]; then why+=("the distribution $lid is removed")
    else why+=("the distribution $lid is replaced: a new one, with a new cloudfront.net name, and the old one deleted"); fi
  done <<< "$STACK_REFUSED"
  cfn get-template --stack-name "$STACK_NAME" --query TemplateBody --output text > "$old" || aws_fail "reading $STACK_NAME's template"
  stack_aliases "$old" | LC_ALL=C sort > "$old.aliases"
  stack_aliases "$STACK_TEMPLATE" | LC_ALL=C sort > "$CB_TMP/stack-new.aliases"
  while read -r lid a; do
    [ -n "$a" ] || continue
    why+=("the hostname $a is taken off the distribution $lid")
  done < <(LC_ALL=C comm -23 "$old.aliases" "$CB_TMP/stack-new.aliases")
  [ "${#why[@]}" -gt 0 ] || return 0
  for w in "${why[@]}"; do echo "downtime: $w" >&2; done
  if [ "${CBASE_ALLOW_DOWNTIME:-}" = 1 ]; then
    echo "downtime: allowed (--allow-downtime): the app is unreachable at those hostnames until the change is done and DNS follows" >&2
    return 0
  fi
  stack_discard
  echo "cbase: this change set would make $CONF_APP unreachable for a while (above), and after an app's first deploy no deploy may: refused, and the change set deleted; nothing was executed. Run ./cbase deploy --allow-downtime (CBASE_ALLOW_DOWNTIME=1) to accept it." >&2
  exit 1
}

# stack_keep_guard: THE ONE HARD STOP. A change set that would replace or remove a bucket (every
# file in it lost) or a VM group's instance (every member's data on its disk lost) is refused,
# deleted, and nothing runs.
stack_keep_guard() {
  local lid type a r what
  stack_refusals "ResourceChange.ResourceType=='AWS::S3::Bucket' || ResourceChange.ResourceType=='AWS::EC2::Instance'"
  [ -n "$STACK_REFUSED" ] || return 0
  while IFS=$'\t' read -r lid type a r; do
    [ -n "$lid" ] || continue
    if [ "$type" = AWS::S3::Bucket ]; then what=bucket; else what=instance; fi
    echo "refused: the $what $lid would be $([ "$a" = Remove ] && echo removed || echo "replaced (Replacement: $r)")" >&2
  done <<< "$STACK_REFUSED"
  stack_discard
  echo "cbase: the change set would replace or remove a bucket (and every file in it) or a VM group's instance (and every app's data on its disk): refused, and the change set deleted; nothing was executed" >&2
  exit 1
}

# stack_wait WAITER: CloudFormation's wait on STACK_NAME, or the stack's failed resources (masked)
# and the tool stops; WHAT names the step for the message.
stack_wait() {
  cfn wait "$1" --stack-name "$STACK_NAME" && return 0
  echo "cbase: the $STACK_NAME stack $2 ($1). Its failed resources:" >&2
  cfn describe-stack-events --stack-name "$STACK_NAME" \
    --query "StackEvents[?ends_with(ResourceStatus, 'FAILED')].[LogicalResourceId,ResourceStatusReason]" --output text \
    > "$AWS_ERR" 2> /dev/null || true
  aws_show 10
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
  stack_wait stack-delete-complete "was not deleted"
  echo "stack: $STACK_NAME is deleted"
}

# stack_execute: runs the change set and waits. With STACK_NO_ROLLBACK=1 (an attach's update, the
# aliases already moved onto the distribution) a failed update is not rolled back: a rollback would
# take the moved hostnames off it, served nowhere. The next deploy updates the UPDATE_FAILED stack.
stack_execute() {
  cfn execute-change-set --stack-name "$STACK_NAME" --change-set-name "$STACK_CS" \
      ${STACK_NO_ROLLBACK:+--disable-rollback} > /dev/null \
    || aws_fail "executing change set $STACK_CS on $STACK_NAME"
  echo "stack: executing $STACK_CS (a new distribution takes some minutes)"
  local w=stack-update-complete
  [ "$STACK_TYPE" = UPDATE ] || w=stack-create-complete
  stack_wait "$w" "did not finish"
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
