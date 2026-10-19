#!/bin/bash
# prof.sh LABEL GAMES : perf record of the server for 15 s of steady play
R=${NANO_RIG:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)}
l=$1; g=$2
rm -rf $R/runs/prof_$g
$R/srv_up.sh 1.6 serve-$l prof_$g >/dev/null
( until [ -f $R/runs/prof_$g/release ]; do sleep 0.5; done; sleep 1
  docker run --rm --name nano-perf-prof --cpus=0.1 --privileged --pid=container:nano-perf-srv -v $R:/nano nano-perf:perf bash -c \
   "perf buildid-cache -a /nano/bin/serve-$l.debug >/dev/null 2>&1; perf record -F 4999 -g -p \$(pgrep -x cbase) -o /nano/runs/prof_$g/perf.data -- sleep 30 >/dev/null 2>&1" ) &
SIGNUP_CPUS=1.6 PLAY_CPUS=1.0 LOAD_PLAY_CPUS=0.9 $R/step.sh ctr prof_$g $g ${TMIN:-2000} ${TMAX:-4000} 100000 45000 0.4 2>&1 | tail -7
wait
