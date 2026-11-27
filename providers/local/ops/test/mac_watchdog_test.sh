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
# nothing is sent; pf is enabled and the anchor loaded when it is empty (macOS's main ruleset first when that is
# missing), left alone when loaded, a failed load checks in to /fail, the abuse table is expired every run, and
# with no rules file pf is not touched; the status line carries the stable IPv6 address, never the temporary one.
#
# Mutation checks (each change made to mac_watchdog.sh in turn, the test red on the assertion named, then
# restored): the shutdown -h line deleted -> "battery at 8% shuts down"; "-le" in the battery test made
# "-lt" -> "battery at 8% shuts down"; WIFI_CYCLE_AFTER=2 made 3 -> "the second offline run cycles Wi-Fi";
# the uptime condition deleted -> "no reboot in the first hour of uptime"; the reboot-interval condition
# deleted -> "no second reboot within 6 hours"; write_state offline_runs 0 on the online branch deleted ->
# "back online resets the count"; the kickstart line deleted -> "tailscale down is kickstarted"; the
# add_issue for a full disk deleted -> "a full disk checks in to /fail"; the boot time read with a greedy
# ".*sec = " (which matches usec) -> "no reboot in the first hour of uptime"; the anchor load line deleted ->
# "an empty anchor is loaded"; the pfctl -e deleted -> "a disabled pf is enabled"; the expire line deleted ->
# "the abuse table is expired"; /secured/ dropped from ipv6_address -> "the status line names ..."; the last_report write deleted ->
# "the status line is kept for mac_setup.sh"; last_report written before the issues are added -> "the kept
# status line carries the issues"; the address check dropped from public_ipv4 -> "an answer that is not an
# address is left out"; the "!=" made "=" -> "a record that points elsewhere checks in to /fail"; the
# [ -n "$dns_ipv4" ] guard dropped -> "no answer from DNS is not an alarm"; tail -c made head -c -> "the cut
# keeps the end"; the size test made -ge 0 -> "a log under the cap is left alone"; the front door's "!" dropped ->
# "a front door that does not answer checks in to /fail"; the address comparison made "=" -> "a private Wi-Fi
# address checks in to /fail".
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
    *checkip.amazonaws.com*) echo "$FAKE_IPV4"; exit 0 ;;
    *--resolve*) [ "$FAKE_FRONT" = 1 ]; exit $? ;;
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
[ "$1" = -getmacaddress ] && echo "Ethernet Address: 74:8f:3c:c0:cc:af (Device: en0)"
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
stub pfctl '
echo "pfctl $*" >> "$FAKE_LOG"
case "$*" in
    "-s Anchors") [ "$FAKE_PF_MAIN" = 1 ] && echo "  com.apple" ;;
    *"-s rules") [ "$FAKE_PF_LOADED" = 1 ] && echo "pass in proto tcp from any to any port = 22" ;;
    "-s info") [ "$FAKE_PF_ENABLED" = 1 ] && echo "Status: Enabled for 0 days" || echo "Status: Disabled" ;;
    *"-f "*/pf.conf) [ "$FAKE_PF_LOAD_FAILS" = 1 ] && exit 1 ;;
esac
exit 0'
stub dig 'echo "dig $*" >> "$FAKE_LOG"; [ -n "$FAKE_DNS" ] && echo "$FAKE_DNS"; exit 0'
stub route 'printf "   route to: default\ninterface: en0\n"'
stub ifconfig 'printf "\tether $FAKE_MAC\n\tinet6 fe80::1%%en0 prefixlen 64 secured scopeid 0xb\n\tinet6 2601:db8::5ec prefixlen 64 autoconf temporary\n\tinet6 2601:db8::abc prefixlen 64 autoconf secured\n"'

export MAC_WATCHDOG_PATH="$tmp/bin:/usr/bin:/bin"
export MAC_WATCHDOG_STATE_DIR="$tmp/state" MAC_WATCHDOG_CONF_DIR="$tmp/conf"
now=$(date +%s)

# Each case starts from a healthy, plugged-in, online Mac that booted 2 hours ago.
fresh() {
    rm -rf "$tmp/state" "$tmp/conf" "$tmp/kicked" "$tmp/checkins" "$FAKE_LOG"
    mkdir -p "$tmp/conf"
    echo "https://hc.example/abc" > "$tmp/conf/healthcheck-url"
    echo "pass" > "$tmp/conf/pf.conf"
    export FAKE_IPV4=203.0.113.7 FAKE_DNS=203.0.113.7 FAKE_FRONT=1 FAKE_MAC=74:8f:3c:c0:cc:af
    export FAKE_PF_MAIN=1 FAKE_PF_LOADED=1 FAKE_PF_ENABLED=1 FAKE_PF_LOAD_FAILS=0
    export FAKE_POWER="AC Power" FAKE_BATT=80 FAKE_ONLINE=1 FAKE_TS=1 FAKE_TS_RECOVERS=0 FAKE_DISK=40
    export FAKE_BOOT=$((now - 7200))
    touch "$FAKE_LOG"
}
run() { bash "$watchdog" > "$tmp/out" 2>&1; }
checkin_url() { cut -d'|' -f1 "$tmp/checkins" 2>/dev/null; }
checkin_data() { cut -d'|' -f2 "$tmp/checkins" 2>/dev/null; }
calls() { grep -c -e "$1" "$FAKE_LOG"; }
state() { cat "$tmp/state/$1" 2>/dev/null; }

# ---- healthy ------------------------------------------------------------------
fresh; run
check "a healthy run checks in to the plain URL" "$(checkin_url)" "https://hc.example/abc"
check "the status line names host, uptime, power, battery, disk and tailscale" \
    "$(checkin_data)" "host=us-east-3 uptime=2h power=AC Power battery=80% disk=40% tailscale=100.64.0.7 ipv4=203.0.113.7 ipv6=2601:db8::abc"
check "the status line is kept for mac_setup.sh" "$(state last_report)" "$(checkin_data)"
check "a healthy run does not shut down" "$(calls shutdown)" "0"
check "a healthy run does not touch launchctl" "$(calls launchctl)" "0"

fresh; FAKE_IPV4="<html>captive portal</html>"; run
check "an answer that is not an address is left out" "$(checkin_data | grep -o 'ipv4=[^ ]*')" "ipv4="

# ---- the public name's A record ----------------------------------------------
fresh; echo "home.example.com" > "$tmp/conf/domain"; run
check "a record that matches checks in to the plain URL" "$(checkin_url)" "https://hc.example/abc"
check "the record is asked of a public resolver" "$(calls 'dig +short +time=5 +tries=2 A home.example.com @1.1.1.1')" "1"

fresh; echo "home.example.com" > "$tmp/conf/domain"; FAKE_DNS=198.51.100.9; run
check "a record that points elsewhere checks in to /fail" "$(checkin_url)" "https://hc.example/abc/fail"
check "the /fail line says what to point it at" \
    "$(checkin_data | grep -c 'the A record for home.example.com is 198.51.100.9 but the house is at 203.0.113.7: point it at 203.0.113.7')" "1"

fresh; echo "home.example.com" > "$tmp/conf/domain"; FAKE_DNS=""; run
check "no answer from DNS is not an alarm" "$(checkin_url)" "https://hc.example/abc"

fresh; echo "home.example.com" > "$tmp/conf/domain"; FAKE_DNS=198.51.100.9 FAKE_IPV4=""; run
check "no house address is not an alarm" "$(checkin_url)" "https://hc.example/abc"

fresh; FAKE_DNS=198.51.100.9; run
check "with no public name DNS is not asked" "$(calls dig)" "0"

# ---- the front door -----------------------------------------------------------
fresh; echo "home.example.com" > "$tmp/conf/domain"; run
check "the front door is asked through the house's address" "$(calls 'curl -fsS -m 10 -o /dev/null --resolve home.example.com:443:203.0.113.7 https://home.example.com/')" "1"
check "a front door that answers checks in to the plain URL" "$(checkin_url)" "https://hc.example/abc"

fresh; echo "home.example.com" > "$tmp/conf/domain"; FAKE_FRONT=0; run
check "a front door that does not answer checks in to /fail" "$(checkin_url)" "https://hc.example/abc/fail"
check "the /fail line names the front door" "$(checkin_data | grep -c 'https://home.example.com does not answer through the router (203.0.113.7:443)')" "1"

fresh; FAKE_FRONT=0; run
check "with no public name the front door is not asked" "$(calls '--resolve')" "0"

# ---- the Wi-Fi address --------------------------------------------------------
fresh; FAKE_MAC=de:cb:ad:ac:9b:b3; run
check "a private Wi-Fi address checks in to /fail" "$(checkin_url)" "https://hc.example/abc/fail"
check "the /fail line names both addresses" "$(checkin_data | grep -c 'Wi-Fi uses de:cb:ad:ac:9b:b3, not its hardware address 74:8f:3c:c0:cc:af')" "1"

# ---- capped logs --------------------------------------------------------------
fresh
{ head -c 6000000 /dev/zero | tr '\0' 'a'; printf 'THE-END'; } > "$tmp/big.log"
head -c 2000000 /dev/zero | tr '\0' 'b' > "$tmp/small.log"   # past the keep size, under the cap
printf '%s\n' "$tmp/big.log" "$tmp/small.log" "$tmp/missing.log" > "$tmp/conf/capped-logs"
run
check "a log past the cap is cut to the keep size" "$(wc -c < "$tmp/big.log" | tr -d ' ')" "1048576"
check "the cut keeps the end" "$(tail -c 7 "$tmp/big.log")" "THE-END"
check "a log under the cap is left alone" "$(wc -c < "$tmp/small.log" | tr -d ' ')" "2000000"
check "no .cap file is left behind" "$(find "$tmp" -name '*.cap' | wc -l | tr -d ' ')" "0"
check "a missing log is not an issue" "$(checkin_url)" "https://hc.example/abc"

# ---- firewall -----------------------------------------------------------------
fresh; run
check "a loaded anchor is left alone" "$(calls 'pfctl -q -a com.apple/250.cbase-mac -f')" "0"
check "a loaded main ruleset is left alone" "$(calls 'pfctl -q -f /etc/pf.conf')" "0"
check "an enabled pf is left alone" "$(calls 'pfctl -q -e')" "0"
check "the abuse table is expired" "$(calls 'pfctl -q -a com.apple/250.cbase-mac -t cbase_ssh_abuse -T expire 86400')" "1"

fresh; FAKE_PF_LOADED=0; run
check "an empty anchor is loaded" "$(calls "pfctl -q -a com.apple/250.cbase-mac -f $tmp/conf/pf.conf")" "1"
check "a loaded anchor checks in to the plain URL" "$(checkin_url)" "https://hc.example/abc"

fresh; FAKE_PF_LOADED=0 FAKE_PF_MAIN=0; run
check "a missing main ruleset is loaded from macOS's" "$(calls 'pfctl -q -f /etc/pf.conf')" "1"

fresh; FAKE_PF_ENABLED=0; run
check "a disabled pf is enabled" "$(calls 'pfctl -q -e')" "1"

fresh; FAKE_PF_LOADED=0 FAKE_PF_LOAD_FAILS=1; run
check "rules that do not load check in to /fail" "$(checkin_url)" "https://hc.example/abc/fail"
check "the /fail line names pf" "$(checkin_data | grep -c 'pf rules did not load')" "1"

fresh; rm "$tmp/conf/pf.conf"; run
check "with no rules file pf is not touched" "$(calls pfctl)" "0"

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
check "tailscale down is kickstarted" "$(grep -c 'launchctl kickstart -k system/sh.brew.tailscale' "$FAKE_LOG")" "1"
check "tailscale still down checks in to /fail" "$(checkin_url)" "https://hc.example/abc/fail"
check "the /fail line names tailscale" "$(checkin_data | grep -c 'tailscale down after restart')" "1"

fresh; FAKE_TS=0 FAKE_TS_RECOVERS=1; run
check "tailscale that recovers checks in to the plain URL" "$(checkin_url)" "https://hc.example/abc"

# ---- disk ---------------------------------------------------------------------
fresh; FAKE_DISK=95; run
check "a full disk checks in to /fail" "$(checkin_url)" "https://hc.example/abc/fail"
check "the /fail line names the disk" "$(checkin_data | grep -c 'disk 95% full')" "1"
check "the kept status line carries the issues" "$(state last_report | grep -c 'issues: disk 95% full')" "1"

# ---- no URL -------------------------------------------------------------------
fresh; rm "$tmp/conf/healthcheck-url"; run
check "with no URL nothing is sent" "$(checkin_url)" ""

[ "$fails" -eq 0 ] || { echo "$fails failed"; exit 1; }
echo "all passed"
