#!/bin/bash
# prof_mm_h.sh LABEL GAMES : callers of mmap at HUMAN pace (perf record dwarf, 15 s)
R=${NANO_RIG:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)}
l=$1; g=$2
rm -rf $R/runs/mmh_$g
$R/srv_up.sh 1.6 serve-$l mmh_$g >/dev/null
( until [ -f $R/runs/mmh_$g/release ]; do sleep 0.5; done; sleep 3
  docker run --rm --name nano-perf-prof --cpus=1.0 --privileged --pid=container:nano-perf-srv -v $R:/nano nano-perf:perf bash -c \
   "perf buildid-cache -a /nano/bin/serve-$l.debug >/dev/null 2>&1; cp /nano/bin/serve-$l.debug /usr/local/bin/cbase; perf record -e syscalls:sys_enter_mmap --call-graph dwarf,16384 -F 300 -p \$(pgrep -x cbase) -o /nano/runs/mmh_$g/perf.data -- sleep 15 >/dev/null 2>&1; perf report -i /nano/runs/mmh_$g/perf.data --no-children --stdio -g caller,0.5,callee,function,percent 2>&1 | grep -v '^#' | grep -v '^\$' | head -70" > $R/runs/mmh_$g.txt ) &
SIGNUP_CPUS=1.6 PLAY_CPUS=1.0 LOAD_PLAY_CPUS=1.9 $R/step.sh ctr mmh_$g $g 2000 4000 40 30000 0.4 2>&1 | grep -E "moves_per_s|window|server_cpu" | cut -c1-140
wait
docker rm -f nano-perf-srv nano-perf-load nano-perf-sampler nano-perf-prof >/dev/null 2>&1
