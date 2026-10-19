#!/bin/bash
# Runs in its own container sharing the server's PID namespace: VmRSS/VmHWM and CPU ticks of cbase, once a second, builtins only.
out=$1
while :; do
  pid=$(cat /run/x 2>/dev/null)
  for d in /proc/[0-9]*; do read -r c < $d/comm 2>/dev/null; [ "$c" = cbase ] && { pid=${d#/proc/}; break; }; done
  if [ -n "$pid" ] && [ -r /proc/$pid/status ]; then
    rss=; hwm=; thr=
    while read -r k v _; do case $k in VmRSS:) rss=$v;; VmHWM:) hwm=$v;; Threads:) thr=$v;; esac; done < /proc/$pid/status
    read -r st < /proc/$pid/stat; set -- ${st#*) }; ut=${12}; stt=${13}
    printf '%s %s %s %s %s %s\n' "$EPOCHREALTIME" "$pid" "$rss" "$hwm" "$((ut+stt))" "$thr" >> $out
  fi
  sleep 1
done
