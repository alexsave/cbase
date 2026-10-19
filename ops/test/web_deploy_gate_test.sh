#!/bin/bash
# web_deploy.sh, vm_release.sh and lambda_deploy.sh refuse to deploy from a tree behind
# origin/main (require_main.sh), and let a tree that contains main through. In
# CI, the first attempt of a run of an older commit of main deploys (the newer
# push's run is queued behind it), and a re-run of one is refused. No
# AWS: `aws` is a stub that records any call, and the gate has to fire before
# the first one.
#
# Run: bash c_base/ops/test/web_deploy_gate_test.sh
set -euo pipefail

ops="$(cd "$(dirname "$0")/.." && pwd)"
aws_ops="$ops/../providers/aws/ops"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

fail() { echo "FAIL: $*" >&2; exit 1; }

# The stub: on PATH so `command -v aws` passes, and any call is a failure the
# test can see.
mkdir -p "$tmp/bin"
printf '#!/bin/bash\necho "aws $*" >> "%s/aws_calls"\nexit 99\n' "$tmp" > "$tmp/bin/aws"
chmod +x "$tmp/bin/aws"
export PATH="$tmp/bin:$PATH"
# By hand unless a case says otherwise (this test itself runs in CI).
unset GITHUB_ACTIONS
# The ops settings file: an empty one of the test's own, never this Mac's.
export CBASE_OPS_ENV="$tmp/ops.env"

g() { git -c user.name=t -c user.email=t@t -c init.defaultBranch=main "$@"; }

# origin with main at A; a clone at A; then main moves to B on origin.
g init -q --bare "$tmp/origin.git"
g clone -q "$tmp/origin.git" "$tmp/seed" 2>/dev/null
mkdir -p "$tmp/seed/c_base/ops" "$tmp/seed/c_base/providers/aws/ops/site" "$tmp/seed/c_base/providers/local/ops"
cp "$ops/require_main.sh" "$ops/ops_env.sh" "$tmp/seed/c_base/ops/"
cp "$aws_ops/lambda_deploy.sh" "$tmp/seed/c_base/providers/aws/ops/"
cp "$aws_ops/site/web_deploy.sh" "$tmp/seed/c_base/providers/aws/ops/site/"
cp "$ops/../providers/local/ops/vm_release.sh" "$tmp/seed/c_base/providers/local/ops/"
g -C "$tmp/seed" add -A
g -C "$tmp/seed" commit -q -m A
g -C "$tmp/seed" push -q origin HEAD:main
g clone -q "$tmp/origin.git" "$tmp/work"
echo fix > "$tmp/seed/fix"
g -C "$tmp/seed" add -A
g -C "$tmp/seed" commit -q -m B
g -C "$tmp/seed" push -q origin HEAD:main

run() {
  # NEXT_PUBLIC_BACKEND_URL unset on purpose: a tree the gate lets through then
  # stops at deploy_site's first check, still before any AWS call.
  env -u NEXT_PUBLIC_BACKEND_URL \
    bash "$tmp/work/c_base/providers/aws/ops/site/web_deploy.sh" site foolish > "$tmp/out" 2>&1 && return 0 || return $?
}

run_lambda() {
  echo zip > "$tmp/fake.zip"
  bash "$tmp/work/c_base/providers/aws/ops/lambda_deploy.sh" "$tmp/fake.zip" > "$tmp/out" 2>&1 && return 0 || return $?
}

run_vm() {
  echo bin > "$tmp/fake.bin"
  bash "$tmp/work/c_base/providers/local/ops/vm_release.sh" "$tmp/fake.bin" > "$tmp/out" 2>&1 && return 0 || return $?
}

# 1. Behind main: refused, by the gate, with no AWS call.
if run; then fail "a tree behind origin/main deployed"; fi
grep -q 'does not contain origin/main' "$tmp/out" || { cat "$tmp/out" >&2; fail "behind main: refused, but not by require_main"; }
[ ! -e "$tmp/aws_calls" ] || fail "behind main: aws was called before the refusal: $(cat "$tmp/aws_calls")"
echo "ok - a tree behind origin/main is refused before any AWS call"

if run_lambda; then fail "lambda: a tree behind origin/main deployed"; fi
grep -q 'does not contain origin/main' "$tmp/out" || { cat "$tmp/out" >&2; fail "lambda, behind main: refused, but not by require_main"; }
[ ! -e "$tmp/aws_calls" ] || fail "lambda, behind main: aws was called before the refusal: $(cat "$tmp/aws_calls")"
echo "ok - lambda: a tree behind origin/main is refused before any AWS call"

if run_vm; then fail "vm: a tree behind origin/main deployed"; fi
grep -q 'does not contain origin/main' "$tmp/out" || { cat "$tmp/out" >&2; fail "vm, behind main: refused, but not by require_main"; }
[ ! -e "$tmp/aws_calls" ] || fail "vm, behind main: aws was called before the refusal: $(cat "$tmp/aws_calls")"
echo "ok - vm: a tree behind origin/main is refused before any AWS call"

# 1b. In CI, on the first attempt of a run on main, the same tree is an older
#     commit OF main whose newer push's run is queued behind this one (the
#     workflow's concurrency group): it deploys, whatever main changed since.
#     Main first moves on with a change to an input of both deploys (shared/c/),
#     the case the old SUPERSEDED rule skipped and so starved.
mkdir -p "$tmp/seed/shared/c"
echo input > "$tmp/seed/shared/c/x"
g -C "$tmp/seed" add -A
g -C "$tmp/seed" commit -q -m B2
g -C "$tmp/seed" push -q origin HEAD:main
ci() { GITHUB_ACTIONS=true GITHUB_REF=refs/heads/main GITHUB_RUN_ATTEMPT="${attempt:-1}" "$@"; }
if ci run; then fail "ci, queued: expected the NEXT_PUBLIC_* check to stop the deploy"; fi
grep -q 'the run queued behind this one deploys' "$tmp/out" || { cat "$tmp/out" >&2; fail "ci, queued: web did not get past the gate"; }
if ci run_lambda; then fail "ci, queued: expected the stub aws to stop the lambda deploy"; fi
grep -q '^aws cloudformation describe-stacks' "$tmp/aws_calls" 2>/dev/null || { cat "$tmp/out" >&2; fail "ci, queued: lambda never reached the stack"; }
rm -f "$tmp/aws_calls"
echo "ok - ci: an older commit of main deploys on its run's first attempt"

# 1c. A re-run of that old run (attempt 2) is queued behind nothing newer, and
#     would roll main back: refused, before any AWS call.
if attempt=2 ci run_lambda; then fail "ci, re-run: lambda did not refuse"; fi
grep -q 'does not contain origin/main' "$tmp/out" || { cat "$tmp/out" >&2; fail "ci, re-run: refused, but not by require_main"; }
if attempt=2 ci run; then fail "ci, re-run: web did not refuse"; fi
grep -q 'does not contain origin/main' "$tmp/out" || { cat "$tmp/out" >&2; fail "ci, re-run: web refused, but not by require_main"; }
[ ! -e "$tmp/aws_calls" ] || fail "ci, re-run: aws was called: $(cat "$tmp/aws_calls")"
echo "ok - ci: a re-run of an old run of main is refused before any AWS call"

# 1d. By hand, the same older commit is still refused: nobody is queued to
#     deploy main's tip after it.
if run_lambda; then fail "by hand, behind: lambda did not refuse"; fi
grep -q 'does not contain origin/main' "$tmp/out" || { cat "$tmp/out" >&2; fail "by hand, behind: refused, but not by require_main"; }
echo "ok - by hand: an older commit of main is refused"

# 1e. In CI, a branch behind main (not a commit of main) is refused: the
#     queue only orders main's own pushes.
echo branch > "$tmp/work/diverged"
g -C "$tmp/work" add -A
g -C "$tmp/work" commit -q -m D
if ci run_lambda; then fail "ci, diverged: a branch behind main did not refuse"; fi
grep -q 'does not contain origin/main' "$tmp/out" || { cat "$tmp/out" >&2; fail "ci, diverged: refused, but not by require_main"; }
[ ! -e "$tmp/aws_calls" ] || fail "ci, diverged: aws was called: $(cat "$tmp/aws_calls")"
g -C "$tmp/work" reset -q --hard HEAD~1
echo "ok - ci: a branch behind main is refused, not skipped"

# 2. Main merged in (a branch of its own on top of main): through the gate.
g -C "$tmp/work" pull -q --no-rebase origin main
echo branch > "$tmp/work/branch_only"
g -C "$tmp/work" add -A
g -C "$tmp/work" commit -q -m C
if run; then fail "expected the NEXT_PUBLIC_* check to stop the deploy"; fi
if grep -q 'does not contain origin/main' "$tmp/out"; then cat "$tmp/out" >&2; fail "a tree containing main was refused"; fi
grep -q 'NEXT_PUBLIC_BACKEND_URL' "$tmp/out" || { cat "$tmp/out" >&2; fail "contains main: did not reach deploy_site"; }
[ ! -e "$tmp/aws_calls" ] || fail "contains main: unexpected aws call: $(cat "$tmp/aws_calls")"
echo "ok - a tree containing origin/main gets past the gate"

# The Lambda gets past the gate to its first AWS call (the stub fails it there).
if run_lambda; then fail "lambda: expected the stub aws to stop the deploy"; fi
if grep -q 'does not contain origin/main' "$tmp/out"; then cat "$tmp/out" >&2; fail "lambda: a tree containing main was refused"; fi
grep -q '^aws cloudformation describe-stacks' "$tmp/aws_calls" 2>/dev/null || { cat "$tmp/out" >&2; fail "lambda, contains main: never reached the stack"; }
echo "ok - lambda: a tree containing origin/main gets past the gate"

# 3. A depth-1 clone of a branch that contains main (actions/checkout's default
#    on a workflow_dispatch from a branch): main is in history the clone does
#    not have, and the gate must deepen it rather than refuse.
g -C "$tmp/work" push -q origin HEAD:refs/heads/feature
rm -f "$tmp/aws_calls"
g clone -q --depth 1 --branch feature "file://$tmp/origin.git" "$tmp/shallow" 2>/dev/null
[ "$(git -C "$tmp/shallow" rev-parse --is-shallow-repository)" = true ] || fail "the test clone is not shallow"
echo zip > "$tmp/fake.zip"
if bash "$tmp/shallow/c_base/providers/aws/ops/lambda_deploy.sh" "$tmp/fake.zip" > "$tmp/out" 2>&1; then fail "shallow: expected the stub aws to stop the deploy"; fi
if grep -q 'does not contain origin/main' "$tmp/out"; then cat "$tmp/out" >&2; fail "shallow: a shallow clone of a branch containing main was refused"; fi
grep -q '^aws cloudformation describe-stacks' "$tmp/aws_calls" 2>/dev/null || { cat "$tmp/out" >&2; fail "shallow: never reached the stack"; }
echo "ok - a shallow clone of a branch containing origin/main gets past the gate"
