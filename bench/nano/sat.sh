#!/bin/bash
# sat.sh LABEL [GAMES=8] [DUR_MS=25000] : saturation (1 CPU, no think time); XENV="-e K=V" passes server env
R=${NANO_RIG:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)}
l=$1; g=${2:-8}; d=${3:-25000}
$R/srv_up.sh 1.6 serve-$l sat_$l >/dev/null
SIGNUP_CPUS=1.6 PLAY_CPUS=1.0 LOAD_PLAY_CPUS=0.9 $R/step.sh ctr sat_${l}_$g $g 0 1 100000 $d 0.4 2>&1 | grep -E "moves_per_s|move_ms|server_cpu|rss_max"
docker rm -f nano-perf-sampler nano-perf-srv nano-perf-load >/dev/null 2>&1
