#!/bin/bash
# mac_prof.sh PAIRS : native cbase serve (SQLite) + native clients on loopback; /usr/bin/sample of the server for 15 s
R=${NANO_RIG:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)}
P=${NANO_RIG:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)}
pairs=$1; out=$P/runs/mac_p$pairs; rm -rf $out; mkdir -p $out
set -a; . $R/secrets.env; . $R/client.env; set +a
CBASE_PUBLIC_URL=http://x.example CBASE_LIMIT_SIGNUP_ALL=1000000 CBASE_LIMIT_SIGNUP_IP=1000000 CBASE_MAX_CONNS=16384 CBASE_STORE=sqlite CBASE_DB=$out/db.sqlite CBASE_LISTEN=127.0.0.1:18090 CBASE_RATED_MIN_AGE=0 /Users/alex/Dev/foolish/.claude/worktrees/vm-fixes/c_base/build/cbase serve > $out/server.log 2>&1 &
srv=$!; echo $srv > $out/srv.pid
for i in $(seq 1 100); do grep -q "listening on" $out/server.log && break; sleep 0.1; done
tag=$(date +%s | tail -c 6); pids=""
for i in $(seq 0 $((pairs-1))); do
  mkdir -p $out/rdv$i
  P3VIEWER=172.16.$((i/250)).$((i%250)):1 P3DURATION_MS=30000 $R/bin/p3load-mac 18090 MA${tag}x$i a $out/rdv$i 2000 4000 $((i*2+11)) 40 0 > $out/a$i.out 2> $out/a$i.err & pids="$pids $!"
  P3VIEWER=172.17.$((i/250)).$((i%250)):1 P3DURATION_MS=30000 $R/bin/p3load-mac 18090 MB${tag}x$i b $out/rdv$i 2000 4000 $((i*2+12)) 40 0 > $out/b$i.out 2> $out/b$i.err & pids="$pids $!"
done
until [ $(ls $out/rdv*/ready.* 2>/dev/null | wc -l) -ge $((pairs*2)) ]; do sleep 0.2; done
python3 -c 'import time;print(time.time())' > $out/t_go
ps -o time= -p $srv > $out/cpu0
for d in $out/rdv*; do : > $d/go; done
sleep 5; /usr/bin/sample $srv 20 1 -mayDie -file $out/sample.txt >/dev/null 2>&1
until [ $(ls $out/rdv*/done.* 2>/dev/null | wc -l) -ge $((pairs*2)) ]; do sleep 0.2; done
python3 -c 'import time;print(time.time())' > $out/t_done
ps -o time= -p $srv > $out/cpu1
ps -o rss= -p $srv > $out/rss
for d in $out/rdv*; do : > $d/quit; done
wait $pids 2>/dev/null
kill -TERM $srv; wait $srv 2>/dev/null
cat $out/cpu0 $out/cpu1 $out/rss
