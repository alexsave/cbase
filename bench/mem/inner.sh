#!/bin/sh
# runs inside alpine. $1 = tag, rest = command. captures to /out/$tag
cap(){ # tag label pid
  d=/out/$1; mkdir -p $d
  cat /proc/$3/status > $d/status_$2.txt; cat /proc/$3/smaps > $d/smaps_$2.txt; cat /proc/$3/smaps_rollup > $d/rollup_$2.txt
}
run(){ tag=$1; shift
  "$@" >/out/$tag.log 2>&1 &
  pid=$!
  sleep 2; cap $tag t2 $pid
  sleep 28; cap $tag t30 $pid
  if [ "$tag" != "${tag#min}" ]; then
    i=0; while [ $i -lt 1000 ]; do wget -q -O /dev/null http://127.0.0.1:8642/api/hello; i=$((i+1)); done
    sleep 1; cap $tag req $pid
    wget -q -O - http://127.0.0.1:8642/api/hello > /out/$tag.hello
  fi
  ls /proc/$pid/task | wc -l > /out/$tag.threads
  kill $pid; wait $pid 2>/dev/null
}
apk add --no-cache gcc musl-dev >/dev/null
cd /out
gcc -O2 -static -o /nothp /s/scripts/nothp.c
gcc -O2 -static -o /floor0 /s/scripts/floor0.c
gcc -O2 -static -o /floor5 /s/scripts/floor5.c -pthread
cat /sys/kernel/mm/transparent_hugepage/enabled > /out/thp.txt; uname -r >> /out/thp.txt
export CBASE_LISTEN=0.0.0.0:8642
run min_nothp /nothp /out/minimal serve
run min_thp /out/minimal serve
