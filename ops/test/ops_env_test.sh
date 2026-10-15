#!/bin/bash
# ops_env_test.sh - ops/ops_env.sh: which settings file a run reads, what it
# reads from it, and that a secret is generated only for a stack that does not
# exist yet. No AWS calls.
set -u
here="$(cd "$(dirname "$0")/../.." && pwd)"
# shellcheck source=../ops_env.sh
. "$here/ops/ops_env.sh"
fails=0
check() { if [ "$2" = "$3" ]; then echo "ok   $1"; else echo "FAIL $1: got [$2] want [$3]"; fails=$((fails + 1)); fi; }

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
fake_c_base="$tmp/c_base"
mkdir -p "$fake_c_base/ops" "$tmp/home"
export HOME="$tmp/home"
unset CBASE_OPS_ENV CBASE_JWT_SECRET CBASE_ADMIN_TOKEN CBASE_X OTHER_SETTING

# Which file: the home file when neither exists (a first deploy writes there).
check "default is the home file" "$(cbase_ops_env_file "$fake_c_base")" "$HOME/.config/cbase/ops.env"
# The old ops/.env is still read when it is the only one.
: > "$fake_c_base/ops/.env"
check "an old ops/.env alone is read" "$(cbase_ops_env_file "$fake_c_base")" "$fake_c_base/ops/.env"
# The home file wins over it once it exists.
mkdir -p "$HOME/.config/cbase" && : > "$HOME/.config/cbase/ops.env"
check "the home file wins" "$(cbase_ops_env_file "$fake_c_base")" "$HOME/.config/cbase/ops.env"
# CBASE_OPS_ENV wins over both.
check "CBASE_OPS_ENV wins" "$(CBASE_OPS_ENV="$tmp/x.env" cbase_ops_env_file "$fake_c_base")" "$tmp/x.env"

# Reading: CBASE_* only, comments skipped, the environment wins.
f="$tmp/read.env"
printf '%s\n' '# a comment' 'CBASE_X=from-file' 'PATH=/nope' 'OTHER_SETTING=no' 'CBASE_JWT_SECRET=s3cret' > "$f"
export CBASE_X=from-env
cbase_ops_read "$f"
check "the environment wins" "$CBASE_X" "from-env"
check "a CBASE_ value is read" "${CBASE_JWT_SECRET:-}" "s3cret"
check "only CBASE_ names are read" "${OTHER_SETTING:-unset}" "unset"
case "$PATH" in /nope) check "PATH untouched" bad good ;; *) check "PATH untouched" good good ;; esac
unset CBASE_X CBASE_JWT_SECRET

# A live stack: a missing secret stops the run and the file is not touched.
f="$tmp/live.env"
printf 'CBASE_ADMIN_TOKEN=keep\n' > "$f"
before="$(cat "$f")"
cbase_ops_secret CBASE_JWT_SECRET "$f" yes 2>/dev/null
check "a live stack refuses to generate" "$?" "1"
check "the file is untouched" "$(cat "$f")" "$before"
check "no secret was set" "${CBASE_JWT_SECRET:-}" ""
# A secret that is there is used as it is, live or not.
CBASE_ADMIN_TOKEN=keep cbase_ops_secret CBASE_ADMIN_TOKEN "$f" yes
check "a present secret passes" "$?" "0"

# A new stack: generated into the file, 64 hex digits, mode 0600.
f="$tmp/new/ops.env"
cbase_ops_secret CBASE_JWT_SECRET "$f" no >/dev/null
check "a new stack generates" "$(grep -c '^CBASE_JWT_SECRET=[0-9a-f]\{64\}$' "$f")" "1"
check "the file is 0600" "$(stat -f %Lp "$f" 2>/dev/null || stat -c %a "$f")" "600"

if [ "$fails" -eq 0 ]; then echo "ops_env: all pass"; else echo "ops_env: $fails failed"; exit 1; fi
