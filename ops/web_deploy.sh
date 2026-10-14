#!/bin/bash
# Deploys the static web (ops/web.yaml) with the AWS CLI: the stack, and each
# site's files. The same script runs on this Mac and in .github/workflows/web_aws.yml.
#
#   c_base/ops/web_deploy.sh stack                  # create or update the stack (by hand: template changes)
#   c_base/ops/web_deploy.sh site foolish           # build foolish/, upload, route, invalidate
#   c_base/ops/web_deploy.sh site uttt              # the same for uttt/web
#   c_base/ops/web_deploy.sh site utils --out ../foolish-utils/out   # the sibling repo's export
#   c_base/ops/web_deploy.sh site foolish --out DIR # upload an already built export
#   c_base/ops/web_deploy.sh site foolish --prune-static   # and delete hashed files this build does not use
#
# A site deploy, in order (c_base/docs/VERCEL.md section 4d):
#   0. refuses a tree that does not contain origin/main (require_main below;
#      for utils, the foolish-utils tree that holds --out);
#   1. builds the export (npm run build: gen, wasm, next build), unless --out;
#   2. uploads _next/static/** with Cache-Control immutable for a year, without
#      deleting anything: a tab still holding the old HTML asks for old chunks;
#   3. uploads the *.gz wasm modules as application/octet-stream with NO
#      Content-Encoding (the client gunzips them itself), uttt's *.wasm as
#      application/wasm for an hour, and the association file as application/json;
#   4. writes the export's page list into the stack's router (an update of the
#      stack's parameters on its PREVIOUS template, so CI never applies a
#      template change), only when the list changed; utils has no list, since
#      its router gives every extensionless path .html;
#   5. uploads everything else (HTML, the RSC .txt payloads, public/) last,
#      with --delete, so no page points at a chunk that has not landed;
#   6. one invalidation of /* (one path, free).
#
# Settings, from the environment:
#   CBASE_WEB_STACK          cbase-web (the stack name)
#   CBASE_STACK              cbase (the API stack; `stack` reads its FunctionUrl for /m/*)
#   CBASE_WEB_FOOLISH_CERT   `stack`: the foolish.cards certificate ARN, once ISSUED
#   CBASE_WEB_UTTT_CERT      `stack`: the uttt.live certificate ARN, once ISSUED
#   CBASE_WEB_ISOLATION      `stack`: yes for the COOP/COEP headers (default: keep)
#   CBASE_WEB_GITHUB_REPO    `stack`: owner/name, to create the GitHub deploy role
#   CBASE_WEB_GITHUB_OIDC    `stack`: yes if this stack should create the OIDC provider
#   NEXT_PUBLIC_SUPABASE_URL, NEXT_PUBLIC_SUPABASE_KEY   building foolish: the backend
# A `stack` parameter left unset keeps the stack's current value.
set -euo pipefail

repo="$(cd "$(dirname "$0")/../.." && pwd)"
stack="${CBASE_WEB_STACK:-cbase-web}"
api_stack="${CBASE_STACK:-cbase}"
region="${AWS_REGION:-${AWS_DEFAULT_REGION:-us-east-1}}"
export AWS_REGION="$region"

die() { echo "web_deploy.sh: $*" >&2; exit 1; }
usage() { sed -n '2,11p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }
command -v aws >/dev/null 2>&1 || die "the AWS CLI is not installed"

output() {
  aws cloudformation describe-stacks --stack-name "$1" \
    --query "Stacks[0].Outputs[?OutputKey=='$2'].OutputValue" --output text
}
param() {
  aws cloudformation describe-stacks --stack-name "$stack" \
    --query "Stacks[0].Parameters[?ParameterKey=='$1'].ParameterValue" --output text
}

# ---- stack ----------------------------------------------------------------------------
deploy_stack() {
  local fn_url api_host
  fn_url="$(output "$api_stack" FunctionUrl)"
  if [ -z "$fn_url" ] || [ "$fn_url" = None ]; then die "the $api_stack stack has no FunctionUrl output"; fi
  api_host="${fn_url#https://}"
  api_host="${api_host%/}"
  local overrides=("ApiOriginDomain=$api_host")
  [ -n "${CBASE_WEB_FOOLISH_CERT:-}" ] && overrides+=("FoolishCertificateArn=$CBASE_WEB_FOOLISH_CERT")
  [ -n "${CBASE_WEB_UTTT_CERT:-}" ] && overrides+=("UtttCertificateArn=$CBASE_WEB_UTTT_CERT")
  [ -n "${CBASE_WEB_ISOLATION:-}" ] && overrides+=("CrossOriginIsolation=$CBASE_WEB_ISOLATION")
  [ -n "${CBASE_WEB_GITHUB_REPO:-}" ] && overrides+=("GitHubRepo=$CBASE_WEB_GITHUB_REPO")
  [ -n "${CBASE_WEB_GITHUB_OIDC:-}" ] && overrides+=("CreateGitHubOidcProvider=$CBASE_WEB_GITHUB_OIDC")
  # Unset parameters keep their values on an existing stack (the page lists
  # included, which only `site` writes).
  aws cloudformation deploy \
    --stack-name "$stack" \
    --template-file "$repo/c_base/ops/web.yaml" \
    --capabilities CAPABILITY_IAM CAPABILITY_NAMED_IAM \
    --no-fail-on-empty-changeset \
    --parameter-overrides "${overrides[@]}"
  aws cloudformation describe-stacks --stack-name "$stack" \
    --query 'Stacks[0].Outputs[].[OutputKey,OutputValue]' --output table
}

# ---- site -----------------------------------------------------------------------------
# The export's pages as the router's list: every .html outside _next, as its
# path without the extension, space-separated, sorted (so an unchanged build
# is an unchanged parameter).
page_list() {
  (cd "$1" && find . -name '*.html' -not -path './_next/*' | sed -e 's|^\./||' -e 's|\.html$||' | LC_ALL=C sort | tr '\n' ' ' | sed 's/ $//')
}

# Point the router at this build's pages: the stack's previous template, every
# parameter kept but this one.
set_pages() {
  local key="$1" pages="$2" current args=() k
  current="$(param "$key")"
  [ "$current" = None ] && current=""
  if [ "$current" = "$pages" ]; then
    echo "router: $key unchanged"
    return
  fi
  for k in $(aws cloudformation describe-stacks --stack-name "$stack" --query 'Stacks[0].Parameters[].ParameterKey' --output text); do
    if [ "$k" = "$key" ]; then
      args+=("ParameterKey=$k,ParameterValue=$pages")
    else
      args+=("ParameterKey=$k,UsePreviousValue=true")
    fi
  done
  echo "router: $key = $pages"
  aws cloudformation update-stack --stack-name "$stack" --use-previous-template \
    --capabilities CAPABILITY_IAM CAPABILITY_NAMED_IAM --parameters "${args[@]}" >/dev/null
  aws cloudformation wait stack-update-complete --stack-name "$stack"
}

# A site is deployed only from a tree that contains origin/main.
#
# WHY. On 2026-10-14 foolish.cards was deployed by hand from claude/c_base,
# which had branched before PR #287 (the kernel fix for "anim_build_plan error
# -2", the lobby that stays up after Ready) reached main. The site then served a
# web.wasm byte-identical to a c_base build and to no build of main, so a fix
# that was merged, tested and green was not live, and the defect it fixed was
# seen twice in production. Every build step here is a function of the tree; the
# one thing nothing checked was WHICH tree. A tree behind main ships whatever
# main has fixed since, unfixed, so this refuses it: merge main and deploy again.
#
# On a push to main in CI, HEAD is the pushed commit. If main has moved on since,
# the refusal is right: the newer push deploys. A shallow checkout is enough for
# that, because HEAD can only contain the fetched main by being it.
require_main() {
  local tree="$1" main head
  git -C "$tree" fetch --quiet origin main || die "cannot fetch origin main to check $tree against it"
  main="$(git -C "$tree" rev-parse FETCH_HEAD)"
  head="$(git -C "$tree" rev-parse HEAD)"
  if ! git -C "$tree" merge-base --is-ancestor "$main" "$head"; then
    die "this tree ($head) does not contain origin/main ($main): a deploy from it would ship code main has already replaced. Merge main and deploy again."
  fi
}

deploy_site() {
  local site="$1" out="$2" prune="$3" app pages_key dist_key
  case "$site" in
    foolish) app="$repo/foolish"; pages_key=FoolishPages; dist_key=FoolishDistributionId ;;
    uttt) app="$repo/uttt/web"; pages_key=UtttPages; dist_key=UtttDistributionId ;;
    # utils.foolish.cards: the sibling repo foolish-utils, served by the foolish
    # distribution under utils/. Built there (npm run build), so only --out.
    utils) app=""; pages_key=""; dist_key=FoolishDistributionId
           [ -n "$out" ] || die "utils is built in the foolish-utils repo: pass --out its out/ directory" ;;
    *) die "unknown site $site (foolish, uttt or utils)" ;;
  esac

  if [ -z "$out" ]; then
    if [ "$site" = foolish ]; then
      if [ -z "${NEXT_PUBLIC_SUPABASE_URL:-}" ] || [ -z "${NEXT_PUBLIC_SUPABASE_KEY:-}" ]; then
        die "building foolish needs NEXT_PUBLIC_SUPABASE_URL and NEXT_PUBLIC_SUPABASE_KEY (the backend the bundle talks to)"
      fi
    fi
    (cd "$app" && unset VERCEL && npm run build)
    out="$app/out"
  fi
  [ -f "$out/index.html" ] || die "$out is not a static export (no index.html)"
  if [ -n "$pages_key" ] && [ ! -f "$out/_shell.html" ]; then
    die "$out has no _shell.html, the page the router serves for game ids and codes"
  fi

  local bucket dist dest
  bucket="$(output "$stack" BucketName)"
  dist="$(output "$stack" "$dist_key")"
  dest="s3://$bucket/$site"
  local immutable='public, max-age=31536000, immutable'
  # Browsers revalidate every time; CloudFront keeps it until the invalidation
  # at the end of the next deploy (a day at most, if one is ever missed).
  local revalidate='public, max-age=0, s-maxage=86400, must-revalidate'
  local q=(--only-show-errors)

  # 2. hashed chunks, never deleted here
  if [ -d "$out/_next/static" ]; then
    aws s3 sync "$out/_next/static" "$dest/_next/static" "${q[@]}" \
      --exclude '*.gz' --cache-control "$immutable"
    # 3a. the bundled wasm (bots.wasm.gz, web.wasm.gz under media/): hashed, raw gzip bytes
    aws s3 sync "$out/_next/static" "$dest/_next/static" "${q[@]}" \
      --exclude '*' --include '*.gz' --content-type application/octet-stream --cache-control "$immutable"
  fi
  # 3b. public/*.wasm.gz (the oracle) and utils' *.geojson.gz: raw gzip bytes
  #     the page gunzips itself (DecompressionStream), revalidated
  aws s3 sync "$out" "$dest" "${q[@]}" --exclude '*' --include '*.gz' --exclude '_next/*' \
    --content-type application/octet-stream --cache-control "$revalidate"
  # 3c. uttt's kernels (and any utils wasm), an hour as on Vercel
  aws s3 sync "$out" "$dest" "${q[@]}" --exclude '*' --include '*.wasm' --exclude '_next/*' \
    --content-type application/wasm --cache-control 'public, max-age=3600'
  # 3d. the universal-links association file: JSON, at the apex, no extension
  if [ -f "$out/.well-known/apple-app-site-association" ]; then
    aws s3 cp "$out/.well-known/apple-app-site-association" "$dest/.well-known/apple-app-site-association" \
      "${q[@]}" --content-type application/json --cache-control "$revalidate"
  fi

  # 4. the router's page list (utils has none)
  [ -z "$pages_key" ] || set_pages "$pages_key" "$(page_list "$out")"

  # 5. everything else, last, and the only pass that deletes
  aws s3 sync "$out" "$dest" "${q[@]}" --delete \
    --exclude '_next/static/*' --exclude '*.gz' --exclude '*.wasm' --exclude '.well-known/apple-app-site-association' \
    --cache-control "$revalidate"

  # 6. one invalidation
  local inv
  inv="$(aws cloudfront create-invalidation --distribution-id "$dist" --paths '/*' --query Invalidation.Id --output text)"
  echo "invalidation $inv on $dist"
  aws cloudfront wait invalidation-completed --distribution-id "$dist" --id "$inv"

  if [ "$prune" = yes ]; then
    # Hashed files no page of THIS build names. A tab open on an older build
    # loses its chunks and reloads, so run it now and then, not every deploy.
    local keep stale
    keep="$(cd "$out" && find _next/static -type f | LC_ALL=C sort)"
    stale="$(aws s3api list-objects-v2 --bucket "$bucket" --prefix "$site/_next/static/" \
      --query 'Contents[].Key' --output text | tr '\t' '\n' | sed "s|^$site/||" | LC_ALL=C sort \
      | comm -23 - <(printf '%s\n' "$keep") | grep -v '^None$' || true)"
    if [ -n "$stale" ]; then
      printf '%s\n' "$stale" | while IFS= read -r k; do
        aws s3 rm "s3://$bucket/$site/$k" "${q[@]}"
      done
      echo "pruned $(printf '%s\n' "$stale" | wc -l | tr -d ' ') stale hashed files"
    fi
  fi
  echo "deployed $site to $(output "$stack" "${dist_key%Id}Domain")"
}

[ $# -ge 1 ] || usage
case "$1" in
  stack) deploy_stack ;;
  site)
    [ $# -ge 2 ] || usage
    site="$2"; shift 2
    out=""; prune=no
    while [ $# -gt 0 ]; do
      case "$1" in
        --out) [ $# -ge 2 ] || usage; out="$(cd "$2" && pwd)"; shift 2 ;;
        --prune-static) prune=yes; shift ;;
        *) usage ;;
      esac
    done
    if [ "$site" = utils ]; then
      [ -n "$out" ] || die "utils is built in the foolish-utils repo: pass --out its out/ directory"
      require_main "$out"
    else
      require_main "$repo"
    fi
    deploy_site "$site" "$out" "$prune" ;;
  -h|--help) usage ;;
  *) usage ;;
esac
