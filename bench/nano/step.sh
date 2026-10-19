#!/bin/bash
# step.sh MODE(mac|ctr) LABEL PAIRS THINK_MIN THINK_MAX GAMES DURATION_MS CLIENT_CPUS
# mac: native clients through the published port 18081. ctr: clients in their own container
# (--cpus CLIENT_CPUS) sharing nano-perf-srv's network namespace, on loopback 8080.
set -uo pipefail
mode=$1; label=$2; pairs=$3; tmin=$4; tmax=$5; games=$6; dur=$7; ccpu=${8:-0.5}
R=${NANO_RIG:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)}
out=$R/runs/$label; rm -rf "$out"; mkdir -p "$out"
tag=$(date +%s | tail -c 6)$((RANDOM % 100))
# server-side counters before
docker exec nano-perf-srv sh -c 'cat /sys/fs/cgroup/cpu.stat; echo; wc -c < /data/server.log; du -sk /data/cbase' > "$out/srv_before.txt"
cat > "$out/clients.sh" <<CL
#!/bin/sh
VBASE=$(( $(cat $R/vbase 2>/dev/null || echo 0) ))
# runs the pairs; each process its own viewer address
cd /out
i=0; pids=""
while [ \$i -lt $pairs ]; do
  mkdir -p rdv\$i
  a=\$(( (VBASE + i) / 250 % 250 + 1)); b=\$(( (VBASE + i) % 250 ))
  P3VIEWER=1\$((VBASE % 9)).\$a.\$b.1:4\$((i%1000)) P3DURATION_MS=$dur \$BIN \$PORT A${tag}x\$i a rdv\$i $tmin $tmax \$((i*2+11)) $games 0 > a\$i.out 2> a\$i.err & pids="\$pids \$!"
  P3VIEWER=1\$((VBASE % 9)).\$a.\$b.2:5\$((i%1000)) P3DURATION_MS=$dur \$BIN \$PORT B${tag}x\$i b rdv\$i $tmin $tmax \$((i*2+12)) $games 0 > b\$i.out 2> b\$i.err & pids="\$pids \$!"
  i=\$((i+1))
done
# wait until every pair is ready (signed up) or a client died
t=0
while :; do
  n=\$(ls rdv*/ready.* 2>/dev/null | wc -l)
  [ \$n -ge \$(( $pairs * 2 )) ] && break
  dead=0; for p in \$pids; do kill -0 \$p 2>/dev/null || dead=\$((dead+1)); done
  [ \$((n + dead)) -ge \$(( $pairs * 2 )) ] && break
  sleep 0.2; t=\$((t+1)); [ \$t -gt 3000 ] && break
done
: > all_ready
for _w in \$(seq 1500); do [ -f release ] && break; sleep 0.1; done
date +%s.%N > t_go
for d in rdv*; do : > \$d/go; done
while :; do
  n=\$(ls rdv*/done.* 2>/dev/null | wc -l)
  alive=0; for p in \$pids; do kill -0 \$p 2>/dev/null && alive=1; done
  [ \$n -ge \$(( $pairs * 2 )) ] && break
  [ "\$alive" = 0 ] && break
  sleep 0.2
done
date +%s.%N > t_done
for d in rdv*; do : > \$d/quit; done
wait
CL
chmod +x "$out/clients.sh"
echo $(( $(cat $R/vbase 2>/dev/null || echo 0) + pairs )) > $R/vbase
if [ "$mode" = mac ]; then
  ( for _w in $(seq 450); do [ -f "$out/all_ready" ] && break; sleep 0.2; done; : > "$out/release" ) &
  (cd "$out" && sed -e "s#cd /out#cd $out#" clients.sh > clients_mac.sh && chmod +x clients_mac.sh && set -a && . $R/client.env && set +a && BIN=$R/bin/p3load-mac PORT=18081 ./clients_mac.sh)
else
  ( for _w in $(seq 450); do [ -f "$out/all_ready" ] && break; sleep 0.2; done
    if [ -n "${PLAY_CPUS:-}" ]; then docker update --cpus=$PLAY_CPUS nano-perf-srv >/dev/null; fi
    if [ -n "${LOAD_PLAY_CPUS:-}" ]; then docker update --cpus=$LOAD_PLAY_CPUS nano-perf-load >/dev/null; fi
    docker exec nano-perf-srv sh -c 'cat /sys/fs/cgroup/cpu.stat' > "$out/srv_go.txt"
    : > "$out/release" ) &
  docker run --rm --name nano-perf-load --platform linux/arm64 --cpus=$ccpu --network container:nano-perf-srv \
    --env-file $R/client.env -e BIN=/nano/bin/p3load2-linux -e PORT=8080 -e P3HOST=127.0.0.1 \
    -v $R:/nano:ro -v "$out":/out nano-perf:al2023 sh -c '/out/clients.sh; cat /sys/fs/cgroup/cpu.stat > /out/load_cpu.txt'
fi
docker exec nano-perf-srv sh -c 'cat /sys/fs/cgroup/cpu.stat; echo; wc -c < /data/server.log; du -sk /data/cbase; cat /sys/fs/cgroup/memory.peak' > "$out/srv_after.txt"
if [ -n "${SIGNUP_CPUS:-}" ]; then docker update --cpus=$SIGNUP_CPUS nano-perf-srv >/dev/null; fi
python3 $R/summ.py "$out"
