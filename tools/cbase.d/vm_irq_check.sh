#!/bin/bash
# vm_irq_check.sh: the NIC interrupts' layout on a group's VM, read and changed by nothing, run by SSM
# after vm_irq_setup.sh (vm.py sends it as it is). For each queue interrupt (<if>-Tx-Rx-<k>) and the
# management one (ena-mgmnt): its number, smp_affinity_list and effective_affinity_list; irqbalance's
# state (systemctl is-active, is-enabled); and one verdict line: "one queue per vCPU" when there are as
# many queues as vCPUs (2 or more), each queue's affinity is one vCPU, every vCPU has one, and
# irqbalance is not active; otherwise "NOT one queue per vCPU: <why>".
# It prints "CBASE: " lines alone (vm.py prints those). CBASE_VM_ROOT is unset on the instance;
# cbase_vm_test.sh runs it under a scratch root with a stand-in systemctl.
set -u
R="${CBASE_VM_ROOT:-}"
say() { echo "CBASE: irq: $*"; }
ints="$R/proc/interrupts"
[ -r "$ints" ] || { say "NOT one queue per vCPU: no $ints"; exit 0; }
ncpu="$(awk 'NR == 1 { n = 0; for (i = 1; i <= NF; i++) if ($i ~ /^CPU[0-9]+$/) n++; print n; exit }' "$ints")"
active="$(systemctl is-active irqbalance.service 2> /dev/null)"
enabled="$(systemctl is-enabled irqbalance.service 2> /dev/null)"
say "irqbalance ${active:-unknown}, ${enabled:-unknown}"
why=""
cpus=""
nq=0
while read -r irq name; do
  [ -n "$irq" ] || continue
  aff="$(cat "$R/proc/irq/$irq/smp_affinity_list" 2> /dev/null)"
  eff="$(cat "$R/proc/irq/$irq/effective_affinity_list" 2> /dev/null)"
  say "$name (irq $irq): affinity ${aff:-unreadable}, effective ${eff:-unreadable}"
  case "$name" in
    ena-mgmnt*) continue ;;
  esac
  nq=$((nq + 1))
  case "$aff" in
    '' | *[!0-9]*) why="${why:+$why; }$name's affinity is ${aff:-unreadable}, not one vCPU" ;;
    *) cpus="$cpus $aff" ;;
  esac
done <<< "$(awk '$NF ~ /^[A-Za-z0-9_.]+-Tx-Rx-[0-9]+$/ || $NF ~ /^ena-mgmnt/ { irq = $1; sub(/:$/, "", irq); n = $NF; sub(/@.*/, "", n); print irq, n }' "$ints")"
[ "${ncpu:-0}" -ge 2 ] || why="${why:+$why; }${ncpu:-0} vCPU(s)"
[ "$nq" = "${ncpu:-0}" ] || why="${why:+$why; }$nq queue(s) on ${ncpu:-0} vCPUs"
if [ -z "$why" ]; then
  [ "$(tr ' ' '\n' <<< "$cpus" | sed '/^$/d' | sort -n | tr '\n' ' ')" = "$(seq 0 $((ncpu - 1)) | tr '\n' ' ')" ] || why="the queues' vCPUs are$cpus, not each vCPU once"
fi
[ "$active" != active ] || why="${why:+$why; }irqbalance is active"
if [ -z "$why" ]; then
  say "one queue per vCPU ($nq queues on $ncpu vCPUs), irqbalance not active"
else
  say "NOT one queue per vCPU: $why"
fi
