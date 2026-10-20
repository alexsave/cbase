#!/bin/bash
# run_rig.sh [OUTDIR] : run the same calbench binary in the local Docker arm64 rig (alpine 3.24, --cpus=2, the
# container setup of ../nano) so its rows can be compared with an AWS run: ./compare.py <aws run dir> <OUTDIR>.
# UNTESTED: written from the nano rig's docker flags and cpu_model.md, never run. Needs bin/calbench (./build.sh).
# Three passes, 5 min apart (CAL_RIG_PASSES, CAL_RIG_GAP_S), the groups of the AWS runner except ebs and steal
# (the rig's disk and steal numbers are the Docker VM's, not the model's).
set -euo pipefail
D=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
OUT=${1:-$D/results_rig_$(date -u +%Y%m%dT%H%M%SZ)}
[ -x "$D/bin/calbench" ] || { echo "no bin/calbench: run ./build.sh first" >&2; exit 1; }
mkdir -p "$OUT"
docker rm -f calib-rig >/dev/null 2>&1 || true
docker run --rm --cpus=2 --name calib-rig --platform linux/arm64 -e CAL_CPU=1 -e CAL_DIR=/tmp \
  -e PASSES="${CAL_RIG_PASSES:-3}" -e GAP="${CAL_RIG_GAP_S:-300}" \
  -v "$D/bin":/b:ro -v "$OUT":/o alpine:3.24@sha256:294b683cb724975bec92580e1e685676bd4b50bda910ddb8c51d4cabeaec77e6 \
  sh -c 'p=1; while [ $p -le $PASSES ]; do for g in env clock core kernel net wake real str malloc mem stream; do /b/calbench $g $p > /o/p${p}_$g.tsv 2> /o/p${p}_$g.err; done; [ $p -lt $PASSES ] && sleep $GAP; p=$((p+1)); done'
echo "rig results: $OUT"
echo "compare: $D/compare.py <aws run dir> $OUT"
