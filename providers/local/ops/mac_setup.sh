#!/bin/bash
# mac_setup.sh - turns a MacBook into an unattended server (MAC_RUNBOOK.md): it comes back on the network by
# itself after any reboot, and is reached and watched from anywhere for months without anyone at the keyboard.
# Idempotent: re-run it after any change. It asks questions, so run it in a Terminal window:
#
#   sudo providers/local/ops/mac_setup.sh [hostname] [public name]
#
# The hostname defaults to us-east-3. The public name (for example us-east-3.example.com, an A record to the
# house's address, with the router forwarding 80 and 443 to the Mac) turns on Caddy; without it Caddy is left off.
#
# Needs Homebrew's tailscale and batt installed first: brew install tailscale batt

# The whole script is one block, so bash reads all of it before running any of it:
# an edit to this file while a run waits at a prompt cannot change what that run does next.
{
    set -u

    NAME="${1:-us-east-3}"
    DOMAIN="${2:-}"
    HERE="$(cd "$(dirname "$0")" && pwd)"
    BREW=/opt/homebrew/bin/brew
    TAILSCALE=/opt/homebrew/bin/tailscale
    BATT=/opt/homebrew/bin/batt
    BATTERY_LIMIT=80
    LABEL=com.cbase.mac-watchdog
    CONF_DIR=/etc/cbase-mac

    step() { printf '\n\033[1m==> %s\033[0m\n' "$*"; }
    warn() { printf '\033[33mWARN: %s\033[0m\n' "$*"; }

    # (Re)load a launchd job: bootout returns before the job has stopped, and a bootstrap sent then fails
    # ("Input/output error"), which left Caddy unloaded on us-east-3. So wait until it is gone first.
    # reload_job <domain> <label> <plist>, for example: reload_job system com.cbase.mac-caddy /Library/...
    reload_job() {
        local n=0
        launchctl bootout "$1/$2" 2>/dev/null
        while launchctl print "$1/$2" > /dev/null 2>&1 && [ $n -lt 30 ]; do
            sleep 1
            n=$((n + 1))
        done
        launchctl bootstrap "$1" "$3" || warn "launchctl could not load $2"
    }

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
    # A Mac that never sleeps never hibernates: no sleep image (it is the size of RAM), and none written again.
    pmset -a hibernatemode 0
    rm -f /private/var/vm/sleepimage
    # On battery (an outage) the CPU runs slower and the battery lasts longer; on AC it runs at full speed.
    pmset -b lowpowermode 1
    pmset -c lowpowermode 0
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
        # Load it now: the watchdog loads the anchor only when it is empty, so a changed rule waits otherwise.
        pfctl -q -a com.apple/250.cbase-mac -f "$CONF_DIR/pf.conf" 2>/dev/null
    else
        warn "pf rejected mac_pf.conf; the watchdog runs without the SSH rate limit"
    fi
    reload_job system "$LABEL" "/Library/LaunchDaemons/$LABEL.plist"

    step "Tailscale (a system daemon: up at boot, before anyone logs in)"
    # Root may not download Homebrew's package index (a new macOS needs a new one), so fetch it as the user.
    sudo -u "$SUDO_USER" "$BREW" info tailscale batt > /dev/null
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
    if [ -s "$AK" ]; then
        echo "$(grep -c . "$AK") key(s) already in $AK:"
        ssh-keygen -l -f "$AK" | sed 's/^/  /'
        echo "Press Enter to keep them, or paste more keys, one per line, then an empty line."
    else
        echo "Paste the SSH public keys of the machines you will connect from, one per line, then an empty line."
        echo "Press Enter at once to skip: Tailscale SSH works with your tailnet login alone."
    fi
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
    # macOS makes the host keys on the first connection, so a Mac that never had Remote Login on has none,
    # and sshd -t refuses without them ("no hostkeys available"). Make any that are missing.
    ssh-keygen -A
    if /usr/sbin/sshd -t; then
        launchctl enable system/com.openssh.sshd
        launchctl bootstrap system /System/Library/LaunchDaemons/ssh.plist 2>/dev/null
    else
        warn "sshd rejected $SSHD_CONF, removing it"
        rm -f "$SSHD_CONF"
    fi
    # sshd reports every login, accepted or refused, at the Info level, which macOS keeps only in memory and then
    # drops, so the record of who knocked was gone. Keep sshd's Info messages on disk, where logd's own size limit
    # caps them with the rest of the system log. Read them with:
    #     log show --last 1d --info --predicate 'process BEGINSWITH "sshd"'
    for p in sshd sshd-session sshd-auth; do
        log config --process "$p" --mode "level:info,persist:info" || warn "log config refused $p"
    done

    step "Passwordless sudo for routine fixes (mac_sudoers: exact Apple commands only)"
    SUDOERS_NEW=$(mktemp)
    sed "s/__USER__/$SUDO_USER/g" "$HERE/mac_sudoers" > "$SUDOERS_NEW"
    if visudo -cf "$SUDOERS_NEW" > /dev/null; then
        install -m 440 -o root -g wheel "$SUDOERS_NEW" /etc/sudoers.d/cbase-mac
    else
        warn "visudo rejected mac_sudoers; /etc/sudoers.d/cbase-mac left as it was"
    fi
    rm -f "$SUDOERS_NEW"

    step "macOS firewall: on, Apple's own services allowed, anything else only by name"
    # pf limits ports; this limits programs: a server a session starts on every interface stays off the home
    # network unless it is named here. Caddy and tailscaled are named; the watchdog renames them after a
    # brew upgrade moves their binaries.
    FW=/usr/libexec/ApplicationFirewall/socketfilterfw
    "$FW" --setglobalstate on > /dev/null
    "$FW" --setallowsigned on > /dev/null
    "$FW" --setallowsignedapp off > /dev/null
    "$FW" --setstealthmode on > /dev/null
    for b in /opt/homebrew/bin/caddy /opt/homebrew/opt/tailscale/bin/tailscaled; do
        [ -e "$b" ] || continue
        "$FW" --add "$(realpath "$b")" > /dev/null
        "$FW" --unblockapp "$(realpath "$b")" > /dev/null
    done

    step "Screen Sharing (for the rare prompt that only the GUI can answer)"
    launchctl enable system/com.apple.screensharing
    launchctl bootstrap system /System/Library/LaunchDaemons/com.apple.screensharing.plist 2>/dev/null

    step "Claude Code Remote Control (a LaunchAgent: up when automatic login opens the session)"
    CLAUDE_AGENT="$USER_HOME/Library/LaunchAgents/com.cbase.mac-claude.plist"
    if [ -x "$USER_HOME/.local/bin/claude" ]; then
        install -d -m 755 -o "$SUDO_USER" -g staff "$USER_HOME/Library/LaunchAgents" "$USER_HOME/Library/Logs" "$USER_HOME/Dev"
        CLAUDE_NEW=$(mktemp)
        sed -e "s|__HOME__|$USER_HOME|g" -e "s|__NAME__|$NAME|g" "$HERE/mac_claude.plist" > "$CLAUDE_NEW"
        USER_ID=$(id -u "$SUDO_USER")
        # Restart it only for a new plist: stopping Remote Control waits while it winds its sessions down, and a
        # copy started before the old one has let go exits 69 (EX_UNAVAILABLE) until launchd retries.
        if cmp -s "$CLAUDE_NEW" "$CLAUDE_AGENT" && launchctl print "gui/$USER_ID/com.cbase.mac-claude" > /dev/null 2>&1; then
            echo "unchanged and loaded: left running"
        else
            install -m 644 -o "$SUDO_USER" -g staff "$CLAUDE_NEW" "$CLAUDE_AGENT"
            if launchctl print "gui/$USER_ID" > /dev/null 2>&1; then
                reload_job "gui/$USER_ID" com.cbase.mac-claude "$CLAUDE_AGENT"
            else
                echo "no one is logged in; it starts at the next login"
            fi
        fi
        rm -f "$CLAUDE_NEW"
    else
        warn "$USER_HOME/.local/bin/claude is missing; install Claude Code and re-run"
    fi

    step "Caddy on 80 and 443 (HTTPS for ${DOMAIN:-no public name}, its certificates by itself)"
    if [ -n "$DOMAIN" ] && [ -x /opt/homebrew/bin/caddy ]; then
        install -m 644 "$HERE/mac_Caddyfile" "$CONF_DIR/Caddyfile"
        echo "$DOMAIN" > "$CONF_DIR/domain"
        sed -e "s|__USER__|$SUDO_USER|g" -e "s|__HOME__|$USER_HOME|g" -e "s|__DOMAIN__|$DOMAIN|g" \
            "$HERE/mac_caddy.plist" > /Library/LaunchDaemons/com.cbase.mac-caddy.plist
        chown root:wheel /Library/LaunchDaemons/com.cbase.mac-caddy.plist
        chmod 644 /Library/LaunchDaemons/com.cbase.mac-caddy.plist
        reload_job system com.cbase.mac-caddy /Library/LaunchDaemons/com.cbase.mac-caddy.plist
    else
        echo "left off: pass the public name as the second argument, and brew install caddy"
    fi

    # The watchdog caps these (they outgrow newsyslog: their writers keep them open).
    printf '%s\n' "$USER_HOME/Library/Logs/cbase-mac-claude.log" "$USER_HOME/Library/Logs/cbase-mac-caddy.log" \
        > "$CONF_DIR/capped-logs"

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
    check "no hibernation image" "pmset -g | grep -qE '^ hibernatemode +0' && [ ! -e /private/var/vm/sleepimage ]"
    check "lid-close sleep is off" "pmset -g | grep -q 'SleepDisabled.*1'"
    check "watchdog loaded" "launchctl print system/$LABEL"
    check "tailscale logged in" "$TAILSCALE status"
    check "the watchdog can restart tailscale (its job is loaded)" "launchctl print system/sh.brew.tailscale"
    check "batt daemon answers" "$BATT status"
    check "SSH listening on 22" "nc -z -G 2 127.0.0.1 22"
    check "SSH password login is off (port 22 may face the internet)" "/usr/sbin/sshd -T | grep -qix 'passwordauthentication no'"
    # A test login would pass before the setting too: log show also reads the recent messages still in memory.
    check "sshd's login messages are kept on disk" "( for p in sshd sshd-session sshd-auth; do log config --process \$p --status | grep -q PERSIST_INFO || exit 1; done )"
    check "pf on, SSH rate limit loaded" "pfctl -a com.apple/250.cbase-mac -s rules | grep -q 'port = 22'"
    check "passwordless sudo for routine fixes" "sudo -n -l -U $SUDO_USER | grep -q 'NOPASSWD: /sbin/shutdown -r now'"
    check "macOS firewall on" "/usr/libexec/ApplicationFirewall/socketfilterfw --getglobalstate | grep -q enabled"
    check "the firewall lets Caddy in" "/usr/libexec/ApplicationFirewall/socketfilterfw --getappblocked \$(realpath /opt/homebrew/bin/caddy) | grep -q permitted"
    check "Screen Sharing answers on Tailscale (pf turns the rest away)" "nc -z -G 2 \$($TAILSCALE ip -4) 5900"
    check "Low Power Mode on battery only" "pmset -g custom | awk '/Battery Power/ { b = 1 } /AC Power/ { b = 0 } b && /lowpowermode/ { print \$2 }' | grep -qx 1"
    check "Claude Remote Control loaded (just restarted, it can show spawn scheduled for 30 s)" "launchctl print gui/\$(id -u $SUDO_USER)/com.cbase.mac-claude | grep -qE 'state = (running|spawn scheduled)'"
    if [ -n "$DOMAIN" ]; then
        check "Caddy running" "launchctl print system/com.cbase.mac-caddy | grep -q 'state = running'"
        check "https://$DOMAIN answers (a minute or so after the A record exists)" "curl -fsS -m 20 https://$DOMAIN/"
        check "web requests are logged (the one just above, at least)" "[ -s $USER_HOME/Library/Logs/cbase-mac-caddy-access.log ]"
    fi
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

    exit
}
