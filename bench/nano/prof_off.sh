#!/bin/bash
# prof_off.sh LABEL [GAMES=8] : off-CPU view at saturation (1 CPU): perf trace summary (time inside each syscall per thread)
# and the syscalls slower than 1 ms, for 12 s of steady play. Output: runs/off_LABEL.txt
R=${NANO_RIG:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)}
l=$1; g=${2:-8}
rm -rf $R/runs/off_$g
$R/srv_up.sh 1.6 serve-$l off_$l >/dev/null
( until [ -f $R/runs/off_$g/release ]; do sleep 0.5; done; sleep 2
  docker run --rm --name nano-perf-prof --cpus=1.0 --privileged --pid=container:nano-perf-srv -v $R:/nano nano-perf:perf bash -c \
   "timeout -s INT 12 perf trace -s --duration 1 -p \$(pgrep -x cbase) 2>&1 | tail -150" > $R/runs/off_$l.txt ) &
SIGNUP_CPUS=1.6 PLAY_CPUS=1.0 LOAD_PLAY_CPUS=0.9 $R/step.sh ctr off_$g $g 0 1 100000 20000 0.4 2>&1 | grep -E "moves_per_s|move_ms"
wait
docker rm -f nano-perf-srv nano-perf-load nano-perf-sampler nano-perf-prof >/dev/null 2>&1
