#!/bin/bash
# build.sh LABEL : static serve build of the vm-fixes worktree (alpine arm64, 2 cpus, -j2) into bin/serve-LABEL
set -euo pipefail
W=${NANO_REPO:-$(git -C "$(dirname "${BASH_SOURCE[0]}")" rev-parse --show-toplevel)}
R=${NANO_RIG:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)}
docker rm -f nano-perf-build >/dev/null 2>&1 || true
docker run --rm --cpus=2 --name nano-perf-build --platform linux/arm64 -v "$W":/repo:ro -v "$R/bin":/out -e UT=${UT:-} alpine:3.20 sh -euc '
  apk add --no-cache build-base openssl-dev openssl-libs-static sqlite-dev sqlite-static linux-headers zip >/dev/null
  mkdir -p /w; cd /repo
  tar cf - --exclude="./c_base/build" --exclude="./c_base/dist" --exclude="./foolish/c/build" ./c_base ./foolish/c ./foolish/server ./shared | tar xf - -C /w
  cd /w/c_base
  make -s -j2 STATIC=1 build/cbase
  [ -z "$UT" ] || { make -s STATIC=1 build/test/unit_loop && build/test/unit_loop; }
  cp build/cbase /out/serve-'"$1"'.debug
  strip -o /out/serve-'"$1"' build/cbase
  ls -l /out/serve-'"$1"'
'
