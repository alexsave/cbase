# Mac server runbook

A MacBook run as an unattended server for the local provider: it comes back on the network by itself after any reboot, and is reached and watched from anywhere for months with nobody at the keyboard.
The first one is `us-east-3`.

## Quick start

1. `brew install tailscale batt`
2. Make a check in healthchecks.io: period 5 minutes, grace 10 minutes, and copy its ping URL.
3. In a Terminal window: `sudo providers/local/ops/mac_setup.sh us-east-3`
4. In the Tailscale admin console, open the machine and choose Disable key expiry.
5. Plug in power (and Ethernet, when there is a port for it), leave the lid open, and reboot once to prove it comes back: `sudo reboot`, then `ssh us-east-3` from another tailnet machine.

## State (2026-11-26)

`us-east-3` is a MacBook Pro 13" M1 (2020, `MacBookPro17,1`) on macOS 26.2, on Wi-Fi.

## What mac_setup.sh does, and why

Each step answers one way an unattended laptop goes dark.

| failure | answer |
|---|---|
| A reboot stops at the FileVault unlock screen, before the network is up | FileVault off (`fdesetup disable`); the disk is then readable by whoever holds the laptop |
| The Mac sleeps (idle, lid closed, standby, Power Nap) | `pmset -a sleep 0 standby 0 powernap 0 disablesleep 1` |
| A kernel freeze | `systemsetup -setrestartfreeze on` |
| A power cut | the battery carries it; at 8% the watchdog shuts down cleanly, and the Mac boots again when power returns (`nvram auto-boot=true`, no `BootPreference`) |
| Months at 100% charge swell the battery | `batt limit 80`, a root daemon |
| The Mac sits behind NAT, or changes networks | Tailscale as a system daemon (`brew services`), up at boot before anyone logs in, with `--ssh`, as `<hostname>` |
| The Tailscale login expires after 180 days | Disable key expiry in the admin console (quick start step 4) |
| The server's DNS follows Tailscale's | `--accept-dns=false` |
| Wi-Fi drops, or the router hangs | the watchdog cycles Wi-Fi after 10 minutes offline and reboots after 30, at most once every 6 hours and never in the first hour of uptime |
| `tailscaled` dies or wedges | the watchdog kickstarts it |
| Nobody learns that it is down | the watchdog checks in to healthchecks.io every 5 minutes, which emails when check-ins stop; on battery, with the disk over 90% or with Tailscale down, it checks in to `/fail` instead |
| A prompt that only the GUI can answer | Screen Sharing, over Tailscale |
| A security fix waits for someone to click | security and minor macOS updates install by themselves; with FileVault off, their restart comes back on its own |

Every service is a LaunchDaemon, so the server works at the login window with nobody logged in.

## Getting in

- `ssh vlad@us-east-3` through Tailscale SSH, with your tailnet login.
- OpenSSH on port 22 with the keys in `~/.ssh/authorized_keys`; once that file has a key, password login is off (`/etc/ssh/sshd_config.d/050-cbase-mac.conf`).
- Screen Sharing: `vnc://us-east-3` from Finder (Go, Connect to Server).

## Where things are

| what | where |
|---|---|
| watchdog | `/usr/local/libexec/cbase-mac/mac_watchdog.sh`, run by `/Library/LaunchDaemons/com.cbase.mac-watchdog.plist` |
| its log | `/var/log/cbase-mac.log`, rotated by `/etc/newsyslog.d/cbase-mac.conf` |
| its state | `/var/db/cbase-mac/` (`offline_runs`, `last_watchdog_reboot`) |
| the check-in URL | `/etc/cbase-mac/healthcheck-url` (mode 0600) |
| Tailscale | `sudo brew services info tailscale`, `tailscale status` |
| battery limit | `batt status` |

The watchdog's check-in is one plain-text POST, so a cbase endpoint on the VM can take healthchecks.io's place later by changing the URL file.

## Testing

`make ops-test` runs `test/mac_watchdog_test.sh`: the watchdog against stub macOS commands, on Linux as on macOS.
After a change to `mac_setup.sh`, re-run it on the Mac and read its Verify block, then reboot and check in from another machine.
