# shellcheck shell=bash
# static.sh - an app's static files into its folder of the static group's bucket, s3://<bucket>/<app>/
# (README.md "./cbase deploy tool"). Sourced by tools/cbase.
#
# WHAT IS UPLOADED: every file under the static dir (CONF_STATIC_DIR), which must hold index.html,
# but a path with a part that starts with '.' (.git, .DS_Store), .well-known/ excepted. A name that
# holds a control character (a tab, a newline) stops the deploy: S3 would take it, and no listing
# could be read back line by line.
#
# EACH FILE'S HEADERS come from one table, static_rules.tsv beside this file: the first glob that
# matches the file's lower-cased path gives its Content-Type (.wasm is application/wasm). Its
# Cache-Control is html's no-cache, a content-hashed file's year (CBASE_HASHED's globs; never html),
# or no-cache.
#
# THE ORDER: the content-hashed files, then every other file but html, then the html, so a page never
# names a file that has not landed. Each phase is one `aws s3 cp --recursive` per pair of headers,
# over a scratch tree of links to the files, so every upload carries exactly its pair. Every file is
# sent every deploy: a rule change reaches every file, and nothing is compared.
#
# PRUNE (--prune, or CBASE_PRUNE=1) deletes what the app's folder holds and the build does not. It
# lists the bucket under "<app>/" alone and, before each delete, checks again that the key starts
# with "<app>/": a key outside the app's folder is never deleted, whatever a listing says. Without it
# nothing is deleted (a tab still open on the last build asks for the last build's files).

# static_bucket_find: STACK_BUCKET and STACK_BUCKET_REGION, the app's bucket: CBASE_STATIC_BUCKET
# (adopted, by its name: no group, no tag, no state file), or the static group's, by its tag; and
# STACK_PREFIX, the app's folder in it (CBASE_STATIC_PREFIX, or the app's name).
# shellcheck disable=SC2034 # stack.sh and tools/cbase read it
STACK_PREFIX=""
static_bucket_find() {
  if [ -n "$CONF_STATIC_BUCKET" ]; then STACK_BUCKET="$CONF_STATIC_BUCKET"
  else STACK_BUCKET="$(aws_group_find static "$CONF_STATIC")"; fi
  STACK_BUCKET_REGION="$(aws_call s3api get-bucket-location --bucket "$STACK_BUCKET" --query LocationConstraint --output text)" \
    || aws_fail "reading s3://$STACK_BUCKET's region"
  case "$STACK_BUCKET_REGION" in None|'') STACK_BUCKET_REGION=us-east-1 ;; EU) STACK_BUCKET_REGION=eu-west-1 ;; esac
  # shellcheck disable=SC2034 # stack.sh and tools/cbase read it
  STACK_PREFIX="${CONF_STATIC_PREFIX:-$CONF_APP}"
}

# static_policy_check BUCKET: an adopted bucket's policy lets the account's distributions read it,
# as a cbase static group's does (cloudfront.amazonaws.com on aws:SourceAccount). Deploy never
# writes a bucket policy: without that statement, the statement to add (beside the others, which
# stay) is printed, and the deploy stops before anything changes; a dry run says so and goes on.
static_policy_check() {
  local b="$1" pol
  if pol="$(aws_call s3api get-bucket-policy --bucket "$b" --query Policy --output text)"; then
    if [[ "$pol" == *cloudfront.amazonaws.com* && "$pol" == *aws:SourceAccount* && "$pol" == *"$AWS_ACCOUNT"* ]]; then
      echo "static bucket s3://$b: its policy lets the account's distributions read it"
      return 0
    fi
  else
    case "$(aws_code)" in NoSuchBucketPolicy) ;; *) aws_fail "reading s3://$b's policy" ;; esac
  fi
  cat >&2 << EOF
cbase: s3://$b's policy does not let this account's distributions read it, so the app's new distribution would answer 403.
  Add this statement to the policy's Statement list, keeping every statement it has (S3 console: the bucket, Permissions, Bucket policy, Edit):
    {"Sid": "TheAccountsDistributionsRead", "Effect": "Allow", "Principal": {"Service": "cloudfront.amazonaws.com"},
     "Action": ["s3:GetObject", "s3:ListBucket"], "Resource": ["arn:aws:s3:::$b", "arn:aws:s3:::$b/*"],
     "Condition": {"StringEquals": {"aws:SourceAccount": "$AWS_ACCOUNT"}}}
  It lets any CloudFront distribution of account $AWS_ACCOUNT read the bucket (a cbase static group's own statement), and removes nothing.
EOF
  if [ "${DRY_RUN:-}" = 1 ]; then echo "dry run: a deploy would stop here, on the bucket policy above; this dry run goes on"; return 0; fi
  echo "cbase: then deploy again; nothing was changed" >&2
  exit 1
}

ST_RULE_GLOB=()
ST_RULE_TYPE=()
ST_IMMUTABLE='public, max-age=31536000, immutable'
ST_NOCACHE='no-cache'
ST_FILES=""     # the scratch file listing the static dir's files, one relative path a line
ST_TREE=""      # the scratch tree of the upload groups
ST_G_PHASE=()   # each group's phase (1 hashed, 2 other, 3 html), Content-Type and Cache-Control
ST_G_TYPE=()
ST_G_CACHE=()
ST_G_COUNT=()

static_rules_load() {
  # shellcheck disable=SC2154 # here is tools/cbase's directory
  local f="$here/cbase.d/static_rules.tsv" g t extra
  ST_RULE_GLOB=()
  ST_RULE_TYPE=()
  while IFS=$'\t' read -r g t extra || [ -n "$g" ]; do
    case "$g" in ''|'#'*) continue ;; esac
    { [ -n "$t" ] && [ -z "$extra" ]; } || { echo "cbase: $f: the rule for '$g' is not two TAB-separated fields" >&2; exit 1; }
    ST_RULE_GLOB+=("$g")
    ST_RULE_TYPE+=("$t")
  done < "$f"
  [ "${ST_RULE_GLOB[${#ST_RULE_GLOB[@]}-1]}" = "*" ] || { echo "cbase: $f does not end with the fallback rule \"*\"" >&2; exit 1; }
}

# static_hashed PATH: PATH is under one of CBASE_HASHED's globs.
static_hashed() {
  local g
  for g in ${CONF_HASHED[@]+"${CONF_HASHED[@]}"}; do
    # shellcheck disable=SC2053 # the glob is a pattern
    [[ "$1" == $g ]] && return 0
  done
  return 1
}

# static_classify PATH: ST_TYPE and ST_CACHE, the file's Content-Type and Cache-Control.
ST_TYPE=""
ST_CACHE=""
static_classify() {
  local i
  ST_TYPE=""
  shopt -s nocasematch
  for ((i = 0; i < ${#ST_RULE_GLOB[@]}; i++)); do
    # shellcheck disable=SC2053 # the glob is a pattern
    if [[ "$1" == ${ST_RULE_GLOB[$i]} ]]; then ST_TYPE="${ST_RULE_TYPE[$i]}"; break; fi
  done
  shopt -u nocasematch
  ST_CACHE="$ST_NOCACHE"
  case "$ST_TYPE" in
    text/html*) ;;
    *) if static_hashed "$1"; then ST_CACHE="$ST_IMMUTABLE"; fi ;;
  esac
}

# static_scan DIR: ST_FILES lists DIR's files to upload, sorted. Stops on a dir that is not a build.
static_scan() {
  local dir="$1" f
  [ -d "$dir" ] || { echo "cbase: the static dir $dir does not exist: build the app first, or set CBASE_STATIC_DIR in cbase.conf (the directory with index.html)" >&2; exit 1; }
  [ -f "$dir/index.html" ] || { echo "cbase: $dir has no index.html, so it is not a built site: nothing was uploaded or deleted (CBASE_STATIC_DIR names the directory with index.html)" >&2; exit 1; }
  ST_FILES="$CB_TMP/static-files"
  : > "$ST_FILES"
  (cd "$dir" && find . -type f -print0) > "$CB_TMP/static-find" \
    || { echo "cbase: could not list every file under $dir; nothing was uploaded" >&2; exit 1; }
  while IFS= read -r -d '' f; do
    f="${f#./}"
    case "/$f" in
      */.well-known/*) ;;
      */.*) continue ;;
    esac
    if [[ "$f" == *[[:cntrl:]]* ]]; then
      echo "cbase: $dir holds a file whose name has a control character (a tab, a newline): rename it; nothing was uploaded" >&2
      exit 1
    fi
    printf '%s\n' "$f" >> "$ST_FILES"
  done < "$CB_TMP/static-find"
  LC_ALL=C sort -o "$ST_FILES" "$ST_FILES"
}

# static_plan DIR: the upload groups, each a scratch tree of links under ST_TREE/g<i>.
static_plan() {
  local dir="$1" f phase i g
  static_rules_load
  ST_TREE="$CB_TMP/static-tree"
  rm -rf "$ST_TREE"
  mkdir -p "$ST_TREE"
  ST_G_PHASE=(); ST_G_TYPE=(); ST_G_CACHE=(); ST_G_COUNT=()
  while IFS= read -r f; do
    static_classify "$f"
    if [ "$ST_CACHE" = "$ST_IMMUTABLE" ]; then phase=1
    else case "$ST_TYPE" in text/html*) phase=3 ;; *) phase=2 ;; esac; fi
    g=""
    for ((i = 0; i < ${#ST_G_PHASE[@]}; i++)); do
      if [ "${ST_G_PHASE[$i]}" = "$phase" ] && [ "${ST_G_TYPE[$i]}" = "$ST_TYPE" ] && [ "${ST_G_CACHE[$i]}" = "$ST_CACHE" ]; then g="$i"; break; fi
    done
    if [ -z "$g" ]; then
      g="${#ST_G_PHASE[@]}"
      ST_G_PHASE+=("$phase"); ST_G_TYPE+=("$ST_TYPE"); ST_G_CACHE+=("$ST_CACHE"); ST_G_COUNT+=(0)
    fi
    ST_G_COUNT[g]=$((ST_G_COUNT[g] + 1))
    mkdir -p "$ST_TREE/g$g/$(dirname "$f")"
    ln "$dir/$f" "$ST_TREE/g$g/$f" 2> /dev/null || cp -p "$dir/$f" "$ST_TREE/g$g/$f"
  done < "$ST_FILES"
}

# static_print_plan: the groups in upload order.
static_print_plan() {
  local phase i
  for phase in 1 2 3; do
    for ((i = 0; i < ${#ST_G_PHASE[@]}; i++)); do
      [ "${ST_G_PHASE[$i]}" = "$phase" ] || continue
      printf '  %4d file%s  %s  %s\n' "${ST_G_COUNT[$i]}" "$([ "${ST_G_COUNT[$i]}" = 1 ] && echo ' ' || echo s)" "${ST_G_TYPE[$i]}" "${ST_G_CACHE[$i]}"
    done
  done
}

# static_upload BUCKET REGION APP: the groups, phase by phase, into s3://BUCKET/APP/.
static_upload() {
  local bucket="$1" region="$2" app="$3" phase i n=0
  for phase in 1 2 3; do
    for ((i = 0; i < ${#ST_G_PHASE[@]}; i++)); do
      [ "${ST_G_PHASE[$i]}" = "$phase" ] || continue
      aws_call s3 cp "$ST_TREE/g$i" "s3://$bucket/$app/" --recursive --only-show-errors --region "$region" \
        --content-type "${ST_G_TYPE[$i]}" --cache-control "${ST_G_CACHE[$i]}" > /dev/null \
        || aws_fail "uploading to s3://$bucket/$app/"
      n=$((n + ST_G_COUNT[i]))
    done
  done
  echo "uploaded $n file$([ "$n" = 1 ] || echo s) to s3://$bucket/$app/ (html last)"
}

# static_prune BUCKET REGION APP: deletes the keys under APP/ that this build does not have.
static_prune() {
  local bucket="$1" region="$2" app="$3" k rel n=0 text keys=()
  # Two guards, for two things: --prefix keeps the listing to the folder (no other app's key is
  # read, and a big shared bucket is not walked); the case below holds whatever a listing says,
  # so no key outside the folder is ever deleted. cbase_test.sh fails without either.
  text="$(aws_call s3api list-objects-v2 --bucket "$bucket" --prefix "$app/" --region "$region" \
            --query 'Contents[].Key' --output text)" || aws_fail "listing s3://$bucket/$app/"
  # The CLI's text output: a page's keys on a line, tab-separated (no key holds a tab: static_scan
  # refuses one, and a key that does is passed by below as outside the folder).
  while IFS=$'\t' read -r -a keys; do
    for k in ${keys[@]+"${keys[@]}"}; do
      [ "$k" != None ] || continue
      # THE FOLDER GUARD: whatever the listing holds, only a key under "<app>/" is ever deleted.
      case "$k" in
        "$app"/?*) ;;
        *) echo "cbase: prune: s3://$bucket/$k is outside $app/, and is left alone" >&2; continue ;;
      esac
      rel="${k#"$app"/}"
      grep -Fxq -- "$rel" "$ST_FILES" && continue
      aws_call s3api delete-object --bucket "$bucket" --key "$k" --region "$region" > /dev/null \
        || aws_fail "deleting s3://$bucket/$k"
      echo "pruned s3://$bucket/$k"
      n=$((n + 1))
    done
  done <<< "$text"
  echo "pruned $n file$([ "$n" = 1 ] || echo s) the build no longer has, under s3://$bucket/$app/ alone"
}
