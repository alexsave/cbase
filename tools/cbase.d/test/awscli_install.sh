#!/bin/bash
# awscli_install.sh PREFIX PYTHON - AWS's own AWS CLI v2, built from source at the pinned tag and commit
# (awscli.pin), into PREFIX with PYTHON: AWS's documented source build (./configure, make, make install)
# with the system-sandbox install type, a virtual environment of its own under PREFIX/lib/aws-cli whose
# `aws` is a Python script, so the resident mode (lib.sh CBASE_TEST_AWS=resident) can load it.
# --with-download-deps fetches the Python dependencies AWS's build pins. A PREFIX that already holds the
# pinned version (a CI cache) is kept. Ends with PREFIX/bin/aws --version, and fails unless it is the pin.
set -euo pipefail
prefix="$1"; py="$2"
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=tools/cbase.d/test/awscli.pin
. "$here/awscli.pin"
want="aws-cli/$AWSCLI_VERSION "
if [ -x "$prefix/bin/aws" ] && { "$prefix/bin/aws" --version 2>&1 || true; } | grep -q "^$want"; then
  echo "awscli: $AWSCLI_VERSION already in $prefix"
else
  src="$(mktemp -d "${RUNNER_TEMP:-${TMPDIR:-/tmp}}/awscli-src.XXXXXX")"
  git -c advice.detachedHead=false clone -q --depth 1 --branch "$AWSCLI_VERSION" https://github.com/aws/aws-cli.git "$src"
  got="$(git -C "$src" rev-parse HEAD)"
  [ "$got" = "$AWSCLI_SHA" ] || { echo "awscli: tag $AWSCLI_VERSION is commit $got, not the pinned $AWSCLI_SHA" >&2; exit 1; }
  rm -rf "$prefix"
  (cd "$src" && PYTHON="$py" ./configure --prefix="$prefix" --with-install-type=system-sandbox --with-download-deps > "$src.configure.log" \
     && make > "$src.make.log" && make install > "$src.install.log") || { tail -40 "$src".*.log >&2; exit 1; }
  rm -rf "$src"
fi
rc=0; v="$("$prefix/bin/aws" --version 2>&1)" || rc=$?
echo "awscli: $v"
[ "$rc" = 0 ] || { echo "awscli: $prefix/bin/aws --version ended with status $rc" >&2; exit 1; }
case "$v" in "$want"*) ;; *) echo "awscli: $prefix/bin/aws is not the pinned $AWSCLI_VERSION" >&2; exit 1 ;; esac
