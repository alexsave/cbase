#!/bin/bash
# mac_watchdog.sh - keeps an unattended Mac server (MAC_RUNBOOK.md) reachable.
# Runs as root from launchd every 5 minutes (mac_watchdog.plist), installed by mac_setup.sh.
#
# Each run, in order:
#   * on battery at or below SHUTDOWN_BATTERY_PCT: shut down cleanly (the Mac boots again when power returns);
#   * offline (Apple's captive check and a ping both fail): count the run; from WIFI_CYCLE_AFTER runs on,
#     turn Wi-Fi off and on; from REBOOT_AFTER runs on, still offline, reboot, at most once per
#     MIN_SECONDS_BETWEEN_REBOOTS and never in the first hour of uptime, so a long ISP outage is not a loop;
#   * pf enabled and the anchor $PF_ANCHOR loaded from $CONF_DIR/pf.conf (mac_pf.conf), reloaded when
#     something else (Internet Sharing, an update) flushed it, and its SSH abuse table's day-old entries expired;
#   * online: Tailscale that does not answer is restarted (launchctl kickstart);
#   * then a check-in to the dead-man's switch: a POST of one status line to the URL in
#     $CONF_DIR/healthcheck-url, or to that URL + "/fail" with the issues (on battery, disk full,
#     Tailscale down, pf rules that do not load). The switch alerts when check-ins stop or fail. The line
#     carries the house's public IPv4 address (the router forwards 22 to the Mac) and the Mac's stable IPv6
#     address, so the switch's dashboard shows where to SSH when Tailscale is down.
#
# The test (test/mac_watchdog_test.sh) runs it against stub commands through MAC_WATCHDOG_PATH and a
# scratch MAC_WATCHDOG_STATE_DIR and MAC_WATCHDOG_CONF_DIR.

PATH="${MAC_WATCHDOG_PATH:-/usr/bin:/bin:/usr/sbin:/sbin:/opt/homebrew/bin}"
STATE_DIR="${MAC_WATCHDOG_STATE_DIR:-/var/db/cbase-mac}"
CONF_DIR="${MAC_WATCHDOG_CONF_DIR:-/etc/cbase-mac}"
HC_URL_FILE="$CONF_DIR/healthcheck-url"
# Homebrew's label for `sudo brew services start tailscale` (Homebrew 5; mac_setup.sh's Verify checks it is loaded).
TAILSCALE_JOB=system/sh.brew.tailscale
PF_ANCHOR=com.apple/250.cbase-mac
PF_RULES="$CONF_DIR/pf.conf"
PF_ABUSE_EXPIRE_S=86400

WIFI_CYCLE_AFTER=2
REBOOT_AFTER=6
MIN_SECONDS_BETWEEN_REBOOTS=21600
SHUTDOWN_BATTERY_PCT=8
DISK_ALERT_PCT=90

mkdir -p "$STATE_DIR"

log() {
    echo "$(date '+%Y-%m-%d %H:%M:%S') $*"
}

read_state() {
    cat "$STATE_DIR/$1" 2>/dev/null || echo 0
}

write_state() {
    echo "$2" > "$STATE_DIR/$1"
}

wifi_device() {
    networksetup -listallhardwareports | awk '/Hardware Port: Wi-Fi/ { getline; print $2 }'
}

is_online() {
    curl -s -m 10 http://captive.apple.com/hotspot-detect.html | grep -q Success && return 0
    ping -c 2 -t 5 1.1.1.1 > /dev/null 2>&1
}

# The house's public IPv4 address, as AWS sees it (the Mac itself only has the router's private one).
public_ipv4() {
    curl -4 -s -m 10 https://checkip.amazonaws.com | grep -E '^[0-9]+(\.[0-9]+){3}$'
}

# The stable global IPv6 address on the default route's interface: macOS marks it "secured" (RFC 7217);
# the rotating privacy addresses are "temporary" and never "secured".
ipv6_address() {
    local dev
    dev=$(route -n get -inet6 default 2>/dev/null | awk '/interface:/ { print $2 }')
    [ -n "$dev" ] || return 0
    ifconfig "$dev" | awk '$1 == "inet6" && /secured/ && $2 !~ /^(fe80|fd)/ { print $2; exit }'
}

uptime_seconds() {
    local boot
    # "{ sec = 1764100000, usec = 123 } ...": the first number (a greedy ".*sec =" would match usec).
    boot=$(sysctl -n kern.boottime | sed -E 's/^[^0-9]*([0-9]+).*/\1/')
    echo $(( $(date +%s) - boot ))
}

issues=""
add_issue() {
    issues="$issues$1; "
}

# ---- power --------------------------------------------------------------------

power_source=$(pmset -g ps | head -1 | sed -E "s/.*'(.*)'.*/\1/")
battery_pct=$(pmset -g batt | grep -o '[0-9]*%' | head -1 | tr -d '%')
if [ "$power_source" != "AC Power" ]; then
    add_issue "on battery at ${battery_pct}% (power outage?)"
    if [ -n "$battery_pct" ] && [ "$battery_pct" -le "$SHUTDOWN_BATTERY_PCT" ]; then
        log "battery at ${battery_pct}% on battery power, shutting down cleanly"
        shutdown -h now "mac_watchdog: battery at ${battery_pct}%"
        exit 0
    fi
fi

# ---- firewall (before the network: it holds while offline too) ----------------

if [ -s "$PF_RULES" ]; then
    # The main ruleset holds the anchor "com.apple/*" that ours hangs under; with none loaded, load macOS's.
    pfctl -s Anchors 2>/dev/null | grep -q 'com.apple' || pfctl -q -f /etc/pf.conf
    if ! pfctl -a "$PF_ANCHOR" -s rules 2>/dev/null | grep -q .; then
        log "loading pf anchor $PF_ANCHOR"
        pfctl -q -a "$PF_ANCHOR" -f "$PF_RULES" || add_issue "pf rules did not load"
    fi
    pfctl -s info 2>/dev/null | grep -q 'Status: Enabled' || pfctl -q -e
    pfctl -q -a "$PF_ANCHOR" -t cbase_ssh_abuse -T expire "$PF_ABUSE_EXPIRE_S"
fi

# ---- network ------------------------------------------------------------------

if is_online; then
    write_state offline_runs 0
else
    offline_runs=$(( $(read_state offline_runs) + 1 ))
    write_state offline_runs "$offline_runs"
    log "offline (run $offline_runs)"

    if [ "$offline_runs" -ge "$WIFI_CYCLE_AFTER" ]; then
        dev=$(wifi_device)
        if [ -n "$dev" ]; then
            log "cycling Wi-Fi on $dev"
            networksetup -setairportpower "$dev" off
            sleep 5
            networksetup -setairportpower "$dev" on
            sleep 20
        fi
    fi

    if ! is_online && [ "$offline_runs" -ge "$REBOOT_AFTER" ]; then
        now=$(date +%s)
        last_reboot=$(read_state last_watchdog_reboot)
        if [ "$(uptime_seconds)" -gt 3600 ] && [ $(( now - last_reboot )) -gt "$MIN_SECONDS_BETWEEN_REBOOTS" ]; then
            log "offline for $offline_runs runs, rebooting"
            write_state last_watchdog_reboot "$now"
            write_state offline_runs 0
            shutdown -r now "mac_watchdog: network down"
            exit 0
        fi
    fi
    # The check-in has nowhere to go while offline; the switch notices the silence.
    exit 0
fi

# ---- Tailscale ----------------------------------------------------------------

if ! tailscale status > /dev/null 2>&1; then
    log "tailscale not answering, restarting tailscaled"
    launchctl kickstart -k "$TAILSCALE_JOB"
    sleep 15
    if ! tailscale status > /dev/null 2>&1; then
        add_issue "tailscale down after restart"
    fi
fi
tailscale_ip=$(tailscale ip -4 2>/dev/null | head -1)

# ---- disk ---------------------------------------------------------------------

disk_pct=$(df -P /System/Volumes/Data | awk 'NR == 2 { gsub("%", "", $5); print $5 }')
if [ -n "$disk_pct" ] && [ "$disk_pct" -ge "$DISK_ALERT_PCT" ]; then
    add_issue "disk ${disk_pct}% full"
fi

# ---- check-in -----------------------------------------------------------------

report="host=$(scutil --get LocalHostName) uptime=$(( $(uptime_seconds) / 3600 ))h power=$power_source battery=${battery_pct}% disk=${disk_pct}% tailscale=${tailscale_ip:-none} ipv4=$(public_ipv4) ipv6=$(ipv6_address)"

if [ -n "$issues" ]; then
    report="$report issues: $issues"
    log "unhealthy: $issues"
fi
write_state last_report "$report"
if [ -s "$HC_URL_FILE" ]; then
    hc_url=$(cat "$HC_URL_FILE")
    [ -z "$issues" ] || hc_url="$hc_url/fail"
    curl -fsS -m 10 --retry 3 -o /dev/null --data-raw "$report" "$hc_url" || log "check-in failed"
fi
