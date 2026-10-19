#!/bin/bash
# prof8.sh LABEL TABLES : perf record of the server for 30 s of 8-seat (+3 spectator) play at 1 CPU; TMIN=0 TMAX=1 for saturation
R=${NANO_RIG:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)}
l=$1; g=$2
rm -rf $R/runs/prof8_$g
$R/srv_up.sh 1.6 serve-$l prof8_$g >/dev/null
( until [ -f $R/runs/prof8_$g/release ]; do sleep 0.5; done; sleep 1
  docker run --rm --name nano-perf-prof --cpus=1.0 --privileged --pid=container:nano-perf-srv -v $R:/nano nano-perf:perf bash -c \
   "perf buildid-cache -a /nano/bin/serve-$l.debug >/dev/null 2>&1; perf record -F 4999 -g -p \$(pgrep -x cbase) -o /nano/runs/prof8_$g/perf.data -- sleep 25 >/dev/null 2>&1" ) &
SEATS=8 SPEC=3 P3SEATS=8 P3BIN=p3load2-linux SIGNUP_CPUS=1.6 PLAY_CPUS=1.0 LOAD_PLAY_CPUS=0.9 $R/step3.sh ctr prof8_$g $g ${TMIN:-2000} ${TMAX:-4000} 100000 40000 0.4 2>&1 | tail -7
wait
docker rm -f nano-perf-srv nano-perf-load nano-perf-sampler nano-perf-prof >/dev/null 2>&1
