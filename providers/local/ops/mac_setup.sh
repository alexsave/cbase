#!/bin/bash
# mac_setup.sh - turns a MacBook into an unattended server (MAC_RUNBOOK.md): it comes back on the network by
# itself after any reboot, and is reached and watched from anywhere for months without anyone at the keyboard.
# Idempotent: re-run it after any change. It asks questions, so run it in a Terminal window:
#
#   sudo providers/local/ops/mac_setup.sh [hostname]     (hostname defaults to us-east-3)
#
# Needs Homebrew's tailscale and batt installed first: brew install tailscale batt

set -u

NAME="${1:-us-east-3}"
HERE="$(cd "$(dirname "$0")" && pwd)"
BREW=/opt/homebrew/bin/brew
TAILSCALE=/opt/homebrew/bin/tailscale
BATT=/opt/homebrew/bin/batt
BATTERY_LIMIT=80
LABEL=com.cbase.mac-watchdog
CONF_DIR=/etc/cbase-mac

step() { printf '\n\033[1m==> %s\033[0m\n' "$*"; }
warn() { printf '\033[33mWARN: %s\033[0m\n' "$*"; }

if [ "$(id -u)" -ne 0 ] || [ -z "${SUDO_USER:-}" ]; then
    echo "Run with sudo from your own account: sudo $0"
    exit 1
fi
for t in "$TAILSCALE" "$BATT"; do
    [ -x "$t" ] || { echo "$t is missing: brew install tailscale batt"; exit 1; }
done
USER_HOME=$(dscl . -read "/Users/$SUDO_USER" NFSHomeDirectory | awk '{ print $2 }')

step "Hostname: $NAME"
scutil --set ComputerName "$NAME"
scutil --set LocalHostName "$NAME"
scutil --set HostName "$NAME"
dscacheutil -flushcache

step "Power: never sleep, lid closed included; restart after a freeze or a power loss"
pmset -a sleep 0 disksleep 0 standby 0 powernap 0 tcpkeepalive 1 womp 1 ttyskeepawake 1
pmset -a displaysleep 5
pmset -a autorestart 1 2>/dev/null || warn "this model has no restart-after-power-loss setting; the battery covers outages"
pmset -a disablesleep 1
systemsetup -setrestartfreeze on > /dev/null 2>&1 || warn "could not turn on restart after a freeze"
# Apple silicon laptops boot when power is connected unless BootPreference says otherwise.
nvram -d BootPreference 2>/dev/null
nvram auto-boot=true

step "Software updates: security and minor macOS updates install by themselves"
SU=/Library/Preferences/com.apple.SoftwareUpdate
defaults write $SU AutomaticCheckEnabled -bool true
defaults write $SU AutomaticDownload -bool true
defaults write $SU CriticalUpdateInstall -bool true
defaults write $SU ConfigDataInstall -bool true
defaults write $SU AutomaticallyInstallMacOSUpdates -bool true
defaults write /Library/Preferences/com.apple.commerce AutoUpdate -bool true

step "Watchdog (a LaunchDaemon, every 5 minutes)"
install -d -m 755 /usr/local/libexec/cbase-mac "$CONF_DIR"
install -m 755 "$HERE/mac_watchdog.sh" /usr/local/libexec/cbase-mac/mac_watchdog.sh
install -m 644 -o root -g wheel "$HERE/mac_watchdog.plist" /Library/LaunchDaemons/$LABEL.plist
install -m 644 "$HERE/mac_newsyslog.conf" /etc/newsyslog.d/cbase-mac.conf
if pfctl -n -a com.apple/250.cbase-mac -f "$HERE/mac_pf.conf" 2>/dev/null; then
    install -m 644 "$HERE/mac_pf.conf" "$CONF_DIR/pf.conf"
else
    warn "pf rejected mac_pf.conf; the watchdog runs without the SSH rate limit"
fi
launchctl bootout system/$LABEL 2>/dev/null
launchctl bootstrap system /Library/LaunchDaemons/$LABEL.plist

step "Tailscale (a system daemon: up at boot, before anyone logs in)"
"$BREW" services start tailscale > /dev/null
n=0
until [ -S /var/run/tailscaled.socket ] || [ $n -gt 30 ]; do
    sleep 1
    n=$((n + 1))
done
# --accept-dns=false: the server's own DNS never depends on Tailscale.
if "$TAILSCALE" status > /dev/null 2>&1; then
    "$TAILSCALE" set --hostname="$NAME" --ssh --accept-dns=false
    echo "already logged in"
else
    echo "Open the URL below and log in to your tailnet:"
    "$TAILSCALE" up --hostname="$NAME" --ssh --accept-dns=false
fi

step "Battery: charge held at $BATTERY_LIMIT% (months at 100% wears and swells it)"
"$BREW" services start batt > /dev/null
n=0
until "$BATT" status > /dev/null 2>&1 || [ $n -gt 20 ]; do
    sleep 1
    n=$((n + 1))
done
"$BATT" limit $BATTERY_LIMIT || warn "batt could not set the limit"

step "SSH (Remote Login)"
AK="$USER_HOME/.ssh/authorized_keys"
echo "Paste the SSH public keys of the machines you will connect from, one per line, then an empty line."
echo "Press Enter at once to skip: Tailscale SSH works with your tailnet login alone."
while IFS= read -r key && [ -n "$key" ]; do
    install -d -m 700 -o "$SUDO_USER" -g staff "$USER_HOME/.ssh"
    touch "$AK"
    grep -qxF "$key" "$AK" || echo "$key" >> "$AK"
done
if [ -f "$AK" ]; then
    chown "$SUDO_USER":staff "$AK"
    chmod 600 "$AK"
fi
SSHD_CONF=/etc/ssh/sshd_config.d/050-cbase-mac.conf
{
    echo "# mac_setup.sh. 050- sorts before macOS's 100-macos.conf, and sshd keeps the first value it reads."
    echo "PermitRootLogin no"
    echo "ClientAliveInterval 60"
    echo "MaxAuthTries 3"
    echo "LoginGraceTime 20"
    if [ -s "$AK" ]; then
        echo "PasswordAuthentication no"
        echo "KbdInteractiveAuthentication no"
    fi
} > "$SSHD_CONF"
[ -s "$AK" ] || warn "authorized_keys is empty, so SSH still takes a password; re-run after adding keys to turn that off"
if /usr/sbin/sshd -t; then
    launchctl enable system/com.openssh.sshd
    launchctl bootstrap system /System/Library/LaunchDaemons/ssh.plist 2>/dev/null
else
    warn "sshd rejected $SSHD_CONF, removing it"
    rm -f "$SSHD_CONF"
fi

step "Screen Sharing (for the rare prompt that only the GUI can answer)"
launchctl enable system/com.apple.screensharing
launchctl bootstrap system /System/Library/LaunchDaemons/com.apple.screensharing.plist 2>/dev/null

step "Dead-man's switch: the check-in URL"
HC="$CONF_DIR/healthcheck-url"
if [ -s "$HC" ]; then
    echo "set: $(cat "$HC")"
else
    printf "Paste the ping URL (for example https://hc-ping.com/<uuid>), or Enter to skip: "
    IFS= read -r url
    if [ -n "$url" ]; then
        echo "$url" > "$HC"
        chmod 600 "$HC"
    fi
fi
launchctl kickstart -k system/$LABEL

step "FileVault off: an unplanned reboot must come back without a password typed at the keyboard"
if fdesetup status | grep -q "FileVault is On"; then
    fdesetup disable
fi
fdesetup status

step "Verify"
sleep 3
check() {
    if eval "$2" > /dev/null 2>&1; then
        printf '  \033[32mOK\033[0m   %s\n' "$1"
    else
        printf '  \033[31mFAIL\033[0m %s\n' "$1"
    fi
}
check "hostname is $NAME" "[ \"\$(scutil --get LocalHostName)\" = \"$NAME\" ]"
check "sleep is off" "pmset -g | grep -qE '^ sleep +0'"
check "lid-close sleep is off" "pmset -g | grep -q 'SleepDisabled.*1'"
check "watchdog loaded" "launchctl print system/$LABEL"
check "tailscale logged in" "$TAILSCALE status"
check "batt daemon answers" "$BATT status"
check "SSH listening on 22" "nc -z -G 2 127.0.0.1 22"
check "SSH password login is off (port 22 may face the internet)" "/usr/sbin/sshd -T | grep -qx 'passwordauthentication no'"
check "pf on, SSH rate limit loaded" "pfctl -a com.apple/250.cbase-mac -s rules | grep -q 'port = 22'"
check "Screen Sharing listening on 5900" "nc -z -G 2 127.0.0.1 5900"
check "check-in URL set" "[ -s $HC ]"
check "automatic login is on (unlocks the login keychain: claude, gh)" "defaults read /Library/Preferences/com.apple.loginwindow autoLoginUser"
check "FileVault off or decrypting" "! fdesetup status | grep -qx 'FileVault is On.'"
echo
echo "Tailscale IP: $("$TAILSCALE" ip -4 2>/dev/null)"
echo "The watchdog's last status line (ipv6= is where to SSH when Tailscale is down):"
echo "  $(cat /var/db/cbase-mac/last_report 2>/dev/null)"
echo "Watchdog log: /var/log/cbase-mac.log"
echo "Next, by hand:"
echo "  - Tailscale admin console: Disable key expiry for $NAME (the login otherwise expires in 180 days)."
echo "  - System Settings > Users & Groups > Automatically log in as: $SUDO_USER (after FileVault finishes decrypting)."
echo "  - Once SSH password login shows OK above: xFi app, open TCP 22 (and 8080 for staging) over IPv6 to this Mac."
