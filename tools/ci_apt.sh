#!/usr/bin/env bash
# ci_apt.sh PACKAGE... - installs Ubuntu packages on a CI machine, for every workflow step that needs them.
#
# GitHub's runner image lists Microsoft's package servers, and `apt-get update` fails the step when one of
# them answers 403. Every package asked for here is Ubuntu's, so those lists are removed first; both apt-get
# calls are then retried, as ci_llvm.sh retries its own (whose retry this is).
#
# APT_SOURCES is the directory of source lists (/etc/apt/sources.list.d).
set -euo pipefail
[ $# -gt 0 ] || { echo "usage: ci_apt.sh PACKAGE..." >&2; exit 2; }
packages=("$@")
set --
# shellcheck source=ci_llvm.sh
CI_LLVM_DEFINE_ONLY=1 . "$(dirname "${BASH_SOURCE[0]}")/ci_llvm.sh"

SUDO="$(command -v sudo || true)"
APT_SOURCES="${APT_SOURCES:-/etc/apt/sources.list.d}"
for list in "$APT_SOURCES"/*; do
    if [ -f "$list" ] && grep -q 'packages\.microsoft\.com' "$list"; then $SUDO rm -f "$list"; fi
done
retry 4 $SUDO apt-get update -q
retry 4 $SUDO apt-get install -y -q "${packages[@]}"
