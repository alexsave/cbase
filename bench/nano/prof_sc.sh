#!/bin/bash
# prof_sc.sh LABEL GAMES : syscalls per move at saturation (perf stat of the server's syscall tracepoints for 15 s, 1 CPU)
R=${NANO_RIG:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)}
l=$1; g=$2
rm -rf $R/runs/sc_$g
$R/srv_up.sh 1.6 serve-$l sc_$g >/dev/null
( until [ -f $R/runs/sc_$g/release ]; do sleep 0.5; done; sleep 2
  docker run --rm --name nano-perf-prof --cpus=1.0 --privileged --pid=container:nano-perf-srv -v $R:/nano nano-perf:perf bash -c \
   "${SC_PRE:-}perf stat -a -x, -e syscalls:sys_enter_write,syscalls:sys_enter_writev,syscalls:sys_enter_read,syscalls:sys_enter_recvfrom,syscalls:sys_enter_sendto,syscalls:sys_enter_epoll_pwait,syscalls:sys_enter_futex,syscalls:sys_enter_mmap,syscalls:sys_enter_munmap,syscalls:sys_enter_fdatasync,syscalls:sys_enter_pwrite64,syscalls:sys_enter_accept4 -p \$(pgrep -x cbase) -- sleep 15 2>&1 | cut -d, -f1,3" > $R/runs/sc_$g.txt ) &
SEATS=${SEATS:-2} SPEC=${SPEC:-0} P3SEATS=${SEATS:-2} P3BIN=p3load2-linux SIGNUP_CPUS=1.6 PLAY_CPUS=1.0 LOAD_PLAY_CPUS=0.9 $R/step3.sh ctr sc_$g $g 0 1 100000 22000 0.4 2>&1 | grep -E "moves_per_s|window" | cut -c1-100
wait
docker rm -f nano-perf-srv nano-perf-load nano-perf-sampler nano-perf-prof >/dev/null 2>&1
