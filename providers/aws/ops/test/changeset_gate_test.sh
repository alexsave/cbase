#!/bin/bash
# deploy.sh's replacement gate (providers/aws/ops/changeset_replacements.jq) against
# recorded change sets: a code deploy's "Conditional" FunctionUrl on
# Function.Arn passes; a real replacement, a conditional on a property, and a
# conditional on an attribute of a replaced resource each stop it.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
fail() { echo "changeset_gate_test: $*" >&2; exit 1; }
gate() { jq -r -f "$here/changeset_replacements.jq" "$here/test/changesets/$1.json"; }
got="$(gate code_only)"
[ -z "$got" ] || fail "code_only: expected nothing to be replaced, got '$got'"
got="$(gate mixed)"
[ "$got" = "Other Dep Big" ] || fail "mixed: expected 'Other Dep Big', got '$got'"
echo "changeset_gate_test: ok"
