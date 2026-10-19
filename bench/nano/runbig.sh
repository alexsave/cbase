#!/bin/bash
# run1024.sh LABEL PLAYCPUS : 1024 human-pace games
R=${NANO_RIG:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)}
l=$1; c=$2; G=${3:-512}
$R/srv_up.sh 1.6 serve-$l ${l}_1024_$c
LP=$(python3 -c "print(round(2.0-$c,2))")
SIGNUP_CPUS=1.6 PLAY_CPUS=$c LOAD_PLAY_CPUS=$LP $R/step.sh ctr ${l}_h${G}_c$c $G 2000 4000 40 30000 0.4 2>&1 | tail -8
docker exec nano-perf-srv sh -c 'grep -ci "refus\|cap\|error" /data/server.log; grep -i "cap\|refus\|error" /data/server.log | tail -3'
docker rm -f nano-perf-sampler nano-perf-srv >/dev/null 2>&1
