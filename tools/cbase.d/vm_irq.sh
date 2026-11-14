#!/bin/bash
# cbase-irq-split: one NIC queue interrupt per vCPU, with irqbalance stopped (cbase.conf CBASE_VM_IRQ =
# split). ./cbase deploy installs this file as /usr/local/sbin/cbase-irq-split, and cbase-irq.service
# (vm_irq.service, a oneshot that no app's unit depends on) runs it now and at every boot.
#
# The layout is the one measured on a t4g.nano (the foolish benchmark campaign, trips 2 and 3, "split"):
# the NIC's queue k on vCPU (k + 1) mod N and its management interrupt on vCPU 1 mod N, irqbalance
# stopped and disabled. With N queues on N vCPUs that is one queue per vCPU, the same layout at every
# boot, where irqbalance's own choices made throughput vary by about 10 percent from boot to boot.
#
# Nothing is changed, and the exit is 0, when the layout is not the expected one: no /proc/interrupts,
# not exactly one NIC with <if>-Tx-Rx-<k> queues (ENA's names), queues not numbered 0 to N-1, fewer or
# more queues than vCPUs, fewer than 2 vCPUs, or an affinity file that cannot be written. All of that
# is checked before irqbalance is touched. Should a write fail after irqbalance was stopped, irqbalance
# is started (and enabled, if it was) again and the exit is 1, so the unit shows failed. Run again, it
# writes the same values (idempotent). Every line it prints goes to the journal: journalctl -u cbase-irq.
# CBASE_VM_ROOT is unset on the instance; cbase_vm_test.sh runs it under a scratch root.
set -u
R="${CBASE_VM_ROOT:-}"
say() { echo "cbase-irq: $*"; }
leave() { say "$*: nothing changed"; exit 0; }

ints="$R/proc/interrupts"
[ -r "$ints" ] || leave "no $ints"
# The vCPUs: the CPU<n> columns of the header line.
ncpu="$(awk 'NR == 1 { n = 0; for (i = 1; i <= NF; i++) if ($i ~ /^CPU[0-9]+$/) n++; print n; exit }' "$ints")"
# The queues, "<irq> <name> <k>", one per line whose last field is <if>-Tx-Rx-<k>; and the management
# interrupt, ena-mgmnt (its last field is ena-mgmnt@pci:...).
queues="$(awk '$NF ~ /^[A-Za-z0-9_.]+-Tx-Rx-[0-9]+$/ { irq = $1; sub(/:$/, "", irq); k = $NF; sub(/.*-Tx-Rx-/, "", k); print irq, $NF, k }' "$ints")"
mgmt="$(awk '$NF ~ /^ena-mgmnt/ { irq = $1; sub(/:$/, "", irq); print irq; exit }' "$ints")"
[ "${ncpu:-0}" -ge 2 ] || leave "${ncpu:-0} vCPU(s), not 2 or more"
[ -n "$queues" ] || leave "no <if>-Tx-Rx-<k> queue interrupts in /proc/interrupts"
nics="$(awk '{ n = $2; sub(/-Tx-Rx-[0-9]+$/, "", n); print n }' <<< "$queues" | sort -u | wc -l | tr -d ' ')"
[ "$nics" = 1 ] || leave "queue interrupts of $nics NICs, not one"
nq="$(wc -l <<< "$queues" | tr -d ' ')"
[ "$nq" = "$ncpu" ] || leave "$nq queue(s) on $ncpu vCPUs, not one per vCPU"
[ "$(awk '{ print $3 }' <<< "$queues" | sort -n | tr '\n' ' ')" = "$(seq 0 $((nq - 1)) | tr '\n' ' ')" ] || leave "the queues are not numbered 0 to $((nq - 1))"

# The plan, "<irq> <name> <vCPU>", every affinity file checked writable before anything changes.
plan="$(awk -v n="$ncpu" '{ print $1, $2, ($3 + 1) % n }' <<< "$queues")"
[ -z "$mgmt" ] || plan="$plan"$'\n'"$mgmt ena-mgmnt $((1 % ncpu))"
while read -r irq name cpu; do
  case "$irq" in '' | *[!0-9]*) leave "$name's interrupt number \"$irq\" is not a number" ;; esac
  [ -w "$R/proc/irq/$irq/smp_affinity_list" ] || leave "$R/proc/irq/$irq/smp_affinity_list ($name) cannot be written"
done <<< "$plan"

# irqbalance stopped and disabled first, so it does not move them again.
was_enabled=0
systemctl is-enabled --quiet irqbalance.service 2> /dev/null && was_enabled=1
systemctl stop irqbalance.service 2> /dev/null || true
systemctl disable --quiet irqbalance.service 2> /dev/null || true
n=0
while systemctl is-active --quiet irqbalance.service && [ "$n" -lt 25 ]; do sleep 0.2; n=$((n + 1)); done
undo() {
  say "$*: irqbalance started again"
  [ "$was_enabled" = 0 ] || systemctl enable --quiet irqbalance.service || true
  systemctl start irqbalance.service || true
  exit 1
}
! systemctl is-active --quiet irqbalance.service || undo "irqbalance is still running"
while read -r irq name cpu; do
  f="$R/proc/irq/$irq/smp_affinity_list"
  echo "$cpu" > "$f" || undo "writing $cpu to $f ($name) failed"
  got="$(cat "$f")"
  [ "$got" = "$cpu" ] || undo "$f ($name) reads $got after $cpu was written"
  say "$name (irq $irq) on vCPU $cpu"
done <<< "$plan"
say "one queue interrupt per vCPU ($nq queues on $ncpu vCPUs), irqbalance stopped and disabled"
