#!/bin/bash
# vm_boot_logs_test.sh - vm_boot.sh's step 2b (volatile journald, chrony's own
# logs off) run against a temp root with a stub systemctl: the drop-in's exact
# content, the old journal and chrony logs removed, a service restarted only
# when its file changed, and a second run changing nothing. No AWS calls.
set -u
here="$(cd "$(dirname "$0")/.." && pwd)"
fails=0
check() { if [ "$2" = "$3" ]; then echo "ok   $1"; else echo "FAIL $1: got [$2] want [$3]"; fails=$((fails + 1)); fi; }

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/bin"
# shellcheck disable=SC2016  # the stub expands its own variables when it runs
printf '%s\n' '#!/bin/bash' 'echo "$*" >> "$VM_ROOT/calls"' > "$tmp/bin/systemctl"
chmod +x "$tmp/bin/systemctl"
sed -n '/^# 2b\./,/^# end 2b/p' "$here/vm_boot.sh" > "$tmp/step.sh"
check "the step was found" "$(grep -c '^jconf=' "$tmp/step.sh")" "1"

export VM_ROOT="$tmp/root"
mkdir -p "$VM_ROOT/etc" "$VM_ROOT/var/log/journal/abc" "$VM_ROOT/var/log/chrony"
: > "$VM_ROOT/var/log/journal/abc/system.journal"
: > "$VM_ROOT/var/log/chrony/tracking.log"
printf 'pool x iburst\nlogdir /var/log/chrony\nlog measurements statistics tracking\n' > "$VM_ROOT/etc/chrony.conf"
run() { PATH="$tmp/bin:$PATH" bash "$tmp/step.sh"; }

run
want='[Journal]
Storage=volatile
RuntimeMaxUse=16M
RuntimeKeepFree=32M
ForwardToSyslog=no'
check "drop-in content" "$(cat "$VM_ROOT/etc/systemd/journald.conf.d/cbase.conf")" "$want"
check "old journal removed" "$(find "$VM_ROOT/var/log/journal" -type f | wc -l | tr -d ' ')" "0"
check "chrony logs removed" "$(find "$VM_ROOT/var/log/chrony" -type f | wc -l | tr -d ' ')" "0"
check "log line commented, logdir kept" "$(grep -c '^#log measurements\|^logdir' "$VM_ROOT/etc/chrony.conf")" "2"
check "no active log line" "$(grep -c '^log ' "$VM_ROOT/etc/chrony.conf")" "0"
check "both services restarted once" "$(sort "$VM_ROOT/calls" | tr '\n' ,)" "restart chronyd,restart systemd-journald,"

# Second run: nothing changed, nothing restarted, new files are left alone.
: > "$VM_ROOT/calls"
mkdir -p "$VM_ROOT/var/log/journal/new"
run
check "idempotent: no restarts" "$(wc -c < "$VM_ROOT/calls" | tr -d ' ')" "0"
check "idempotent: journal untouched" "$(find "$VM_ROOT/var/log/journal" -type d -name new)" "$VM_ROOT/var/log/journal/new"

if [ "$fails" -eq 0 ]; then echo "vm_boot_logs: all pass"; else echo "vm_boot_logs: $fails failed"; exit 1; fi
