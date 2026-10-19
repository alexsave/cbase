#!/bin/bash
# run.sh LABEL : fresh nano server (0.1 cpu, 512m) from bin/serve-LABEL, human-pace load at 64 and 192 games
R=${NANO_RIG:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)}
l=$1
for g in 64 192; do
  $R/srv_up.sh 1.6 serve-$l ${l}_$g
  SIGNUP_CPUS=1.6 PLAY_CPUS=0.1 LOAD_PLAY_CPUS=1.9 $R/step.sh ctr ${l}_h_p$g $g 2000 4000 40 30000 0.4 2>&1 | tail -8
  docker exec nano-perf-srv sh -c 'grep -ci "error\|fatal" /data/server.log'
done
docker rm -f nano-perf-sampler nano-perf-srv >/dev/null 2>&1
