#!/bin/bash
# journal_meas.sh LABEL sat|h192 : one measurement for the move journal's before/after.
#   sat  : 8 two-seat games, no think time, 1 CPU, 20 s; syscalls counted for 8 s mid-run (perf stat),
#          then SIGTERM and the store's WAL meter and the journal's close line (bytes since start).
#   h192 : 192 games at human pace (2-4 s), 0.1 CPU, 30 s.
# Server env: CBASE_REQ_WORKERS=1 (a real 0.1-CPU box) and CBASE_RESTART=1 (the container outlives a SIGTERM).
R=${NANO_RIG:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)}
l=$1; m=$2
export XENV="-e CBASE_REQ_WORKERS=1 -e CBASE_RESTART=1 ${JXENV:-}"
if [ "$m" = sat ]; then
  rm -rf $R/runs/jsat_$l
  $R/srv_up.sh 1.6 serve-$l jsat_$l >/dev/null
  ( until [ -f $R/runs/jsat_$l/release ]; do sleep 0.5; done; sleep 5
    docker run --rm --name nano-perf-prof --cpus=0.2 --privileged --pid=container:nano-perf-srv nano-perf:perf bash -c \
      'perf stat -e raw_syscalls:sys_enter -p $(pgrep -x cbase) -- sleep 8 2>&1 | grep -E "raw_syscalls|seconds time"' > $R/runs/jsat_${l}_sys.txt 2>&1 ) &
  SIGNUP_CPUS=1.6 PLAY_CPUS=1.0 LOAD_PLAY_CPUS=0.9 $R/step.sh ctr jsat_$l 8 0 1 100000 20000 0.4 2>&1 | grep -E "moves|move_ms|server_cpu|rss_max"
  wait
  cat $R/runs/jsat_${l}_sys.txt
  docker exec nano-perf-srv sh -c 'kill -TERM $(cat /run/cbase.pid)'
  for i in $(seq 50); do docker exec nano-perf-srv grep -aq "store: wal:" /data/server.log && break; sleep 0.2; done
  docker exec nano-perf-srv grep -aE "store: wal:|journal: [0-9]" /data/server.log | tail -2
else
  $R/srv_up.sh 1.6 serve-$l jh_$l >/dev/null
  SIGNUP_CPUS=1.6 PLAY_CPUS=0.1 LOAD_PLAY_CPUS=1.9 $R/step.sh ctr jh_${l}_192 192 2000 4000 40 30000 0.4 2>&1 | tail -8
  docker exec nano-perf-srv sh -c 'kill -TERM $(cat /run/cbase.pid)'
  for i in $(seq 50); do docker exec nano-perf-srv grep -aq "store: wal:" /data/server.log && break; sleep 0.2; done
  docker exec nano-perf-srv grep -aE "store: wal:|journal: [0-9]" /data/server.log | tail -2
fi
docker rm -f nano-perf-srv nano-perf-load nano-perf-sampler nano-perf-prof >/dev/null 2>&1
