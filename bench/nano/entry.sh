#!/bin/bash
# The nano's boot: a 2 GB ext4 data disk (loop image), then cbase serve as a supervised child plus an RSS sampler.
set -u
if [ ! -f /data.img ]; then truncate -s 2G /data.img && mkfs.ext4 -q /data.img; fi
mkdir -p /data; mountpoint -q /data || mount -o loop /data.img /data
mkdir -p /data/cbase
export CBASE_STORE=sqlite CBASE_DB=/data/cbase/cbase.sqlite CBASE_LISTEN=0.0.0.0:8080 CBASE_LOG_LEVEL=${CBASE_LOG_LEVEL:-info}
cp -f /nano/bin/${CBASE_BIN:-cbase-serve-linux-arm64} /usr/local/bin/cbase
while :; do
echo "boot $(date -u +%FT%TZ.%N)" >> /data/boots.log
LD_PRELOAD=${CBASE_PRELOAD:-} /usr/local/bin/cbase serve >> /data/server.log 2>&1 &
pid=$!
echo $pid > /run/cbase.pid
wait $pid
rc=$?
echo "cbase exited rc=$rc $(date -u +%FT%TZ.%N)" >> /data/boots.log
[ "${CBASE_RESTART:-0}" = 1 ] || exit $rc
sleep 1   # systemd Restart=always, RestartSec=1
done
