# shellcheck shell=bash
# cert.sh - the certificate and the DNS records for an app's hostnames (CBASE_DOMAIN, and
# CBASE_API_HOST), automatic (README.md "./cbase deploy tool"). Sourced by tools/cbase.
#
# THE HOSTNAMES are CBASE_DOMAIN's (one or more, the first the app's URL) and CBASE_API_HOST; each
# is found its own zone, so they may sit in different zones, and one certificate covers them all.
#
# THE CERTIFICATE, an ACM one in us-east-1 (the one region CloudFront reads), found in this order:
#   1. CBASE_CERT_ARN, an override: used as it is, never replaced; it must be ISSUED and cover
#      CBASE_DOMAIN's hostnames (and CBASE_API_HOST, unless CBASE_API_CERT_ARN names the API
#      host's own). CBASE_API_CERT_ARN, the same for the API host's distribution alone;
#   2. an ISSUED certificate that covers every hostname, the first by ARN;
#   3. one still PENDING_VALIDATION that covers them (an earlier deploy's request);
#   4. a new request, DNS validation, for the first hostname with every other one as a SAN.
# A certificate covers a hostname when one of its names (the domain and its SANs, from
# describe-certificate) is the hostname, or is "*." and the hostname's parent: a wildcard covers one
# label, so *.example.com covers www.example.com and neither example.com nor a.b.example.com.
#
# THE ZONE for a hostname is CBASE_ZONE, an override, or the account's public Route 53 hosted zone
# whose name is the longest suffix of the hostname (list-hosted-zones-by-name, asked one suffix at a
# time). Two public zones of one name stop the deploy: CBASE_ZONE says which.
#   With a zone for every hostname, deploy writes a pending certificate's validation CNAMEs there,
#   waits for Route 53 and then for ACM to issue it, and after the stack writes each hostname's alias
#   (A and AAAA) to its distribution.
#   Without one, deploy prints the exact records to add where the domain's DNS is and stops, status 3,
#   having changed nothing else; the next deploy finds the certificate pending, prints them again
#   until ACM has issued it, then goes on, and prints the record that points each hostname at its
#   distribution. Nothing registers a domain or makes a hosted zone.
#
# With no CBASE_DOMAIN there is no certificate: the app is at its distribution's cloudfront.net name.

CERT_REGION=us-east-1
CERT_ARN=""        # the certificate the stack serves the hostnames with ("" for none yet)
CERT_API_ARN=""    # the one CBASE_API_HOST's distribution serves (CBASE_API_CERT_ARN, else CERT_ARN)
CERT_NEED=()       # the hostnames the found or requested certificate must cover
CERT_HOSTS=()      # the hostnames
CERT_ZONES=()      # each hostname's zone id, or "" for none

acm() { aws_call acm --region "$CERT_REGION" "$@"; }

# cert_names ARN: the certificate's names (its domain is among its SANs), one a line.
cert_names() {
  local text names=()
  text="$(acm describe-certificate --certificate-arn "$1" --query 'Certificate.SubjectAlternativeNames' --output text)" \
    || aws_fail "reading certificate $1"
  read -r -a names <<< "$text"
  printf '%s\n' ${names[@]+"${names[@]}"}
}

# cert_covers HOST NAME...: one of the names covers HOST.
cert_covers() {
  local host="$1" n
  shift
  for n in "$@"; do
    [ "$n" != "$host" ] || return 0
    # A wildcard: its rest is the hostname's parent (the hostname less its first label).
    if [[ "$n" == '*.'* ]] && [[ "$host" == *.* ]] && [ "${n#\*.}" = "${host#*.}" ]; then return 0; fi
  done
  return 1
}

# cert_covers_all ARN: the certificate covers every one of CERT_NEED. A failed read stops the tool
# (never "does not cover", which would go on to request a certificate).
cert_covers_all() {
  local names=() n h text
  text="$(cert_names "$1")" || exit 1
  while IFS= read -r n; do [ -z "$n" ] || names+=("$n"); done <<< "$text"
  [ "${#names[@]}" -gt 0 ] || return 1
  for h in "${CERT_NEED[@]}"; do
    cert_covers "$h" "${names[@]}" || return 1
  done
}

# cert_find STATUS: the first certificate of STATUS, by ARN, that covers every hostname; status 4
# when none does. It runs in $(...), so a failed call exits it 1: the caller tells the two apart.
CERT_NONE=4
cert_find() {
  local text arns=() a
  # Every key type CloudFront serves (ListCertificates names RSA_2048 alone unless asked).
  text="$(acm list-certificates --certificate-statuses "$1" --includes keyTypes=RSA_2048,EC_prime256v1 \
            --query 'CertificateSummaryList[].CertificateArn' --output text)" || aws_fail "listing ACM certificates"
  read -r -a arns <<< "$text"
  for a in $(printf '%s\n' ${arns[@]+"${arns[@]}"} | grep -v '^None$' | LC_ALL=C sort); do
    if cert_covers_all "$a"; then printf '%s\n' "$a"; return 0; fi
  done
  return "$CERT_NONE"
}

# cert_lookup STATUS: CERT_FOUND, the certificate cert_find finds, or "" when none does; any other
# failure stops the tool.
CERT_FOUND=""
cert_lookup() {
  local rc=0
  CERT_FOUND="$(cert_find "$1")" || rc=$?
  [ "$rc" -eq 0 ] || [ "$rc" -eq "$CERT_NONE" ] || exit "$rc"
  [ "$rc" -eq 0 ] || CERT_FOUND=""
}

# cert_status ARN
cert_status() {
  local st
  st="$(acm describe-certificate --certificate-arn "$1" --query 'Certificate.Status' --output text)" \
    || aws_fail "reading certificate $1"
  printf '%s' "$st"
}

# zone_for_host HOST: the hosted zone id for HOST (CBASE_ZONE, or the longest suffix's public
# zone), or nothing.
zone_for_host() {
  local host="$1" s text ids=()
  if [ -n "$CONF_ZONE" ]; then printf '%s' "$CONF_ZONE"; return 0; fi
  s="$host"
  while [[ "$s" == *.* ]]; do
    text="$(aws_call route53 list-hosted-zones-by-name --dns-name "$s" --max-items 10 \
              --query "HostedZones[?Name=='$s.' && Config.PrivateZone==\`false\`].Id" --output text)" \
      || aws_fail "looking for a Route 53 hosted zone for $s"
    read -r -a ids <<< "$text"
    if [ "${#ids[@]}" -gt 0 ] && [ "${ids[0]}" != None ]; then
      if [ "${#ids[@]}" -gt 1 ]; then
        echo "cbase: ${#ids[@]} public Route 53 hosted zones are named $s: set CBASE_ZONE in cbase.conf to the one that serves the domain" >&2
        exit 1
      fi
      printf '%s' "${ids[0]##*/}"
      return 0
    fi
    s="${s#*.}"
  done
}

# cert_records ARN: the validation records, "<name> CNAME <value>" a line, once each; waits (a
# while) for ACM to have made them, which it does seconds after a request.
cert_records() {
  local arn="$1" n=0 text line name type value missing seen=" "
  while :; do
    text="$(acm describe-certificate --certificate-arn "$arn" \
              --query 'Certificate.DomainValidationOptions[].[DomainName,ResourceRecord.Name,ResourceRecord.Type,ResourceRecord.Value]' \
              --output text)" || aws_fail "reading certificate $arn's validation records"
    missing=""
    [ "$text" != None ] || missing=1
    while IFS=$'\t' read -r _ name type value; do
      [ -n "$name" ] || continue
      if [ "$name" = None ] || [ "$value" = None ]; then missing=1; fi
    done <<< "$text"
    [ -n "$missing" ] || [ -z "$text" ] || break
    n=$((n + 1))
    [ "$n" -le 60 ] || { echo "cbase: ACM has not made certificate $arn's validation records yet: deploy again in a minute" >&2; exit 1; }
    sleep "$POLL_SECONDS"
  done
  while IFS=$'\t' read -r line name type value; do
    [ -n "$name" ] || continue
    [[ "$seen" != *" $name "* ]] || continue
    seen="$seen$name "
    printf '%s %s %s %s\n' "$line" "$name" "$type" "$value"
  done <<< "$text"
}

# r53_change ZONE BATCH WHAT: one change batch (the CLI's shorthand), waited for.
r53_change() {
  local id
  id="$(aws_call route53 change-resource-record-sets --hosted-zone-id "$1" --change-batch "$2" \
          --query ChangeInfo.Id --output text)" || aws_fail "writing $3 in Route 53 zone $1"
  aws_call route53 wait resource-record-sets-changed --id "$id" || aws_fail "waiting for Route 53 change $id"
}

# cert_named ARN KEY: the certificate a key names, used as it is: ISSUED, and covering CERT_NEED.
cert_named() {
  local st
  st="$(cert_status "$1")"
  [ "$st" = ISSUED ] || { echo "cbase: $2 $1 is $st, not ISSUED" >&2; exit 1; }
  cert_covers_all "$1" || { echo "cbase: $2 $1 does not cover ${CERT_NEED[*]} (its names: $(cert_names "$1" | tr '\n' ' '))" >&2; exit 1; }
  echo "certificate: $1 ($2, used as it is)"
}

# cert_resolve: CERT_ARN for the hostnames, found, reused, or requested and validated. Stops,
# status 3, when the records must be added by hand. DRY_RUN requests nothing and writes nothing.
cert_resolve() {
  CERT_ARN=""
  CERT_API_ARN=""
  CERT_HOSTS=()
  CERT_ZONES=()
  CERT_NEED=()
  [ -n "$CONF_DOMAIN" ] || return 0
  # shellcheck disable=SC2153 # conf.sh sets CONF_DOMAINS
  CERT_HOSTS=("${CONF_DOMAINS[@]}")
  [ -z "$CONF_API_HOST" ] || CERT_HOSTS+=("$CONF_API_HOST")
  local h z
  for h in "${CERT_HOSTS[@]}"; do
    z="$(zone_for_host "$h")"
    CERT_ZONES+=("$z")
    if [ -n "$z" ]; then echo "dns: $h is in Route 53 zone $z"; else echo "dns: no Route 53 hosted zone in this account covers $h"; fi
  done
  if [ -n "$CONF_API_CERT_ARN" ]; then
    CERT_NEED=("$CONF_API_HOST")
    cert_named "$CONF_API_CERT_ARN" CBASE_API_CERT_ARN
    CERT_API_ARN="$CONF_API_CERT_ARN"
  fi
  CERT_NEED=("${CONF_DOMAINS[@]}")
  [ -z "$CONF_API_HOST" ] || [ -n "$CONF_API_CERT_ARN" ] || CERT_NEED+=("$CONF_API_HOST")
  if [ -n "$CONF_CERT_ARN" ]; then
    cert_named "$CONF_CERT_ARN" CBASE_CERT_ARN
    CERT_ARN="$CONF_CERT_ARN"
    CERT_API_ARN="${CERT_API_ARN:-$CERT_ARN}"
    return 0
  fi
  cert_auto
  [ -z "$CERT_ARN" ] || CERT_API_ARN="${CERT_API_ARN:-$CERT_ARN}"
  [ -n "$CERT_ARN" ] || CERT_API_ARN=""
}

# cert_auto: CERT_ARN, the issued certificate that covers CERT_NEED, else the pending one, else a
# new request (cert_resolve's steps 2 to 4).
cert_auto() {
  local z a
  cert_lookup ISSUED
  if [ -n "$CERT_FOUND" ]; then
    CERT_ARN="$CERT_FOUND"
    echo "certificate: $CERT_ARN, issued, covers ${CERT_NEED[*]}"
    return 0
  fi
  cert_lookup PENDING_VALIDATION
  a="$CERT_FOUND"
  if [ -n "$a" ]; then
    echo "certificate: $a, requested before, still pending validation"
  elif [ "${DRY_RUN:-}" = 1 ]; then
    echo "certificate: none covers ${CERT_NEED[*]}; a deploy would request one (DNS validation). This dry run goes on without the hostnames."
    return 0
  else
    local args=(--domain-name "${CERT_NEED[0]}" --validation-method DNS)
    [ "${#CERT_NEED[@]}" -eq 1 ] || args+=(--subject-alternative-names "${CERT_NEED[@]:1}")
    # The same app's requests for the same hostnames within the hour are one request (ACM's
    # idempotency token); other hostnames are another request.
    a="$(acm request-certificate "${args[@]}" --idempotency-token "cbase$(printf '%s %s' "$CONF_APP" "${CERT_NEED[*]}" | cksum | cut -d' ' -f1)" \
           --query CertificateArn --output text)" || aws_fail "requesting a certificate for ${CERT_NEED[*]}"
    echo "certificate: requested $a for ${CERT_NEED[*]} (DNS validation)"
  fi
  # Pending: its records, written where there is a zone, printed where there is not.
  local records host name type value i hand=()
  records="$(cert_records "$a")"
  while read -r host name type value; do
    [ -n "$name" ] || continue
    z=""
    for i in "${!CERT_HOSTS[@]}"; do
      [ "${CERT_HOSTS[$i]}" != "$host" ] || z="${CERT_ZONES[$i]}"
    done
    if [ -n "$z" ] && [ "${DRY_RUN:-}" != 1 ]; then
      r53_change "$z" "Changes=[{Action=UPSERT,ResourceRecordSet={Name=$name,Type=$type,TTL=300,ResourceRecords=[{Value=$value}]}}]" \
        "the validation record for $host"
      echo "dns: wrote $name $type $value in zone $z (validates the certificate for $host)"
    else
      hand+=("$name $type $value $host")
    fi
  done <<< "$records"
  if [ "${#hand[@]}" -gt 0 ]; then
    local r
    if [ "${DRY_RUN:-}" = 1 ]; then echo "dns: the certificate's validation records (a deploy writes those with a zone, and stops on the rest):"
    else echo "cbase: add these DNS records where the domain's DNS is served (no Route 53 zone in this account covers them):"; fi
    for r in "${hand[@]}"; do
      read -r name type value host <<< "$r"
      printf '  %s  %s  %s   (proves the certificate for %s)\n' "$name" "$type" "$value" "$host"
    done
    if [ "${DRY_RUN:-}" = 1 ]; then echo "dns: this dry run goes on without the hostnames"; return 0; fi
    echo "then run ./cbase deploy again: once ACM has issued the certificate (minutes after the records are visible) it goes on, and prints the record that points each hostname at the app. Nothing else was changed."
    exit 3
  fi
  [ "${DRY_RUN:-}" != 1 ] || { echo "dns: this dry run goes on without the hostnames (the certificate is pending)"; return 0; }
  echo "certificate: waiting for ACM to validate $a"
  acm wait certificate-validated --certificate-arn "$a" || aws_fail "waiting for certificate $a to be issued"
  CERT_ARN="$a"
  echo "certificate: $CERT_ARN, issued"
}

# cert_alias_check OWN_ID...: every hostname (CERT_HOSTS) is free for this app's distributions.
# CloudFront serves a hostname from one distribution alone, and a stack that names one another
# distribution holds fails late, mid-execute (CNAMEAlreadyExists). So each hostname another
# distribution holds (not one of OWN_ID, this app's) is named with that distribution and the one
# step that frees it, and the deploy stops before anything changes; a dry run says so and goes on.
# Moving it is the person's step, never automatic.
cert_alias_check() {
  local text id dom aliases h a found=0 own=" $* "
  [ "${#CERT_HOSTS[@]}" -gt 0 ] || return 0
  # shellcheck disable=SC2016 # the backticks are a JMESPath literal
  text="$(aws_call cloudfront list-distributions \
            --query 'DistributionList.Items[?Aliases.Quantity > `0`].[Id,DomainName,join(`,`,Aliases.Items)]' --output text)" \
    || aws_fail "listing the account's CloudFront distributions"
  while IFS=$'\t' read -r id dom aliases; do
    { [ -n "$id" ] && [ "$id" != None ]; } || continue
    [[ "$own" != *" $id "* ]] || continue
    for h in "${CERT_HOSTS[@]}"; do
      for a in ${aliases//,/ }; do
        [ "$a" = "$h" ] || continue
        found=1
        echo "cbase: $h is an alternate domain name of the distribution $id ($dom), which is not this app's, and CloudFront serves a hostname from one distribution alone (CNAMEAlreadyExists)." >&2
        echo "  free it: remove $h from $id's alternate domain names (in the stack that made $id, or in the CloudFront console), then deploy again" >&2
      done
    done
  done <<< "$text"
  [ "$found" = 1 ] || return 0
  if [ "${DRY_RUN:-}" = 1 ]; then echo "dry run: a deploy would stop here, on the hostnames above; this dry run goes on"; return 0; fi
  echo "cbase: nothing was changed" >&2
  exit 1
}

# cert_point DOMAIN HOST: each hostname at its distribution: an alias in its zone, or the record to
# add by hand.
cert_point() {
  local host="$1" target="$2" z="" i
  for i in "${!CERT_HOSTS[@]}"; do
    [ "${CERT_HOSTS[$i]}" != "$host" ] || z="${CERT_ZONES[$i]}"
  done
  if [ -n "$z" ]; then
    local alias="AliasTarget={HostedZoneId=$CF_ZONE_ID,DNSName=$target,EvaluateTargetHealth=false}"
    r53_change "$z" "Changes=[{Action=UPSERT,ResourceRecordSet={Name=$host,Type=A,$alias}},{Action=UPSERT,ResourceRecordSet={Name=$host,Type=AAAA,$alias}}]" \
      "the alias for $host"
    echo "dns: $host is an alias (A and AAAA) of $target in zone $z"
  else
    echo "dns: point $host at $target where its DNS is served: a CNAME (an ALIAS or ANAME record at a domain's apex)"
  fi
}
