#!/bin/bash
# Runs the idle, 1000-request and floor measurements. Usage: OUT=/abs/dir bench/mem/run.sh
# $OUT must hold minimal and minimal.unstripped from build.sh. Needs Docker.
# Captures status, smaps and smaps_rollup at 2 s, 30 s and after 1000 requests into $OUT/<tag>/.
set -e
here="$(cd "$(dirname "$0")" && pwd)"
: "${OUT:?set OUT}"
docker run --rm --platform linux/arm64 -v "$here":/s/scripts:ro -v "$OUT":/out \
  alpine:3.24@sha256:294b683cb724975bec92580e1e685676bd4b50bda910ddb8c51d4cabeaec77e6 sh /s/scripts/inner.sh
