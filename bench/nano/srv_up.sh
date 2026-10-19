#!/bin/bash
# srv_up.sh CPUS BIN LABEL
R=${NANO_RIG:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)}
docker rm -f nano-perf-sampler nano-perf-srv >/dev/null 2>&1
docker run -d --name nano-perf-srv --platform linux/arm64 --privileged --ulimit nofile=65536:65536 --cpus=$1 --memory=512m --memory-swap=512m \
  --env-file $R/secrets.env -e CBASE_BIN=$2 -e CBASE_RATED_MIN_AGE=0 -e CBASE_PUBLIC_URL=http://nano.example \
  $XENV -e CBASE_MAX_CONNS=16384 -e CBASE_LISTEN_BACKLOG=4096 -e CBASE_LIMIT_CREATE_USER=1000000 -e CBASE_LIMIT_BUMP_USER=1000000 -e CBASE_LIMIT_CHAT_USER=1000000 -e CBASE_LIMIT_SUB_USER=1000000 -e CBASE_LIMIT_SIGNUP_ALL=1000000 -e CBASE_LIMIT_SIGNUP_IP=1000000 -v $R:/nano:ro nano-perf:al2023 /nano/entry.sh >/dev/null
for i in $(seq 1 100); do docker exec nano-perf-srv grep -q "listening on" /data/server.log 2>/dev/null && break; sleep 0.2; done
docker run -d --name nano-perf-sampler --cpus=0.05 --pid=container:nano-perf-srv -v $R:/nano nano-perf:al2023 /nano/sampler.sh /nano/rss_$3.log >/dev/null
docker exec nano-perf-srv tail -1 /data/server.log
