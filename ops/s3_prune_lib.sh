#!/bin/bash
# Sourced by the deploy scripts: prunes the code bucket's old releases of ONE
# kind after a new release is confirmed live.
#
#   prune_plan KIND LIVE [PREV]    stdin: bucket listing, one "key" or "key<TAB>size" per line
#                                  stdout: the lines to delete. No AWS call.
#   prune_bucket KIND BUCKET LIVE [PREV]
#                                  lists, plans, then deletes (or only prints, when
#                                  CBASE_PRUNE_DRY_RUN=1). Prints counts and bytes.
#
# KIND is the only thing the plan may touch:
#   vm      vm/cbase-serve-<version> and vm/cbase-serve-<version>.sha256
#   boot    vm/cbase-vm-boot-<sha12> (no .sha256: the pointer holds the sha256)
#   lambda  cbase-lambda-<name>.zip
# Anything else (templates/, vm/amazon-ssm-agent-*.rpm, other prefixes) is never
# planned for deletion, whatever the keep list says.
#
# Fail safe:
#   - LIVE comes from the caller, which read it from the source of truth (the SSM
#     release parameter, the stack's CodeKey); it is never inferred from the
#     listing's timestamps. An empty LIVE, a LIVE of another kind, or a LIVE the
#     listing does not contain (wrong bucket, a short listing), plans nothing and
#     returns 1.
#   - Kept: LIVE and PREV (the release LIVE replaced: the one-step rollback) and,
#     for vm, each one's .sha256. The kinds are disjoint: a vm plan never lists a
#     boot object, so the live boot script survives any serve prune, and the
#     reverse.
#   - A failed list or delete returns 1. Callers warn and carry on: a deploy that
#     already went live never fails because the cleanup did.

# The key pattern of a kind, as an awk regex.
_prune_pattern() {
  case "$1" in
    vm) echo '^vm/cbase-serve-[^/]+$' ;;
    boot) echo '^vm/cbase-vm-boot-[^/]+$' ;;
    lambda) echo '^cbase-lambda-[^/]+[.]zip$' ;;
    *) return 1 ;;
  esac
}

# The prefix to list, so a listing never reads the rest of the bucket.
_prune_prefix() {
  case "$1" in
    vm) echo 'vm/cbase-serve-' ;;
    boot) echo 'vm/cbase-vm-boot-' ;;
    lambda) echo 'cbase-lambda-' ;;
    *) return 1 ;;
  esac
}

prune_plan() {
  local kind="$1" live="${2:-}" prev="${3:-}" pat keep listing out
  pat="$(_prune_pattern "$kind")" || { echo "prune_plan: unknown kind '$kind'" >&2; return 1; }
  [ -n "$live" ] || { echo "prune_plan: no live key; nothing is pruned" >&2; return 1; }
  # A live key of another kind (a serve key given to the boot plan) would keep nothing of this kind.
  printf '%s\n' "$live" | grep -Eq "$pat" || { echo "prune_plan: the live key $live is not a $kind key; nothing is pruned" >&2; return 1; }
  # Keys hold no space: the keep list is space-separated (BSD awk takes no newline in -v).
  keep="$live $prev"
  if [ "$kind" = vm ]; then keep="$keep $live.sha256"; [ -z "$prev" ] || keep="$keep $prev.sha256"; fi
  listing="$(cat)"
  # shellcheck disable=SC2016 # awk program
  out="$(printf '%s\n' "$listing" | awk -F'\t' -v pat="$pat" -v keep="$keep" -v live="$live" '
    BEGIN { n = split(keep, ks, " "); for (i = 1; i <= n; i++) if (ks[i] != "") kept[ks[i]] = 1 }
    $1 == "" { next }
    $1 == live { seen = 1 }
    $1 ~ pat && !($1 in kept) { print }
    END { if (!seen) { print "prune_plan: the live key " live " is not in the listing; nothing is pruned" > "/dev/stderr"; exit 3 } }
  ')" || return 1
  [ -z "$out" ] || printf '%s\n' "$out"
}

prune_bucket() {
  local kind="$1" bucket="$2" live="$3" prev="${4:-}" prefix listing plan n bytes key
  prefix="$(_prune_prefix "$kind")" || { echo "prune: unknown kind '$kind'" >&2; return 1; }
  [ -n "$live" ] || { echo "prune: the live $kind key is unknown; nothing is pruned" >&2; return 1; }
  listing="$(aws s3api list-objects-v2 --bucket "$bucket" --prefix "$prefix" \
             --query 'Contents[].[Key,Size]' --output text)" || { echo "prune: cannot list s3://$bucket/$prefix; nothing is pruned" >&2; return 1; }
  [ "$listing" != None ] || listing=""
  plan="$(printf '%s\n' "$listing" | prune_plan "$kind" "$live" "$prev")" || return 1
  n=0; bytes=0
  if [ -n "$plan" ]; then
    n="$(printf '%s\n' "$plan" | wc -l | tr -d ' ')"
    bytes="$(printf '%s\n' "$plan" | awk -F'\t' '{ s += $2 } END { print s + 0 }')"
  fi
  echo "prune $kind: keeping $live${prev:+ and $prev}; $n old object(s), $bytes bytes"
  [ "$n" -gt 0 ] || return 0
  if [ "${CBASE_PRUNE_DRY_RUN:-}" = 1 ]; then
    printf '%s\n' "$plan" | awk -F'\t' '{ print "would delete " $1 " (" $2 " bytes)" }'
    return 0
  fi
  while IFS=$'\t' read -r key _; do
    aws s3api delete-object --bucket "$bucket" --key "$key" >/dev/null || { echo "prune: cannot delete $key; stopping" >&2; return 1; }
  done <<<"$plan"
  echo "prune $kind: deleted $n object(s), $bytes bytes"
}
