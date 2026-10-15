# shellcheck shell=bash
# Sourced, not run: require_main TREE refuses (exits) unless TREE contains
# origin/main. web_deploy.sh and lambda_deploy.sh call it before their first
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
# On a push to main in CI, HEAD is the pushed commit. If main has moved on since
# (a re-run of an old push), the refusal is right: the newer push deploys.
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
  if ! git -C "$tree" merge-base --is-ancestor "$main" "$head"; then
    echo "this tree ($head) does not contain origin/main ($main): a deploy from it would ship code main has already replaced. Merge main and deploy again." >&2
    exit 1
  fi
}
