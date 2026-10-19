#!/bin/bash
# botg.sh LABEL GAMES SEATS : N human clients, each creating a table and adding SEATS-1 Moscow (octogen) bots; bot pacing x100
R=${NANO_RIG:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)}
l=$1; n=$2; s=${3:-2}
IDS=4b6ecbc2-45cf-5a0f-be01-4eb716ffb6c0,040599d9-c94b-5736-8956-c3802d19b176,bf78b6e6-4d62-5c44-b673-0fa59c7791c5,70ad5197-fa05-5cd0-a2cf-e18170c7cdd0,597a6e99-78d9-5070-a7ec-e2a52f22ece8,e600ebbe-6bd7-5703-a44a-b3f2b2128e52,0a00cbfc-0994-59d2-b49e-bccda6767766
XENV="-e CBASE_REQ_WORKERS=1 -e CBASE_BOT_SPEEDUP=${SPEEDUP:-100}" $R/srv_up.sh 1.6 serve-$l ${l}_bot_$n >/dev/null
BOTMODE=1 SEATS=$s P3SEATS=$s P3BOTIDS=$IDS P3BIN=p3load2-linux SIGNUP_CPUS=1.6 PLAY_CPUS=${CPU:-0.1} LOAD_PLAY_CPUS=1.9 $R/step3.sh ctr ${l}_bot_${s}_$n $n 2000 4000 10 ${DUR:-30000} 0.4 2>&1 | tail -8
docker exec nano-perf-srv sh -c 'grep -ci "error\|fatal" /data/server.log'
[ -n "$KEEPLOG" ] && docker exec nano-perf-srv cat /data/server.log > $KEEPLOG
docker rm -f nano-perf-sampler nano-perf-srv nano-perf-load >/dev/null 2>&1
