/* ics_test.c - the calendar writer and the link builder (ics.h, gcal.h): goldens byte for byte
 * (lib/ics/test/golden, read from $CBASE_ROOT or the current directory; `ics_test --write` rewrites them),
 * folding at 75 octets never inside a UTF-8 sequence, escaping, the snprintf-style length, the zone
 * (DTSTART;TZID and a VTIMEZONE from the rule) and the link's ctz. The goldens are also read by
 * lib/ics/test/ics_oracle.py against Python's icalendar (make ics-oracle).
 *
 * Exits 1 on any failure. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../test/check.h"
#include "ics.h"
#include "gcal.h"
#include "../tz/tz.h"

/* The zones this test links, as an app names its own. Cairo's rule has a /24 time. */
const tz_zone tz_zones[] = {
    { "America/New_York", "EST5EDT,M3.2.0,M11.1.0" },
    { "Europe/Oslo", "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Asia/Seoul", "KST-9" },
    { "Africa/Cairo", "EET-2EEST,M4.5.5/0,M10.5.4/24" },
};
const int tz_zone_count = 4;

#define S(x) ((ics_str){ (x), sizeof(x) - 1 })

static int64_t utc(int y, int mo, int d, int h, int mi) { return ((int64_t)tz_days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60) * 1000; }

/* 2026-11-20 18:00:00 UTC for one hour; the clock is 2026-11-18 09:30:15 UTC. */
#define T_START 1795197600000LL
#define T_END (T_START + 3600000LL)
#define T_NOW 1794994215000LL

static ics_event base(void)
{
    ics_event e;
    memset(&e, 0, sizeof e);
    e.id = S("e1");
    e.occ_day = S("2026-11-20");
    e.uid_host = S("calendar.example");
    e.now_ms = T_NOW;
    e.start_ms = T_START;
    e.end_ms = T_END;
    e.summary = S("Run");
    return e;
}

static char buf[1 << 14];

static int ics_of(const ics_event *e)
{
    int n = ics_write(buf, sizeof buf, e);
    CHECK(n < 0 || (size_t)n == strlen(buf), "the length is the text's: %d against %zu", n, strlen(buf));
    return n;
}

/* Copy of the text with every "\r\n " fold removed. */
static char *unfold(const char *s, size_t n)
{
    char *o = malloc(n + 1);
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\r' && i + 2 < n && s[i + 1] == '\n' && s[i + 2] == ' ') {
            i += 2;
            continue;
        }
        o[k++] = s[i];
    }
    o[k] = 0;
    return o;
}

/* The shape every output must have: CRLF only, lines <= 75 octets, no fold inside a UTF-8 sequence. */
static void check_shape(const char *what, const char *s, size_t n)
{
    size_t i = 0;
    while (i < n) {
        size_t j = i;
        while (j < n && s[j] != '\r') j++;
        CHECK(j + 1 < n && s[j + 1] == '\n', "%s: line without CRLF at %zu", what, i);
        CHECK(j - i <= 75, "%s: line of %zu octets", what, j - i);
        if (s[i] == ' ') CHECK(((unsigned char)s[i + 1] & 0xC0) != 0x80, "%s: fold inside UTF-8 at %zu", what, i);
        for (size_t k = i; k < j; k++) CHECK(s[k] != '\n', "%s: bare LF", what);
        i = j + 2;
    }
}

/* ---- goldens ---- */

typedef struct {
    const char *name;
    ics_event ev;
} golden_case;

#define NGOLDEN 6

static void cases(golden_case *g)
{
    ics_event o = base();
    o.id = S("e-oslo-fredag");
    o.summary = S("Fredagsl\xc3\xb8p p\xc3\xa5 Frognerparken");
    o.location = S("Frognerparken, Oslo, Norge");
    o.description = S("Rolig 5K, alle tempoer.\nM\xc3\xb8te ved fontenen; ta med vann, \\ ikke hund.");
    o.has_geo = 1;
    o.lat_udeg = 59927500;
    o.lng_udeg = 10700600;
    o.url = S("https://calendar.example/e/e-oslo-fredag/2026-11-20");
    g[0] = (golden_case){ "oneoff", o };

    ics_event w = base();
    w.id = S("e-seoul-han");
    w.occ_day = S("2026-11-24");
    w.start_ms = 1795473900000LL;   /* 2026-11-23 22:45 UTC: Tuesday 07:45 in Seoul */
    w.end_ms = w.start_ms + 5400000LL;
    w.summary = S("\xed\x95\x9c\xea\xb0\x95 \xeb\x9f\xac\xeb\x8b\x9d \xed\x81\xac\xeb\xa3\xa8 Han River Run");
    w.location = S("\xeb\x9a\x9d\xec\x84\xa0\xed\x95\x9c\xea\xb0\x95\xea\xb3\xb5\xec\x9b\x90, \xec\x84\x9c\xec\x9a\xb8");
    w.description = S("\xed\x99\x94\xec\x9a\x94\xec\x9d\xbc\xea\xb3\xbc \xeb\xaa\xa9\xec\x9a\x94\xec\x9d\xbc, 6K \xec\xa1\xb0\xea\xb9\x85. \xeb\xaa\xa8\xeb\x91\x90 \xed\x99\x98\xec\x98\x81\xed\x95\xb4\xec\x9a\x94 and everyone is welcome to join the long easy run along the river.");
    w.has_geo = 1;
    w.lat_udeg = 37532600;
    w.lng_udeg = 126990300;
    w.weekly = 1;
    w.byday = ICS_TU | ICS_TH;
    w.zone = S("Asia/Seoul");
    g[1] = (golden_case){ "weekly", w };

    ics_event n = base();
    n.id = S("e-boston-charles");
    n.occ_day = S("2026-11-22");
    n.start_ms = T_START + 2 * 86400000LL;
    n.end_ms = n.start_ms + 3600000LL;
    n.summary = S("Charles River loop");
    n.location = S("Boston, MA");
    n.has_geo = 1;
    n.lat_udeg = -500;
    n.lng_udeg = -71087500;
    n.weekly = 1;
    n.byday = ICS_SU | ICS_MO | ICS_WE;
    n.zone = S("America/New_York");   /* Sunday 13:00 in Boston */
    g[2] = (golden_case){ "weekly_neg", n };

    /* The same weekly runs in a zone: Tuesday 07:00 New York (12:00 UTC in November), Tuesday 07:00 Oslo
     * (06:00 UTC), Tuesday and Thursday 07:45 Seoul (22:45 UTC the day before). */
    ics_event ny = base();
    ny.id = S("e-ny-tue");
    ny.occ_day = S("2026-11-24");
    ny.start_ms = utc(2026, 11, 24, 12, 0);
    ny.end_ms = ny.start_ms + 3600000LL;
    ny.summary = S("Tuesday run, Central Park");
    ny.location = S("Central Park, New York");
    ny.weekly = 1;
    ny.byday = ICS_TU;
    ny.zone = S("America/New_York");
    g[3] = (golden_case){ "weekly_ny", ny };

    ics_event os = ny;
    os.id = S("e-oslo-tue");
    os.start_ms = utc(2026, 11, 24, 6, 0);
    os.end_ms = os.start_ms + 3600000LL;
    os.summary = S("Tirsdagsl\xc3\xb8p");
    os.location = S("Oslo");
    os.zone = S("Europe/Oslo");
    g[4] = (golden_case){ "weekly_oslo", os };

    ics_event se = ny;
    se.id = S("e-seoul-tue-thu");
    se.start_ms = utc(2026, 11, 23, 22, 45);   /* Tuesday 07:45 in Seoul */
    se.end_ms = se.start_ms + 5400000LL;
    se.summary = S("Han River");
    se.location = S("Seoul");
    se.byday = ICS_TU | ICS_TH;
    se.zone = S("Asia/Seoul");
    g[5] = (golden_case){ "weekly_seoul", se };

}

/* The same New York run as weekly_ny with no zone: refused by both writers, since in UTC a calendar app
 * shows it an hour off after each DST change. lib/ics/test/golden/weekly_ny_utc.* are that old form, kept
 * by hand as the oracle's negative control (ics_oracle.py); no writer produces them any more. */
static void test_weekly_needs_zone(void)
{
    golden_case g[NGOLDEN];
    cases(g);
    ics_event un = g[3].ev;
    un.id = S("e-ny-tue-utc");
    un.zone = (ics_str){ NULL, 0 };
    TEST("refusal: a repeating event with no zone is not written in UTC");
    char out[64] = "x";
    CHECK(ics_write(out, sizeof out, &un) == ICS_EINVAL && out[0] == 0, "ics_write wrote a weekly event with no zone");
    out[0] = 'x';
    CHECK(gcal_url(out, sizeof out, &un) == ICS_EINVAL && out[0] == 0, "gcal_url wrote a weekly event with no zone");
    un.weekly = 0;
    CHECK(ics_write(NULL, 0, &un) > 0, "a one-off event with no zone is still written, in UTC");
}

static char *slurp(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *b = malloc(1 << 16);
    *n = fread(b, 1, (1 << 16) - 1, f);
    b[*n] = 0;
    fclose(f);
    return b;
}

static void golden_one(const char *name, const char *ext, const char *got, size_t gn, int write)
{
    char path[512];
    const char *root = getenv("CBASE_ROOT");
    snprintf(path, sizeof path, "%s/lib/ics/test/golden/%s.%s", root && *root ? root : ".", name, ext);
    if (write) {
        FILE *f = fopen(path, "wb");
        if (f) {
            fwrite(got, 1, gn, f);
            fclose(f);
        }
        return;
    }
    size_t wn = 0;
    char *want = slurp(path, &wn);
    CHECK(want != NULL, "missing golden %s", path);
    if (!want) return;
    CHECK(wn == gn && memcmp(want, got, gn) == 0, "golden %s differs\n--- want\n%s\n--- got\n%s", path, want, got);
    free(want);
}

static void test_goldens(int write)
{
    golden_case g[NGOLDEN];
    cases(g);
    for (int i = 0; i < NGOLDEN; i++) {
        TEST("golden: the .ics and the link of each case, byte for byte");
        int n = ics_of(&g[i].ev);
        CHECK(n > 0, "write %s: %d", g[i].name, n);
        check_shape(g[i].name, buf, strlen(buf));
        golden_one(g[i].name, "ics", buf, strlen(buf), write);
        char url[2048];
        int un = gcal_url(url, sizeof url, &g[i].ev);
        CHECK(un > 0 && (size_t)un == strlen(url), "gcal %s: %d", g[i].name, un);
        golden_one(g[i].name, "url", url, strlen(url), write);
    }
}

/* ---- folding ---- */

static void fold_sweep(const char *label, const char *unit, size_t unit_len)
{
    /* ASCII padding 0..5 so each alignment of the multibyte unit against the 75-octet boundary is hit. */
    for (int pad = 0; pad < 6; pad++) {
        char text[512];
        size_t n = 0;
        for (int i = 0; i < pad; i++) text[n++] = 'a';
        for (int i = 0; i < 40; i++) {
            memcpy(text + n, unit, unit_len);
            n += unit_len;
        }
        ics_event e = base();
        e.summary = (ics_str){ text, n };
        CHECK(ics_of(&e) > 0, "write");
        check_shape(label, buf, strlen(buf));
        char *u = unfold(buf, strlen(buf));
        char want[600];
        int wl = snprintf(want, sizeof want, "SUMMARY:");
        memcpy(want + wl, text, n);
        want[wl + n] = 0;
        CHECK(strstr(u, want) != NULL, "%s pad %d: unfolded text lost", label, pad);
        CHECK(strstr(buf, "\r\n ") != NULL, "%s pad %d: no fold", label, pad);
        free(u);
    }
}

static void test_fold(void)
{
    TEST("fold: Hangul and Norwegian text folds only between characters");
    fold_sweep("hangul", "\xed\x95\x9c", 3);
    fold_sweep("norwegian", "\xc3\xa6", 2);

    TEST("fold: the 75th octet stays, the 76th goes to a continuation line of 74");
    /* "SUMMARY:" is 8 octets; 67 more make exactly 75, which must not fold. */
    char t[100];
    memset(t, 'x', 68);
    ics_event e = base();
    e.summary = (ics_str){ t, 67 };
    CHECK(ics_of(&e) > 0, "write");
    char want[256];
    memcpy(want, "SUMMARY:", 8);
    memset(want + 8, 'x', 67);
    memcpy(want + 75, "\r\nEND:VEVENT", 13);   /* with its NUL */
    CHECK(strstr(buf, want) != NULL, "75-octet line folded:\n%s", buf);
    e.summary = (ics_str){ t, 68 };
    CHECK(ics_of(&e) > 0, "write");
    memcpy(want + 75, "\r\n x\r\nEND:VEVENT", 17);
    CHECK(strstr(buf, want) != NULL, "76-octet line not folded after 75:\n%s", buf);
    char long_t[200];
    memset(long_t, 'z', 67 + 74 + 5);
    e.summary = (ics_str){ long_t, 67 + 74 + 5 };
    CHECK(ics_of(&e) > 0, "write");
    check_shape("long", buf, strlen(buf));
    const char *second = strstr(buf, "\r\n z");
    CHECK(second != NULL, "no second line");
    if (second) {
        const char *third = strstr(second + 2, "\r\n ");
        CHECK(third != NULL && (size_t)(third - (second + 2)) == 75, "second physical line is not 75 octets");
    }
}

/* ---- escaping ---- */

static void test_escape(void)
{
    TEST("escape: backslash, semicolon, comma and newlines in text");
    ics_event e = base();
    e.summary = S("a\\b;c,d\ne\r\nf\rg");
    e.location = S("\\;,");
    e.description = S("line1\nline2");
    CHECK(ics_of(&e) > 0, "write");
    CHECK(strstr(buf, "SUMMARY:a\\\\b\\;c\\,d\\ne\\nf\\ng\r\n") != NULL, "summary escaping:\n%s", buf);
    CHECK(strstr(buf, "LOCATION:\\\\\\;\\,\r\n") != NULL, "location escaping:\n%s", buf);
    CHECK(strstr(buf, "DESCRIPTION:line1\\nline2\r\n") != NULL, "description escaping:\n%s", buf);

    TEST("escape: a control byte in id, host or url cannot inject a line");
    e = base();
    e.id = S("x\r\nEND:VEVENT");
    e.uid_host = S("h\r\nBEGIN:Y");
    e.url = S("http://h/\r\nBEGIN:X");
    CHECK(ics_of(&e) > 0, "write");
    CHECK(strstr(buf, "\r\nEND:VEVENT\r\nDTSTAMP") == NULL, "id injected a line:\n%s", buf);
    CHECK(strstr(buf, "\r\nBEGIN:Y") == NULL, "host injected a line:\n%s", buf);
    CHECK(strstr(buf, "\r\nBEGIN:X") == NULL, "url injected a line:\n%s", buf);
}

/* ---- time, geo, fields, errors ---- */

static void test_time(void)
{
    TEST("time: UTC stamps, floored to the second, either side of the epoch");
    struct { int64_t ms; const char *want; } t[] = {
        { 0, "19700101T000000Z" },
        { -1000, "19691231T235959Z" },
        { -1, "19691231T235959Z" },
        { 1709251199999LL, "20240229T235959Z" },
        { 1709251200000LL, "20240301T000000Z" },
        { T_NOW, "20261118T093015Z" },
        { 253402300799000LL, "99991231T235959Z" },
    };
    for (size_t i = 0; i < sizeof t / sizeof *t; i++) {
        char out[ICS_UTC_LEN];
        CHECK(ics_fmt_utc(out, t[i].ms) == 0 && strcmp(out, t[i].want) == 0, "ms %lld: got %s want %s", (long long)t[i].ms, out, t[i].want);
    }
    char out[ICS_UTC_LEN];
    CHECK(ics_fmt_utc(out, 253402300800000LL) == ICS_EINVAL && ics_fmt_utc(out, -62167219200001LL) == ICS_EINVAL, "year 10000 and year -1 are refused");
    CHECK(ics_fmt_utc(out, -62167219200000LL) == 0 && !strcmp(out, "00000101T000000Z"), "year 0 is the first");
    char loc[ICS_LOCAL_LEN];
    CHECK(ics_fmt_local(loc, tz_days_from_civil(2027, 3, 14), 3 * 3600 + 30 * 60 + 5) == 0 && !strcmp(loc, "20270314T033005"), "a local stamp: %s", loc);
}

static void test_fields(void)
{
    TEST("fields: geo, uid, stamps, and the optional lines");
    ics_event e = base();
    e.has_geo = 1;
    e.lat_udeg = -500;
    e.lng_udeg = -179999999;
    CHECK(ics_of(&e) > 0, "write");
    CHECK(strstr(buf, "\r\nGEO:-0.000500;-179.999999\r\n") != NULL, "geo:\n%s", buf);
    CHECK(strstr(buf, "\r\nUID:e1-2026-11-20@calendar.example\r\n") != NULL, "uid:\n%s", buf);
    CHECK(strstr(buf, "\r\nPRODID:-//cbase//calendar//EN\r\n") != NULL, "prodid:\n%s", buf);
    CHECK(strstr(buf, "\r\nDTSTAMP:20261118T093015Z\r\n") != NULL, "dtstamp:\n%s", buf);
    CHECK(strstr(buf, "\r\nDTSTART:20261120T180000Z\r\nDTEND:20261120T190000Z\r\n") != NULL, "dtstart/dtend:\n%s", buf);
    CHECK(strstr(buf, "RRULE") == NULL, "unexpected RRULE");
    CHECK(strstr(buf, "LOCATION") == NULL && strstr(buf, "DESCRIPTION") == NULL && strstr(buf, "URL:") == NULL, "empty fields written");
    e.has_geo = 0;
    e.weekly = 1;
    e.byday = ICS_FR | ICS_SA;
    CHECK(ics_of(&e) > 0, "write");
    CHECK(strstr(buf, "GEO") == NULL, "unexpected GEO");
    CHECK(strstr(buf, "\r\nRRULE:FREQ=WEEKLY;BYDAY=FR,SA\r\n") != NULL, "rrule:\n%s", buf);
    CHECK(strstr(buf, "VTIMEZONE") == NULL && strstr(buf, "DTSTART:20261120T180000Z") != NULL, "a repeating event with no zone is in UTC");
    e.byday = 0;
    CHECK(ics_of(&e) > 0, "write");
    CHECK(strstr(buf, "\r\nRRULE:FREQ=WEEKLY\r\n") != NULL, "rrule without byday:\n%s", buf);
    e = base();
    e.lat_udeg = 123456789;
    e.lng_udeg = 5000000;
    e.has_geo = 1;
    CHECK(ics_of(&e) > 0 && strstr(buf, "\r\nGEO:123.456789;5.000000\r\n") != NULL, "a three-digit degree:\n%s", buf);
}

static void test_errors(void)
{
    TEST("errors: refusals leave the empty string");
    ics_event e = base();
    e.end_ms = e.start_ms - 1;
    strcpy(buf, "keep");
    CHECK(ics_write(buf, sizeof buf, &e) == ICS_EINVAL && buf[0] == 0, "ics end<start accepted");
    strcpy(buf, "keep");
    CHECK(gcal_url(buf, sizeof buf, &e) == ICS_EINVAL && buf[0] == 0, "gcal end<start accepted");
    e = base();
    e.start_ms = 253402300800000LL; /* year 10000 */
    e.end_ms = e.start_ms;
    CHECK(ics_write(buf, sizeof buf, &e) == ICS_EINVAL, "year 10000 accepted");
    CHECK(gcal_url(buf, sizeof buf, &e) == ICS_EINVAL, "year 10000 in the link accepted");
    e = base();
    e.uid_host = (ics_str){ NULL, 0 };
    CHECK(ics_write(buf, sizeof buf, &e) == ICS_EINVAL, "no UID host accepted");

    TEST("errors: an unknown zone, or a rule the VTIMEZONE cannot say");
    e = base();
    e.weekly = 1;
    e.byday = ICS_FR;
    e.zone = S("Mars/Olympus");
    CHECK(ics_write(buf, sizeof buf, &e) == ICS_EZONE && gcal_url(buf, sizeof buf, &e) == ICS_EZONE, "an unknown zone");
    e.zone = S("Africa/Cairo");
    CHECK(ics_write(buf, sizeof buf, &e) == ICS_EZONE, "Cairo's /24 end is not an nSU rule");
    e.zone = (ics_str){ "America/New_York_with_a_name_that_is_far_too_long_to_be_any_zone_at_all", 70 };
    CHECK(ics_write(buf, sizeof buf, &e) == ICS_EZONE, "a name too long to be a zone");
    e = base();
    e.zone = S("Mars/Olympus");
    CHECK(ics_write(buf, sizeof buf, &e) > 0, "a one-off event is in UTC and never reads the zone");
}

static void test_length(void)
{
    TEST("length: the length needed, snprintf-style, whatever the capacity");
    ics_event e = base();
    e.summary = S("Fredagsl\xc3\xb8p");
    int n = ics_write(NULL, 0, &e);
    CHECK(n > 100, "length with no buffer: %d", n);
    char full[2048], small[64];
    CHECK(ics_write(full, sizeof full, &e) == n && (size_t)n == strlen(full), "the same with a buffer");
    CHECK(ics_write(small, 10, &e) == n && !strcmp(small, "BEGIN:VCA") && strlen(small) == 9, "cut to fit: \"%s\"", small);
    CHECK(ics_write(small, 1, &e) == n && small[0] == 0, "a one-byte buffer holds the NUL");
    char *exact = malloc((size_t)n + 1);
    CHECK(ics_write(exact, (size_t)n + 1, &e) == n && !strcmp(exact, full), "an exact fit");
    CHECK(ics_write(exact, (size_t)n, &e) == n && strlen(exact) == (size_t)n - 1 && !memcmp(exact, full, (size_t)n - 1), "one byte short loses the last");
    free(exact);
    char link[512];
    int ln = gcal_url(NULL, 0, &e);
    CHECK(ln > 40 && gcal_url(link, sizeof link, &e) == ln && (size_t)ln == strlen(link), "the link's length");
    CHECK(gcal_url(small, 8, &e) == ln && !strcmp(small, "https:/"), "the link cut to fit: \"%s\"", small);
    CHECK(pct_encode(NULL, 0, "a b", 3) == 5 && pct_encode(small, 4, "a b", 3) == 5 && !strcmp(small, "a%2"), "pct_encode counts what it needs");
}

/* ---- the zone ---- */

static void test_zone(void)
{
    ics_event e = base();
    e.weekly = 1;
    e.byday = ICS_TU;
    e.zone = S("America/New_York");
    e.start_ms = utc(2026, 11, 24, 12, 0);
    e.end_ms = e.start_ms + 3600000LL;

    TEST("zone: a repeating event is written in the zone's local time with a VTIMEZONE from its rule");
    CHECK(ics_of(&e) > 0, "write");
    CHECK(strstr(buf, "\r\nDTSTART;TZID=America/New_York:20261124T070000\r\nDTEND;TZID=America/New_York:20261124T080000\r\n") != NULL, "local DTSTART and DTEND:\n%s", buf);
    CHECK(strstr(buf, "\r\nDTSTAMP:20261118T093015Z\r\n") != NULL, "DTSTAMP stays UTC");
    const char *tz = strstr(buf, "BEGIN:VTIMEZONE\r\nTZID:America/New_York\r\n");
    CHECK(tz != NULL && tz < strstr(buf, "BEGIN:VEVENT"), "a VTIMEZONE before the event:\n%s", buf);
    CHECK(strstr(buf, "BEGIN:DAYLIGHT\r\nTZOFFSETFROM:-0500\r\nTZOFFSETTO:-0400\r\nTZNAME:EDT\r\nDTSTART:20260308T020000\r\nRRULE:FREQ=YEARLY;BYMONTH=3;BYDAY=2SU\r\nEND:DAYLIGHT\r\n") != NULL, "the DAYLIGHT part:\n%s", buf);
    CHECK(strstr(buf, "BEGIN:STANDARD\r\nTZOFFSETFROM:-0400\r\nTZOFFSETTO:-0500\r\nTZNAME:EST\r\nDTSTART:20261101T020000\r\nRRULE:FREQ=YEARLY;BYMONTH=11;BYDAY=1SU\r\nEND:STANDARD\r\n") != NULL, "the STANDARD part:\n%s", buf);

    TEST("zone: the last week is -1, a time other than 02:00 is the part's DTSTART");
    e.zone = S("Europe/Oslo");
    e.start_ms = utc(2026, 11, 24, 6, 0);
    e.end_ms = e.start_ms + 3600000LL;
    CHECK(ics_of(&e) > 0, "write");
    CHECK(strstr(buf, "BEGIN:DAYLIGHT\r\nTZOFFSETFROM:+0100\r\nTZOFFSETTO:+0200\r\nTZNAME:CEST\r\nDTSTART:20260329T020000\r\nRRULE:FREQ=YEARLY;BYMONTH=3;BYDAY=-1SU\r\n") != NULL, "Oslo's DAYLIGHT part:\n%s", buf);
    CHECK(strstr(buf, "BEGIN:STANDARD\r\nTZOFFSETFROM:+0200\r\nTZOFFSETTO:+0100\r\nTZNAME:CET\r\nDTSTART:20261025T030000\r\nRRULE:FREQ=YEARLY;BYMONTH=10;BYDAY=-1SU\r\n") != NULL, "Oslo's STANDARD part, at 03:00:\n%s", buf);

    TEST("zone: a fixed zone has one STANDARD part and no rule");
    e.zone = S("Asia/Seoul");
    e.start_ms = utc(2026, 11, 23, 22, 45);
    e.end_ms = e.start_ms + 5400000LL;
    CHECK(ics_of(&e) > 0, "write");
    CHECK(strstr(buf, "BEGIN:VTIMEZONE\r\nTZID:Asia/Seoul\r\nBEGIN:STANDARD\r\nTZOFFSETFROM:+0900\r\nTZOFFSETTO:+0900\r\nTZNAME:KST\r\nDTSTART:19700101T000000\r\nEND:STANDARD\r\nEND:VTIMEZONE\r\n") != NULL, "Seoul:\n%s", buf);
    CHECK(strstr(buf, "DTSTART;TZID=Asia/Seoul:20261124T074500\r\n") != NULL && strstr(buf, "RRULE:FREQ=YEARLY") == NULL, "Seoul's local start, and no yearly rule");

    TEST("zone: a one-off event with a zone stays in UTC");
    e.weekly = 0;
    CHECK(ics_of(&e) > 0 && strstr(buf, "VTIMEZONE") == NULL && strstr(buf, "TZID") == NULL && strstr(buf, "DTSTART:20261123T224500Z") != NULL, "one-off:\n%s", buf);

    TEST("zone: the link carries ctz and local times for a repeating event, UTC otherwise");
    e = base();
    e.weekly = 1;
    e.byday = ICS_TU;
    e.zone = S("America/New_York");
    e.start_ms = utc(2026, 11, 24, 12, 0);
    e.end_ms = e.start_ms + 3600000LL;
    char url[1024];
    CHECK(gcal_url(url, sizeof url, &e) > 0, "link");
    CHECK(strstr(url, "&dates=20261124T070000/20261124T080000&") != NULL && strstr(url, "&ctz=America%2FNew_York") != NULL, "local dates and ctz: %s", url);
    CHECK(strstr(url, "&recur=RRULE%3AFREQ%3DWEEKLY%3BBYDAY%3DTU") != NULL, "recur: %s", url);
    e.weekly = 0;
    CHECK(gcal_url(url, sizeof url, &e) > 0 && strstr(url, "&dates=20261124T120000Z/20261124T130000Z") != NULL && strstr(url, "ctz") == NULL, "a one-off link is UTC: %s", url);
}

/* ---- the web calendar link ---- */

static void test_gcal(void)
{
    TEST("gcal: percent-encoding and the link's parts");
    char out[256];
    CHECK(pct_encode(out, sizeof out, "aZ09-._~ /?&=#+%\xc3\xa6\xed\x95\x9c", 21) == 47 && strcmp(out, "aZ09-._~%20%2F%3F%26%3D%23%2B%25%C3%A6%ED%95%9C") == 0, "encoded: %s", out);

    ics_event e = base();
    e.summary = S("Fredagsl\xc3\xb8p & mer");
    e.description = S("a\nb");
    e.location = S("Oslo, Norge");
    e.weekly = 1;
    e.byday = ICS_MO | ICS_TH;
    char url[1024];
    CHECK(gcal_url(url, sizeof url, &e) > 0, "gcal");
    const char *want =
        "https://calendar.google.com/calendar/render?action=TEMPLATE&text=Fredagsl%C3%B8p%20%26%20mer"
        "&dates=20261120T180000Z/20261120T190000Z&details=a%0Ab&location=Oslo%2C%20Norge"
        "&recur=RRULE%3AFREQ%3DWEEKLY%3BBYDAY%3DMO%2CTH";
    CHECK(strcmp(url, want) == 0, "gcal url:\n%s", url);
}

int main(int argc, char **argv)
{
    int write = argc > 1 && strcmp(argv[1], "--write") == 0;
    test_goldens(write);
    test_weekly_needs_zone();
    test_fold();
    test_escape();
    test_time();
    test_fields();
    test_errors();
    test_length();
    test_zone();
    test_gcal();
    return report("ics");
}
