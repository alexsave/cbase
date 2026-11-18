"""ics_oracle.py - the calendar writer's goldens (lib/ics/test/golden, written by lib/ics/ics_test.c) read back by
Python's icalendar, against values written here by hand and against zoneinfo, never against the C.

  python3 lib/ics/test/ics_oracle.py [golden dir]        (make ics-oracle; pip install icalendar tzdata)

Per golden: the shape (CRLF, 75 octets, no fold inside a UTF-8 sequence), every field, the link's parts.
For a repeating event with a zone: the VTIMEZONE in the file is turned into a timezone by icalendar
(lookup_tzid=False: the file's own rules, not the database's name), the weekly RRULE is expanded two years
across both DST changes with dateutil, and every occurrence must keep its local time and land on the UTC
instant zoneinfo gives for that local time in the real zone; the file's zone must also give zoneinfo's
offset at every half hour of those two years. weekly_ny_utc is the negative control: the same event in UTC
shows its local hour drifting, which is the bug the zone fixes, so the oracle is shown able to see it.
Test only; nothing here ships."""
import sys
import urllib.parse
from datetime import datetime, timedelta, timezone
from pathlib import Path
from zoneinfo import ZoneInfo

from dateutil.rrule import rrulestr
from icalendar import Calendar

U = timezone.utc
NOW = datetime(2026, 11, 18, 9, 30, 15, tzinfo=U)
HOST = "calendar.example"

EXPECT = {
    "oneoff": dict(
        uid="e-oslo-fredag-2026-11-20@" + HOST,
        start=datetime(2026, 11, 20, 18, 0, tzinfo=U), end=datetime(2026, 11, 20, 19, 0, tzinfo=U),
        summary="Fredagsløp på Frognerparken", location="Frognerparken, Oslo, Norge",
        description="Rolig 5K, alle tempoer.\nMøte ved fontenen; ta med vann, \\ ikke hund.",
        geo=(59.9275, 10.7006), url="https://calendar.example/e/e-oslo-fredag/2026-11-20", rrule=None, zone=None),
    "weekly": dict(
        uid="e-seoul-han-2026-11-24@" + HOST,
        start=datetime(2026, 11, 24, 22, 45, tzinfo=U), end=datetime(2026, 11, 25, 0, 15, tzinfo=U),
        summary="한강 러닝 크루 Han River Run",
        location=bytes.fromhex("eb9a9dec84a0ed959ceab095eab3b5ec9b90").decode() + ", " + bytes.fromhex("ec849cec9ab8").decode(),
        description="화요일과 목요일, 6K 조깅. 모두 환영해요 and everyone is welcome to join the long easy run along the river.",
        geo=(37.5326, 126.9903), url=None, rrule={"FREQ": ["WEEKLY"], "BYDAY": ["TU", "TH"]}, zone=None),
    "weekly_neg": dict(
        uid="e-boston-charles-2026-11-22@" + HOST,
        start=datetime(2026, 11, 22, 18, 0, tzinfo=U), end=datetime(2026, 11, 22, 19, 0, tzinfo=U),
        summary="Charles River loop", location="Boston, MA", description=None,
        geo=(-0.0005, -71.0875), url=None, rrule={"FREQ": ["WEEKLY"], "BYDAY": ["MO", "WE", "SU"]}, zone=None),
    "weekly_ny": dict(
        uid="e-ny-tue-2026-11-24@" + HOST,
        start=datetime(2026, 11, 24, 12, 0, tzinfo=U), end=datetime(2026, 11, 24, 13, 0, tzinfo=U),
        summary="Tuesday run, Central Park", location="Central Park, New York", description=None, geo=None, url=None,
        rrule={"FREQ": ["WEEKLY"], "BYDAY": ["TU"]}, zone="America/New_York", local=datetime(2026, 11, 24, 7, 0)),
    "weekly_oslo": dict(
        uid="e-oslo-tue-2026-11-24@" + HOST,
        start=datetime(2026, 11, 24, 6, 0, tzinfo=U), end=datetime(2026, 11, 24, 7, 0, tzinfo=U),
        summary="Tirsdagsløp", location="Oslo", description=None, geo=None, url=None,
        rrule={"FREQ": ["WEEKLY"], "BYDAY": ["TU"]}, zone="Europe/Oslo", local=datetime(2026, 11, 24, 7, 0)),
    "weekly_seoul": dict(
        uid="e-seoul-tue-thu-2026-11-24@" + HOST,
        start=datetime(2026, 11, 23, 22, 45, tzinfo=U), end=datetime(2026, 11, 24, 0, 15, tzinfo=U),
        summary="Han River", location="Seoul", description=None, geo=None, url=None,
        rrule={"FREQ": ["WEEKLY"], "BYDAY": ["TU", "TH"]}, zone="Asia/Seoul", local=datetime(2026, 11, 24, 7, 45)),
    "weekly_ny_utc": dict(
        uid="e-ny-tue-utc-2026-11-24@" + HOST,
        start=datetime(2026, 11, 24, 12, 0, tzinfo=U), end=datetime(2026, 11, 24, 13, 0, tzinfo=U),
        summary="Tuesday run, Central Park", location="Central Park, New York", description=None, geo=None, url=None,
        rrule={"FREQ": ["WEEKLY"], "BYDAY": ["TU"]}, zone=None),
}


def check(cond, msg):
    if not cond:
        print("ORACLE FAIL:", msg)
        check.bad += 1


check.bad = 0


def fields(name, want, cal, raw, d):
    for line in raw.split(b"\r\n")[:-1]:
        check(len(line) <= 75, f"{name}: line over 75 octets: {line!r}")
        check(not (line.startswith(b" ") and 0x80 <= line[1] < 0xC0), f"{name}: fold inside UTF-8")
    check(raw.endswith(b"\r\n") and raw.count(b"\n") == raw.count(b"\r\n"), f"{name}: CRLF")
    evs = [c for c in cal.walk() if c.name == "VEVENT"]
    check(len(evs) == 1, f"{name}: one VEVENT")
    ev = evs[0]
    check(str(cal["VERSION"]) == "2.0", f"{name}: VERSION")
    check("raceclub" not in raw.decode().lower(), f"{name}: names an app")
    check(str(ev["UID"]) == want["uid"], f"{name}: UID {ev['UID']}")
    check(ev["DTSTAMP"].dt == NOW, f"{name}: DTSTAMP {ev['DTSTAMP'].dt}")
    check(ev["DTSTART"].dt == want["start"], f"{name}: DTSTART {ev['DTSTART'].dt} not {want['start']}")
    check(ev["DTEND"].dt == want["end"], f"{name}: DTEND {ev['DTEND'].dt} not {want['end']}")
    check(str(ev["SUMMARY"]) == want["summary"], f"{name}: SUMMARY {ev['SUMMARY']!r}")
    check(str(ev["LOCATION"]) == want["location"], f"{name}: LOCATION {ev['LOCATION']!r}")
    if want["description"] is None:
        check("DESCRIPTION" not in ev, f"{name}: DESCRIPTION present")
    else:
        check(str(ev["DESCRIPTION"]) == want["description"], f"{name}: DESCRIPTION {ev['DESCRIPTION']!r}")
    if want["geo"] is None:
        check("GEO" not in ev, f"{name}: GEO present")
    else:
        g = ev["GEO"]
        check(abs(g.latitude - want["geo"][0]) < 1e-9 and abs(g.longitude - want["geo"][1]) < 1e-9, f"{name}: GEO")
    check((want["url"] is None) == ("URL" not in ev) and (want["url"] is None or str(ev["URL"]) == want["url"]), f"{name}: URL")
    check(dict(ev["RRULE"]) == want["rrule"] if want["rrule"] else "RRULE" not in ev, f"{name}: RRULE")

    url = (d / f"{name}.url").read_text()
    u = urllib.parse.urlsplit(url)
    q = urllib.parse.parse_qs(u.query, strict_parsing=True, keep_blank_values=True)
    check(u.netloc == "calendar.google.com" and q["action"] == ["TEMPLATE"], f"{name}: link host/action")
    check(q["text"] == [want["summary"]], f"{name}: link text")
    check(q.get("location", [None])[0] == want["location"], f"{name}: link location")
    check(q.get("details", [None])[0] == want["description"], f"{name}: link details")
    if want["zone"]:
        loc = want["local"]
        end = loc + (want["end"] - want["start"])
        check(q["ctz"] == [want["zone"]], f"{name}: ctz {q.get('ctz')}")
        check(q["dates"] == [loc.strftime("%Y%m%dT%H%M%S") + "/" + end.strftime("%Y%m%dT%H%M%S")], f"{name}: local dates {q['dates']}")
    else:
        check("ctz" not in q, f"{name}: ctz present")
        check(q["dates"] == [want["start"].strftime("%Y%m%dT%H%M%SZ") + "/" + want["end"].strftime("%Y%m%dT%H%M%SZ")], f"{name}: dates {q['dates']}")
    if want["rrule"]:
        check(q["recur"] == ["RRULE:FREQ=WEEKLY;BYDAY=" + ",".join(want["rrule"]["BYDAY"])], f"{name}: recur {q.get('recur')}")
    else:
        check("recur" not in q, f"{name}: recur present")
    return ev


def zone_case(name, want, cal, ev):
    zone = want["zone"]
    zi = ZoneInfo(zone)
    vts = [c for c in cal.walk() if c.name == "VTIMEZONE"]
    check(len(vts) == 1 and str(vts[0]["TZID"]) == zone, f"{name}: one VTIMEZONE for {zone}")
    check(ev["DTSTART"].params["TZID"] == zone and ev["DTEND"].params["TZID"] == zone, f"{name}: TZID on DTSTART and DTEND")
    vtz = vts[0].to_tz(lookup_tzid=False)   # the file's own rules
    local0 = want["local"]
    # The file's zone gives zoneinfo's offset at every half hour of the two years the event runs.
    t = datetime(2026, 11, 1, tzinfo=U)
    while t < datetime(2028, 12, 1, tzinfo=U):
        a, b = t.astimezone(vtz).utcoffset(), t.astimezone(zi).utcoffset()
        check(a == b, f"{name}: at {t} the file's zone has offset {a}, zoneinfo {b}")
        if a != b:
            break
        t += timedelta(minutes=30)
    # The weekly rule, expanded, keeps its local time and lands where zoneinfo puts that local time.
    rule = rrulestr(ev["RRULE"].to_ical().decode(), dtstart=local0)
    hours = set()
    n = 0
    for occ in rule.between(local0, datetime(2028, 11, 30), inc=True):
        n += 1
        utc_file = occ.replace(tzinfo=vtz).astimezone(U)
        utc_real = occ.replace(tzinfo=zi).astimezone(U)
        check(utc_file == utc_real, f"{name}: {occ} is {utc_file} by the file, {utc_real} by zoneinfo")
        check((occ.hour, occ.minute) == (local0.hour, local0.minute), f"{name}: {occ} left its local time")
        hours.add(utc_real.hour)
    check(n >= 100, f"{name}: {n} occurrences")
    if zone != "Asia/Seoul":
        check(len(hours) == 2, f"{name}: the expansion spans a DST change (UTC hours {sorted(hours)})")
    else:
        check(len(hours) == 1, f"{name}: a fixed zone keeps one UTC hour")


def control(name, want, ev, raw):
    """The same event in UTC, seen in the real zone: its local hour drifts, which is the bug the zone fixes."""
    check(b"TZID" not in raw and ev["DTSTART"].dt.tzinfo is not None and ev["DTSTART"].dt.utcoffset() == timedelta(0), f"{name}: the control is in UTC")
    zi = ZoneInfo("America/New_York")
    rule = rrulestr(ev["RRULE"].to_ical().decode(), dtstart=ev["DTSTART"].dt)
    local_hours = {occ.astimezone(zi).hour for occ in rule.between(ev["DTSTART"].dt, datetime(2028, 11, 30, tzinfo=U), inc=True)}
    check(local_hours == {7, 8}, f"{name}: the UTC event should show at 07:00 and 08:00 New York time, got {sorted(local_hours)}")


def main(d):
    d = Path(d)
    for name, want in EXPECT.items():
        raw = (d / f"{name}.ics").read_bytes()
        cal = Calendar.from_ical(raw)
        ev = fields(name, want, cal, raw, d)
        if want["zone"]:
            zone_case(name, want, cal, ev)
        else:
            check(not [c for c in cal.walk() if c.name == "VTIMEZONE"], f"{name}: a VTIMEZONE in a UTC event")
        if name == "weekly_ny_utc":
            control(name, want, ev, raw)
    if check.bad:
        sys.exit(1)
    print("ics oracle passed for", ", ".join(EXPECT))


main(sys.argv[1] if len(sys.argv) > 1 else "lib/ics/test/golden")
