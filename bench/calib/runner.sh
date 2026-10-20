#!/bin/bash
# runner.sh BUCKET ROLE : the calibration passes on one box, uploading after each group, then power off.
# On the box only (run_aws.sh ships it with calbench in pkg.tgz, under systemd-run). CAL_PASSES (default 5) and
# CAL_GAP_S (default 900) set the pass count and the idle gap between passes.
set -u
B=$1; ROLE=$2
export AWS_USE_DUALSTACK_ENDPOINT=true AWS_DEFAULT_REGION=us-east-1 CAL_CPU=1 CAL_DIR=/var/tmp/cal
cd /var/tmp/cal
TOKEN=$(curl -s -X PUT http://[fd00:ec2::254]/latest/api/token -H 'X-aws-ec2-metadata-token-ttl-seconds: 21600' || curl -s -X PUT http://169.254.169.254/latest/api/token -H 'X-aws-ec2-metadata-token-ttl-seconds: 21600')
IID=$(curl -s -H "X-aws-ec2-metadata-token: $TOKEN" http://[fd00:ec2::254]/latest/meta-data/instance-id || curl -s -H "X-aws-ec2-metadata-token: $TOKEN" http://169.254.169.254/latest/meta-data/instance-id)
P=s3://$B/perf-temp-cal/$ROLE-$IID
up() { for i in 1 2 3 4 5; do aws s3 cp --only-show-errors "$1" "$P/$(basename "$1")" && return 0; sleep 5; done; echo "upload failed $1" >> runner.log; }
stat_line() { echo "$(date -u +%FT%TZ) $1 $(head -1 /proc/stat)" >> stat.txt; }
{
  echo "== role $ROLE iid $IID"; date -u; uname -a; lscpu; echo; cat /proc/cpuinfo; echo
  for c in /sys/devices/system/cpu/cpu0/cache/index*; do echo "$c level=$(cat $c/level) type=$(cat $c/type) size=$(cat $c/size) line=$(cat $c/coherency_line_size) ways=$(cat $c/ways_of_associativity) sets=$(cat $c/number_of_sets) shared=$(cat $c/shared_cpu_list)"; done
  grep . /sys/devices/system/cpu/vulnerabilities/*; echo
  cat /sys/devices/system/cpu/cpu0/regs/identification/midr_el1 2>/dev/null
  free -m; cat /proc/meminfo; df -h /; lsblk -o NAME,SIZE,MODEL 2>/dev/null; cat /proc/cmdline
  cat /sys/kernel/mm/transparent_hugepage/enabled /sys/devices/system/clocksource/clocksource0/current_clocksource
  ls /sys/devices/system/cpu/cpuidle 2>/dev/null; cat /sys/devices/system/cpu/cpuidle/current_driver 2>/dev/null
  ip -6 addr; systemctl list-units --type=service --state=running --no-pager
} > spec_$ROLE.txt 2>&1
up spec_$ROLE.txt
NP=${CAL_PASSES:-5}
for pass in $(seq 1 $NP); do
  GROUPS_THIS="env clock core kernel net wake real str malloc mem stream ebs"
  [ $pass = 1 ] || [ $pass = $NP ] && GROUPS_THIS="$GROUPS_THIS steal"
  for g in $GROUPS_THIS; do
    stat_line "pass$pass-$g-start"
    s=$(date +%s)
    ./calbench $g $pass > p${pass}_$g.tsv 2> p${pass}_$g.err; rc=$?
    stat_line "pass$pass-$g-end"
    echo "$(date -u +%FT%TZ) pass $pass group $g rc=$rc secs=$(( $(date +%s)-s ))" >> runner.log
    up p${pass}_$g.tsv; [ -s p${pass}_$g.err ] && up p${pass}_$g.err
    up runner.log; up stat.txt
  done
  [ $pass = $NP ] || sleep ${CAL_GAP_S:-900}
done
echo "$(date -u +%FT%TZ) ALLDONE" >> runner.log; up runner.log
tar czf all_$ROLE.tgz *.tsv *.txt runner.log; up all_$ROLE.tgz
sync; shutdown -h now
