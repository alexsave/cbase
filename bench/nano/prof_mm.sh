#!/bin/bash
# prof_mm.sh LABEL GAMES : who calls mmap/munmap per move (perf record with dwarf unwinding of the two tracepoints, 6 s)
R=${NANO_RIG:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)}
l=$1; g=$2
rm -rf $R/runs/mm_$g
$R/srv_up.sh 1.6 serve-$l mm_$g >/dev/null
( until [ -f $R/runs/mm_$g/release ]; do sleep 0.5; done; sleep 2
  docker run --rm --name nano-perf-prof --cpus=1.0 --privileged --pid=container:nano-perf-srv -v $R:/nano nano-perf:perf bash -c \
   "perf buildid-cache -a /nano/bin/serve-$l.debug >/dev/null 2>&1; cp /nano/bin/serve-$l.debug /usr/local/bin/cbase; perf record -e syscalls:sys_enter_mmap --call-graph dwarf,16384 -F 300 -p \$(pgrep -x cbase) -o /nano/runs/mm_$g/perf.data -- sleep 6 >/dev/null 2>&1; perf report -i /nano/runs/mm_$g/perf.data --no-children --stdio -g caller,0.5,callee,function,percent 2>&1 | grep -v '^#' | grep -v '^\$' | head -90" > $R/runs/mm_$g.txt ) &
P3SEATS=2 P3BIN=p3load2-linux SIGNUP_CPUS=1.6 PLAY_CPUS=1.0 LOAD_PLAY_CPUS=0.9 $R/step3.sh ctr mm_$g $g 0 1 100000 16000 0.4 2>&1 | grep -E "moves_per_s" | cut -c1-100
wait
docker rm -f nano-perf-srv nano-perf-load nano-perf-sampler nano-perf-prof >/dev/null 2>&1
