#!/bin/bash
# prof_sc_h.sh LABEL GAMES : syscalls per move at HUMAN pace (2-4 s per move, 1 CPU so nothing throttles), perf stat for 20 s
R=${NANO_RIG:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)}
l=$1; g=$2
rm -rf $R/runs/sch_$g
$R/srv_up.sh 1.6 serve-$l sch_$g >/dev/null
( until [ -f $R/runs/sch_$g/release ]; do sleep 0.5; done; sleep 3
  docker run --rm --name nano-perf-prof --cpus=1.0 --privileged --pid=container:nano-perf-srv -v $R:/nano nano-perf:perf bash -c \
   "perf stat -a -x, -e syscalls:sys_enter_write,syscalls:sys_enter_sendto,syscalls:sys_enter_epoll_pwait,syscalls:sys_enter_futex,syscalls:sys_enter_mmap -p \$(pgrep -x cbase) -- sleep 20 2>&1 | cut -d, -f1,3" > $R/runs/sch_$g.txt ) &
SIGNUP_CPUS=1.6 PLAY_CPUS=1.0 LOAD_PLAY_CPUS=1.9 $R/step.sh ctr sch_$g $g 2000 4000 40 30000 0.4 2>&1 | grep -E "moves_per_s|window|server_cpu" | cut -c1-140
wait
docker rm -f nano-perf-srv nano-perf-load nano-perf-sampler nano-perf-prof >/dev/null 2>&1
