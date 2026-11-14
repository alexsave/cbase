# cert.py - the certificate and the DNS records for an app's hostnames (the port of cert.c; cert.sh's
# words). The certificate is found, reused or requested and validated in us-east-1; each hostname's zone is
# found; the validation records are written where there is a zone and printed where there is not (status 3
# then); each hostname is moved onto this app's distribution (a staged attach) or pointed at it.
#
# Deploy's dry run is passed in as DRY: a dry run requests nothing and writes nothing, and says what it
# would do. The functions that the shell stopped with dry_stop return true for the problem, and the caller
# applies dry_stop (a deploy stops; a dry run goes on and lists it at its end).
from __future__ import annotations

import sys
from typing import List, Optional, Sequence, Tuple

import aws
import cli
import conf as confmod

conf = confmod.conf

CERT_REGION = "us-east-1"
CERT_POLL_TRIES = 61


class Cert:
    """The certificate state of one deploy, as cert.sh's CERT_* variables."""

    def __init__(self) -> None:
        self.arn = ""                  # CERT_ARN: the certificate the distribution serves the hostnames with, or ""
        self.api_arn = ""              # CERT_API_ARN: the one CBASE_API_HOST's distribution serves, or ""
        self.need: List[str] = []      # CERT_NEED: the hostnames the certificate must cover
        self.hosts: List[str] = []     # CERT_HOSTS: the hostnames, CBASE_DOMAIN's then CBASE_API_HOST
        self.zones: List[str] = []     # CERT_ZONES: each host's Route 53 zone id, or "" for none
        self.notes: List[str] = []     # CERT_NOTES: notes for DNS served elsewhere, printed at the end
        self.moves: List[str] = []     # CERT_MOVES: "<host> <from-distribution> <to-distribution>" a line each


# ---- the certificate -----------------------------------------------------------------------------


def _names(arn: str) -> List[str]:
    """The certificate's names (its domain among them)."""
    rc, out = aws.call("acm", "--region", CERT_REGION, "describe-certificate", "--certificate-arn", arn,
                       "--query", "Certificate.SubjectAlternativeNames", "--output", "text")
    if rc != 0:
        aws.fail("reading certificate %s" % arn)
    return cli.split_on(cli.text_of(out), " \t\n")


def covers(host: str, names: Sequence[str]) -> bool:
    """One of the NAMES covers HOST: the same name, or "*." and the host's parent (a wildcard covers one
    label)."""
    dot = host.find(".")
    for n in names:
        if n == host:
            return True
        # A wildcard covers one label: its rest is the hostname less its first label.
        if n.startswith("*.") and dot >= 0 and n[2:] == host[dot + 1:]:
            return True
    return False


def _covers_all(arn: str, need: Sequence[str]) -> bool:
    """The certificate covers every one of NEED. A failed read stops the tool."""
    names = _names(arn)
    ok = len(names) > 0
    for h in need:
        if not ok:
            break
        ok = covers(h, names)
    return ok


def _find(status: str, need: Sequence[str]) -> str:
    """The first certificate of STATUS, by ARN, that covers every hostname in NEED; "" when none."""
    # Every key type CloudFront serves (ListCertificates names RSA_2048 alone unless asked).
    rc, out = aws.call("acm", "--region", CERT_REGION, "list-certificates", "--certificate-statuses", status,
                       "--includes", "keyTypes=RSA_2048,EC_prime256v1", "--query",
                       "CertificateSummaryList[].CertificateArn", "--output", "text")
    if rc != 0:
        aws.fail("listing ACM certificates")
    arns = [a for a in cli.split_on(cli.text_of(out), " \t\n") if a != "None"]
    for a in cli.sort_bytes(arns):
        if _covers_all(a, need):
            return a
    return ""


def _status_of(arn: str) -> str:
    rc, out = aws.call("acm", "--region", CERT_REGION, "describe-certificate", "--certificate-arn", arn,
                       "--query", "Certificate.Status", "--output", "text")
    if rc != 0:
        aws.fail("reading certificate %s" % arn)
    return cli.text_of(out)


def _zone_for_host(host: str) -> str:
    """HOST's hosted zone id (CBASE_ZONE, or the public zone named by the longest suffix of HOST), or "".
    Two public zones of one name stop the tool: CBASE_ZONE says which."""
    if conf.zone:
        return conf.zone
    s = host
    while "." in s:
        query = "HostedZones[?Name=='%s.' && Config.PrivateZone==`false`].Id" % s
        rc, out = aws.call("route53", "list-hosted-zones-by-name", "--dns-name", s, "--max-items", "10",
                           "--query", query, "--output", "text")
        if rc != 0:
            aws.fail("looking for a Route 53 hosted zone for %s" % s)
        ids = cli.split_on(cli.text_of(out), " \t\n")
        if len(ids) > 0 and ids[0] != "None":
            if len(ids) > 1:
                cli.fail(cli.EXIT_FAIL, "%d public Route 53 hosted zones are named %s: set CBASE_ZONE in cbase.conf to the one that serves the domain" % (len(ids), s))
            return ids[0][ids[0].rfind("/") + 1:]
        s = s[s.index(".") + 1:]
    return ""


# ---- validation records --------------------------------------------------------------------------

_poll_arn = ""    # the certificate the poll reads
_poll_text = ""   # its last read


def _records_read() -> bool:
    """One read of the validation records; true once every record is there."""
    global _poll_text
    rc, out = aws.call("acm", "--region", CERT_REGION, "describe-certificate", "--certificate-arn", _poll_arn,
                       "--query", "Certificate.DomainValidationOptions[].[DomainName,ResourceRecord.Name,ResourceRecord.Type,ResourceRecord.Value]",
                       "--output", "text")
    if rc != 0:
        aws.fail("reading certificate %s's validation records" % _poll_arn)
    _poll_text = cli.text_of(out)
    missing = _poll_text == "None"
    for line in _poll_text.split("\n"):
        f = cli.split_tabs(line, 4)
        # read -r _ name _ value: a line with no name is skipped; a None name or value is not there yet
        if f[1] and (f[1] == "None" or (f[3] is not None and f[3] == "None")):
            missing = True
    return not missing and _poll_text != ""


def _records(arn: str) -> List[Tuple[str, str, str, str]]:
    """The validation records of ARN (host, name, type, value), once each by name, waiting (a while) for ACM
    to make them."""
    global _poll_arn
    _poll_arn = arn
    if not aws.poll(CERT_POLL_TRIES, _records_read):
        cli.fail(cli.EXIT_FAIL, "ACM has not made certificate %s's validation records yet: deploy again in a minute" % arn)
    recs: List[Tuple[str, str, str, str]] = []
    for line in _poll_text.split("\n"):
        f = cli.split_tabs(line, 4)
        if f[1]:
            if not any(r[1] == f[1] for r in recs):
                recs.append((f[0] or "", f[1], f[2] or "", f[3] or ""))
    return recs


def _r53_change(zone: str, batch: str, what: str) -> None:
    """One change batch in ZONE (the CLI's shorthand), waited for."""
    rc, out = aws.call("route53", "change-resource-record-sets", "--hosted-zone-id", zone, "--change-batch", batch,
                       "--query", "ChangeInfo.Id", "--output", "text")
    if rc != 0:
        aws.fail("writing %s in Route 53 zone %s" % (what, zone))
    cid = cli.text_of(out)
    rc, _o = aws.call("route53", "wait", "resource-record-sets-changed", "--id", cid)
    if rc != 0:
        aws.fail("waiting for Route 53 change %s" % cid)


def _zone_of(c: Cert, host: str) -> str:
    """The zone resolve found for HOST, or ""."""
    for i, h in enumerate(c.hosts):
        if h == host:
            return c.zones[i]
    return ""


# ---- the steps -----------------------------------------------------------------------------------


def _named(arn: str, key: str, need: Sequence[str]) -> None:
    """The certificate a key names, used as it is: ISSUED, and covering NEED."""
    st = _status_of(arn)
    if st != "ISSUED":
        cli.fail(cli.EXIT_FAIL, "%s %s is %s, not ISSUED" % (key, arn, st))
    if not _covers_all(arn, need):
        allnames = "".join("%s " % n for n in _names(arn))
        cli.fail(cli.EXIT_FAIL, "%s %s does not cover %s (its names: %s)" % (key, arn, " ".join(need), allnames))
    cli.out("certificate: %s (%s, used as it is)\n" % (arn, key))


def _auto(c: Cert, dry: bool) -> None:
    """The issued certificate covering NEED, else the pending one, else a new request (the DNS records are
    written where a zone holds them, printed where none does, and ACM is waited for). A dry run requests
    nothing. Stops the tool (status 3) for records to add by hand."""
    need = " ".join(c.need)   # NEED is final here: its text, for every message below
    issued = _find("ISSUED", c.need)
    if issued:
        c.arn = issued
        cli.out("certificate: %s, issued, covers %s\n" % (c.arn, need))
        return
    a = _find("PENDING_VALIDATION", c.need)
    if a:
        cli.out("certificate: %s, requested before, still pending validation\n" % a)
    elif dry:
        cli.out("certificate: none covers %s; a deploy would request one (DNS validation). This dry run goes on without the hostnames.\n" % need)
        return
    else:
        # The request: DNS validation for the first hostname, the others as SANs. ACM's idempotency token
        # makes the same app's request for the same hostnames within the hour one request.
        args = ["acm", "--region", CERT_REGION, "request-certificate", "--domain-name", c.need[0],
                "--validation-method", "DNS"]
        if len(c.need) != 1:
            args.append("--subject-alternative-names")
            args.extend(c.need[1:])
        seed = "%s %s" % (conf.app, need)
        args.extend(["--idempotency-token", "cbase%d" % cli.cksum(seed), "--query", "CertificateArn",
                     "--output", "text"])
        rc, out = aws.call(*args)
        if rc != 0:
            aws.fail("requesting a certificate for %s" % need)
        arn = cli.text_of(out)
        cli.out("certificate: requested %s for %s (DNS validation)\n" % (arn, need))
        a = arn
    rec = _records(a)
    hand: List[str] = []
    for host, name, typ, value in rec:
        z = _zone_of(c, host)
        if z and not dry:
            batch = "Changes=[{Action=UPSERT,ResourceRecordSet={Name=%s,Type=%s,TTL=300,ResourceRecords=[{Value=%s}]}}]" % (name, typ, value)
            _r53_change(z, batch, "the validation record for %s" % host)
            cli.out("dns: wrote %s %s %s in zone %s (validates the certificate for %s)\n" % (name, typ, value, z, host))
        else:
            hand.append("%s %s %s %s" % (name, typ, value, host))
    if hand:
        if dry:
            cli.out("dns: the certificate's validation records (a deploy writes those with a zone, and stops on the rest):\n")
        else:
            cli.out("cbase: add these DNS records where the domain's DNS is served (no Route 53 zone in this account covers them):\n")
        for h in hand:
            name, typ, value, host = h.split(" ", 3)
            cli.out("  %s  %s  %s   (proves the certificate for %s)\n" % (name, typ, value, host))
        if dry:
            cli.out("dns: this dry run goes on without the hostnames\n")
            return
        cli.out("then run ./cbase deploy again: once ACM has issued the certificate (minutes after the records are visible) it goes on, and prints the record that points each hostname at the app. Nothing else was changed.\n")
        sys.exit(3)
    if dry:
        cli.out("dns: this dry run goes on without the hostnames (the certificate is pending)\n")
        return
    cli.out("certificate: waiting for ACM to validate %s\n" % a)
    rc, _o = aws.call("acm", "--region", CERT_REGION, "wait", "certificate-validated", "--certificate-arn", a)
    if rc != 0:
        aws.fail("waiting for certificate %s to be issued" % a)
    c.arn = a
    cli.out("certificate: %s, issued\n" % c.arn)


def resolve(c: Cert, dry: bool) -> None:
    """Sets C's arn and api_arn for the hostnames (CBASE_CERT_ARN and CBASE_API_CERT_ARN used as they are,
    else found, reused or requested). Stops the tool (status 3) when the validation records must be added by
    hand. With DRY, requests nothing."""
    c.arn = ""
    c.api_arn = ""
    c.need = []
    c.hosts = []
    c.zones = []
    if not conf.domain:
        return
    c.hosts.extend(conf.domains)
    if conf.api_host:
        c.hosts.append(conf.api_host)
    for h in c.hosts:
        z = _zone_for_host(h)
        c.zones.append(z)
        if z:
            cli.out("dns: %s is in Route 53 zone %s\n" % (h, z))
        else:
            cli.out("dns: no Route 53 hosted zone in this account covers %s\n" % h)
    if conf.api_cert_arn:
        _named(conf.api_cert_arn, "CBASE_API_CERT_ARN", [conf.api_host])
        c.api_arn = conf.api_cert_arn
    c.need.extend(conf.domains)
    if conf.api_host and not conf.api_cert_arn:
        c.need.append(conf.api_host)
    if conf.cert_arn:
        _named(conf.cert_arn, "CBASE_CERT_ARN", c.need)
        c.arn = conf.cert_arn
        if not c.api_arn:
            c.api_arn = c.arn
        return
    _auto(c, dry)
    if c.arn:
        if not c.api_arn:
            c.api_arn = c.arn
    else:
        c.api_arn = ""


def _txt(host: str) -> str:
    """HOST's TXT record name, CloudFront's proof of ownership: "_" before a name of three labels or more,
    "_." before an apex."""
    return "_%s" % host if host.count(".") >= 2 else "_.%s" % host


def alias_check(c: Cert, to_main: str, to_api: str, own: Sequence[str], dry: bool) -> bool:
    """Every hostname is free for this app's distributions: TO_MAIN and TO_API (their ids, "" before the
    first deploy) are the ones it may move onto (moves planned in C.moves), and OWN is the list of the app's
    own distribution ids. Returns true when a hostname another distribution holds cannot be moved: the
    caller applies dry_stop (its message is printed here)."""
    c.moves = []
    if len(c.hosts) == 0 or conf.aliases == "off":
        return False
    # shell's JMESPath: the distributions that have alternate names, as id, domain and names joined by ','.
    rc, out = aws.call("cloudfront", "list-distributions", "--query",
                       "DistributionList.Items[?Aliases.Quantity > `0`].[Id,DomainName,join(`,`,Aliases.Items)]",
                       "--output", "text")
    if rc != 0:
        aws.fail("listing the account's CloudFront distributions")
    found = False
    for line in cli.text_of(out).split("\n"):
        f = cli.split_tabs(line, 3)
        did = f[0] or ""
        dom = f[1] or ""
        aliases = f[2] or ""
        if did and did != "None" and did not in own:
            names = aliases.split(",")
            for h in c.hosts:
                api = bool(conf.api_host) and h == conf.api_host
                for a in names:
                    if a == h:
                        to = to_api if api else to_main
                        if to:
                            c.moves.append("%s %s %s" % (h, did, to))
                            cli.out("attach: %s moves from the distribution %s (%s) to this app's %s (associate-alias: atomic, no downtime)%s\n"
                                    % (h, did, dom, to, "; a dry run makes no move" if dry else ""))
                        else:
                            found = True
                            cli.err("cbase: %s is an alternate domain name of the distribution %s (%s), which is not this app's, and CloudFront serves a hostname from one distribution alone (CNAMEAlreadyExists).\n" % (h, did, dom))
                            cli.err("  free it: remove %s from %s's alternate domain names (in the stack that made %s, or in the CloudFront console), then deploy again\n" % (h, did, did))
                            cli.err("  or move it with no downtime (a staged attach): ./cbase deploy --no-aliases, set the DNS records it prints, then ./cbase deploy --attach\n")
    return found


def attach(c: Cert) -> None:
    """Each planned move, made (associate-alias). Stops the tool on a failed move."""
    for m in c.moves:
        parts = m.split()
        if len(parts) != 3:
            continue
        h, frm, to = parts
        rc, _o = aws.call("cloudfront", "associate-alias", "--target-distribution-id", to, "--alias", h)
        if rc != 0:
            txt = _txt(h)
            rc2, dn = aws.call("cloudfront", "get-distribution", "--id", to, "--query", "Distribution.DomainName",
                               "--output", "text")
            if rc2 == 0:
                dom = cli.text_of(dn)
            else:
                dom = "<%s's cloudfront.net name>" % to
            cli.err("cbase: add TXT %s = %s, then run ./cbase deploy --attach again\n" % (txt, dom))
            aws.fail("moving %s from the distribution %s to %s (associate-alias)" % (h, frm, to))
        cli.out("attach: %s moved from %s to %s\n" % (h, frm, to))


def point(c: Cert, host: str, target: str, cf_zone_id: str) -> None:
    """HOST at TARGET (its distribution's name): an alias in its zone, or a note (C.notes) for DNS served
    elsewhere, or with aliases off the records to set are printed. CF_ZONE_ID is the hosted zone every
    CloudFront distribution has, for the alias."""
    if conf.aliases == "off":
        txt = _txt(host)
        cli.out("point %s at %s (a CNAME; an ALIAS or flattened record at a domain's apex)\n" % (host, target))
        cli.out("  and add: %s TXT %s (CloudFront's proof of ownership, which deploy --attach needs)\n" % (txt, target))
        return
    z = _zone_of(c, host)
    if z:
        alias = "AliasTarget={HostedZoneId=%s,DNSName=%s,EvaluateTargetHealth=false}" % (cf_zone_id, target)
        batch = "Changes=[{Action=UPSERT,ResourceRecordSet={Name=%s,Type=A,%s}},{Action=UPSERT,ResourceRecordSet={Name=%s,Type=AAAA,%s}}]" % (host, alias, host, alias)
        _r53_change(z, batch, "the alias for %s" % host)
        cli.out("dns: %s is an alias (A and AAAA) of %s in zone %s\n" % (host, target, z))
    else:
        c.notes.append("note: point %s at %s where its DNS is served, if it isn't already: a CNAME (an ALIAS or ANAME record at a domain's apex)" % (host, target))
