#!/bin/bash
# eval.sh LABEL [make vars...] : deterministic octogen CPU benchmark on the emulator's toolchain (alpine/musl arm64, 1 CPU).
# Builds foolish/c's cnitro_eval from the worktree, plays GAMES seeded octogen-vs-handwritten 2p games,
# prints user CPU seconds and a digest of the result table (the bit-identity proof: same seeds, same table).
W=${NANO_REPO:-$(git -C "$(dirname "${BASH_SOURCE[0]}")" rev-parse --show-toplevel)}
l=$1; shift; GAMES=${GAMES:-40}
docker run --rm --cpus=1 --platform linux/arm64 -v "$W":/repo:ro alpine:3.20 sh -uc '
  apk add --no-cache build-base linux-headers >/dev/null
  mkdir -p /w; cd /repo; tar cf - ./foolish/c ./shared --exclude=./foolish/c/build | tar xf - -C /w
  cd /w/foolish/c; make -s -j2 "$@" build/cnitro_eval 2>&1 | grep -E " error|Error " || true
  t0=$(date +%s); ./build/cnitro_eval --strategy=octogen --opp=handwritten --players=2,3 --games='$GAMES' --seed-start=1 2>/dev/null > out.txt; t1=$(date +%s); echo wall_s=$(( t1-t0 ))s
  tail -n 6 out.txt | md5sum' sh "$@" 2>&1 | tail -30
