#!/bin/bash
# ab.sh LABEL... : per label, saturation (8 games) and syscalls per move; prints moves/s and futex/sendto per move
R=${NANO_RIG:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)}
for l in "$@"; do
  echo "== $l"
  XENV="-e CBASE_REQ_WORKERS=1 $EXTRA" $R/prof_sc.sh $l 8
  awk -F, '{print}' $R/runs/sc_8.txt | grep -E "futex|sendto|write|epoll|mmap"
done
