#!/bin/bash
# sat.sh LABEL [GAMES=8] [DUR_MS=25000] : saturation (1 CPU, no think time); XENV="-e K=V" passes server env
R=${NANO_RIG:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)}
l=$1; g=${2:-8}; d=${3:-25000}
$R/srv_up.sh 1.6 serve-$l sat_$l >/dev/null
SIGNUP_CPUS=1.6 PLAY_CPUS=1.0 LOAD_PLAY_CPUS=0.9 $R/step.sh ctr sat_${l}_$g $g 0 1 100000 $d 0.4 2>&1 | grep -E "moves_per_s|move_ms|server_cpu|rss_max"
[ -z "$LOGGREP" ] || docker exec nano-perf-srv grep -a "$LOGGREP" /data/server.log | tail -2
# WAL=1 (with XENV holding -e CBASE_RESTART=1, so the container outlives the server): stop the server with
# SIGTERM and print the store's WAL meter (frames and bytes since it opened)
if [ -n "$WAL" ]; then
  docker exec nano-perf-srv sh -c 'kill -TERM $(cat /run/cbase.pid)'
  for i in $(seq 50); do docker exec nano-perf-srv grep -aq "store: wal:" /data/server.log && break; sleep 0.2; done
  docker exec nano-perf-srv grep -a "store: wal:" /data/server.log | tail -1
fi
[ -n "$KEEP" ] || docker rm -f nano-perf-sampler nano-perf-srv nano-perf-load >/dev/null 2>&1
