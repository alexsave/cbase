#!/bin/bash
# mem.sh LABEL GAMES : smaps of the server mid-load
R=${NANO_RIG:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)}
l=$1; g=$2
rm -rf $R/runs/mem_$g
$R/srv_up.sh 1.6 serve-$l mem_$g >/dev/null
( SIGNUP_CPUS=1.6 PLAY_CPUS=0.1 LOAD_PLAY_CPUS=1.9 $R/step.sh ctr mem_$g $g 2000 4000 40 30000 0.4 > $R/runs/mem_$g.sum 2>&1 ) &
until [ -f $R/runs/mem_$g/release ]; do sleep 1; done
sleep 12
docker exec nano-perf-srv sh -c 'p=$(pidof cbase); echo threads=$(ls /proc/$p/task | wc -l) fds=$(ls /proc/$p/fd | wc -l); cat /proc/$p/smaps' > $R/runs/smaps_$g.txt
wait
