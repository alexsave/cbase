// ics.c - the RFC 5545 writer (ics.h). Output goes through one small writer that counts every byte it is
// given and stores the ones that fit, so the length needed is known whatever the buffer, and folds
// each content line at 75 octets, never inside a UTF-8 sequence (RFC 5545 section 3.1).
#include "ics.h"
#include "../tz/tz.h"

typedef struct {
    char *p;
    size_t cap, len;
    size_t col;   // octets in the physical line so far
    int err;
} W;

static void put(W *w, char c)
{
    if (w->p && w->len + 1 < w->cap) w->p[w->len] = c;
    w->len++;
}

// One unit (a byte, or a whole UTF-8 sequence) onto the line, folding first when it would pass 75 octets.
static __attribute__((noinline)) void unit(W *w, const char *s, size_t n)
{
    if (w->col + n > 75) {
        put(w, '\r');
        put(w, '\n');
        put(w, ' ');
        w->col = 1;
    }
    for (size_t i = 0; i < n; i++) put(w, s[i]);
    w->col += n;
}

static void eol(W *w)
{
    put(w, '\r');
    put(w, '\n');
    w->col = 0;
}

enum { RAW, TEXT, PLAIN };

// The n bytes at s as part of a line: TEXT escapes \ ; , and newlines (RFC 5545 section 3.3.11), PLAIN
// turns control bytes into '-' (a UID or URL cannot start a line of its own).
static __attribute__((noinline)) void bytes(W *w, const char *s, size_t n, int mode)
{
    for (size_t i = 0; i < n;) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 0xC0) {   // a UTF-8 lead byte and the continuation bytes that follow it
            size_t k = 1, need = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
            while (k <= need && i + k < n && ((unsigned char)s[i + k] & 0xC0) == 0x80) k++;
            unit(w, s + i, k);
            i += k;
            continue;
        }
        i++;
        if (mode == TEXT && (c == '\\' || c == ';' || c == ',')) {
            unit(w, "\\", 1);
            unit(w, (const char *)&c, 1);
        } else if (mode == TEXT && (c == '\r' || c == '\n')) {
            if (c == '\r' && i < n && s[i] == '\n') i++;
            unit(w, "\\", 1);
            unit(w, "n", 1);
        } else {
            char o = (char)c;
            if (mode == PLAIN && (c < 0x20 || c == 0x7F)) o = '-';
            unit(w, &o, 1);
        }
    }
}

static __attribute__((noinline)) void z(W *w, const char *s)
{
    size_t n = 0;
    while (s[n]) n++;
    bytes(w, s, n, RAW);
}

// A whole line of fixed text.
static void line(W *w, const char *s)
{
    z(w, s);
    eol(w);
}

static __attribute__((noinline)) void num(W *w, int64_t v, int digits)
{
    char t[12];
    for (int i = digits - 1; i >= 0; i--) {
        t[i] = (char)('0' + v % 10);
        v /= 10;
    }
    bytes(w, t, (size_t)digits, RAW);
}

static void text_line(W *w, const char *name, ics_str v)
{
    z(w, name);
    z(w, ":");
    bytes(w, v.p, v.n, TEXT);
    eol(w);
}

int ics_fmt_local(char out[ICS_LOCAL_LEN], int32_t day, int sec)
{
    int64_t y;
    int m, d;
    if (day < -719528 || day > 2932896) return ICS_EINVAL;   // 0000-01-01 .. 9999-12-31
    tz_civil_from_days(day, &y, &m, &d);
    int v[6] = { (int)y, m, d, sec / 3600, sec / 60 % 60, sec % 60 };
    static const int dig[6] = { 4, 2, 2, 2, 2, 2 };
    int o = 0;
    for (int k = 0; k < 6; k++) {
        for (int i = dig[k] - 1; i >= 0; i--) {
            out[o + i] = (char)('0' + v[k] % 10);
            v[k] /= 10;
        }
        o += dig[k];
        if (k == 2) out[o++] = 'T';
    }
    out[15] = 0;
    return 0;
}

int ics_fmt_utc(char out[ICS_UTC_LEN], int64_t ms)
{
    int64_t secs = ms / 1000 - (ms % 1000 < 0);
    int64_t days = secs / 86400 - (secs % 86400 < 0);
    if (days < INT32_MIN || days > INT32_MAX || ics_fmt_local(out, (int32_t)days, (int)(secs - days * 86400))) return ICS_EINVAL;
    out[15] = 'Z';
    out[16] = 0;
    return 0;
}

// A local stamp onto the line; a day outside 0000..9999 is an error.
static void local_stamp(W *w, int32_t day, int sec)
{
    char t[ICS_LOCAL_LEN];
    if (ics_fmt_local(t, day, sec)) w->err = ICS_EINVAL;
    else z(w, t);
}

static void utc_line(W *w, const char *name, int64_t ms)
{
    char t[ICS_UTC_LEN];
    if (ics_fmt_utc(t, ms)) {
        w->err = ICS_EINVAL;
        return;
    }
    z(w, name);
    z(w, ":");
    z(w, t);
    eol(w);
}

static void udeg(W *w, int32_t v)
{
    int64_t a = v;
    if (a < 0) {
        z(w, "-");
        a = -a;
    }
    num(w, a / 1000000, a / 1000000 >= 100 ? 3 : a / 1000000 >= 10 ? 2 : 1);
    z(w, ".");
    num(w, a % 1000000, 6);
}

static void byday(W *w, unsigned mask)
{
    static const char *const DAYS[7] = { "MO", "TU", "WE", "TH", "FR", "SA", "SU" };
    int first = 1;
    for (int i = 0; i < 7; i++) {
        if (!(mask & (1u << i))) continue;
        if (!first) z(w, ",");
        z(w, DAYS[i]);
        first = 0;
    }
}

// ---- the zone -----------------------------------------------------------------------------------

static void offset(W *w, const char *name, int32_t off)
{
    z(w, name);
    z(w, ":");
    z(w, off < 0 ? "-" : "+");
    if (off < 0) off = -off;
    num(w, off / 3600, 2);
    num(w, off / 60 % 60, 2);
    eol(w);
}

// One part of a VTIMEZONE: from the offset in force before the change to the one after it, first seen at the
// local time `s` (seconds since the epoch's local midnight, on the wall clock before the change) and
// recurring each year on `date`, if there is one.
static void part(W *w, const char *kind, int32_t from, int32_t to, const char *abbr, int64_t s, const tz_date *date)
{
    static const char *const DAYS[7] = { "SU", "MO", "TU", "WE", "TH", "FR", "SA" };
    int32_t day = (int32_t)(s / 86400 - (s % 86400 < 0));
    z(w, "BEGIN:");
    z(w, kind);
    eol(w);
    offset(w, "TZOFFSETFROM", from);
    offset(w, "TZOFFSETTO", to);
    z(w, "TZNAME:");
    z(w, abbr);
    eol(w);
    z(w, "DTSTART:");
    local_stamp(w, day, (int)(s - (int64_t)day * 86400));
    eol(w);
    if (date) {
        z(w, "RRULE:FREQ=YEARLY;BYMONTH=");
        num(w, date->m, date->m >= 10 ? 2 : 1);
        z(w, ";BYDAY=");
        if (date->w == 5) z(w, "-1");
        else num(w, date->w, 1);
        z(w, DAYS[date->d]);
        eol(w);
    }
    z(w, "END:");
    z(w, kind);
    eol(w);
}

// The VTIMEZONE of `name` made from its rule, read for the year the event starts in.
static void vtimezone(W *w, const char *name, const tz_rule *r, int year)
{
    line(w, "BEGIN:VTIMEZONE");
    z(w, "TZID:");
    z(w, name);
    eol(w);
    if (r->has_dst) {
        int64_t t[2];
        tz_rule_transitions(r, year, t);
        if (r->start.time_s < 0 || r->start.time_s >= 86400 || r->end.time_s < 0 || r->end.time_s >= 86400 || r->start.kind != 'M' || r->end.kind != 'M') {
            w->err = ICS_EZONE;
            return;
        }
        part(w, "DAYLIGHT", r->std_off, r->dst_off, r->dst, t[0] / 1000 + r->std_off, &r->start);
        part(w, "STANDARD", r->dst_off, r->std_off, r->std, t[1] / 1000 + r->dst_off, &r->end);
    } else {
        part(w, "STANDARD", r->std_off, r->std_off, r->std, 0, NULL);   // no change: from the epoch's midnight
    }
    line(w, "END:VTIMEZONE");
}

static void local_line(W *w, const char *name, const char *tzid, const tz_rule *r, int64_t ms)
{
    int32_t day;
    int minute;
    tz_rule_local(r, ms, &day, &minute, NULL, NULL);
    int64_t s = ms / 1000 % 60;
    z(w, name);
    z(w, ";TZID=");
    z(w, tzid);
    z(w, ":");
    local_stamp(w, day, minute * 60 + (int)(s < 0 ? s + 60 : s));
    eol(w);
}

// ---- the event ----------------------------------------------------------------------------------

static void write_all(W *w, const ics_event *ev)
{
    char zone[64];
    tz_rule rule;
    int local = 0;
    if (ev->end_ms < ev->start_ms || !ev->uid_host.n) {
        w->err = ICS_EINVAL;
        return;
    }
    // A repeating event takes a zone: written in UTC it shows an hour off in a calendar app after each DST
    // change of the zone its people live in (ics.h, TIME), so it is refused rather than written so.
    if (ev->weekly && !ev->zone.n) {
        w->err = ICS_EINVAL;
        return;
    }
    if (ev->weekly) {
        if (ev->zone.n >= sizeof zone) {
            w->err = ICS_EZONE;
            return;
        }
        for (size_t i = 0; i < ev->zone.n; i++) zone[i] = ev->zone.p[i];
        zone[ev->zone.n] = 0;
        int zi = tz_find(zone);
        if (zi < 0 || tz_rule_parse(tz_zone_rule(zi), &rule)) {
            w->err = ICS_EZONE;
            return;
        }
        local = 1;
    }
    line(w, "BEGIN:VCALENDAR");
    line(w, "VERSION:2.0");
    line(w, "PRODID:-//cbase//calendar//EN");
    line(w, "CALSCALE:GREGORIAN");
    if (local) {
        int32_t day;
        tz_rule_local(&rule, ev->start_ms, &day, NULL, NULL, NULL);
        int64_t y;
        int m, d;
        tz_civil_from_days(day, &y, &m, &d);
        vtimezone(w, zone, &rule, (int)y);
        if (w->err) return;
    }
    line(w, "BEGIN:VEVENT");
    z(w, "UID:");
    bytes(w, ev->id.p, ev->id.n, PLAIN);
    z(w, "-");
    bytes(w, ev->occ_day.p, ev->occ_day.n, PLAIN);
    z(w, "@");
    bytes(w, ev->uid_host.p, ev->uid_host.n, PLAIN);
    eol(w);
    utc_line(w, "DTSTAMP", ev->now_ms);
    if (local) {
        local_line(w, "DTSTART", zone, &rule, ev->start_ms);
        local_line(w, "DTEND", zone, &rule, ev->end_ms);
    } else {
        utc_line(w, "DTSTART", ev->start_ms);
        utc_line(w, "DTEND", ev->end_ms);
    }
    if (ev->weekly) {
        z(w, "RRULE:FREQ=WEEKLY");
        if (ev->byday & 0x7F) {
            z(w, ";BYDAY=");
            byday(w, ev->byday);
        }
        eol(w);
    }
    text_line(w, "SUMMARY", ev->summary);
    if (ev->location.n) text_line(w, "LOCATION", ev->location);
    if (ev->description.n) text_line(w, "DESCRIPTION", ev->description);
    if (ev->has_geo) {
        z(w, "GEO:");
        udeg(w, ev->lat_udeg);
        z(w, ";");
        udeg(w, ev->lng_udeg);
        eol(w);
    }
    if (ev->url.n) {
        z(w, "URL:");
        bytes(w, ev->url.p, ev->url.n, PLAIN);
        eol(w);
    }
    line(w, "END:VEVENT");
    line(w, "END:VCALENDAR");
}

int ics_write(char *out, size_t cap, const ics_event *ev)
{
    W w = { out, cap, 0, 0, 0 };
    write_all(&w, ev);
    if (w.err) {
        if (out && cap) out[0] = 0;
        return w.err;
    }
    if (out && cap) out[w.len < cap ? w.len : cap - 1] = 0;
    return (int)w.len;
}
