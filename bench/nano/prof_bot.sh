#!/bin/bash
# prof_bot.sh LABEL GAMES : perf record of the server (octogen bot games, bot pacing x100) for 30 s of play at 1 CPU
R=${NANO_RIG:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)}
l=$1; n=$2; lab=${l}_pbot_$n
IDS=4f6f2ebb-1d3e-4761-a5a9-c91313eba087,f60b53ec-74f6-45fe-9e0f-315e5495bc18,45f7a264-aedc-4ee4-8473-4681d6e0bcb8,acc775ae-6d35-41d2-88bc-7dc1599cff82,df16e0de-4569-4771-804e-98f8d13affd1,fc185071-a522-424e-a1bd-0c4d34b32d3e,abe1fcfa-992c-4bf5-b163-32c0b0a21a7b
XENV="-e CBASE_REQ_WORKERS=1 -e CBASE_BOT_SPEEDUP=100" $R/srv_up.sh 1.6 serve-$l $lab >/dev/null
( until [ -f $R/runs/$lab/release ]; do sleep 0.5; done; sleep 1
  docker run --rm --name nano-perf-prof --cpus=0.1 --privileged --pid=container:nano-perf-srv -v $R:/nano nano-perf:perf bash -c \
   "perf buildid-cache -a /nano/bin/serve-$l.debug >/dev/null 2>&1; perf record -F 999 -g -p \$(pgrep -x cbase) -o /nano/runs/$lab/perf.data -- sleep 30 >/dev/null 2>&1" ) &
BOTMODE=1 SEATS=2 P3SEATS=2 P3BOTIDS=$IDS P3BIN=p3load2-linux SIGNUP_CPUS=1.6 PLAY_CPUS=1.0 LOAD_PLAY_CPUS=0.9 $R/step3.sh ctr $lab $n 2000 4000 10 45000 0.4 2>&1 | grep -E "moves=|cpu_s"
wait
docker rm -f nano-perf-sampler nano-perf-srv nano-perf-load >/dev/null 2>&1
