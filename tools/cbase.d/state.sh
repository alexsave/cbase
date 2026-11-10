# shellcheck shell=bash
# state.sh - who is in a group: the state file (README.md "./cbase deploy tool"). Sourced by
# tools/cbase.
#
# WHERE: s3://cbase-state-<account>/vm/<group>.txt and s3://cbase-state-<account>/static/<group>.txt.
# Tags find a group (aws.sh aws_group_find); its state file alone says who is in it.
#
# THE FORMAT, plain text, one line per member, sorted by app, written whole by this file alone:
#   cbase-state 1 vm <group> origin=<host> addr=<ipv6|dualstack>
#   app=<name> port=<port> service=cbase-<name>.service mem=<MiB>
# where mem is the declared MiB (CBASE_MEMORY), or ~<MiB> for an app that declares none, counted
# at that estimate (conf.sh CONF_MEMORY_ESTIMATE): "mem=~128". A grandfathered member (conf.sh
# CBASE_VM_PORT, CBASE_VM_DATA) adds " data=<dir> legacy=1": its own port, outside the claimed
# range, and its own data directory, both as they were. And for a static group
#   cbase-state 1 static <group>
#   app=<name> folder=<name>/
# The first line names the format's version, the kind and the group, and must match the file's
# key. A vm group's adds its VM's origin, the one hostname CloudFront reaches every member at, and
# its address type (the distribution's IpAddressType): group-init records the instance's own,
# dualstack; an adopted group's first deploy with CBASE_VM_ORIGIN records that host, ipv6. A
# header without it (written before it was recorded) is read as a group with no origin yet.
# Every field is present, in this order, separated by one blank. A line that is not so, a second
# line for an app, and (vm) a port two apps hold or one outside the range stop the tool,
# naming the file and the line.
#
# THE WRITE is S3's conditional PUT: If-Match with the ETag the read returned, or If-None-Match: *
# when there was no file. A precondition failure (412, or 409 for a write in flight) means another
# run wrote first: the file is read again and the change made again on what it now holds, up to
# STATE_ATTEMPTS times. A join keeps an app's port when it is already a member; a new member
# claims the lowest port in STATE_PORT_MIN..STATE_PORT_MAX that no member holds, in the same write
# that records it, so two racing joins can never hold one port.
#
# THE MEMORY FIT (vm groups, deploy's join): with STATE_FIT_LIMIT set (the MiB the group's instance
# leaves for apps, vm.sh), a join is checked in the same read-modify-write that claims the port:
# every member's mem plus the joining app's must fit, or the join is refused and nothing written.
# An undeclared member (mem=~128) counts at its estimate, and the report says so. Two racing joins
# cannot both fit into the room for one: the second one's write fails its precondition, and its
# retry checks again against the file the first one wrote.
#
# THE BACKOFF between attempts is deterministic, not random: there is no sanctioned seedable
# source in shell (cbase's is lib/rng.h, in C), and $RANDOM is not one. Attempt k waits
# min(2000, 100 * 2^(k-1)) ms plus a stagger of 0-99 ms that is the app's name's cksum mod 100,
# so two apps racing for one file do not retry in step, and the same run always waits the same.
# A conditional write always lets one writer through, so every attempt that loses is lost to a
# write that won: the retries are bounded by the number of racing writers.

STATE_VERSION=1
STATE_PORT_MIN=8700
STATE_PORT_MAX=8799
STATE_ATTEMPTS=8

# The state file as read: its members' lines and their apps, ports and the line numbers.
ST_LINES=()
ST_APPS=()
ST_PORTS=()   # vm groups: each member's port
ST_ORIGIN=""       # vm groups: the header's origin host, or "" when none is recorded
ST_ORIGIN_ADDR=""  # and its address type, ipv6 or dualstack
STATE_ORIGIN=""    # the joining app's CBASE_VM_ORIGIN, or ""
STATE_ORIGIN_NEED=""  # 1: a join with no origin recorded or named is refused (deploy's, vm.sh)

STATE_FIT_LIMIT=""   # MiB the vm group's instance leaves for apps; "" for no fit check
STATE_FIT_WHAT=""    # how the limit came about, for the report ("t4g.nano: 512 MiB less 192 for the OS")
STATE_FIT_REPORT=""  # the last fit check's report
STATE_JOINED_NEW=""  # 1 when the last join added the app (it was not a member)
STATE_PORT=""        # the port the last vm join holds
STATE_LEGACY_PORT="" # a grandfathered member's join: its own port and data dir (conf.sh), or ""
STATE_LEGACY_DATA=""

state_bucket() { printf 'cbase-state-%s' "$AWS_ACCOUNT"; }
state_key() { printf '%s/%s.txt' "$1" "$2"; }

# state_parse KIND GROUP FILE WHERE: ST_* from FILE (WHERE names it in errors).
state_parse() {
  local kind="$1" group="$2" file="$3" where="$4" line no=0 app port i
  ST_LINES=()
  ST_APPS=()
  ST_PORTS=()
  ST_ORIGIN=""
  ST_ORIGIN_ADDR=""
  while IFS= read -r line || [ -n "$line" ]; do
    no=$((no + 1))
    if [ "$no" -eq 1 ]; then
      if [ "$kind" = vm ] && [[ "$line" =~ ^cbase-state\ $STATE_VERSION\ vm\ $group\ origin=([a-z0-9.-]+)\ addr=(ipv6|dualstack)$ ]]; then
        ST_ORIGIN="${BASH_REMATCH[1]}"
        ST_ORIGIN_ADDR="${BASH_REMATCH[2]}"
        continue
      fi
      [ "$line" = "cbase-state $STATE_VERSION $kind $group" ] \
        || state_die "$where" "$no" "is not \"cbase-state $STATE_VERSION $kind $group\" (the format's version, the kind and the group; a vm group's may add \" origin=<host> addr=<ipv6|dualstack>\")"
      continue
    fi
    if [ "$kind" = vm ]; then
      [[ "$line" =~ ^app=([a-z]([a-z0-9-]{0,30}[a-z0-9])?)\ port=([0-9]{4,5})\ service=([a-z0-9-]+)\.service\ mem=~?([0-9]{1,6})(\ data=(/[A-Za-z0-9._/-]+)\ legacy=1)?$ ]] \
        || state_die "$where" "$no" "is not \"app=<name> port=<port> service=cbase-<name>.service mem=<MiB>\" (mem=~<MiB> when estimated; \" data=<dir> legacy=1\" after it for a grandfathered member)"
      app="${BASH_REMATCH[1]}"
      port="$((10#${BASH_REMATCH[3]}))"
      [ "${BASH_REMATCH[4]}" = "cbase-$app" ] || state_die "$where" "$no" "names service ${BASH_REMATCH[4]}.service for app $app (it is cbase-$app.service)"
      if [ -n "${BASH_REMATCH[6]}" ]; then
        { [ "$port" -ge 1024 ] && [ "$port" -le 65535 ] && { [ "$port" -lt "$STATE_PORT_MIN" ] || [ "$port" -gt "$STATE_PORT_MAX" ]; }; } \
          || state_die "$where" "$no" "holds the grandfathered port $port, which is not 1024-65535 outside $STATE_PORT_MIN-$STATE_PORT_MAX"
      else
        { [ "$port" -ge "$STATE_PORT_MIN" ] && [ "$port" -le "$STATE_PORT_MAX" ]; } \
          || state_die "$where" "$no" "holds port $port, outside $STATE_PORT_MIN-$STATE_PORT_MAX"
      fi
      for i in "${!ST_PORTS[@]}"; do
        [ "${ST_PORTS[$i]}" != "$port" ] || state_die "$where" "$no" "gives port $port to $app, and ${ST_APPS[$i]} holds it"
      done
    else
      [[ "$line" =~ ^app=([a-z]([a-z0-9-]{0,30}[a-z0-9])?)\ folder=([a-z0-9-]+)/$ ]] \
        || state_die "$where" "$no" "is not \"app=<name> folder=<name>/\""
      app="${BASH_REMATCH[1]}"
      port=""
      [ "${BASH_REMATCH[3]}" = "$app" ] || state_die "$where" "$no" "gives app $app the folder ${BASH_REMATCH[3]}/ (it is $app/)"
    fi
    for i in "${!ST_APPS[@]}"; do
      [ "${ST_APPS[$i]}" != "$app" ] || state_die "$where" "$no" "lists $app again"
    done
    ST_LINES+=("$line")
    ST_APPS+=("$app")
    ST_PORTS+=("$port")
  done < "$file"
  [ "$no" -ge 1 ] || state_die "$where" 1 "is empty (it starts \"cbase-state $STATE_VERSION $kind $group\")"
}

# state_mem LINE: STATE_MEM, a vm member line's MiB, and STATE_MEM_EST, 1 when it is the
# estimate an undeclared app is counted at (mem=~N).
STATE_MEM=""
STATE_MEM_EST=""
state_mem() {
  local m="${1##* mem=}"
  m="${m%% *}"   # a grandfathered member's " data=... legacy=1" follows
  STATE_MEM_EST=""
  if [ "${m#\~}" != "$m" ]; then STATE_MEM_EST=1; m="${m#\~}"; fi
  STATE_MEM="$((10#$m))"
}

# state_fit APP LINE: whether ST_LINES, with APP's line made LINE, fit in STATE_FIT_LIMIT MiB.
# Fills STATE_FIT_REPORT, the members by name; status 1 when they do not fit.
state_fit() {
  local app="$1" new="$2" i a line total=0 notes="" members="" sorted
  local lines=("${ST_LINES[@]+"${ST_LINES[@]}"}") apps=("${ST_APPS[@]+"${ST_APPS[@]}"}") at=""
  for i in "${!apps[@]}"; do [ "${apps[$i]}" != "$app" ] || at="$i"; done
  if [ -n "$at" ]; then lines[at]="$new"; else lines+=("$new"); fi
  sorted="$(printf '%s\n' "${lines[@]}" | LC_ALL=C sort)"
  while IFS= read -r line; do
    a="${line#app=}"
    a="${a%% *}"
    state_mem "$line"
    total=$((total + STATE_MEM))
    if [ -n "$STATE_MEM_EST" ]; then
      notes="$notes  $a: memory not declared, counted as ${STATE_MEM}M"$'\n'
      members="$members${members:+, }$a ~$STATE_MEM"
    else
      members="$members${members:+, }$a $STATE_MEM"
    fi
  done <<< "$sorted"
  STATE_FIT_REPORT="$notes  memory: $members = $total MiB of the $STATE_FIT_LIMIT MiB for apps ($STATE_FIT_WHAT)"
  [ "$total" -le "$STATE_FIT_LIMIT" ]
}

# state_origin GROUP: the vm group's origin, ST_ORIGIN and ST_ORIGIN_ADDR, as the file read holds
# it, or taken from STATE_ORIGIN (the app's CBASE_VM_ORIGIN, ipv6) when it holds none. One VM has
# one origin: an app naming another is refused. Status 1 when neither names one.
state_origin() {
  if [ -n "$STATE_ORIGIN" ]; then
    if [ -z "$ST_ORIGIN" ]; then ST_ORIGIN="$STATE_ORIGIN"; ST_ORIGIN_ADDR=ipv6
    elif [ "$ST_ORIGIN" != "$STATE_ORIGIN" ]; then
      echo "cbase: $CONF_APP's CBASE_VM_ORIGIN is $STATE_ORIGIN, and vm group $1's VM is reached at $ST_ORIGIN (its state file): one VM can't have two origins. Drop CBASE_VM_ORIGIN, or set it to $ST_ORIGIN. Nothing was changed." >&2
      exit 1
    fi
  fi
  [ -n "$ST_ORIGIN" ]
}

state_die() {
  echo "cbase: state file $1 line $2 $3. It is written by ./cbase alone; fix or restore it by hand only with care." >&2
  exit 1
}

# state_render KIND GROUP FILE: the header and ST_LINES sorted by app, into FILE.
state_render() {
  {
    printf 'cbase-state %s %s %s%s\n' "$STATE_VERSION" "$1" "$2" "${ST_ORIGIN:+ origin=$ST_ORIGIN addr=$ST_ORIGIN_ADDR}"
    if [ "${#ST_LINES[@]}" -gt 0 ]; then printf '%s\n' "${ST_LINES[@]}" | LC_ALL=C sort; fi
  } > "$3"
}

# state_free_port: the lowest port in the range no member holds, or nothing when all are taken.
state_free_port() {
  local p held
  for ((p = STATE_PORT_MIN; p <= STATE_PORT_MAX; p++)); do
    held=""
    for q in "${ST_PORTS[@]+"${ST_PORTS[@]}"}"; do
      [ "$q" != "$p" ] || { held=1; break; }
    done
    [ -n "$held" ] || { printf '%s\n' "$p"; return 0; }
  done
}

state_backoff() {   # state_backoff ATTEMPT APP
  local ms=$((100 * (1 << ($1 - 1)))) stagger
  [ "$ms" -le 2000 ] || ms=2000
  stagger=$(( $(printf '%s' "$2" | cksum | cut -d' ' -f1) % 100 ))
  ms=$((ms + stagger))
  sleep "$(printf '%d.%03d' $((ms / 1000)) $((ms % 1000)))"
}

# state_read KIND GROUP: ST_* and STATE_ETAG from the group's file ("*" when there is none yet,
# and no members).
# STATE_NO_BUCKET=1 (a dry run's, when the account has no state bucket yet) reads no file at all.
STATE_ETAG=""
STATE_TMP=""
STATE_NO_BUCKET=""
state_read() {
  local kind="$1" group="$2" bucket key rc=0
  bucket="$(state_bucket)"
  key="$(state_key "$kind" "$group")"
  [ -n "$STATE_TMP" ] || { STATE_TMP="$(mktemp -d "${TMPDIR:-/tmp}/cbase-state.XXXXXX")"; CB_CLEANUP+=("$STATE_TMP"); }
  rm -f "$STATE_TMP/cur"
  if [ "$STATE_NO_BUCKET" = 1 ]; then rc=3
  else STATE_ETAG="$(aws_s3_get "$bucket" "$key" "$STATE_TMP/cur")" || rc=$?; fi
  if [ "$rc" -eq 3 ]; then
    STATE_ETAG="*"
    ST_LINES=()
    ST_APPS=()
    ST_PORTS=()
    ST_ORIGIN=""
    ST_ORIGIN_ADDR=""
    return 0
  fi
  [ "$rc" -eq 0 ] || exit "$rc"
  state_parse "$kind" "$group" "$STATE_TMP/cur" "s3://$bucket/$key"
}

# state_change KIND GROUP APP MEM OP: one join or leave, read-modify-conditional-write, retried.
# OP is join or leave. Prints what it did.
state_change() {
  local kind="$1" group="$2" app="$3" mem="$4" op="$5" attempt line i at port bucket key rc old_origin=
  bucket="$(state_bucket)"
  key="$(state_key "$kind" "$group")"
  for ((attempt = 1; attempt <= STATE_ATTEMPTS; attempt++)); do
    state_read "$kind" "$group"
    at=""
    for i in "${!ST_APPS[@]}"; do
      [ "${ST_APPS[$i]}" != "$app" ] || at="$i"
    done
    if [ "$op" = leave ]; then
      if [ -z "$at" ]; then
        echo "$app is not in $kind group $group: nothing to leave"
        return 0
      fi
      line="${ST_LINES[$at]}"
      local keep=() keep_apps=() keep_ports=()
      for i in "${!ST_APPS[@]}"; do
        [ "$i" = "$at" ] && continue
        keep+=("${ST_LINES[$i]}")
        keep_apps+=("${ST_APPS[$i]}")
        keep_ports+=("${ST_PORTS[$i]}")
      done
      ST_LINES=("${keep[@]+"${keep[@]}"}")
      ST_APPS=("${keep_apps[@]+"${keep_apps[@]}"}")
      ST_PORTS=("${keep_ports[@]+"${keep_ports[@]}"}")
    else
      if [ "$kind" = vm ]; then
        old_origin="$ST_ORIGIN"
        state_origin "$group" || [ -z "$STATE_ORIGIN_NEED" ] || { echo "cbase: vm group $group has no origin recorded and $app names none: set CBASE_VM_ORIGIN (the VM's AAAA hostname) once, in this app's cbase.conf. Nothing was changed." >&2; exit 1; }
        if [ -n "$STATE_LEGACY_PORT" ]; then
          # A grandfathered member: its own port, which no other member may hold.
          port="$STATE_LEGACY_PORT"
          for i in "${!ST_PORTS[@]}"; do
            [ "$i" = "$at" ] || [ "${ST_PORTS[$i]}" != "$port" ] || { echo "cbase: $app's port $port (CBASE_VM_PORT) is ${ST_APPS[$i]}'s in vm group $group: nothing was changed" >&2; exit 1; }
          done
        elif [ -n "$at" ] && [[ "${ST_LINES[$at]}" != *" legacy=1" ]]; then port="${ST_PORTS[$at]}"
        else port="$(state_free_port)"; fi
        [ -n "$port" ] || { echo "cbase: vm group $group has no free port: all of $STATE_PORT_MIN-$STATE_PORT_MAX are held" >&2; exit 1; }
        line="app=$app port=$port service=cbase-$app.service mem=$mem"
        [ -z "$STATE_LEGACY_PORT" ] || line="$line data=$STATE_LEGACY_DATA legacy=1"
        # shellcheck disable=SC2034 # vm.sh reads it
        STATE_PORT="$port"
        # THE MEMORY FIT, on the file this write is conditional on.
        if [ -n "$STATE_FIT_LIMIT" ] && ! state_fit "$app" "$line"; then
          printf '%s\n' "$STATE_FIT_REPORT" >&2
          echo "cbase: $app does not fit on vm group $group: its members would need more than the $STATE_FIT_LIMIT MiB its instance leaves for apps ($STATE_FIT_WHAT). Declare less memory (CBASE_MEMORY), put the app in another group, or give the group a bigger instance (./cbase group-init vm $group --type <type>). Nothing was changed." >&2
          exit 1
        fi
      else
        line="app=$app folder=$app/"
      fi
      # shellcheck disable=SC2034 # vm.sh reads it
      if [ -z "$at" ]; then STATE_JOINED_NEW=1; else STATE_JOINED_NEW=""; fi
      if [ -n "$at" ] && [ "${ST_LINES[$at]}" = "$line" ] && [ "${old_origin:-}" = "$ST_ORIGIN" ]; then
        echo "$app is in $kind group $group: $line"
        [ -z "$STATE_FIT_LIMIT" ] || [ "$kind" != vm ] || printf '%s\n' "$STATE_FIT_REPORT"
        return 0
      fi
      if [ -n "$at" ]; then ST_LINES[at]="$line"; else ST_LINES+=("$line"); ST_APPS+=("$app"); ST_PORTS+=("${port:-}"); fi
    fi
    state_render "$kind" "$group" "$STATE_TMP/new"
    rc=0
    aws_s3_put "$bucket" "$key" "$STATE_TMP/new" "$STATE_ETAG" || rc=$?
    if [ "$rc" -eq 0 ]; then
      if [ "$op" = leave ]; then echo "$app left $kind group $group (it held: $line)"
      else
        echo "$app joined $kind group $group: $line"
        [ -z "$STATE_FIT_LIMIT" ] || [ "$kind" != vm ] || printf '%s\n' "$STATE_FIT_REPORT"
      fi
      return 0
    fi
    [ "$rc" -eq 3 ] || exit "$rc"
    echo "cbase: s3://$bucket/$key changed while $app's $op was being written (attempt $attempt of $STATE_ATTEMPTS): reading it again" >&2
    [ "$attempt" -eq "$STATE_ATTEMPTS" ] || state_backoff "$attempt" "$app"
  done
  echo "cbase: s3://$bucket/$key kept changing: $app's $op lost $STATE_ATTEMPTS conditional writes in a row; run it again" >&2
  exit 1
}

# state_list KIND GROUP: the group's members, a table: a vm group's port, service and memory (the
# declared MiB, "512M", or "~128 counted" for an app that declares none), a static group's folder.
# Status 3 when there is no file.
state_list() {
  local kind="$1" group="$2" line m f=()
  state_read "$kind" "$group"
  if [ "$STATE_ETAG" = "*" ]; then return 3; fi
  echo "$kind group $group (s3://$(state_bucket)/$(state_key "$kind" "$group")): ${#ST_LINES[@]} app$([ "${#ST_LINES[@]}" -eq 1 ] || echo s)"
  [ "${#ST_LINES[@]}" -gt 0 ] || return 0
  if [ "$kind" = vm ]; then printf '  %-24s %-5s %-34s %s\n' app port service mem
  else printf '  %-24s %s\n' app folder; fi
  while IFS= read -r line; do
    read -r -a f <<< "$line"   # app=... port=... service=... mem=..., or app=... folder=...
    if [ "$kind" = vm ]; then
      state_mem "$line"
      if [ -n "$STATE_MEM_EST" ]; then m="~$STATE_MEM counted"; else m="${STATE_MEM}M"; fi
      printf '  %-24s %-5s %-34s %s\n' "${f[0]#app=}" "${f[1]#port=}" "${f[2]#service=}" "$m"
    else
      printf '  %-24s %s\n' "${f[0]#app=}" "${f[1]#folder=}"
    fi
  done < <(printf '%s\n' "${ST_LINES[@]}" | LC_ALL=C sort)
}
