# shellcheck shell=bash
# Sourced, not run: require_main TREE refuses (exits 1) unless TREE contains
# origin/main, or is the first attempt of a CI run of a commit of main (QUEUED
# below). web_deploy.sh and lambda_deploy.sh call it before their first AWS
# call; test/web_deploy_gate_test.sh proves it for both.
#
# WHY. On 2026-10-14 foolish.cards was deployed by hand from claude/c_base,
# which had branched before PR #287 (the kernel fix for "anim_build_plan error
# -2", the lobby that stays up after Ready) reached main. The site then served a
# web.wasm byte-identical to a c_base build and to no build of main, so a fix
# that was merged, tested and green was not live, and the defect it fixed was
# seen twice in production. Every build step is a function of the tree; the one
# thing nothing checked was WHICH tree. A tree behind main ships whatever main
# has fixed since, unfixed, so this refuses it: merge main and deploy again.
#
# QUEUED. In CI, a run on main deploys its own commit even when main has moved
# on while it built. The deploying workflows (c_base.yml, web_aws.yml) hold one
# workflow-level concurrency group per ref with cancel-in-progress false on
# main: one run at a time, and the one waiting slot always holds the newest
# push (GitHub cancels an older waiting run when a newer one arrives). So a
# newer commit of main that changes this deploy's inputs already has a run
# waiting behind this one, and it deploys main's tip right after; a newer
# commit that changes none of them builds the same bytes. Either way deploying
# HEAD is correct, and the newest commit always deploys.
#
# This replaced a SUPERSEDED rule that ended such a run skipped. It starved:
# from 01:25 to 03:05 UTC on 2026-10-18, c_base runs took about 20 minutes and
# main moved on inside every one of them, so every run that reached its deploy
# job found itself superseded and skipped, and the run behind it then did the
# same. Nothing shipped until the pushes paused.
#
# The exception is the first attempt only (GITHUB_RUN_ATTEMPT=1). A re-run of
# an old run is not queued behind anything newer: deploying it would roll main
# back, so it is refused like a tree behind main by hand. That is a different
# vector from the queue (a person pressing Re-run, not a push), which is why
# the attempt is checked here and the queue is the workflow's concurrency.
#
# A shallow checkout (actions/checkout's default) is deepened first: on any
# other ref, HEAD contains main through history the clone does not have, and
# the ancestry test would refuse a tree that is fine.
require_main() {
  local tree="$1" main head
  if [ "$(git -C "$tree" rev-parse --is-shallow-repository)" = true ]; then
    git -C "$tree" fetch --quiet --unshallow origin || { echo "cannot unshallow $tree to check it against origin/main" >&2; exit 1; }
  fi
  git -C "$tree" fetch --quiet origin main || { echo "cannot fetch origin main to check $tree against it" >&2; exit 1; }
  main="$(git -C "$tree" rev-parse FETCH_HEAD)"
  head="$(git -C "$tree" rev-parse HEAD)"
  git -C "$tree" merge-base --is-ancestor "$main" "$head" && return 0
  if [ "${GITHUB_ACTIONS:-}" = true ] && [ "${GITHUB_REF:-}" = refs/heads/main ] \
     && [ "${GITHUB_RUN_ATTEMPT:-}" = 1 ] && git -C "$tree" merge-base --is-ancestor "$head" "$main"; then
    echo "this commit ($head) is behind origin/main ($main): deploying it, and the run queued behind this one deploys main's tip."
    return 0
  fi
  echo "this tree ($head) does not contain origin/main ($main): a deploy from it would ship code main has already replaced. Merge main and deploy again." >&2
  exit 1
}
