# shellcheck shell=bash
# Sourced, not run: require_main TREE [INPUT_PATH...] refuses (exits 1) unless
# TREE contains origin/main, or ends a superseded CI run as skipped (exits 0,
# SUPERSEDED below). web_deploy.sh and lambda_deploy.sh call it before their first
# AWS call; test/web_deploy_gate_test.sh proves it for both.
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
# SUPERSEDED. On a push to main in CI, HEAD is the pushed commit, and main can
# move on while the run builds (the workflow queues the newer push's run behind
# this one). HEAD is then an older commit OF main, not a branch behind it. If
# what main changed since touches any of the deploy's INPUTS (the paths its
# workflow's push filter names, passed after TREE), that newer push started a
# run of its own which deploys main's tip, so this run ends as skipped: exit 0
# with a GitHub ::notice::, no AWS call. Red here was a false alarm, and a
# deploy of the older tree would briefly roll main's newer change back. If main
# changed none of the inputs, no newer run exists and HEAD's build IS main's:
# it deploys. The skip is CI only (GITHUB_ACTIONS=true) and needs the inputs
# named; by hand, an old commit of main is refused like any tree behind main,
# because nobody else is about to deploy the newer one.
#
# A superseded run is not cancelled instead, because the cancel would land
# mid-way through a CloudFormation update as often as not; a skip decided
# before the first AWS call never does.
# A shallow checkout (actions/checkout's default) is deepened first: on any
# other ref, HEAD contains main through history the clone does not have, and
# the ancestry test would refuse a tree that is fine.
require_main() {
  local tree="$1" main head
  shift
  if [ "$(git -C "$tree" rev-parse --is-shallow-repository)" = true ]; then
    git -C "$tree" fetch --quiet --unshallow origin || { echo "cannot unshallow $tree to check it against origin/main" >&2; exit 1; }
  fi
  git -C "$tree" fetch --quiet origin main || { echo "cannot fetch origin main to check $tree against it" >&2; exit 1; }
  main="$(git -C "$tree" rev-parse FETCH_HEAD)"
  head="$(git -C "$tree" rev-parse HEAD)"
  git -C "$tree" merge-base --is-ancestor "$main" "$head" && return 0
  if [ "${GITHUB_ACTIONS:-}" = true ] && [ $# -gt 0 ] && git -C "$tree" merge-base --is-ancestor "$head" "$main"; then
    if git -C "$tree" diff --quiet "$head" "$main" -- "$@"; then
      echo "this commit ($head) is behind origin/main ($main), but main has changed none of this deploy's inputs since: deploying it."
      return 0
    fi
    echo "::notice title=deploy skipped::superseded: origin/main moved on to $main, which changes this deploy's inputs, and that push's own run deploys it. Nothing from $head was deployed."
    exit 0
  fi
  echo "this tree ($head) does not contain origin/main ($main): a deploy from it would ship code main has already replaced. Merge main and deploy again." >&2
  exit 1
}
