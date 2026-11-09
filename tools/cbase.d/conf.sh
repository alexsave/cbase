# shellcheck shell=bash
# conf.sh - cbase.conf, the deploy tool's settings file in an app's repository (README.md
# "./cbase deploy tool"). Sourced by tools/cbase; nothing here calls AWS.
#
# THE FORMAT, strict on purpose: what is not one of these is refused with the line's number and a
# sentence, never guessed at.
#   - One setting per line, KEY = value. Blanks around the '=' are optional and trimmed, as are
#     blanks at either end of the value. The value is everything after the first '=', taken as
#     written: no quotes (a quote is refused), no escapes, no expansion. The file is read, never
#     sourced.
#   - A line whose first non-blank character is '#' is a comment; a blank line is skipped. A '#'
#     anywhere else is refused (a comment goes on a line of its own). A "\r\n" line end is "\n".
#   - KEY is one of the keys below. Every key but CBASE_ROUTE may appear once.
#
# THE KEYS:
#   CBASE_APP      the one required key: the app's name, [a-z][a-z0-9-]*, at most 32 characters, not "none".
#                  It names the app's systemd service, its port claim and its bucket folder.
#   CBASE_VM       the VM group the app runs in, or none (a static-only app); default CBASE_APP.
#   CBASE_STATIC   the static group (a bucket) its files go to, or none (a VM-only app); default
#                  CBASE_APP. Not both none.
#   CBASE_MEMORY   optional, for an app with a VM: the memory it declares, <n>M or <n>G (16M to
#                  64G). Declared, it is the service's hard cap (MemoryMax) and the app's exact
#                  count in its group's fit check. Unset, the service has no hard cap and the fit
#                  check (phase 3) counts it at CONF_MEMORY_ESTIMATE (128M), saying so:
#                  "shop: memory not declared, counted as 128M". conf_load resolves both:
#                  CONF_MEMORY (declared MiB, or "") and CONF_MEMORY_ESTIMATED (1 when unset).
#   CBASE_ROUTE    repeated, one route a line, matched in order. Written only to differ from the
#                  defaults: an app with a VM and static files gets "/api/* vm" and "* static", a
#                  static-only app "* static", a VM-only app "* vm". ANY CBASE_ROUTE line REPLACES
#                  the defaults (they are not merged in), except that CloudFront must have a
#                  default behaviour: written routes with no "*" get "* static" (or "* vm" for a
#                  VM-only app) added last. Each line is
#                    CBASE_ROUTE = <pattern> <origin> [cache|nocache] [<methods>]
#                  pattern: CloudFront's, "*" or a path starting with '/' ("/api/*").
#                  origin: vm, static, or ws (the VM, no caching, every method, long timeouts:
#                  a WebSocket route; it takes no more fields).
#                  cache: default cache for static, nocache for vm.
#                  methods: ALL or a comma list of GET,HEAD,OPTIONS,PUT,POST,PATCH,DELETE;
#                  default GET,HEAD for static (which takes only GET, HEAD and OPTIONS), ALL for
#                  vm. CloudFront allows the smallest of its three method sets that covers the
#                  list (GET,HEAD; GET,HEAD,OPTIONS; all), and the origin refuses the rest.
#                  A "*" route goes last, wherever it was written.
#   CBASE_DOMAIN   optional: the app's hostname (www.example.com). Without it the app is reached
#                  at its distribution's cloudfront.net address.
#   CBASE_CERT_ARN optional, an override: the ACM certificate (us-east-1, CloudFront's region)
#                  for the domain and the API hostname, arn:aws:acm:us-east-1:<account>:certificate/<uuid>.
#                  Without it deploy finds or makes one (cert.sh): an ISSUED us-east-1 certificate
#                  covering every hostname (its SANs and wildcards counted), else one it requested
#                  before and is still pending, else a new request with DNS validation.
#   CBASE_ZONE     optional, an override: the Route 53 hosted zone id (Z...) the domain's records
#                  go in. Without it deploy looks for the account's public hosted zone that covers
#                  each hostname (the longest suffix) and writes the records there; with none it
#                  prints the DNS records to add by hand and stops (cert.sh).
#   CBASE_API_HOST optional: a second hostname whose every request goes to the VM (api.example.com);
#                  needs CBASE_DOMAIN.
#   CBASE_STATIC_DIR optional, for an app with static files: the directory deploy uploads, relative
#                  to the repository's root, holding index.html. Default "build" for a static site
#                  (cdom's `make new` writes build/index.html) and "ui/out" for an app with a VM
#                  (the starter's page; its server builds elsewhere, so no binary is published).
#   CBASE_HASHED   optional: the content-hashed files, blank-separated globs matched against a file's
#                  path in the static dir (* crosses /), say "assets/*". They are cached for a year,
#                  immutable; html never is. Default none: every file is revalidated (no-cache).
#
# NO SECRETS. cbase.conf is committed and public. Before the syntax is read, every line is held
# to conf_secret_scan below, and one that looks like a secret stops the tool, naming the line and
# never echoing it.

CONF_KEYS="CBASE_APP CBASE_VM CBASE_STATIC CBASE_MEMORY CBASE_ROUTE CBASE_DOMAIN CBASE_CERT_ARN CBASE_ZONE CBASE_API_HOST CBASE_STATIC_DIR CBASE_HASHED"

# The parsed settings (conf_load fills them).
CONF_FILE=""
CONF_APP=""
CONF_VM=""
CONF_STATIC=""
CONF_MEMORY=""      # the declared MiB; "" when undeclared or the app has no VM
CONF_MEMORY_ESTIMATED=""   # 1: a VM app with no CBASE_MEMORY, counted at CONF_MEMORY_ESTIMATE
CONF_MEMORY_ESTIMATE=128   # MiB: the conservative count for an undeclared app (phase 3's fit check)
CONF_DOMAIN=""
CONF_CERT_ARN=""
CONF_ZONE=""
CONF_API_HOST=""
CONF_STATIC_DIR=""  # the static dir, no trailing '/'; "" for a VM-only app
CONF_STATIC_DIR_DEFAULT=""   # 1: CONF_STATIC_DIR is the default, not written
CONF_HASHED=()      # the content-hashed globs
CONF_ROUTES=()      # "<pattern> <origin> <cache> <methods>", normalized, the "*" route last

conf_die() {   # conf_die LINE SENTENCE: the file's line and why, and the tool stops
  echo "cbase: $CONF_FILE line $1: $2" >&2
  exit 1
}

conf_die_file() {
  echo "cbase: $CONF_FILE $1" >&2
  exit 1
}

# A key spelled as a name can be printed; anything else on a line that looks like a secret is not.
conf_printable_key() { [[ "$1" =~ ^[A-Z][A-Z0-9_]{0,63}$ ]]; }

# conf_secret_scan LINE_NO TEXT: stops the tool when the line looks like it holds a secret.
#   - the key's name holds SECRET, TOKEN, PASSWORD, PASSWD, PRIVATE or CREDENTIAL (any case);
#   - the line holds a PEM block's marker ("-----BEGIN" or "-----END");
#   - the line holds an AWS access key id: AKIA or ASIA and 16 more of [A-Z0-9];
#   - the line holds a long high-entropy run: 24 or more of [A-Za-z0-9+/] with an upper-case
#     letter, a lower-case letter and a digit in it (base64, an AWS secret key); 32 or more hex
#     digits in a row (a hex token); or 40 or more letters and digits in a row.
# The values cbase.conf takes are never so: a name, a hostname, a lower-case hex ARN in
# dash-separated groups, a Z... zone id of upper-case letters and digits, a route.
conf_secret_scan() {
  local no="$1" text="$2" key="" what="" run
  if [[ "$text" == *=* ]]; then
    key="${text%%=*}"
    key="${key#"${key%%[![:blank:]]*}"}"
    key="${key%"${key##*[![:blank:]]}"}"
  fi
  local upper
  upper="$(printf '%s' "$key" | tr '[:lower:]' '[:upper:]')"
  case "$upper" in
    *SECRET*|*TOKEN*|*PASSWORD*|*PASSWD*|*PRIVATE*|*CREDENTIAL*)
      if conf_printable_key "$key"; then
        conf_die "$no" "$key is named like a secret. cbase.conf is committed and public, so a secret never goes in it: keep it in the app's environment on the VM (or SSM), and out of the repository"
      fi
      conf_die "$no" "the key is named like a secret. cbase.conf is committed and public, so a secret never goes in it" ;;
  esac
  if [[ "$text" == *-----BEGIN* || "$text" == *-----END* ]]; then
    what="a PEM block (a key or a certificate's private part)"
  elif [[ "$text" =~ (AKIA|ASIA)[A-Z0-9]{16} ]]; then
    what="an AWS access key id"
  else
    while IFS= read -r run; do
      [ -n "$run" ] || continue
      if [[ ${#run} -ge 24 && "$run" =~ [A-Z] && "$run" =~ [a-z] && "$run" =~ [0-9] ]]; then
        what="a long random-looking string (an access key, a token)"; break
      fi
    done < <(printf '%s\n' "$text" | grep -oE '[A-Za-z0-9+/]{24,}' || true)
    if [ -z "$what" ] && printf '%s\n' "$text" | grep -qE '[0-9A-Fa-f]{32,}'; then
      what="a long hex string (a token, a key)"
    elif [ -z "$what" ] && printf '%s\n' "$text" | grep -qE '[A-Za-z0-9]{40,}'; then
      what="a long random-looking string (an access key, a token)"
    fi
  fi
  [ -z "$what" ] && return 0
  if conf_printable_key "$key"; then
    conf_die "$no" "the value of $key looks like a secret: $what. cbase.conf is committed and public, so a secret never goes in it"
  fi
  conf_die "$no" "this line looks like a secret: $what. cbase.conf is committed and public, so a secret never goes in it"
}

conf_name_ok() { [[ "$1" =~ ^[a-z]([a-z0-9-]{0,30}[a-z0-9])?$ ]]; }

conf_host_ok() {
  local h="$1" label
  [ "${#h}" -le 253 ] || return 1
  [[ "$h" =~ ^[a-z0-9.-]+$ ]] || return 1   # and so no glob character reaches the loop below
  [[ "$h" == *.* ]] || return 1
  local IFS=.
  for label in $h; do
    [[ "$label" =~ ^[a-z0-9]([a-z0-9-]{0,61}[a-z0-9])?$ ]] || return 1
  done
}

# conf_route LINE_NO VALUE: one CBASE_ROUTE, appended to CONF_ROUTES normalized.
conf_route() {
  local no="$1" value="$2" pattern origin cache="" methods="" f rest
  read -r -a rest <<< "$value"
  pattern="${rest[0]:-}"
  origin="${rest[1]:-}"
  [ -n "$origin" ] || conf_die "$no" "CBASE_ROUTE is \"<pattern> <origin> [cache|nocache] [<methods>]\", and this one has no origin (vm, static or ws)"
  if [ "$pattern" != "*" ] && ! [[ "$pattern" =~ ^/[A-Za-z0-9_.~*/-]{0,254}$ ]]; then
    conf_die "$no" "CBASE_ROUTE's pattern is \"*\" or a path starting with '/' of letters, digits and _ . ~ * / - (CloudFront's path patterns)"
  fi
  case "$origin" in
    vm|static) ;;
    ws)
      [ "${#rest[@]}" -eq 2 ] || conf_die "$no" "a ws route takes no cache or methods: it is the VM, uncached, every method, with long timeouts"
      ;;
    *) conf_die "$no" "CBASE_ROUTE's origin is vm, static or ws" ;;
  esac
  for f in "${rest[@]:2}"; do
    case "$f" in
      cache|nocache)
        [ -z "$cache" ] || conf_die "$no" "CBASE_ROUTE names its cache policy twice"
        cache="$f" ;;
      *)
        [ -z "$methods" ] || conf_die "$no" "CBASE_ROUTE names its methods twice (one comma list, no blanks)"
        methods="$(conf_methods "$no" "$f")" || exit 1 ;;
    esac
  done
  if [ "$origin" = static ]; then
    cache="${cache:-cache}"
    methods="${methods:-GET,HEAD}"
    case "$methods" in
      GET|HEAD|OPTIONS|GET,HEAD|GET,OPTIONS|HEAD,OPTIONS|GET,HEAD,OPTIONS) ;;
      *) conf_die "$no" "a static route takes only GET, HEAD and OPTIONS (a bucket answers nothing else)" ;;
    esac
  elif [ "$origin" = ws ]; then
    cache=nocache
    methods=ALL
  else
    cache="${cache:-nocache}"
    methods="${methods:-ALL}"
  fi
  local r
  for r in "${CONF_ROUTES[@]+"${CONF_ROUTES[@]}"}"; do
    [ "${r%% *}" != "$pattern" ] || conf_die "$no" "CBASE_ROUTE $pattern is routed twice"
  done
  CONF_ROUTES+=("$pattern $origin $cache $methods")
}

# conf_methods LINE_NO LIST: ALL, or the list's methods once each in a fixed order.
conf_methods() {
  local no="$1" list="$2" m out="" known="GET HEAD OPTIONS PUT POST PATCH DELETE"
  if [ "$list" = ALL ]; then echo ALL; return 0; fi
  [[ "$list" =~ ^[A-Z]+(,[A-Z]+)*$ ]] || conf_die "$no" "CBASE_ROUTE's methods are ALL or a comma list of $known (upper case, no blanks)"
  local IFS=,
  for m in $list; do
    [[ " $known " == *" $m "* ]] || conf_die "$no" "$m is not a method CloudFront passes ($known)"
  done
  IFS=' '
  for m in $known; do
    [[ ",$list," == *",$m,"* ]] && out="${out:+$out,}$m"
  done
  [ "$out" != "GET,HEAD,OPTIONS,PUT,POST,PATCH,DELETE" ] || out=ALL
  echo "$out"
}

# conf_load FILE: reads and checks cbase.conf, filling the CONF_ variables. Stops on any error.
conf_load() {
  CONF_FILE="$1"
  [ -f "$CONF_FILE" ] || conf_die_file "does not exist (an app's settings are cbase.conf at its repository's root, or -f FILE)"
  [ -r "$CONF_FILE" ] || conf_die_file "cannot be read"
  # A NUL byte: the file's size and its size with NULs dropped differ (read never sees one).
  if [ "$(LC_ALL=C tr -d '\000' < "$CONF_FILE" | wc -c)" -ne "$(wc -c < "$CONF_FILE")" ]; then conf_die_file "holds a NUL byte"; fi
  local lines=() line no=0
  while IFS= read -r line || [ -n "$line" ]; do
    lines+=("${line%$'\r'}")
  done < "$CONF_FILE"
  # Every line is held to the secret scan first, before anything is parsed or printed.
  for line in "${lines[@]+"${lines[@]}"}"; do
    no=$((no + 1))
    conf_secret_scan "$no" "$line"
  done
  local key value seen=" " seen_at=" " k
  local app_no="" vm_no="" static_no="" mem_no="" domain_no="" cert_no="" zone_no="" api_no=""
  local dir_no="" hashed_no="" dir_set="" g
  local vm_set="" static_set="" memory="" route_nos=()
  CONF_HASHED=()
  no=0
  for line in "${lines[@]+"${lines[@]}"}"; do
    no=$((no + 1))
    [[ "$line" =~ ^[[:blank:]]*$ ]] && continue
    [[ "$line" =~ ^[[:blank:]]*# ]] && continue
    [[ "$line" == *=* ]] || conf_die "$no" "is not KEY = value (no '=')"
    key="${line%%=*}"
    value="${line#*=}"
    key="${key#"${key%%[![:blank:]]*}"}"
    key="${key%"${key##*[![:blank:]]}"}"
    value="${value#"${value%%[![:blank:]]*}"}"
    value="${value%"${value##*[![:blank:]]}"}"
    [[ "$key" =~ ^[A-Z][A-Z0-9_]*$ ]] || conf_die "$no" "is not KEY = value (a key is upper-case letters, digits and _, and starts the line)"
    [[ " $CONF_KEYS " == *" $key "* ]] || conf_die "$no" "$key is not a cbase.conf key (the keys: $CONF_KEYS)"
    [[ "$value" != *'#'* ]] || conf_die "$no" "has a '#' in its value: a comment goes on a line of its own"
    [[ "$value" != *[\"\']* ]] || conf_die "$no" "has a quote in its value: values are never quoted (everything after the '=' is the value)"
    [ -n "$value" ] || conf_die "$no" "$key has no value (leave the line out for the default)"
    if [ "$key" != CBASE_ROUTE ] && [[ "$seen" == *" $key "* ]]; then
      for k in $seen_at; do
        [ "${k%%:*}" != "$key" ] || conf_die "$no" "sets $key again (line ${k#*:} set it)"
      done
    fi
    seen="$seen$key "
    seen_at="$seen_at$key:$no "
    case "$key" in
      CBASE_APP)
        { conf_name_ok "$value" && [ "$value" != none ]; } \
          || conf_die "$no" "CBASE_APP is a name: a lower-case letter, then lower-case letters, digits and '-', at most 32 characters, not ending in '-' (and not none)"
        CONF_APP="$value"; app_no="$no" ;;
      CBASE_VM|CBASE_STATIC)
        { [ "$value" = none ] || conf_name_ok "$value"; } \
          || conf_die "$no" "$key is a group name (a lower-case letter, then lower-case letters, digits and '-', at most 32 characters) or none"
        if [ "$key" = CBASE_VM ]; then vm_set="$value"; vm_no="$no"; else static_set="$value"; static_no="$no"; fi ;;
      CBASE_MEMORY)
        [[ "$value" =~ ^([0-9]{1,6})([MG])$ ]] || conf_die "$no" "CBASE_MEMORY is <n>M or <n>G (256M, 1G)"
        memory=$((10#${BASH_REMATCH[1]}))
        [ "${BASH_REMATCH[2]}" = M ] || memory=$((memory * 1024))
        { [ "$memory" -ge 16 ] && [ "$memory" -le 65536 ]; } || conf_die "$no" "CBASE_MEMORY is from 16M to 64G"
        mem_no="$no" ;;
      CBASE_ROUTE)
        route_nos+=("$no")
        conf_route "$no" "$value" ;;
      CBASE_DOMAIN|CBASE_API_HOST)
        conf_host_ok "$value" || conf_die "$no" "$key is a lower-case hostname (www.example.com)"
        if [ "$key" = CBASE_DOMAIN ]; then CONF_DOMAIN="$value"; domain_no="$no"; else CONF_API_HOST="$value"; api_no="$no"; fi ;;
      CBASE_CERT_ARN)
        if [[ "$value" =~ ^arn:aws:acm:([a-z0-9-]+):[0-9]{12}:certificate/[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$ ]]; then
          [ "${BASH_REMATCH[1]}" = us-east-1 ] || conf_die "$no" "CBASE_CERT_ARN is in ${BASH_REMATCH[1]}, and CloudFront takes a certificate from us-east-1 alone"
        else
          conf_die "$no" "CBASE_CERT_ARN is an ACM certificate's ARN: arn:aws:acm:us-east-1:<account>:certificate/<uuid>"
        fi
        CONF_CERT_ARN="$value"; cert_no="$no" ;;
      CBASE_ZONE)
        [[ "$value" =~ ^Z[A-Z0-9]{1,31}$ ]] || conf_die "$no" "CBASE_ZONE is a Route 53 hosted zone id (Z and upper-case letters and digits)"
        CONF_ZONE="$value"; zone_no="$no" ;;
      CBASE_STATIC_DIR)
        value="${value%/}"
        { [[ "$value" =~ ^[A-Za-z0-9_-][A-Za-z0-9._-]*(/[A-Za-z0-9_-][A-Za-z0-9._-]*)*$ ]] && [[ "/$value/" != */../* ]]; } \
          || conf_die "$no" "CBASE_STATIC_DIR is a directory inside the repository, relative to its root (build, ui/out): letters, digits and . _ - /, no leading '/', no '..'"
        dir_set="$value"; dir_no="$no" ;;
      CBASE_HASHED)
        read -r -a CONF_HASHED <<< "$value"
        for g in "${CONF_HASHED[@]}"; do
          [[ "$g" =~ ^[A-Za-z0-9._/*?-]+$ ]] && [[ "$g" != /* ]] \
            || conf_die "$no" "CBASE_HASHED is blank-separated globs of letters, digits and . _ - / * ? (assets/*), each a path in the static dir with no leading '/'"
        done
        hashed_no="$no" ;;
    esac
  done
  [ -n "$app_no" ] || conf_die_file "has no CBASE_APP line (the app's name: CBASE_APP = myapp)"
  CONF_VM="${vm_set:-$CONF_APP}"
  CONF_STATIC="${static_set:-$CONF_APP}"
  if [ "$CONF_VM" = none ] && [ "$CONF_STATIC" = none ]; then
    conf_die "$(( ${vm_no:-0} > ${static_no:-0} ? ${vm_no:-0} : ${static_no:-0} ))" "CBASE_VM and CBASE_STATIC are both none: an app has a VM, static files, or both"
  fi
  if [ "$CONF_VM" = none ]; then
    [ -z "$mem_no" ] || conf_die "$mem_no" "CBASE_MEMORY is for an app with a VM, and CBASE_VM is none"
    [ -z "$api_no" ] || conf_die "$api_no" "CBASE_API_HOST sends every request to the VM, and CBASE_VM is none"
  else
    CONF_MEMORY="$memory"
    if [ -z "$mem_no" ]; then CONF_MEMORY_ESTIMATED=1; else CONF_MEMORY_ESTIMATED=""; fi
  fi
  local i r origin
  for i in "${!CONF_ROUTES[@]}"; do
    r="${CONF_ROUTES[$i]}"
    origin="$(echo "$r" | cut -d' ' -f2)"
    if [ "$origin" = static ] && [ "$CONF_STATIC" = none ]; then
      conf_die "${route_nos[$i]}" "routes to static, and CBASE_STATIC is none"
    fi
    if [ "$origin" != static ] && [ "$CONF_VM" = none ]; then
      conf_die "${route_nos[$i]}" "routes to the VM ($origin), and CBASE_VM is none"
    fi
  done
  [ -z "$api_no" ] || [ -n "$domain_no" ] || conf_die "$api_no" "CBASE_API_HOST is a second hostname, and there is no CBASE_DOMAIN (the first)"
  [ -z "$cert_no" ] || [ -n "$domain_no" ] || conf_die "$cert_no" "CBASE_CERT_ARN is for CBASE_DOMAIN, and there is none (without a domain the app is at its cloudfront.net address)"
  [ -z "$zone_no" ] || [ -n "$domain_no" ] || conf_die "$zone_no" "CBASE_ZONE is where CBASE_DOMAIN's record goes, and there is no CBASE_DOMAIN"
  if [ "$CONF_STATIC" = none ]; then
    [ -z "$dir_no" ] || conf_die "$dir_no" "CBASE_STATIC_DIR is the static files' directory, and CBASE_STATIC is none"
    [ -z "$hashed_no" ] || conf_die "$hashed_no" "CBASE_HASHED names static files, and CBASE_STATIC is none"
    CONF_STATIC_DIR=""; CONF_STATIC_DIR_DEFAULT=""
  elif [ -n "$dir_set" ]; then
    CONF_STATIC_DIR="$dir_set"; CONF_STATIC_DIR_DEFAULT=""
  else
    if [ "$CONF_VM" = none ]; then CONF_STATIC_DIR=build; else CONF_STATIC_DIR=ui/out; fi
    CONF_STATIC_DIR_DEFAULT=1
  fi
  # No route written: the defaults for the app's shape.
  if [ "${#CONF_ROUTES[@]}" -eq 0 ]; then
    if [ "$CONF_VM" != none ] && [ "$CONF_STATIC" != none ]; then CONF_ROUTES=("/api/* vm nocache ALL"); fi
  fi
  local has_default=""
  for r in "${CONF_ROUTES[@]+"${CONF_ROUTES[@]}"}"; do
    [ "${r%% *}" != "*" ] || has_default=1
  done
  if [ -z "$has_default" ]; then
    if [ "$CONF_STATIC" != none ]; then CONF_ROUTES+=("* static cache GET,HEAD"); else CONF_ROUTES+=("* vm nocache ALL"); fi
  else
    # The default route is CloudFront's default behaviour: it goes last, wherever it was written.
    local rest=() def=""
    for r in "${CONF_ROUTES[@]}"; do
      if [ "${r%% *}" = "*" ]; then def="$r"; else rest+=("$r"); fi
    done
    CONF_ROUTES=("${rest[@]+"${rest[@]}"}" "$def")
  fi
}

# conf_print: the settings as read, defaults filled in (config check, doctor).
conf_print() {
  local r
  printf '%-10s %s\n' app "$CONF_APP"
  if [ "$CONF_VM" = none ]; then printf '%-10s %s\n' vm none
  elif [ -n "$CONF_MEMORY_ESTIMATED" ]; then
    printf '%-10s %s (%s: memory not declared, counted as %sM)\n' vm "$CONF_VM" "$CONF_APP" "$CONF_MEMORY_ESTIMATE"
  else printf '%-10s %s (memory %s MiB)\n' vm "$CONF_VM" "$CONF_MEMORY"; fi
  printf '%-10s %s\n' static "$CONF_STATIC"
  if [ -n "$CONF_STATIC_DIR" ]; then
    if [ -n "$CONF_STATIC_DIR_DEFAULT" ]; then printf '%-10s %s/ (the default)\n' files "$CONF_STATIC_DIR"
    else printf '%-10s %s/\n' files "$CONF_STATIC_DIR"; fi
  fi
  [ "${#CONF_HASHED[@]}" -eq 0 ] || printf '%-10s %s\n' hashed "${CONF_HASHED[*]}"
  [ -z "$CONF_DOMAIN" ] || printf '%-10s %s\n' domain "$CONF_DOMAIN"
  [ -z "$CONF_API_HOST" ] || printf '%-10s %s\n' api-host "$CONF_API_HOST"
  if [ -n "$CONF_CERT_ARN" ]; then printf '%-10s %s\n' cert "$CONF_CERT_ARN"
  elif [ -n "$CONF_DOMAIN" ]; then printf '%-10s %s\n' cert "(found or requested by deploy)"
  else printf '%-10s %s\n' address "(the distribution's cloudfront.net name)"; fi
  [ -z "$CONF_ZONE" ] || printf '%-10s %s\n' zone "$CONF_ZONE"
  for r in "${CONF_ROUTES[@]}"; do printf '%-10s %s\n' route "$r"; done
}
