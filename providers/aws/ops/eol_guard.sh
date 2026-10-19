#!/bin/bash
# eol_guard.sh BINARY : fail when the shipped build's toolchain is past its
# end of support. Checks (1) the Alpine minor pinned in build_bootstrap.sh and
# (2) the OpenSSL line compiled into BINARY (its `OpenSSL x.y.z` string),
# each against the dated tables below, taken from
# https://alpinelinux.org/releases/ and
# https://openssl-library.org/policies/releasestrat/ (checked 2026-10-19).
# A line missing from a table fails too, so a new line must be added on purpose.
# GUARD_TODAY=YYYY-MM-DD overrides the date (for the test).
set -euo pipefail
bin="${1:?usage: eol_guard.sh BINARY}"
here="$(cd "$(dirname "$0")" && pwd)"
today="${GUARD_TODAY:-$(date -u +%F)}"

# End of support, inclusive: the last day the line is supported.
ALPINE_EOL="3.19=2025-11-01 3.20=2026-04-01 3.21=2026-11-01 3.22=2027-05-01 3.23=2027-11-01 3.24=2028-06-01"
# 3.0 and 3.3 are deliberately absent: the page says 3.0 is no longer supported and does not list 3.3,
# and a line missing from the table fails.
OPENSSL_EOL="3.4=2026-10-22 3.5=2030-04-08 3.6=2026-11-01 4.0=2027-05-14"

eol_of() {   # eol_of KEY TABLE
  local kv
  for kv in $2; do [ "${kv%%=*}" = "$1" ] && { echo "${kv#*=}"; return; }; done
}
check() {    # check WHAT KEY TABLE
  local eol; eol="$(eol_of "$2" "$3")"
  if [ -z "$eol" ]; then echo "eol_guard: $1 $2 is not in the table" >&2; return 1; fi
  if [[ "$today" > "$eol" ]]; then echo "eol_guard: $1 $2 reached end of support on $eol" >&2; return 1; fi
  echo "eol_guard: $1 $2 supported until $eol"
}

alpine="$(sed -n 's/^IMAGE="alpine:\([0-9]*\.[0-9]*\)@sha256:.*/\1/p' "$here/build_bootstrap.sh")"
[ -n "$alpine" ] || { echo "eol_guard: no pinned alpine in build_bootstrap.sh" >&2; exit 1; }
ossl="$(strings "$bin" | sed -n 's/^OpenSSL \([0-9]*\.[0-9]*\)\.[0-9]*.*/\1/p' | head -1)"
[ -n "$ossl" ] || { echo "eol_guard: no OpenSSL version string in $bin" >&2; exit 1; }

rc=0
check Alpine "$alpine" "$ALPINE_EOL" || rc=1
check OpenSSL "$ossl" "$OPENSSL_EOL" || rc=1
exit $rc
