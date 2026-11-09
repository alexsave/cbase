#!/bin/bash
# site_deploy.sh - uploads a built static site to its stack (site.yaml): the files, the router's
# page list, one invalidation. `make deploy-web` (site.mk) runs it; it never changes the template.
#
#   SITE_STACK=... SITE_DIR=... site_deploy.sh [--prune] [--force]
#
# In order:
#   1. content-hashed files (SITE_HASHED), immutable for a year, never deleted here: a tab still
#      holding the old html asks for the old ones;
#   2. every other file but html;
#   3. the router's page list: the stack's Pages parameter, updated on the stack's PREVIOUS
#      template with every other parameter kept, and only when the list changed;
#   4. the html, last, so no page names a file that has not landed;
#   5. files the build no longer has are deleted, but content-hashed ones;
#   6. one invalidation of /* (one path, free), waited for;
#   7. with --prune, content-hashed files this build does not have are deleted too.
#      A tab open on an older build loses its files and reloads, so run it now and then.
# Each file's Content-Type and Cache-Control come from one table: SITE_RULES (the product's,
# read first) then site_rules.tsv beside this script. --force uploads every file again, for a
# change of rule (a sync skips a file whose size and time did not change).
#
# Settings, from the environment (site.mk's header says each): SITE_STACK, SITE_DIR,
# SITE_PREFIX, SITE_HASHED, SITE_RULES, SITE_SHELL, AWS_REGION (us-east-1).
# Credentials are the caller's: OIDC in CI (site_role.yaml), `aws login` on a Mac.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
die() { echo "site_deploy.sh: $*" >&2; exit 1; }
export AWS_REGION="${AWS_REGION:-${AWS_DEFAULT_REGION:-us-east-1}}"

prune=no; force=no
while [ $# -gt 0 ]; do
  case "$1" in
    --prune) prune=yes ;;
    --force) force=yes ;;
    *) sed -n '2,22p' "$0" | sed 's/^# \{0,1\}//' >&2; exit 2 ;;
  esac
  shift
done

stack="${SITE_STACK:-}"
[ -n "$stack" ] || die "set SITE_STACK (the site's stack name)"
[ -n "${SITE_DIR:-}" ] || die "set SITE_DIR (the built site's directory)"
[ -d "$SITE_DIR" ] || die "SITE_DIR $SITE_DIR is not a directory"
dir="$(cd "$SITE_DIR" && pwd)"
[ -f "$dir/index.html" ] || die "$dir has no index.html: not a built site, and nothing is uploaded or deleted"
prefix="${SITE_PREFIX:-}"
if [ -n "$prefix" ]; then
  printf '%s' "$prefix" | grep -Eq '^[A-Za-z0-9][A-Za-z0-9._-]*(/[A-Za-z0-9._-]+)*$' \
    || die "SITE_PREFIX '$prefix' is not a key prefix (A-Z a-z 0-9 . _ - /, no leading or trailing /)"
fi
shell="${SITE_SHELL:-}"
if [ -n "$shell" ] && [ ! -f "$dir$shell" ]; then die "$dir has no ${shell#/}, the shell page SITE_SHELL names"; fi
read -r -a hashed <<<"${SITE_HASHED:-}"
command -v aws >/dev/null 2>&1 || die "the AWS CLI is not installed"

# ---- the rules table ------------------------------------------------------------------------
R_GLOB=(); R_TYPE=(); R_CACHE=()
load_rules() {
  local g t c
  while IFS=$'\t' read -r g t c || [ -n "$g" ]; do
    case "$g" in ''|'#'*) continue ;; esac
    [ -n "$t" ] && [ -n "$c" ] || die "$1: the rule for '$g' needs three TAB-separated fields"
    case "$c" in
      revalidate) c='public, max-age=0, s-maxage=86400, must-revalidate' ;;
      immutable) c='public, max-age=31536000, immutable' ;;
    esac
    R_GLOB+=("$g"); R_TYPE+=("$t"); R_CACHE+=("$c")
  done < "$1"
}
if [ -n "${SITE_RULES:-}" ]; then
  [ -f "$SITE_RULES" ] || die "SITE_RULES $SITE_RULES is not a file"
  load_rules "$SITE_RULES"
fi
load_rules "$here/site_rules.tsv"
IMMUTABLE='public, max-age=31536000, immutable'

# is_hashed PATH: PATH is under a content-hashed glob.
is_hashed() {
  local g
  for g in ${hashed[@]+"${hashed[@]}"}; do
    # shellcheck disable=SC2053 # the glob is a pattern
    [[ $1 == $g ]] && return 0
  done
  return 1
}

# classify PATH: sets ct and cc, the file's Content-Type and Cache-Control.
classify() {
  local i
  for ((i = 0; i < ${#R_GLOB[@]}; i++)); do
    # shellcheck disable=SC2053 # the glob is a pattern
    if [[ $1 == ${R_GLOB[$i]} ]]; then ct="${R_TYPE[$i]}"; cc="${R_CACHE[$i]}"; break; fi
  done
  case "$ct" in text/html*) ;; *) if is_hashed "$1"; then cc="$IMMUTABLE"; fi ;; esac
}

# ---- the stack ------------------------------------------------------------------------------
output() {
  aws cloudformation describe-stacks --stack-name "$stack" \
    --query "Stacks[0].Outputs[?OutputKey=='$1'].OutputValue" --output text
}
bucket="$(output BucketName)"
dist="$(output DistributionId)"
{ [ -n "$bucket" ] && [ "$bucket" != None ] && [ -n "$dist" ] && [ "$dist" != None ]; } \
  || die "the $stack stack has no BucketName or DistributionId output: create it first (make deploy-web-stack)"
dest="s3://$bucket${prefix:+/$prefix}"
keypre="${prefix:+$prefix/}"

# ---- the files, grouped by phase and by header pair ----------------------------------------
t="$(mktemp -d)"
trap 'rm -rf "$t"' EXIT
(cd "$dir" && find . -type f | sed 's|^\./||' | LC_ALL=C sort) > "$t/local"
G_PHASE=(); G_TYPE=(); G_CACHE=()
group_of() {   # group_of PHASE CT CC: the group's index, made when new
  local i
  for ((i = 0; i < ${#G_PHASE[@]}; i++)); do
    if [ "${G_PHASE[$i]}" = "$1" ] && [ "${G_TYPE[$i]}" = "$2" ] && [ "${G_CACHE[$i]}" = "$3" ]; then echo "$i"; return; fi
  done
  echo "${#G_PHASE[@]}"
}
while IFS= read -r f; do
  ct=""; cc=""
  classify "$f"
  if [ "$cc" = "$IMMUTABLE" ]; then phase=1; else case "$ct" in text/html*) phase=3 ;; *) phase=2 ;; esac; fi
  g="$(group_of "$phase" "$ct" "$cc")"
  if [ "$g" = "${#G_PHASE[@]}" ]; then G_PHASE+=("$phase"); G_TYPE+=("$ct"); G_CACHE+=("$cc"); fi
  mkdir -p "$t/g$g/$(dirname "$f")"
  ln "$dir/$f" "$t/g$g/$f" 2>/dev/null || cp -p "$dir/$f" "$t/g$g/$f"
done < "$t/local"

upload_phase() {
  local i how=sync
  [ "$force" = no ] || how="cp"
  for ((i = 0; i < ${#G_PHASE[@]}; i++)); do
    [ "${G_PHASE[$i]}" = "$1" ] || continue
    if [ "$how" = sync ]; then
      aws s3 sync "$t/g$i" "$dest" --only-show-errors --content-type "${G_TYPE[$i]}" --cache-control "${G_CACHE[$i]}"
    else
      aws s3 cp "$t/g$i" "$dest" --recursive --only-show-errors --content-type "${G_TYPE[$i]}" --cache-control "${G_CACHE[$i]}"
    fi
  done
}

# ---- the router's page list -----------------------------------------------------------------
# Every html file outside the content-hashed globs, as its path without .html, sorted, so an
# unchanged build is an unchanged parameter.
page_list() {
  local f out=""
  while IFS= read -r f; do
    case "$f" in *.html) ;; *) continue ;; esac
    is_hashed "$f" && continue
    if ! printf '%s' "$f" | grep -Eq '^[A-Za-z0-9_./-]+$'; then
      echo "site_deploy.sh: $f is left out of the page list (a page name holds A-Z a-z 0-9 _ . - / only)" >&2
      continue
    fi
    out="$out ${f%.html}"
  done < "$t/local"
  printf '%s' "${out# }"
}

set_pages() {
  local pages="$1" current args=() k
  [ "${#pages}" -le 4096 ] || die "the page list is ${#pages} bytes, over a parameter's 4096: use SITE_SHELL-less routing (every path gets .html) or fewer pages"
  current="$(aws cloudformation describe-stacks --stack-name "$stack" \
    --query "Stacks[0].Parameters[?ParameterKey=='Pages'].ParameterValue" --output text)"
  [ "$current" != None ] || current=""
  if [ "$current" = "$pages" ]; then echo "router: pages unchanged"; return; fi
  for k in $(aws cloudformation describe-stacks --stack-name "$stack" --query 'Stacks[0].Parameters[].ParameterKey' --output text); do
    if [ "$k" = Pages ]; then args+=("ParameterKey=Pages,ParameterValue=$pages")
    else args+=("ParameterKey=$k,UsePreviousValue=true"); fi
  done
  echo "router: pages = $pages"
  aws cloudformation update-stack --stack-name "$stack" --use-previous-template --parameters "${args[@]}" >/dev/null
  aws cloudformation wait stack-update-complete --stack-name "$stack"
}

# ---- what the bucket holds that this build does not ----------------------------------------
# stale_keys hashed|other: the keys under the prefix with no file in this build, of one kind.
# A failed listing stops the deploy (set -e): it never reads as an empty bucket.
stale_keys() {
  local k rel
  aws s3api list-objects-v2 --bucket "$bucket" ${keypre:+--prefix "$keypre"} --query 'Contents[].Key' --output text \
    > "$t/remote"
  while IFS= read -r k; do
    [ -n "$k" ] && [ "$k" != None ] || continue
    rel="${k#"$keypre"}"
    if [ -n "$keypre" ] && [ "$rel" = "$k" ]; then continue; fi
    grep -Fxq -- "$rel" "$t/local" && continue
    if is_hashed "$rel"; then
      if [ "$1" = hashed ]; then echo "$k"; fi
    elif [ "$1" = other ]; then echo "$k"; fi
  done < <(tr '\t' '\n' < "$t/remote")
}
delete_keys() {   # delete_keys WHAT: the keys on stdin
  local k n=0
  while IFS= read -r k; do
    aws s3 rm "s3://$bucket/$k" --only-show-errors
    n=$((n + 1))
  done
  echo "deleted $n $1"
}

upload_phase 1
upload_phase 2
[ -z "$shell" ] || set_pages "$(page_list)"
upload_phase 3
stale_keys other > "$t/stale"
delete_keys "file(s) the build no longer has" < "$t/stale"
inv="$(aws cloudfront create-invalidation --distribution-id "$dist" --paths '/*' --query Invalidation.Id --output text)"
echo "invalidation $inv on $dist"
aws cloudfront wait invalidation-completed --distribution-id "$dist" --id "$inv"
if [ "$prune" = yes ]; then
  stale_keys hashed > "$t/stale"
  delete_keys "stale content-hashed file(s)" < "$t/stale"
fi
echo "deployed $dir to $stack ($dest)"
