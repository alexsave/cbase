/* tz_test.c - zones from POSIX rules (tz.h): every transition of the oracle (lib/tz/test/transitions.tsv, from
 * Python's zoneinfo, written by tools/tz_gen.py) reproduced by tz_rule_transitions, the local time one minute
 * either side of each, the gap and overlap of New York and Oslo, a weekly run across both changes, the fixed
 * zones at the year boundaries, the parser on every rule of lib/tz/tzdb.tsv, and its refusals.
 * The data files are read from $CBASE_ROOT (cbase.mk exports it) or the current directory.
 *
 * Exits 1 on any failure. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../test/check.h"
#include "tz.h"

/* The zones this test links, as an app names its own (Makefile: APP_TZ_ZONES). */
const tz_zone tz_zones[] = {
    { "America/New_York", "EST5EDT,M3.2.0,M11.1.0" },
    { "America/Los_Angeles", "PST8PDT,M3.2.0,M11.1.0" },
    { "Europe/Oslo", "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Asia/Seoul", "KST-9" },
    { "Asia/Singapore", "<+08>-8" },
};
const int tz_zone_count = 5;

static FILE *open_data(const char *rel)
{
    char path[512];
    const char *root = getenv("CBASE_ROOT");
    snprintf(path, sizeof path, "%s/%s", root && *root ? root : ".", rel);
    FILE *f = fopen(path, "r");
    if (!f) fprintf(stderr, "cannot open %s\n", path);
    return f;
}

static int64_t utc_ms(int y, int mo, int d, int h, int mi) { return ((int64_t)tz_days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60) * 1000; }

static void oracle(void)
{
    FILE *f = open_data("lib/tz/test/transitions.tsv");
    CHECK(f != NULL, "the oracle is there");
    if (!f) return;
    char line[512];
    tz_rule r = { 0 };
    char rule[256] = "", zone[64] = "";
    int rules = 0, from_year = 0, transitions = 0, expect = 0, seen_in_rule = 0;
    int64_t last_t = 0;
    int have = 0;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = 0;
        if (line[0] == '#' || !line[0]) continue;
        if (line[0] == 'Z') {
            if (have) CHECK(seen_in_rule == expect, "%s: the oracle holds %d transitions, the rule gives %d", zone, seen_in_rule, expect);
            char *a = strchr(line + 2, '\t'), *b = a ? strchr(a + 1, '\t') : NULL;
            CHECK(a && b, "a Z line: %s", line);
            if (!a || !b) break;
            *a = *b = 0;
            snprintf(zone, sizeof zone, "%s", line + 2);
            snprintf(rule, sizeof rule, "%s", a + 1);
            from_year = atoi(b + 1);
            TEST("oracle: every rule of the oracle parses");
            CHECK(tz_rule_parse(rule, &r) == 0, "%s: %s", zone, rule);
            rules++;
            have = 1;
            seen_in_rule = 0;
            /* how many transitions the rule gives from from_year to 2050 */
            expect = r.has_dst ? 2 * (2050 - from_year + 1) : 0;
            last_t = 0;
            continue;
        }
        long long t;
        int before, after;
        char abbr[16];
        if (sscanf(line, "T\t%lld\t%d\t%d\t%15s", &t, &before, &after, abbr) != 4) {
            CHECK(0, "a T line: %s", line);
            continue;
        }
        seen_in_rule++;
        transitions++;
        int64_t ms = t * 1000, y;
        int mo, d;
        tz_civil_from_days(t / 86400, &y, &mo, &d);

        TEST("oracle: tz_rule_transitions gives each transition zoneinfo has");
        int64_t tr[2];
        CHECK(tz_rule_transitions(&r, (int)y, tr) == 2, "%s: %lld has a transition of year %d", zone, t, (int)y);
        CHECK(tr[0] == ms || tr[1] == ms, "%s %s: zoneinfo has %lld, rule gives %lld and %lld", zone, rule, (long long)ms, (long long)tr[0], (long long)tr[1]);
        CHECK(t > last_t, "%s: the oracle is in order", zone);
        last_t = t;

        TEST("oracle: one minute either side of a transition has the offset before and after");
        int off, off2;
        char ab[TZ_ABBR_MAX];
        CHECK(tz_rule_local(&r, ms - 60000, NULL, NULL, ab, &off) == 0 && off == before, "%s at %lld-1min: offset %d, zoneinfo %d", zone, t, off, before);
        CHECK(tz_rule_local(&r, ms + 60000, NULL, NULL, ab, &off2) == 0 && off2 == after, "%s at %lld+1min: offset %d, zoneinfo %d", zone, t, off2, after);
        CHECK(!strcmp(ab, abbr), "%s at %lld+1min: abbreviation %s, zoneinfo %s", zone, t, ab, abbr);
        CHECK(tz_rule_local(&r, ms - 1, NULL, NULL, NULL, &off) == 0 && off == before, "%s: a millisecond before is still before", zone);
        CHECK(tz_rule_local(&r, ms, NULL, NULL, NULL, &off) == 0 && off == after, "%s: the transition instant is after", zone);

        TEST("oracle: local times around a transition convert back");
        for (int m = -180; m <= 180; m += 15) {
            int32_t day;
            int minute, o;
            int64_t u = ms + m * 60000LL, back;
            tz_rule_local(&r, u, &day, &minute, NULL, &o);
            /* the local reading, put back through the rule: never later than the instant, and the same reading */
            int rc = tz_rule_utc_ms(&r, day, minute, &back);
            int32_t d2;
            int m2;
            tz_rule_local(&r, back, &d2, &m2, NULL, NULL);
            CHECK(rc >= 0 && d2 == day && m2 == minute, "%s: %+d min from %lld: %d/%d back as %d/%d", zone, m, t, day, minute, d2, m2);
            CHECK(back <= u, "%s: %+d min from %lld: the first reading of a time is not after the instant", zone, m, t);
        }
    }
    if (have) CHECK(seen_in_rule == expect, "%s: the oracle holds %d transitions, the rule gives %d", zone, seen_in_rule, expect);
    fclose(f);
    CHECK(rules >= 90 && transitions >= 1000, "the oracle was read: %d rules, %d transitions", rules, transitions);
}

static void gap_and_overlap(void)
{
    int ny = tz_find("America/New_York"), osl = tz_find("Europe/Oslo");
    int64_t u;
    TEST("gap: a time that never happens moves forward by the gap");
    CHECK(tz_utc_ms(ny, tz_days_from_civil(2027, 3, 14), 2 * 60 + 30, &u) == TZ_GAP && u == utc_ms(2027, 3, 14, 7, 30), "New York 02:30 on 2027-03-14 is %lld", (long long)u);
    char ab[TZ_ABBR_MAX];
    int32_t day;
    int minute, off;
    CHECK(tz_local(ny, u, &day, &minute, ab, &off) == 0 && minute == 3 * 60 + 30 && !strcmp(ab, "EDT") && off == -240, "that is 03:30 EDT");
    CHECK(tz_utc_ms(osl, tz_days_from_civil(2027, 3, 28), 2 * 60 + 30, &u) == TZ_GAP && u == utc_ms(2027, 3, 28, 1, 30), "Oslo 02:30 on 2027-03-28 is %lld", (long long)u);
    CHECK(tz_utc_ms(ny, tz_days_from_civil(2027, 3, 14), 2 * 60, &u) == TZ_GAP && u == utc_ms(2027, 3, 14, 7, 0), "the first minute of the gap");
    CHECK(tz_utc_ms(ny, tz_days_from_civil(2027, 3, 14), 3 * 60, &u) == TZ_OK && u == utc_ms(2027, 3, 14, 7, 0), "03:00 is the first minute after it");
    CHECK(tz_utc_ms(ny, tz_days_from_civil(2027, 3, 14), 1 * 60 + 59, &u) == TZ_OK && u == utc_ms(2027, 3, 14, 6, 59), "01:59 is before it");

    TEST("overlap: a time that happens twice takes the first");
    CHECK(tz_utc_ms(ny, tz_days_from_civil(2027, 11, 7), 1 * 60 + 30, &u) == TZ_OVERLAP && u == utc_ms(2027, 11, 7, 5, 30), "New York 01:30 on 2027-11-07 is %lld", (long long)u);
    CHECK(tz_local(ny, u, NULL, NULL, ab, NULL) == 0 && !strcmp(ab, "EDT"), "that is EDT");
    CHECK(tz_utc_ms(osl, tz_days_from_civil(2027, 10, 31), 2 * 60 + 30, &u) == TZ_OVERLAP && u == utc_ms(2027, 10, 31, 0, 30), "Oslo 02:30 on 2027-10-31 is %lld", (long long)u);
    CHECK(tz_local(osl, u, NULL, NULL, ab, NULL) == 0 && !strcmp(ab, "CEST"), "that is CEST");
    CHECK(tz_utc_ms(ny, tz_days_from_civil(2027, 11, 7), 2 * 60, &u) == TZ_OK && u == utc_ms(2027, 11, 7, 7, 0), "02:00 after the overlap is once");
    CHECK(tz_utc_ms(ny, tz_days_from_civil(2027, 11, 7), 0, &u) == TZ_OK && u == utc_ms(2027, 11, 7, 4, 0), "00:00 before it is once");

    TEST("gap: a southern zone's gap and overlap, and a zone whose DST is the lesser offset");
    tz_rule syd, dub;
    CHECK(tz_rule_parse("AEST-10AEDT,M10.1.0,M4.1.0/3", &syd) == 0 && tz_rule_parse("IST-1GMT0,M10.5.0,M3.5.0/1", &dub) == 0, "parse");
    CHECK(tz_rule_utc_ms(&syd, tz_days_from_civil(2026, 10, 4), 2 * 60 + 30, &u) == TZ_GAP && u == utc_ms(2026, 10, 3, 16, 30), "Sydney gap 02:30 2026-10-04 is %lld", (long long)u);
    CHECK(tz_rule_utc_ms(&syd, tz_days_from_civil(2027, 4, 4), 2 * 60 + 30, &u) == TZ_OVERLAP && u == utc_ms(2027, 4, 3, 15, 30), "Sydney overlap 02:30 2027-04-04 is %lld", (long long)u);
    CHECK(tz_rule_utc_ms(&dub, tz_days_from_civil(2027, 3, 28), 1 * 60 + 30, &u) == TZ_GAP && u == utc_ms(2027, 3, 28, 1, 30), "Dublin gap 01:30 2027-03-28 is %lld", (long long)u);
    CHECK(tz_rule_utc_ms(&dub, tz_days_from_civil(2027, 10, 31), 1 * 60 + 30, &u) == TZ_OVERLAP && u == utc_ms(2027, 10, 31, 0, 30), "Dublin overlap 01:30 2027-10-31 is %lld", (long long)u);

    TEST("gap: bad arguments write nothing");
    u = 7;
    CHECK(tz_utc_ms(ny, 0, 1440, &u) == TZ_ERR && tz_utc_ms(ny, 0, -1, &u) == TZ_ERR && tz_utc_ms(99, 0, 0, &u) == TZ_ERR && tz_utc_ms(-1, 0, 0, &u) == TZ_ERR && u == 7, "refused");
    CHECK(tz_local(99, 0, NULL, NULL, NULL, NULL) == -1 && tz_find("Mars/Olympus") == -1 && tz_find(NULL) == -1 && tz_find("America/New_Yorkx") == -1 && tz_find("America/New_Yor") == -1, "unknown zones");
    CHECK(tz_find("Asia/Singapore") == 4 && tz_find("America/New_York") == 0 && !strcmp(tz_zone_name(2), "Europe/Oslo") && tz_zone_name(5) == NULL, "the handles are positions");
}

static void weekly(void)
{
    int ny = tz_find("America/New_York"), osl = tz_find("Europe/Oslo");
    TEST("weekly: a 07:00 run stays at 07:00 local across both changes, and the cities' gap moves between 6 and 5 hours");
    int32_t d0 = tz_days_from_civil(2027, 1, 4);   /* a Monday */
    int five = 0, six = 0, ny_h[2] = { 0, 0 }, osl_h[2] = { 0, 0 };
    for (int w = 0; w < 52; w++) {
        int64_t un = 0, uo = 0;
        int32_t day, d2;
        int minute, m2;
        CHECK(tz_utc_ms(ny, d0 + 7 * w, 7 * 60, &un) == TZ_OK && tz_utc_ms(osl, d0 + 7 * w, 7 * 60, &uo) == TZ_OK, "week %d", w);
        CHECK(tz_local(ny, un, &day, &minute, NULL, NULL) == 0 && day == d0 + 7 * w && minute == 420, "New York week %d back at %d", w, minute);
        CHECK(tz_local(osl, uo, &d2, &m2, NULL, NULL) == 0 && d2 == d0 + 7 * w && m2 == 420, "Oslo week %d back at %d", w, m2);
        /* New York is later in the day UTC: 12:00 or 11:00 against Oslo's 06:00 or 05:00 */
        int hn = (int)((un / 3600000) % 24), ho = (int)((uo / 3600000) % 24);
        ny_h[hn == 11]++;
        osl_h[ho == 5]++;
        CHECK((hn == 12 || hn == 11) && (ho == 6 || ho == 5), "week %d: UTC hours %d and %d", w, hn, ho);
        int64_t g = ((int64_t)hn - ho);
        if (g == 6) six++;
        if (g == 5) five++;
    }
    CHECK(five == 3 && six == 49, "the gap is 5 hours in %d weeks and 6 in %d (2027-03-15..28 is two, 10-31..11-06 one)", five, six);
    CHECK(ny_h[1] == 34 && ny_h[0] == 18, "New York runs at 11:00 UTC for %d weeks, 12:00 for %d", ny_h[1], ny_h[0]);
    CHECK(osl_h[1] == 31 && osl_h[0] == 21, "Oslo runs at 05:00 UTC for %d weeks, 06:00 for %d", osl_h[1], osl_h[0]);
}

static void fixed(void)
{
    int seoul = tz_find("Asia/Seoul"), sg = tz_find("Asia/Singapore");
    char ab[TZ_ABBR_MAX];
    int32_t day;
    int minute, off;
    int64_t u;
    TEST("fixed: KST-9 and <+08>-8 at the year boundaries");
    CHECK(tz_local(seoul, utc_ms(2026, 12, 31, 14, 59) + 59999, &day, &minute, ab, &off) == 0 && day == tz_days_from_civil(2026, 12, 31) && minute == 1439 && !strcmp(ab, "KST") && off == 540, "Seoul one ms before the new year");
    CHECK(tz_local(seoul, utc_ms(2026, 12, 31, 15, 0), &day, &minute, ab, &off) == 0 && day == tz_days_from_civil(2027, 1, 1) && minute == 0 && off == 540, "Seoul at the new year");
    CHECK(tz_local(sg, utc_ms(2026, 12, 31, 15, 59), &day, &minute, ab, &off) == 0 && day == tz_days_from_civil(2026, 12, 31) && minute == 1439 && !strcmp(ab, "+08") && off == 480, "Singapore a minute before");
    CHECK(tz_local(sg, utc_ms(2026, 12, 31, 16, 0), &day, &minute, ab, &off) == 0 && day == tz_days_from_civil(2027, 1, 1) && minute == 0 && off == 480, "Singapore at the new year");
    CHECK(tz_utc_ms(seoul, tz_days_from_civil(2027, 1, 1), 0, &u) == TZ_OK && u == utc_ms(2026, 12, 31, 15, 0), "Seoul 00:00 is the evening before UTC");
    CHECK(tz_utc_ms(sg, tz_days_from_civil(2028, 2, 29), 23 * 60 + 59, &u) == TZ_OK && u == utc_ms(2028, 2, 29, 15, 59), "Singapore on a leap day");
    int64_t tr[2] = { 5, 5 };
    tz_rule r;
    CHECK(tz_rule_parse("KST-9", &r) == 0 && tz_rule_transitions(&r, 2027, tr) == 0 && tr[0] == 5, "a fixed rule has no transitions");
    CHECK(tz_local(sg, -1, &day, &minute, NULL, NULL) == 0 && day == 0 && minute == 8 * 60 - 1, "an instant before 1970 floors: 1969-12-31 23:59:59.999 UTC is 07:59 in Singapore");
}

static void parser(void)
{
    tz_rule r;
    TEST("parser: fields of a few rules");
    CHECK(tz_rule_parse("<+0330>-3:30", &r) == 0 && r.std_off == 12600 && !r.has_dst && !strcmp(r.std, "+0330"), "quoted name and offset with minutes");
    CHECK(tz_rule_parse("EST5", &r) == 0 && r.std_off == -18000 && !strcmp(r.std, "EST"), "POSIX sign is inverted");
    CHECK(tz_rule_parse("CET-1CEST,M3.5.0,M10.5.0/3", &r) == 0 && r.std_off == 3600 && r.dst_off == 7200 && r.has_dst && r.start.kind == 'M' && r.start.m == 3 && r.start.w == 5 &&
          r.start.d == 0 && r.start.time_s == 7200 && r.end.m == 10 && r.end.time_s == 3 * 3600, "CET's fields, /time default 02:00");
    CHECK(tz_rule_parse("<-02>2<-01>,M3.5.0/-1,M10.5.0/0", &r) == 0 && r.start.time_s == -3600 && r.end.time_s == 0 && r.dst_off == -3600, "a negative time");
    CHECK(tz_rule_parse("EET-2EEST,M3.4.4/50,M10.4.4/50", &r) == 0 && r.start.time_s == 50 * 3600, "an hour past a day (/50)");
    CHECK(tz_rule_parse("X<+01>-1", &r) == -1, "a name of fewer than three letters");
    CHECK(tz_rule_parse("<+0545>-5:45:30<+0645>-6:45:10,J1/167,300/-167:59:59", &r) == 0 && r.std_off == 20730 && r.dst_off == 24310 && r.start.kind == 'J' && r.end.kind == 'N' && r.start.time_s == 167 * 3600 && r.end.time_s == -(167 * 3600 + 59 * 60 + 59), "seconds, J and n dates, the extreme hours");
    CHECK(tz_rule_parse("EST5EDT", &r) == 0 && r.has_dst && r.start.m == 3 && r.start.w == 2 && r.end.m == 11 && r.end.w == 1 && r.dst_off == -14400, "no dates: the POSIX default dates and an hour of DST");
    CHECK(tz_rule_parse("EST5EDT4,M3.2.0,M11.1.0", &r) == 0 && r.dst_off == -14400, "an explicit DST offset");
    CHECK(tz_rule_parse("EST5EDT3,M3.2.0,M11.1.0", &r) == 0 && r.dst_off == -10800, "an explicit DST offset that is not an hour");

    TEST("parser: J and n dates, and the last week");
    int64_t tr[2];
    CHECK(tz_rule_parse("AAA0BBB,J60,J61", &r) == 0 && tz_rule_transitions(&r, 2028, tr) == 2 && tr[0] == utc_ms(2028, 3, 1, 2, 0) && tr[1] == utc_ms(2028, 3, 2, 1, 0), "J60 is March 1 even in a leap year");
    CHECK(tz_rule_parse("AAA0BBB,59,60", &r) == 0 && tz_rule_transitions(&r, 2028, tr) == 2 && tr[0] == utc_ms(2028, 2, 29, 2, 0) && tr[1] == utc_ms(2028, 3, 1, 1, 0), "n = 59 is February 29 in a leap year");
    CHECK(tz_rule_transitions(&r, 2027, tr) == 2 && tr[0] == utc_ms(2027, 3, 1, 2, 0), "and March 1 in another");
    CHECK(tz_rule_parse("AAA0BBB,J1,J365", &r) == 0 && tz_rule_transitions(&r, 2028, tr) == 2 && tr[1] == utc_ms(2028, 12, 31, 1, 0), "J365 is December 31");
    CHECK(tz_rule_parse("AAA0BBB,M10.5.0,M3.1.0", &r) == 0 && tz_rule_transitions(&r, 2027, tr) == 2 && tr[0] == utc_ms(2027, 10, 31, 2, 0), "the last Sunday of October 2027 is the 31st (the fifth)");
    CHECK(tz_rule_transitions(&r, 2026, tr) == 2 && tr[0] == utc_ms(2026, 10, 25, 2, 0), "and the 25th in 2026 (the fourth, since there is no fifth)");

    TEST("parser: every rule of lib/tz/tzdb.tsv parses, and the five of this test are the committed ones");
    FILE *f = open_data("lib/tz/tzdb.tsv");
    CHECK(f != NULL, "tzdb.tsv is there");
    if (f) {
        char line[512];
        int zones = 0, headed = 0, five = 0;
        long rule_bytes = 0;
        while (fgets(line, sizeof line, f)) {
            line[strcspn(line, "\n")] = 0;
            if (!zones && !headed) {
                headed = 1;
                CHECK(!strncmp(line, "# tzdata ", 9), "the header line names the tzdata version: %s", line);
                continue;
            }
            if (line[0] == '#') continue;
            char *tab = strchr(line, '\t');
            CHECK(tab != NULL, "a zone line: %s", line);
            if (!tab) continue;
            *tab = 0;
            tz_rule pr;
            CHECK(tz_rule_parse(tab + 1, &pr) == 0, "%s: %s", line, tab + 1);
            CHECK(pr.std_off % 60 == 0 && pr.dst_off % 60 == 0, "%s: whole-minute offsets", line);
            zones++;
            rule_bytes += (long)strlen(tab + 1);
            int z = tz_find(line);
            if (z >= 0) {
                five++;
                CHECK(!strcmp(tz_zone_rule(z), tab + 1), "%s: the test's rule %s is committed as %s", line, tz_zone_rule(z), tab + 1);
            }
        }
        fclose(f);
        CHECK(zones >= 590 && five == 5, "%d zones, %d of the test's five", zones, five);
        printf("tzdb.tsv: %d zones, %ld rule bytes\n", zones, rule_bytes);
    }

    TEST("parser: refusals");
    const char *bad[] = {
        "", "EST", "EST5EDT,M3.2.0", "EST5EDT,M3.2.0,", "EST5EDT,,M11.1.0", "EST5EDT,M3.2.0,M11.1.0,M1.1.1",
        "EST5EDT,M3.2.0/168,M11.1.0", "EST5EDT,M3.2.0,M11.1.0/-168", "EST5EDT,M13.1.0,M11.1.0", "EST5EDT,M,M11.1.0",
        "EST5EDT,M3,M11.1.0", "EST5EDT,M0.1.0,M11.1.0", "EST5EDT,M3.6.0,M11.1.0", "EST5EDT,M3.0.0,M11.1.0", "EST5EDT,M3.2.7,M11.1.0",
        "EST5EDT,J0,J1", "EST5EDT,J366,J1", "EST5EDT,366,J1", "EST5EDT,M3.2.0,M11.1.0x", "EST5EDT,M3.2.0/1:60,M11.1.0",
        "EST5EDT,M3.2.0/1:5,M11.1.0", "EST25,M3.2.0,M11.1.0", "EST5:60", "EST5:00:60", "EST-", "<>5", "<+08", "<+0 8>-8",
        "ES5", "EST5EDT4:", "EST5 EDT", " EST5", "EST5EDT,M3.2.0,M11.1.0\n", "ABCDEFGH5", "<ABCDEFGH>5", "EST+-5", "EST5EDT,M-3.2.0,M11.1.0",
    };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        tz_rule pr;
        memset(&pr, 0x5a, sizeof pr);
        CHECK(tz_rule_parse(bad[i], &pr) == -1 && pr.std_off == 0x5a5a5a5a, "\"%s\" is refused and leaves the output alone", bad[i]);
    }
    CHECK(tz_rule_parse(NULL, &r) == -1, "NULL");
}

int main(void)
{
    oracle();
    gap_and_overlap();
    weekly();
    fixed();
    parser();
    return report("tz");
}
