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
#                  check (vm.sh) counts it at CONF_MEMORY_ESTIMATE (128M), saying so:
#                  "shop: memory not declared, counted as 128M". conf_load resolves both:
#                  CONF_MEMORY (declared MiB, or "") and CONF_MEMORY_ESTIMATED (1 when unset).
#   CBASE_VM_STACK optional, adoption: the CloudFormation stack (in the CLI's region) that already
#                  holds the group's VM (an AWS::EC2::Instance, or an auto-scaling group of one). The
#                  group (CBASE_VM still names it, and its state file) is found by that stack rather
#                  than by its tag, and needs no group-init; deploy never changes that stack.
#   CBASE_VM_ORIGIN optional: the hostname CloudFront reaches the VM at, an AAAA record of its
#                  fixed IPv6 address (an IPv6-only VM has no public DNS name of AWS's); the origin
#                  is then IPv6 (IpAddressType ipv6). Default: the group's own (group-init's
#                  --origin-host, an instance tag), else the instance's public DNS name.
#   CBASE_VM_PORT, CBASE_VM_DATA optional, both or neither: a grandfathered member, an app that ran
#                  on the VM before cbase. Its port (any of 1024-65535, kept outside the 8700-8799
#                  claims) and its data directory (an absolute path, CBASE_DB is <dir>/cbase.sqlite)
#                  are recorded in the state file as they are ("data=<dir> legacy=1"), and the
#                  service runs there; only a change to these keys moves them.
#   CBASE_VM_SETTINGS optional: the SSM path of the app's settings, which the instance reads into
#                  its env file (the origin secret is its CBASE_ORIGIN_SECRET); default /cbase/<app>/.
#                  An adopted VM's role reads its own path, which this names.
#   CBASE_VM_ARTIFACTS optional: s3://<bucket>/<prefix>/ the app's servers go to, one object per
#                  sha256, and the instance fetches from; default s3://cbase-state-<account>/artifacts/<app>/.
#                  An adopted VM's role reads its own, which this names.
#   CBASE_VM_REPLACES optional, with CBASE_VM_PORT: the systemd unit (name.service) that served the
#                  grandfathered app before cbase. The deploy stops and disables it right before
#                  the app's own unit starts, in the same command; never a unit it is not named.
#   CBASE_BACKUP_KEY optional: the backup's object key; default backups/<app>/cbase.sqlite. An
#                  adopted VM whose boot restores another key names that one, so the two never part.
#   CBASE_BACKUP_BUCKET optional, for an app with a VM: the bucket its backups go to, as
#                  s3://<bucket>/backups/<app>/cbase.sqlite (serve's CBASE_BACKUP_*). Default the
#                  state bucket, which a cbase group's instance role may write under backups/ alone.
#                  An adopted VM's own bucket is named here, so no backup moves.
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
#   CBASE_DOMAIN   optional: the app's hostnames, blank-separated (example.com www.example.com),
#                  each an alias of the one distribution, all on one certificate, each with its own
#                  DNS record (in its own zone). The first is the app's URL. Without it the app is
#                  reached at its distribution's cloudfront.net address.
#   CBASE_CERT_ARN optional, an override: the ACM certificate (us-east-1, CloudFront's region)
#                  for the domain and the API hostname, arn:aws:acm:us-east-1:<account>:certificate/<uuid>.
#                  Without it deploy finds or makes one (cert.sh): an ISSUED us-east-1 certificate
#                  covering every hostname (its SANs and wildcards counted), else one it requested
#                  before and is still pending, else a new request with DNS validation.
#   CBASE_API_CERT_ARN optional, an override like CBASE_CERT_ARN: the certificate CBASE_API_HOST's
#                  distribution serves, used as it is. Without it that distribution serves
#                  CBASE_CERT_ARN's (or the found one), which must then cover the API host too.
#   CBASE_ZONE     optional, an override: the Route 53 hosted zone id (Z...) the domain's records
#                  go in. Without it deploy looks for the account's public hosted zone that covers
#                  each hostname (the longest suffix) and writes the records there; with none it
#                  prints the DNS records to add by hand and stops (cert.sh).
#   CBASE_API_HOST optional: a second hostname whose every request goes to the VM (api.example.com);
#                  needs CBASE_DOMAIN. Its distribution: 60 s read and keep-alive to the VM, CloudFront's
#                  Server-Timing on every answer, the ws routes on the ws origin, and its own routes:
#   CBASE_API_ROUTE repeated, the API host's behaviours before its default "* vm nocache ALL", as
#                  CBASE_ROUTE's lines with origin vm or ws alone. The cache mode origin (vm routes,
#                  here or in CBASE_ROUTE) keeps what the origin's Cache-Control says
#                  (Managed-UseOriginCacheControlHeaders-QueryStrings: a query string is its own
#                  entry): CBASE_API_ROUTE = /v1/leaderboard vm origin GET,HEAD,OPTIONS.
#   CBASE_STATIC_BUCKET optional, adoption: an existing bucket the app's files are in, found by its
#                  name (no static group, no group-init, no state file). Deploy never replaces,
#                  removes or rewrites it, nor its policy: one that does not let the account's
#                  distributions read it gets the statement to add printed (static.sh).
#   CBASE_STATIC_PREFIX optional: the app's folder in its bucket, its distribution's OriginPath;
#                  default the app's name. An adopted bucket's existing folder is named here.
#   CBASE_STATIC_DIR optional, for an app with static files: the directory deploy uploads, relative
#                  to the repository's root, holding index.html. Default "build" for a static site
#                  (a page's `make new` writes build/index.html) and "ui/out" for an app with a VM
#                  (the starter's page; its server builds elsewhere, so no binary is published).
#   CBASE_SHELL    optional, for an app with static files: the app shell, the page of the build
#                  the router answers every path with that is neither a file nor a page (an SPA's
#                  client-side routes: /abc123, /game/7); default /index.html. stack.sh's
#                  stack_router holds the rule.
#   CBASE_HASHED   optional: the content-hashed files, blank-separated globs matched against a file's
#                  path in the static dir (* crosses /), say "assets/*". They are cached for a year,
#                  immutable; html never is. Default none: every file is revalidated (no-cache).
#
# NO SECRETS. cbase.conf is committed and public. Before the syntax is read, every line is held
# to conf_secret_scan below, and one that looks like a secret stops the tool, naming the line and
# never echoing it.

CONF_KEYS="CBASE_APP CBASE_VM CBASE_VM_STACK CBASE_VM_ORIGIN CBASE_VM_PORT CBASE_VM_DATA CBASE_VM_SETTINGS CBASE_VM_ARTIFACTS CBASE_VM_REPLACES CBASE_BACKUP_KEY CBASE_BACKUP_BUCKET CBASE_STATIC CBASE_STATIC_BUCKET CBASE_STATIC_PREFIX CBASE_MEMORY CBASE_ROUTE CBASE_DOMAIN CBASE_CERT_ARN CBASE_API_CERT_ARN CBASE_ZONE CBASE_API_HOST CBASE_API_ROUTE CBASE_STATIC_DIR CBASE_HASHED CBASE_SHELL"

# The parsed settings (conf_load fills them).
CONF_FILE=""
CONF_APP=""
CONF_VM=""
CONF_STATIC=""
CONF_MEMORY=""      # the declared MiB; "" when undeclared or the app has no VM
CONF_MEMORY_ESTIMATED=""   # 1: a VM app with no CBASE_MEMORY, counted at CONF_MEMORY_ESTIMATE
CONF_MEMORY_ESTIMATE=128   # MiB: the conservative count for an undeclared app (vm.sh's fit check)
CONF_VM_STACK=""     # the adopted stack that holds the group's VM, or ""
CONF_VM_ORIGIN=""    # the VM's origin hostname, or ""
CONF_VM_PORT=""      # a grandfathered member's port and data dir, or "" (both)
CONF_VM_DATA=""
CONF_BACKUP_BUCKET=""   # the backups' bucket, or "" for the state bucket
CONF_STATIC_BUCKET=""   # an adopted bucket, or ""
CONF_STATIC_PREFIX=""   # the app's folder in its bucket, or "" for the app's name
CONF_VM_REPLACES=""     # the unit a grandfathered member replaces, or ""
CONF_BACKUP_KEY=""      # the backup's key, or "" for backups/<app>/cbase.sqlite
CONF_VM_SETTINGS=""     # the settings' SSM path, or "" for /cbase/<app>/
CONF_VM_ARTIFACTS=""    # s3://<bucket>/<prefix>/ of the servers, or "" for the state bucket's
CONF_DOMAIN=""       # the first of CONF_DOMAINS (the app's URL), or ""
CONF_DOMAINS=()      # every CBASE_DOMAIN hostname, in the file's order
CONF_CERT_ARN=""
CONF_API_CERT_ARN=""
CONF_ZONE=""
CONF_API_HOST=""
CONF_STATIC_DIR=""  # the static dir, no trailing '/'; "" for a VM-only app
CONF_STATIC_DIR_DEFAULT=""   # 1: CONF_STATIC_DIR is the default, not written
CONF_HASHED=()      # the content-hashed globs
CONF_SHELL=""       # the app shell's path (/_shell.html), or ""
CONF_ROUTES=()      # "<pattern> <origin> <cache> <methods>", normalized, the "*" route last
CONF_API_ROUTES=()  # CBASE_API_ROUTE's, the same form, in the file's order

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

# conf_route LINE_NO VALUE [api]: one CBASE_ROUTE, appended to CONF_ROUTES normalized; with api,
# one CBASE_API_ROUTE, appended to CONF_API_ROUTES (the VM alone: vm or ws, never "*").
conf_route() {
  local no="$1" value="$2" api="${3:-}" pattern origin cache="" methods="" f rest key=CBASE_ROUTE
  [ -z "$api" ] || key=CBASE_API_ROUTE
  read -r -a rest <<< "$value"
  pattern="${rest[0]:-}"
  origin="${rest[1]:-}"
  [ -n "$origin" ] || conf_die "$no" "$key is \"<pattern> <origin> [cache|nocache] [<methods>]\", and this one has no origin (vm, static or ws)"
  if [ "$pattern" != "*" ] && ! [[ "$pattern" =~ ^/[A-Za-z0-9_.~*/-]{0,254}$ ]]; then
    conf_die "$no" "$key's pattern is \"*\" or a path starting with '/' of letters, digits and _ . ~ * / - (CloudFront's path patterns)"
  fi
  if [ -n "$api" ]; then
    [ "$pattern" != "*" ] || conf_die "$no" "CBASE_API_ROUTE's pattern is a path: the API host's default is the VM already"
    case "$origin" in vm|ws) ;; *) conf_die "$no" "CBASE_API_ROUTE's origin is vm or ws (the API host has no static files)" ;; esac
  fi
  case "$origin" in
    vm|static) ;;
    ws)
      [ "${#rest[@]}" -eq 2 ] || conf_die "$no" "a ws route takes no cache or methods: it is the VM, uncached, every method, with long timeouts"
      ;;
    *) conf_die "$no" "$key's origin is vm, static or ws" ;;
  esac
  for f in "${rest[@]:2}"; do
    case "$f" in
      cache|nocache|origin)
        [ -z "$cache" ] || conf_die "$no" "$key names its cache policy twice"
        [ "$f" != origin ] || [ "$origin" = vm ] || conf_die "$no" "the cache mode origin (the origin's Cache-Control) is for a vm route"
        cache="$f" ;;
      *)
        [ -z "$methods" ] || conf_die "$no" "$key names its methods twice (one comma list, no blanks)"
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
  if [ -n "$api" ]; then
    for r in "${CONF_API_ROUTES[@]+"${CONF_API_ROUTES[@]}"}"; do
      [ "${r%% *}" != "$pattern" ] || conf_die "$no" "CBASE_API_ROUTE $pattern is routed twice"
    done
    CONF_API_ROUTES+=("$pattern $origin $cache $methods")
    return 0
  fi
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
  local app_no="" vm_no="" static_no="" mem_no="" domain_no="" cert_no="" zone_no="" api_no="" api_cert_no=""
  local dir_no="" hashed_no="" shell_no="" dir_set="" g
  local vm_set="" static_set="" memory="" route_nos=() api_route_no="" vmkey_no=""
  CONF_VM_STACK=""; CONF_VM_ORIGIN=""; CONF_VM_PORT=""; CONF_VM_DATA=""; CONF_BACKUP_BUCKET=""
  CONF_VM_SETTINGS=""; CONF_VM_ARTIFACTS=""; CONF_STATIC_BUCKET=""; CONF_STATIC_PREFIX=""
  CONF_VM_REPLACES=""; CONF_BACKUP_KEY=""
  local stkey_no=""
  CONF_API_ROUTES=()
  CONF_HASHED=()
  CONF_SHELL=""
  CONF_DOMAINS=()
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
    if [ "$key" != CBASE_ROUTE ] && [ "$key" != CBASE_API_ROUTE ] && [[ "$seen" == *" $key "* ]]; then
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
      CBASE_VM_STACK)
        [[ "$value" =~ ^[A-Za-z][A-Za-z0-9-]{0,127}$ ]] || conf_die "$no" "CBASE_VM_STACK is a CloudFormation stack's name (a letter, then letters, digits and -)"
        CONF_VM_STACK="$value"; vmkey_no="$no" ;;
      CBASE_VM_ORIGIN)
        conf_host_ok "$value" || conf_die "$no" "CBASE_VM_ORIGIN is a lower-case hostname (vm-origin.example.com)"
        CONF_VM_ORIGIN="$value"; vmkey_no="$no" ;;
      CBASE_VM_PORT)
        { [[ "$value" =~ ^[0-9]{4,5}$ ]] && [ "$((10#$value))" -ge 1024 ] && [ "$((10#$value))" -le 65535 ]; } \
          || conf_die "$no" "CBASE_VM_PORT is a port, 1024 to 65535"
        { [ "$((10#$value))" -lt 8700 ] || [ "$((10#$value))" -gt 8799 ]; } \
          || conf_die "$no" "CBASE_VM_PORT is a grandfathered member's own port, outside the 8700-8799 cbase claims"
        CONF_VM_PORT="$((10#$value))"; vmkey_no="$no" ;;
      CBASE_VM_DATA)
        { [[ "$value" =~ ^(/[A-Za-z0-9_][A-Za-z0-9._-]*)+$ ]] && [[ "$value/" != */../* ]] && [[ "$value/" != /srv/cbase/* ]]; } \
          || conf_die "$no" "CBASE_VM_DATA is an absolute directory on the VM (/var/lib/app), not under /srv/cbase/"
        CONF_VM_DATA="$value"; vmkey_no="$no" ;;
      CBASE_VM_REPLACES)
        [[ "$value" =~ ^[A-Za-z0-9_.@-]+\.service$ ]] || conf_die "$no" "CBASE_VM_REPLACES is a systemd unit's name (name.service)"
        CONF_VM_REPLACES="$value"; vmkey_no="$no" ;;
      CBASE_BACKUP_KEY)
        [[ "$value" =~ ^[A-Za-z0-9_-][A-Za-z0-9._-]*(/[A-Za-z0-9_-][A-Za-z0-9._-]*)*$ ]] || conf_die "$no" "CBASE_BACKUP_KEY is an object key (cbase/latest.sqlite): letters, digits and . _ - /, no leading /"
        CONF_BACKUP_KEY="$value"; vmkey_no="$no" ;;
      CBASE_STATIC_BUCKET)
        [[ "$value" =~ ^[a-z0-9][a-z0-9.-]{1,61}[a-z0-9]$ ]] || conf_die "$no" "CBASE_STATIC_BUCKET is an S3 bucket's name"
        CONF_STATIC_BUCKET="$value"; stkey_no="$no" ;;
      CBASE_STATIC_PREFIX)
        [[ "$value" =~ ^[A-Za-z0-9_-][A-Za-z0-9._-]*(/[A-Za-z0-9_-][A-Za-z0-9._-]*)*$ ]] \
          || conf_die "$no" "CBASE_STATIC_PREFIX is a folder in the bucket (site, sites/one): letters, digits and . _ - /, no leading or trailing /"
        CONF_STATIC_PREFIX="$value"; stkey_no="$no" ;;
      CBASE_VM_SETTINGS)
        [[ "$value" =~ ^(/[A-Za-z0-9_.-]+)+/$ ]] || conf_die "$no" "CBASE_VM_SETTINGS is an SSM path, starting and ending with / (/app/env/)"
        CONF_VM_SETTINGS="$value"; vmkey_no="$no" ;;
      CBASE_VM_ARTIFACTS)
        [[ "$value" =~ ^s3://[a-z0-9][a-z0-9.-]{1,61}[a-z0-9](/[A-Za-z0-9_.-]+)*/$ ]] || conf_die "$no" "CBASE_VM_ARTIFACTS is s3://<bucket>/<prefix>/, ending with /"
        CONF_VM_ARTIFACTS="$value"; vmkey_no="$no" ;;
      CBASE_BACKUP_BUCKET)
        [[ "$value" =~ ^[a-z0-9][a-z0-9.-]{1,61}[a-z0-9]$ ]] || conf_die "$no" "CBASE_BACKUP_BUCKET is an S3 bucket's name"
        CONF_BACKUP_BUCKET="$value"; vmkey_no="$no" ;;
      CBASE_MEMORY)
        [[ "$value" =~ ^([0-9]{1,6})([MG])$ ]] || conf_die "$no" "CBASE_MEMORY is <n>M or <n>G (256M, 1G)"
        memory=$((10#${BASH_REMATCH[1]}))
        [ "${BASH_REMATCH[2]}" = M ] || memory=$((memory * 1024))
        { [ "$memory" -ge 16 ] && [ "$memory" -le 65536 ]; } || conf_die "$no" "CBASE_MEMORY is from 16M to 64G"
        mem_no="$no" ;;
      CBASE_ROUTE)
        route_nos+=("$no")
        conf_route "$no" "$value" ;;
      CBASE_API_ROUTE)
        api_route_no="$no"
        conf_route "$no" "$value" api ;;
      CBASE_DOMAIN)
        read -r -a CONF_DOMAINS <<< "$value"
        for g in "${!CONF_DOMAINS[@]}"; do
          conf_host_ok "${CONF_DOMAINS[$g]}" || conf_die "$no" "CBASE_DOMAIN is lower-case hostnames, blank-separated (example.com www.example.com)"
          for k in "${CONF_DOMAINS[@]:0:$g}"; do
            [ "$k" != "${CONF_DOMAINS[$g]}" ] || conf_die "$no" "CBASE_DOMAIN names ${CONF_DOMAINS[$g]} twice"
          done
        done
        CONF_DOMAIN="${CONF_DOMAINS[0]}"; domain_no="$no" ;;
      CBASE_API_HOST)
        conf_host_ok "$value" || conf_die "$no" "CBASE_API_HOST is one lower-case hostname (api.example.com)"
        CONF_API_HOST="$value"; api_no="$no" ;;
      CBASE_CERT_ARN|CBASE_API_CERT_ARN)
        if [[ "$value" =~ ^arn:aws:acm:([a-z0-9-]+):[0-9]{12}:certificate/[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$ ]]; then
          [ "${BASH_REMATCH[1]}" = us-east-1 ] || conf_die "$no" "$key is in ${BASH_REMATCH[1]}, and CloudFront takes a certificate from us-east-1 alone"
        else
          conf_die "$no" "$key is an ACM certificate's ARN: arn:aws:acm:us-east-1:<account>:certificate/<uuid>"
        fi
        if [ "$key" = CBASE_CERT_ARN ]; then CONF_CERT_ARN="$value"; cert_no="$no"; else CONF_API_CERT_ARN="$value"; api_cert_no="$no"; fi ;;
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
          if ! [[ "$g" =~ ^[A-Za-z0-9._/*?-]+$ ]] || [[ "$g" == /* ]]; then
            conf_die "$no" "CBASE_HASHED is blank-separated globs of letters, digits and . _ - / * ? (assets/*), each a path in the static dir with no leading '/'"
          fi
        done
        hashed_no="$no" ;;
      CBASE_SHELL)
        # Each part starts with a letter, a digit, '_' or '-', so no part is '.' or '..'.
        [[ "$value" =~ ^/[A-Za-z0-9_-][A-Za-z0-9._-]*(/[A-Za-z0-9_-][A-Za-z0-9._-]*)*\.html$ ]] \
          || conf_die "$no" "CBASE_SHELL is a page of the build, an absolute path ending in .html (/_shell.html): letters, digits and . _ - /, each part starting with a letter, a digit, _ or -"
        CONF_SHELL="$value"; shell_no="$no" ;;
    esac
  done
  [ -n "$app_no" ] || conf_die_file "has no CBASE_APP line (the app's name: CBASE_APP = myapp)"
  CONF_VM="${vm_set:-$CONF_APP}"
  CONF_STATIC="${static_set:-$CONF_APP}"
  if [ "$CONF_VM" = none ] && [ "$CONF_STATIC" = none ]; then
    conf_die "$(( ${vm_no:-0} > ${static_no:-0} ? ${vm_no:-0} : ${static_no:-0} ))" "CBASE_VM and CBASE_STATIC are both none: an app has a VM, static files, or both"
  fi
  if [ -n "$CONF_VM_PORT$CONF_VM_DATA" ] && { [ -z "$CONF_VM_PORT" ] || [ -z "$CONF_VM_DATA" ]; }; then
    conf_die "$vmkey_no" "CBASE_VM_PORT and CBASE_VM_DATA go together: a grandfathered member's port and its data directory"
  fi
  if [ -n "$CONF_VM_REPLACES" ] && [ -z "$CONF_VM_PORT" ]; then
    conf_die "$vmkey_no" "CBASE_VM_REPLACES is for a grandfathered member (CBASE_VM_PORT and CBASE_VM_DATA): the unit that served it before"
  fi
  if [ "$CONF_VM" = none ]; then
    [ -z "$vmkey_no" ] || conf_die "$vmkey_no" "the CBASE_VM_* and CBASE_BACKUP_BUCKET keys are for an app with a VM, and CBASE_VM is none"
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
  [ -z "$api_route_no" ] || [ -n "$api_no" ] || conf_die "$api_route_no" "CBASE_API_ROUTE is a route of CBASE_API_HOST's, and there is none"
  if [ -n "$api_no" ]; then
    for k in "${CONF_DOMAINS[@]}"; do
      [ "$k" != "$CONF_API_HOST" ] || conf_die "$api_no" "CBASE_API_HOST $k is one of CBASE_DOMAIN's hostnames: a hostname is on one distribution"
    done
  fi
  [ -z "$cert_no" ] || [ -n "$domain_no" ] || conf_die "$cert_no" "CBASE_CERT_ARN is for CBASE_DOMAIN, and there is none (without a domain the app is at its cloudfront.net address)"
  [ -z "$api_cert_no" ] || [ -n "$api_no" ] || conf_die "$api_cert_no" "CBASE_API_CERT_ARN is for CBASE_API_HOST, and there is none"
  [ -z "$zone_no" ] || [ -n "$domain_no" ] || conf_die "$zone_no" "CBASE_ZONE is where CBASE_DOMAIN's record goes, and there is no CBASE_DOMAIN"
  if [ "$CONF_STATIC" = none ]; then
    [ -z "$dir_no" ] || conf_die "$dir_no" "CBASE_STATIC_DIR is the static files' directory, and CBASE_STATIC is none"
    [ -z "$hashed_no" ] || conf_die "$hashed_no" "CBASE_HASHED names static files, and CBASE_STATIC is none"
    [ -z "$shell_no" ] || conf_die "$shell_no" "CBASE_SHELL is a page of the static files, and CBASE_STATIC is none"
    [ -z "$stkey_no" ] || conf_die "$stkey_no" "CBASE_STATIC_BUCKET and CBASE_STATIC_PREFIX are for static files, and CBASE_STATIC is none"
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
  [ -z "$CONF_VM_STACK" ] || printf '%-10s %s (adopted: found by its stack, no group-init)\n' vm-stack "$CONF_VM_STACK"
  [ -z "$CONF_VM_ORIGIN" ] || printf '%-10s %s (IPv6)\n' vm-origin "$CONF_VM_ORIGIN"
  [ -z "$CONF_VM_PORT" ] || printf '%-10s port %s, data %s (grandfathered: kept as it is)\n' legacy "$CONF_VM_PORT" "$CONF_VM_DATA"
  [ -z "$CONF_VM_SETTINGS" ] || printf '%-10s SSM %s\n' settings "$CONF_VM_SETTINGS"
  [ -z "$CONF_VM_ARTIFACTS" ] || printf '%-10s %s\n' artifacts "$CONF_VM_ARTIFACTS"
  [ -z "$CONF_VM_REPLACES" ] || printf '%-10s %s (stopped and disabled by the first deploy)\n' replaces "$CONF_VM_REPLACES"
  if [ -n "$CONF_BACKUP_BUCKET$CONF_BACKUP_KEY" ]; then
    printf '%-10s s3://%s/%s\n' backups "${CONF_BACKUP_BUCKET:-(the state bucket)}" "${CONF_BACKUP_KEY:-backups/$CONF_APP/cbase.sqlite}"
  fi
  if [ -n "$CONF_STATIC_BUCKET" ]; then printf '%-10s s3://%s/%s/ (adopted bucket)\n' static "$CONF_STATIC_BUCKET" "${CONF_STATIC_PREFIX:-$CONF_APP}"
  else printf '%-10s %s\n' static "$CONF_STATIC"; fi
  [ -n "$CONF_STATIC_BUCKET" ] || [ -z "$CONF_STATIC_PREFIX" ] || printf '%-10s %s/\n' folder "$CONF_STATIC_PREFIX"
  if [ -n "$CONF_STATIC_DIR" ]; then
    if [ -n "$CONF_STATIC_DIR_DEFAULT" ]; then printf '%-10s %s/ (the default)\n' files "$CONF_STATIC_DIR"
    else printf '%-10s %s/\n' files "$CONF_STATIC_DIR"; fi
  fi
  [ "${#CONF_HASHED[@]}" -eq 0 ] || printf '%-10s %s\n' hashed "${CONF_HASHED[*]}"
  [ -z "$CONF_SHELL" ] || printf '%-10s %s\n' shell "$CONF_SHELL"
  [ -z "$CONF_DOMAIN" ] || printf '%-10s %s\n' domain "${CONF_DOMAINS[*]}"
  [ -z "$CONF_API_HOST" ] || printf '%-10s %s\n' api-host "$CONF_API_HOST"
  if [ -n "$CONF_CERT_ARN" ]; then printf '%-10s %s\n' cert "$CONF_CERT_ARN"
  elif [ -n "$CONF_DOMAIN" ]; then printf '%-10s %s\n' cert "(found or requested by deploy)"
  else printf '%-10s %s\n' address "(the distribution's cloudfront.net name)"; fi
  [ -z "$CONF_API_CERT_ARN" ] || printf '%-10s %s\n' api-cert "$CONF_API_CERT_ARN"
  [ -z "$CONF_ZONE" ] || printf '%-10s %s\n' zone "$CONF_ZONE"
  for r in "${CONF_ROUTES[@]}"; do printf '%-10s %s\n' route "$r"; done
  for r in "${CONF_API_ROUTES[@]+"${CONF_API_ROUTES[@]}"}"; do printf '%-10s %s\n' api-route "$r"; done
}
