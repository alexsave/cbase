#!/bin/bash
# botg.sh LABEL GAMES SEATS : N human clients, each creating a table and adding SEATS-1 Moscow (octogen) bots; bot pacing x100
R=${NANO_RIG:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)}
l=$1; n=$2; s=${3:-2}
IDS=4f6f2ebb-1d3e-4761-a5a9-c91313eba087,f60b53ec-74f6-45fe-9e0f-315e5495bc18,45f7a264-aedc-4ee4-8473-4681d6e0bcb8,acc775ae-6d35-41d2-88bc-7dc1599cff82,df16e0de-4569-4771-804e-98f8d13affd1,fc185071-a522-424e-a1bd-0c4d34b32d3e,abe1fcfa-992c-4bf5-b163-32c0b0a21a7b
XENV="-e CBASE_REQ_WORKERS=1 -e CBASE_BOT_SPEEDUP=${SPEEDUP:-100}" $R/srv_up.sh 1.6 serve-$l ${l}_bot_$n >/dev/null
BOTMODE=1 SEATS=$s P3SEATS=$s P3BOTIDS=$IDS P3BIN=p3load2-linux SIGNUP_CPUS=1.6 PLAY_CPUS=${CPU:-0.1} LOAD_PLAY_CPUS=1.9 $R/step3.sh ctr ${l}_bot_${s}_$n $n 2000 4000 10 ${DUR:-30000} 0.4 2>&1 | tail -8
docker exec nano-perf-srv sh -c 'grep -ci "error\|fatal" /data/server.log'
[ -n "$KEEPLOG" ] && docker exec nano-perf-srv cat /data/server.log > $KEEPLOG
docker rm -f nano-perf-sampler nano-perf-srv nano-perf-load >/dev/null 2>&1
