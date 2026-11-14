#!/bin/bash
# vm_irq_setup.sh: the NIC interrupts' setup on a group's VM, run as root by SSM after an app's deploy,
# for an app whose cbase.conf sets CBASE_VM_IRQ (vm.py sends it; an app without the key sends nothing).
# vm_remote.py renders it with irq=split or irq=irqbalance set above this text, and with
# /usr/local/sbin/cbase-irq-split (vm_irq.sh) and cbase-irq.service (vm_irq.service) spliced in at
# @CBASE_IRQ_SH@ and @CBASE_IRQ_UNIT@.
#   split:      both files written when they differ from what is installed (a daemon-reload then),
#               the unit enabled (it runs at every boot) and run now (systemctl restart: a oneshot
#               runs again, writing the same values);
#   irqbalance: the unit stopped, disabled and removed if it is there, and irqbalance enabled and
#               started: the way back.
# The interrupts are the VM's, not one app's: the last deploy that names CBASE_VM_IRQ sets them.
# It never touches an app's unit, and a failure here is reported, never fatal to the deploy.
# It prints "CBASE: " lines alone (vm.py prints those). CBASE_VM_ROOT is unset on the instance;
# cbase_vm_test.sh runs it under a scratch root with a stand-in systemctl.
# shellcheck disable=SC2154 # irq: vm_remote.py sets it
set -uo pipefail
R="${CBASE_VM_ROOT:-}"
say() { echo "CBASE: irq: $*"; }
bin="$R/usr/local/sbin/cbase-irq-split"
unitf="$R/etc/systemd/system/cbase-irq.service"

if [ "$irq" = irqbalance ]; then
  if [ -f "$unitf" ]; then
    systemctl disable --now --quiet cbase-irq.service || true
    rm -f "$unitf" "$bin"
    systemctl daemon-reload
    say "cbase-irq.service stopped, disabled and removed"
  fi
  if systemctl enable --now --quiet irqbalance.service; then
    say "irqbalance enabled and started (CBASE_VM_IRQ = irqbalance)"
  else
    say "irqbalance could not be enabled and started"
    exit 1
  fi
  exit 0
fi

mkdir -p "$R/usr/local/sbin" "$R/etc/systemd/system"
cat > "$bin.new" << 'IRQSH'
@CBASE_IRQ_SH@
IRQSH
cat > "$unitf.new" << 'IRQUNIT'
@CBASE_IRQ_UNIT@
IRQUNIT
changed=""
for f in "$bin" "$unitf"; do
  if cmp -s "$f.new" "$f"; then rm -f "$f.new"; else mv "$f.new" "$f"; changed=1; fi
done
chmod 0755 "$bin"
if [ -n "$changed" ]; then
  systemctl daemon-reload
  say "cbase-irq.service and /usr/local/sbin/cbase-irq-split installed"
else
  say "cbase-irq.service and /usr/local/sbin/cbase-irq-split unchanged"
fi
systemctl enable --quiet cbase-irq.service || { say "cbase-irq.service could not be enabled"; exit 1; }
if systemctl restart cbase-irq.service; then
  say "cbase-irq.service ran (its log: journalctl -u cbase-irq)"
else
  say "cbase-irq.service failed (its log: journalctl -u cbase-irq); no app is affected"
  exit 1
fi
