#!/bin/bash
# big8.sh LABEL TABLES : 8-seat tables with 3 spectators each at human pace, server at 0.1 CPU in play
R=${NANO_RIG:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)}
l=$1; n=$2
$R/srv_up.sh 1.6 serve-$l ${l}_8p_$n >/dev/null
SEATS=8 SPEC=3 P3SEATS=8 P3BIN=${P3BIN:-p3load2-linux} SIGNUP_CPUS=1.6 PLAY_CPUS=0.1 LOAD_PLAY_CPUS=1.9 $R/step3.sh ctr ${l}_8p_$n $n 2000 4000 10 30000 0.4 2>&1 | tail -8
docker rm -f nano-perf-sampler nano-perf-srv nano-perf-load >/dev/null 2>&1
