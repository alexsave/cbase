#!/bin/bash
# Builds example/minimal static on musl (the ops/build_static.sh recipe) into $OUT.
# Usage: OUT=/abs/dir CDOM=/abs/cdom-checkout bench/mem/build.sh
# Needs Docker. $OUT gets minimal (stripped), minimal.unstripped, readelf_l.txt, readelf_S.txt.
set -e
cb="$(cd "$(dirname "$0")/../.." && pwd)"
: "${OUT:?set OUT}"; : "${CDOM:?set CDOM}"
mkdir -p "$OUT"
docker run --rm --platform linux/arm64 --cpus=2 -v "$cb":/s/cbase:ro -v "$CDOM":/s/cdom:ro -v "$OUT":/out \
  alpine:3.24@sha256:294b683cb724975bec92580e1e685676bd4b50bda910ddb8c51d4cabeaec77e6 sh -euc '
 apk upgrade --no-cache >/dev/null
 apk add --no-cache build-base openssl-dev openssl-libs-static sqlite-dev sqlite-static linux-headers >/dev/null
 mkdir -p /w && cp -r /s/cbase /s/cdom /w/ && cd /w/cbase/example/minimal
 make -s -j2 STATIC=1 CDOM=/w/cdom build/minimal
 strip -o /out/minimal build/minimal; cp build/minimal /out/minimal.unstripped
 readelf -lW /out/minimal > /out/readelf_l.txt; readelf -SW /out/minimal > /out/readelf_S.txt
'
