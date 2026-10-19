#!/bin/bash
# prof_bot.sh LABEL GAMES : perf record of the server (octogen bot games, bot pacing x100) for 30 s of play at 1 CPU
R=${NANO_RIG:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)}
l=$1; n=$2; lab=${l}_pbot_$n
IDS=4b6ecbc2-45cf-5a0f-be01-4eb716ffb6c0,040599d9-c94b-5736-8956-c3802d19b176,bf78b6e6-4d62-5c44-b673-0fa59c7791c5,70ad5197-fa05-5cd0-a2cf-e18170c7cdd0,597a6e99-78d9-5070-a7ec-e2a52f22ece8,e600ebbe-6bd7-5703-a44a-b3f2b2128e52,0a00cbfc-0994-59d2-b49e-bccda6767766
XENV="-e CBASE_REQ_WORKERS=1 -e CBASE_BOT_SPEEDUP=100" $R/srv_up.sh 1.6 serve-$l $lab >/dev/null
( until [ -f $R/runs/$lab/release ]; do sleep 0.5; done; sleep 1
  docker run --rm --name nano-perf-prof --cpus=0.1 --privileged --pid=container:nano-perf-srv -v $R:/nano nano-perf:perf bash -c \
   "perf buildid-cache -a /nano/bin/serve-$l.debug >/dev/null 2>&1; perf record -F 999 -g -p \$(pgrep -x cbase) -o /nano/runs/$lab/perf.data -- sleep 30 >/dev/null 2>&1" ) &
BOTMODE=1 SEATS=2 P3SEATS=2 P3BOTIDS=$IDS P3BIN=p3load2-linux SIGNUP_CPUS=1.6 PLAY_CPUS=1.0 LOAD_PLAY_CPUS=0.9 $R/step3.sh ctr $lab $n 2000 4000 10 45000 0.4 2>&1 | grep -E "moves=|cpu_s"
wait
docker rm -f nano-perf-sampler nano-perf-srv nano-perf-load >/dev/null 2>&1
