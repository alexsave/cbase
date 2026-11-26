#!/bin/bash
# mac_watchdog_test.sh - the Mac server's watchdog (mac_watchdog.sh) run against stub macOS commands
# (pmset, curl, ping, networksetup, tailscale, launchctl, df, sysctl, scutil, shutdown, sleep) and a scratch
# state and config directory. No network, no root, and it runs on Linux as on macOS.
#
# Covered: a healthy run checks in to the plain URL with the status line; on battery it checks in to /fail;
# at the shutdown level on battery it shuts down and does not check in; offline it counts runs, cycles Wi-Fi
# from the second, reboots at the sixth, and resets the count; the reboot waits out the first hour of uptime
# and the 6 hours since the last one; back online the count resets; Tailscale that does not answer is
# kickstarted, and /fail is used only when it stays down; a full disk checks in to /fail; with no URL set
# nothing is sent.
#
# Mutation checks (each change made to mac_watchdog.sh in turn, the test red on the assertion named, then
# restored): the shutdown -h line deleted -> "battery at 8% shuts down"; "-le" in the battery test made
# "-lt" -> "battery at 8% shuts down"; WIFI_CYCLE_AFTER=2 made 3 -> "the second offline run cycles Wi-Fi";
# the uptime condition deleted -> "no reboot in the first hour of uptime"; the reboot-interval condition
# deleted -> "no second reboot within 6 hours"; write_state offline_runs 0 on the online branch deleted ->
# "back online resets the count"; the kickstart line deleted -> "tailscale down is kickstarted"; the
# add_issue for a full disk deleted -> "a full disk checks in to /fail"; the boot time read with a greedy
# ".*sec = " (which matches usec) -> "no reboot in the first hour of uptime".
# The stub bodies are written to files and expand when the stub runs, so they are single-quoted.
# shellcheck disable=SC2016
set -u
here="$(cd "$(dirname "$0")/.." && pwd)"
watchdog="$here/mac_watchdog.sh"
fails=0
check() { if [ "$2" = "$3" ]; then echo "ok   $1"; else echo "FAIL $1: got [$2] want [$3]"; fails=$((fails + 1)); fi; }

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
mkdir "$tmp/bin"
export FAKE_LOG="$tmp/calls.log" FAKE_DIR="$tmp"

stub() {
    printf '#!/bin/bash\n%s\n' "$2" > "$tmp/bin/$1"
    chmod +x "$tmp/bin/$1"
}
stub pmset '
echo "pmset $*" >> "$FAKE_LOG"
if [ "$2" = ps ]; then echo "Now drawing from '\''$FAKE_POWER'\''"; else echo " -InternalBattery-0 (id=1)	$FAKE_BATT%; charging; present: true"; fi'
stub curl '
echo "curl $*" >> "$FAKE_LOG"
case "$*" in
    *captive.apple.com*) [ "$FAKE_ONLINE" = 1 ] && echo "<HTML>Success</HTML>"; exit 0 ;;
esac
url=""; data=""; prev=""
for a in "$@"; do
    [ "$prev" = --data-raw ] && data="$a"
    case "$a" in https://*) url="$a" ;; esac
    prev="$a"
done
echo "$url|$data" >> "$FAKE_DIR/checkins"'
stub ping 'echo "ping $*" >> "$FAKE_LOG"; [ "$FAKE_ONLINE" = 1 ]'
stub networksetup '
echo "networksetup $*" >> "$FAKE_LOG"
[ "$1" = -listallhardwareports ] && printf "Hardware Port: Ethernet\nDevice: en3\n\nHardware Port: Wi-Fi\nDevice: en0\n"
exit 0'
stub tailscale '
if [ "$FAKE_TS" = 1 ] || { [ -f "$FAKE_DIR/kicked" ] && [ "$FAKE_TS_RECOVERS" = 1 ]; }; then
    [ "$1" = ip ] && echo 100.64.0.7
    exit 0
fi
exit 1'
stub launchctl 'echo "launchctl $*" >> "$FAKE_LOG"; [ "$1" = kickstart ] && touch "$FAKE_DIR/kicked"; exit 0'
stub df 'printf "Filesystem 1024-blocks Used Available Capacity Mounted on\n/dev/disk3s1 100 50 50 $FAKE_DISK%% /System/Volumes/Data\n"'
stub sysctl 'echo "{ sec = $FAKE_BOOT, usec = 0 } Thu Jan  1 00:00:00 2026"'
stub scutil 'echo us-east-3'
stub shutdown 'echo "shutdown $*" >> "$FAKE_LOG"'
stub sleep ':'

export MAC_WATCHDOG_PATH="$tmp/bin:/usr/bin:/bin"
export MAC_WATCHDOG_STATE_DIR="$tmp/state" MAC_WATCHDOG_CONF_DIR="$tmp/conf"
now=$(date +%s)

# Each case starts from a healthy, plugged-in, online Mac that booted 2 hours ago.
fresh() {
    rm -rf "$tmp/state" "$tmp/conf" "$tmp/kicked" "$tmp/checkins" "$FAKE_LOG"
    mkdir -p "$tmp/conf"
    echo "https://hc.example/abc" > "$tmp/conf/healthcheck-url"
    export FAKE_POWER="AC Power" FAKE_BATT=80 FAKE_ONLINE=1 FAKE_TS=1 FAKE_TS_RECOVERS=0 FAKE_DISK=40
    export FAKE_BOOT=$((now - 7200))
    touch "$FAKE_LOG"
}
run() { bash "$watchdog" > "$tmp/out" 2>&1; }
checkin_url() { cut -d'|' -f1 "$tmp/checkins" 2>/dev/null; }
checkin_data() { cut -d'|' -f2 "$tmp/checkins" 2>/dev/null; }
calls() { grep -c "$1" "$FAKE_LOG"; }
state() { cat "$tmp/state/$1" 2>/dev/null; }

# ---- healthy ------------------------------------------------------------------
fresh; run
check "a healthy run checks in to the plain URL" "$(checkin_url)" "https://hc.example/abc"
check "the status line names host, uptime, power, battery, disk and tailscale" \
    "$(checkin_data)" "host=us-east-3 uptime=2h power=AC Power battery=80% disk=40% tailscale=100.64.0.7"
check "a healthy run does not shut down" "$(calls shutdown)" "0"
check "a healthy run does not touch launchctl" "$(calls launchctl)" "0"

# ---- power --------------------------------------------------------------------
fresh; FAKE_POWER="Battery Power" FAKE_BATT=50; run
check "on battery checks in to /fail" "$(checkin_url)" "https://hc.example/abc/fail"
check "the /fail line says why" "$(checkin_data | grep -c 'issues: on battery at 50% (power outage?)')" "1"
check "on battery at 50% does not shut down" "$(calls shutdown)" "0"

fresh; FAKE_POWER="Battery Power" FAKE_BATT=8; run
check "battery at 8% shuts down" "$(grep -c '^shutdown -h now' "$FAKE_LOG")" "1"
check "a shutdown does not check in" "$(checkin_url)" ""

fresh; FAKE_BATT=5; run
check "5% on AC power does not shut down" "$(calls shutdown)" "0"

# ---- offline ------------------------------------------------------------------
fresh; FAKE_ONLINE=0; run
check "the first offline run counts 1" "$(state offline_runs)" "1"
check "the first offline run does not cycle Wi-Fi" "$(calls 'setairportpower')" "0"
check "an offline run does not check in" "$(checkin_url)" ""

fresh; FAKE_ONLINE=0; mkdir -p "$tmp/state"; echo 1 > "$tmp/state/offline_runs"; run
check "the second offline run cycles Wi-Fi" "$(grep 'setairportpower' "$FAKE_LOG" | tr '\n' ',')" \
    "networksetup -setairportpower en0 off,networksetup -setairportpower en0 on,"
check "the second offline run does not reboot" "$(calls shutdown)" "0"

fresh; FAKE_ONLINE=0; mkdir -p "$tmp/state"; echo 5 > "$tmp/state/offline_runs"; run
check "the sixth offline run reboots" "$(grep -c '^shutdown -r now' "$FAKE_LOG")" "1"
check "the reboot resets the count" "$(state offline_runs)" "0"
check "the reboot is recorded" "$([ "$(state last_watchdog_reboot)" -ge "$now" ] && echo yes)" "yes"

fresh; FAKE_ONLINE=0; FAKE_BOOT=$((now - 600)); mkdir -p "$tmp/state"; echo 5 > "$tmp/state/offline_runs"; run
check "no reboot in the first hour of uptime" "$(calls shutdown)" "0"
check "the count keeps going without a reboot" "$(state offline_runs)" "6"

fresh; FAKE_ONLINE=0; mkdir -p "$tmp/state"; echo 5 > "$tmp/state/offline_runs"
echo $((now - 3600)) > "$tmp/state/last_watchdog_reboot"; run
check "no second reboot within 6 hours" "$(calls shutdown)" "0"

fresh; FAKE_ONLINE=0; mkdir -p "$tmp/state"; echo 5 > "$tmp/state/offline_runs"
echo $((now - 30000)) > "$tmp/state/last_watchdog_reboot"; run
check "a reboot again after 6 hours" "$(grep -c '^shutdown -r now' "$FAKE_LOG")" "1"

fresh; mkdir -p "$tmp/state"; echo 4 > "$tmp/state/offline_runs"; run
check "back online resets the count" "$(state offline_runs)" "0"

# ---- Tailscale ----------------------------------------------------------------
fresh; FAKE_TS=0; run
check "tailscale down is kickstarted" "$(grep -c 'launchctl kickstart -k system/homebrew.mxcl.tailscale' "$FAKE_LOG")" "1"
check "tailscale still down checks in to /fail" "$(checkin_url)" "https://hc.example/abc/fail"
check "the /fail line names tailscale" "$(checkin_data | grep -c 'tailscale down after restart')" "1"

fresh; FAKE_TS=0 FAKE_TS_RECOVERS=1; run
check "tailscale that recovers checks in to the plain URL" "$(checkin_url)" "https://hc.example/abc"

# ---- disk ---------------------------------------------------------------------
fresh; FAKE_DISK=95; run
check "a full disk checks in to /fail" "$(checkin_url)" "https://hc.example/abc/fail"
check "the /fail line names the disk" "$(checkin_data | grep -c 'disk 95% full')" "1"

# ---- no URL -------------------------------------------------------------------
fresh; rm "$tmp/conf/healthcheck-url"; run
check "with no URL nothing is sent" "$(checkin_url)" ""

[ "$fails" -eq 0 ] || { echo "$fails failed"; exit 1; }
echo "all passed"
