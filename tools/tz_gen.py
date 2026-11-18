#!/usr/bin/env python3
"""tz_gen.py - lib/tz's data, from a compiled tzdata release (make tz-update TZDATA=<zoneinfo dir>).

Writes two files and stops, naming the zone, on anything lib/tz cannot represent:

  lib/tz/tzdb.tsv            every zone of the release: "name<TAB>rule", the rule the zone's TZif footer
                             holds (RFC 8536 section 3.3), under a header line naming the tzdata version.
  lib/tz/test/transitions.tsv  the test oracle: for each distinct rule, one zone using it and every
                             transition from 2026 to 2050 as Python's zoneinfo gives it (not from the C).

Reading the footers keeps the rules the real database's. Two refusals:

  * a footer with a DST part and no dates ("EST5EDT"): tzdata never writes one, and guessing the
    POSIX default dates would be a guess;
  * a zone whose file, after the HORIZON (now, unless --horizon says otherwise), holds an explicit
    transition its footer rule lacks or lacks one its footer rule predicts (a rule change tzdata has
    already scheduled, or a DST it has suspended):
    the footer alone would be wrong until that date, which is the one case lib/tz does not cover.
    A transition before the horizon is past, and "past transitions are not represented" (tz.h) is the
    stated limit; the design said "from the build year onward", which on tzdata 2026f stops on
    Canada/Pacific's change of 2026-03-08 (British Columbia to permanent MST), already past, whose footer
    is right from that day. Those zones are listed on stderr as notes, never silently.

The check uses the rule evaluator below, written here and not shared with lib/tz/tz.c, so a mistake
in one is not a mistake in both. Python is for this one-off tool and for the oracle only: nothing
here ships and no JSON is written.
"""
import argparse
import bisect
import os
import re
import struct
import sys
import zoneinfo
from datetime import datetime, timedelta, timezone

FROM_YEAR = 2026
TO_YEAR = 2050


def die(msg):
    sys.exit("tz_gen: " + msg)


# ---- TZif -----------------------------------------------------------------------------------------

def read_tzif(path):
    """(transitions [(utc_s, utoff, isdst, abbr)], first type (utoff, isdst, abbr), footer) of a v2+ file."""
    b = open(path, "rb").read()
    if b[:4] != b"TZif" or b[4:5] < b"2":
        die("%s is not a TZif version 2 or later file" % path)

    def header(o):
        return struct.unpack(">6l", b[o + 20:o + 44])  # isutcnt isstdcnt leapcnt timecnt typecnt charcnt

    isut, isstd, leap, tim, typ, chars = header(0)
    o = 44 + tim * 4 + tim + typ * 6 + chars + leap * 8 + isstd + isut
    isut, isstd, leap, tim, typ, chars = header(o)
    o += 44
    times = struct.unpack(">%dq" % tim, b[o:o + tim * 8]); o += tim * 8
    idx = b[o:o + tim]; o += tim
    types = [struct.unpack(">lBB", b[o + i * 6:o + i * 6 + 6]) for i in range(typ)]; o += typ * 6
    ab = b[o:o + chars]; o += chars
    o += leap * 12 + isstd + isut
    footer = b[o:].strip(b"\n").decode()

    def info(t):
        off, dst, ai = types[t]
        return (off, dst, ab[ai:ab.index(b"\0", ai)].decode())

    return [(times[i],) + info(idx[i]) for i in range(tim)], info(0), footer


# ---- the rule, evaluated here ---------------------------------------------------------------------

RULE = re.compile(r"""^(?P<std><[A-Za-z0-9+-]+>|[A-Za-z]{3,})(?P<so>[+-]?\d{1,2}(?::\d\d){0,2})
    (?:(?P<dst><[A-Za-z0-9+-]+>|[A-Za-z]{3,})(?P<do>[+-]?\d{1,2}(?::\d\d){0,2})?
       (?:,(?P<s>[^,]+),(?P<e>[^,]+))?)?$""", re.X)


def hms(t):
    sign = -1 if t.startswith("-") else 1
    p = [int(x) for x in t.lstrip("+-").split(":")] + [0, 0]
    return sign * (p[0] * 3600 + p[1] * 60 + p[2])


def abbr(n):
    return n.strip("<>")


class Rule:
    def __init__(self, text):
        m = RULE.match(text)
        if not m:
            die("not a rule: %r" % text)
        self.text = text
        self.std, self.std_off = abbr(m["std"]), -hms(m["so"])
        self.dst = None
        if m["dst"]:
            if not m["s"]:
                die("a rule with a DST part and no dates: %r (tzdata never writes one)" % text)
            self.dst = abbr(m["dst"])
            self.dst_off = -hms(m["do"]) if m["do"] else self.std_off + 3600
            self.start, self.end = self.date(m["s"]), self.date(m["e"])
        for off in (self.std_off, getattr(self, "dst_off", 0)):
            if off % 60:
                die("an offset that is not whole minutes in %r" % text)
        if any(len(a) > 7 for a in (self.std, self.dst or "")):
            die("an abbreviation over 7 characters in %r" % text)

    @staticmethod
    def date(t):
        d, _, tm = t.partition("/")
        return d, hms(tm) if tm else 7200

    def day(self, spec, year):
        d = spec[0]
        if d[0] == "M":
            m, w, wd = (int(x) for x in d[1:].split("."))
            first = datetime(year, m, 1).date().toordinal()
            sun0 = (datetime(year, m, 1).weekday() + 1) % 7
            day = first + (wd - sun0) % 7 + (w - 1) * 7
            nxt = datetime(year + (m == 12), m % 12 + 1, 1).date().toordinal()
            return day - 7 if day >= nxt else day
        jan1 = datetime(year, 1, 1).date().toordinal()
        if d[0] == "J":
            n = int(d[1:])
            return jan1 + n - 1 + (n >= 60 and (year % 4 == 0 and (year % 100 or year % 400 == 0)))
        return jan1 + int(d)

    EPOCH = datetime(1970, 1, 1).date().toordinal()

    def transitions(self, year):
        """UTC seconds of the DST start and end of `year`."""
        s = (self.day(self.start, year) - self.EPOCH) * 86400 + self.start[1] - self.std_off
        e = (self.day(self.end, year) - self.EPOCH) * 86400 + self.end[1] - self.dst_off
        return s, e

    def state(self, t):
        """(utoff, isdst, abbr) at UTC second t."""
        if not self.dst:
            return (self.std_off, 0, self.std)
        y = datetime.fromtimestamp(t + self.std_off, timezone.utc).year
        ev = []
        for k in (y - 1, y, y + 1):
            s, e = self.transitions(k)
            ev += [(s, 1), (e, 0)]
        past = [x for x in sorted(ev) if x[0] <= t]
        dst = past[-1][1] if past else 0
        return (self.dst_off, 1, self.dst) if dst else (self.std_off, 0, self.std)


def check_future(zone, trans, first, rule, horizon, notes):
    """None when the footer agrees with the file after the horizon, else the date of the first sample where it does not.

    The samples are the horizon, each explicit transition after it and each transition the footer
    predicts after it, up to the file's last explicit one, each at the instant and a second before:
    a change the file holds and the footer lacks, and one the footer has and the file lacks (a DST
    that tzdata has suspended for a year), both show. Explicit transitions from FROM_YEAR to the horizon
    that the footer lacks are past: notes."""
    times = [t[0] for t in trans]

    def file_state(x):
        i = bisect.bisect_right(times, x)
        return trans[i - 1][1:] if i else first

    lo = datetime(FROM_YEAR, 1, 1, tzinfo=timezone.utc).timestamp()
    for i, (t, off, dst, ab) in enumerate(trans):
        if lo <= t <= horizon and (rule.state(t) != (off, dst, ab) or rule.state(t - 1) != file_state(t - 1)):
            notes.append("%s: explicit transition %s, before the horizon, not in footer %r" % (
                zone, datetime.fromtimestamp(t, timezone.utc).isoformat(), rule.text))
    if not times:
        return None
    last = times[-1]
    samples = {horizon}
    for t in times:
        if t > horizon:
            samples |= {t - 1, t}
    if rule.dst:
        for y in range(datetime.fromtimestamp(horizon, timezone.utc).year - 1, datetime.fromtimestamp(last, timezone.utc).year + 2):
            for x in rule.transitions(y):
                if horizon < x <= last:
                    samples |= {x - 1, x}
    for x in sorted(samples):
        if x >= horizon and x <= last and file_state(x) != rule.state(x):
            return datetime.fromtimestamp(x, timezone.utc).strftime("%Y-%m-%d")
    return None


# ---- the oracle, from zoneinfo --------------------------------------------------------------------

def zi_state(zi, t):
    d = datetime.fromtimestamp(t, zi)
    return (int(d.utcoffset().total_seconds()), d.tzname())


def zi_transitions(name, y0):
    zi = zoneinfo.ZoneInfo(name)
    lo = int(datetime(y0, 1, 1, tzinfo=timezone.utc).timestamp())
    hi = int(datetime(TO_YEAR + 1, 1, 1, tzinfo=timezone.utc).timestamp())
    out = []
    t = lo
    prev = zi_state(zi, t)
    while t < hi:
        nxt = zi_state(zi, t + 86400)
        if nxt != prev:
            a, b = t, t + 86400  # state(a) == prev, state(b) != prev
            while b - a > 1:
                mid = (a + b) // 2
                if zi_state(zi, mid) == prev:
                    a = mid
                else:
                    b = mid
            out.append((b, prev[0] // 60, nxt[0] // 60, nxt[1]))
            prev = zi_state(zi, b)
            if zi_state(zi, t + 86400) != prev:
                die("%s: two changes in one day near %d" % (name, t))
        t += 86400
    return out


# ---- main -----------------------------------------------------------------------------------------

def version(d):
    for f in ("+VERSION", "VERSION"):
        p = os.path.join(d, f)
        if os.path.exists(p):
            return open(p).read().split()[0]
    p = os.path.join(d, "tzdata.zi")
    if os.path.exists(p):
        m = re.match(r"# version (\S+)", open(p).readline())
        if m:
            return m[1]
    die("no +VERSION and no tzdata.zi version line in %s" % d)


def skip_list(path):
    """zone -> why, from tools/tz_skip.txt: zone<TAB>reason, # comments."""
    out = {}
    if os.path.exists(path):
        for line in open(path):
            if line.strip() and not line.startswith("#"):
                z, _, why = line.rstrip("\n").partition("\t")
                out[z] = why
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--zoneinfo", required=True, help="a compiled tzdata release's zoneinfo directory")
    ap.add_argument("--tzdb", default="lib/tz/tzdb.tsv")
    ap.add_argument("--oracle", default="lib/tz/test/transitions.tsv")
    ap.add_argument("--skip", default="tools/tz_skip.txt", help="zones deliberately left out (see below)")
    ap.add_argument("--horizon", type=float, default=None, help="UTC seconds: explicit transitions after it must be in the footer (default now)")
    a = ap.parse_args()
    horizon = a.horizon if a.horizon is not None else datetime.now(timezone.utc).timestamp()
    notes = []
    root = os.path.abspath(a.zoneinfo)
    ver = version(root)
    zones = {}
    scheduled = {}     # zone -> the date of its first explicit transition the footer does not reproduce
    windowed = {}      # zone -> the first year the oracle may read from it (its file holds earlier changes the footer lacks)
    for r, ds, fs in os.walk(root):
        ds[:] = [d for d in ds if d not in ("posix", "right")]
        for f in fs:
            p = os.path.join(r, f)
            if open(p, "rb").read(4) != b"TZif":
                continue
            name = os.path.relpath(p, root).replace(os.sep, "/")
            trans, first, footer = read_tzif(p)
            if not footer:
                die("%s: no footer rule" % name)
            rule = Rule(footer)
            n0 = len(notes)
            first_sched = check_future(name, trans, first, rule, horizon, notes)
            if len(notes) > n0:   # past changes the footer lacks: the oracle reads this zone only after them
                windowed[name] = max(int(x.split("transition ")[1][:4]) for x in notes[n0:]) + 1
            if first_sched:
                scheduled[name] = first_sched
                continue
            zones[name] = footer
    skip = skip_list(a.skip)
    unlisted = sorted(set(scheduled) - set(skip))
    if unlisted:
        die("these zones disagree with their footer rule after the horizon (an explicit transition it lacks, or one it "
            "predicts that the file lacks: a rule change tzdata has already scheduled), so the footer alone would be wrong from that date, the one case lib/tz does "
            "not cover: %s. List a zone in %s to leave it out of tzdb.tsv (a decision), or stop here."
            % ("; ".join("%s from %s" % (n, scheduled[n]) for n in unlisted), a.skip))
    stale = sorted(set(skip) - set(scheduled))
    if stale:
        die("%s lists %s, which agree with their footers now: remove the line" % (a.skip, ", ".join(stale)))
    os.makedirs(os.path.dirname(a.tzdb), exist_ok=True)
    with open(a.tzdb, "w") as f:
        f.write("# tzdata %s: name<TAB>rule, the TZif footer of each zone (tools/tz_gen.py)\n" % ver)
        if skip:
            f.write("# left out, for an explicit transition the footer does not reproduce (tools/tz_skip.txt): %s\n" % " ".join(sorted(skip)))
        for n in sorted(zones):
            f.write("%s\t%s\n" % (n, zones[n]))
    zoneinfo.reset_tzpath(to=[root])
    by_rule = {}
    tab = os.path.join(root, "zone.tab")   # the canonical zones witness a rule before a backward link does
    canon = {l.split("\t")[2] for l in open(tab) if l[0] != "#"} if os.path.exists(tab) else set()
    for n in sorted(zones, key=lambda n: (n not in canon, n)):
        if n in windowed and zones[n] in by_rule:
            continue
        if n not in windowed and by_rule.get(zones[n], ("", 0))[1] > FROM_YEAR:
            by_rule[zones[n]] = (n, FROM_YEAR)   # a zone read from the whole window is the better witness
        by_rule.setdefault(zones[n], (n, windowed.get(n, FROM_YEAR)))
    os.makedirs(os.path.dirname(a.oracle), exist_ok=True)
    with open(a.oracle, "w") as f:
        f.write("# tzdata %s, zoneinfo's transitions %d..%d: Z zone rule first_year, then T utc_s offset_before_min offset_after_min abbr_after (tools/tz_gen.py)\n"
                % (ver, FROM_YEAR, TO_YEAR))
        for rule in sorted(by_rule):
            zone, y0 = by_rule[rule]
            f.write("Z\t%s\t%s\t%d\n" % (zone, rule, y0))
            for t, b, af, ab in zi_transitions(zone, y0):
                f.write("T\t%d\t%d\t%d\t%s\n" % (t, b, af, ab))
    for n in notes:
        print("tz_gen: note:", n, file=sys.stderr)
    print("tz_gen: tzdata %s: %d zones, %d distinct rules, %d rule bytes" % (ver, len(zones), len(by_rule), sum(len(r) for r in by_rule)))


if __name__ == "__main__":
    main()
