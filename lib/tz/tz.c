// tz.c - time zones from one POSIX TZ rule per zone (tz.h says the format and the semantics).
//
// A rule is parsed on each call (a few dozen bytes of text), and the two transitions of a year are
// computed on demand from the calendar; there is no transition table. To know whether an instant is
// in DST, the transitions of the year before, of, and after the instant's year are laid out and the
// last one at or before the instant says which side it is on, which holds for a southern zone
// (DST spans New Year) and for a /time that pushes a transition out of its own year alike.
#include "tz.h"
#include "../num_parse.h"

// ---- the calendar -------------------------------------------------------------------------------

static int64_t fdiv(int64_t a, int64_t d)
{
    int64_t q = a / d;
    return (a % d != 0 && ((a < 0) != (d < 0))) ? q - 1 : q;
}

// Hinnant's civil-date algorithms in 32 bits (days within +-5.8 million years), which keeps the
// divisions by constants cheap.
int32_t tz_days_from_civil(int64_t y64, int m, int d)
{
    int32_t y = (int32_t)y64 - (m <= 2);
    int32_t era = (y >= 0 ? y : y - 399) / 400;
    uint32_t yoe = (uint32_t)(y - era * 400);
    uint32_t doy = (uint32_t)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int32_t)doe - 719468;
}

void tz_civil_from_days(int64_t days, int64_t *y, int *m, int *d)
{
    int32_t z = (int32_t)days + 719468;
    int32_t era = (z >= 0 ? z : z - 146096) / 146097;
    uint32_t doe = (uint32_t)(z - era * 146097);
    uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    uint32_t mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int64_t)yoe + (int64_t)era * 400 + (*m <= 2);
}

int tz_weekday(int64_t days) { return (int)(((days + 4) % 7 + 7) % 7); }   // 1970-01-01 was a Thursday

static int leap(int64_t y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

static int month_days(int64_t y, int m)
{
    static const unsigned char D[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    return D[m - 1] + (m == 2 && leap(y));
}

// ---- the parser ---------------------------------------------------------------------------------

// min..max decimal digits at *pp, in lo..hi, through num_parse_i64.
static __attribute__((noinline)) int digits(const char **pp, int min, int max, int64_t lo, int64_t hi, int64_t *out)
{
    const char *s = *pp;
    int n = 0;
    while (n < max && s[n] >= '0' && s[n] <= '9') n++;
    if (n < min || !num_parse_i64(s, (size_t)n, lo, hi, out)) return -1;
    *pp = s + n;
    return 0;
}

// [+-]hh[:mm[:ss]], hours 0..hmax, to signed seconds.
static int hms(const char **pp, int hmax, int32_t *out)
{
    const char *s = *pp;
    int neg = 0;
    int64_t h, m = 0, sec = 0;
    if (*s == '+' || *s == '-') neg = *s++ == '-';
    if (digits(&s, 1, 3, 0, hmax, &h)) return -1;
    if (*s == ':') {
        s++;
        if (digits(&s, 2, 2, 0, 59, &m)) return -1;
        if (*s == ':') {
            s++;
            if (digits(&s, 2, 2, 0, 59, &sec)) return -1;
        }
    }
    int64_t v = h * 3600 + m * 60 + sec;
    *out = (int32_t)(neg ? -v : v);
    *pp = s;
    return 0;
}

static int letter(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
static int quoted_char(char c) { return letter(c) || (c >= '0' && c <= '9') || c == '+' || c == '-'; }

// A name: three or more letters, or one or more of letters, digits, '+' and '-' between '<' and '>'.
static int name(const char **pp, char *out)
{
    const char *s = *pp + (**pp == '<');
    int quoted = s != *pp, n = 0;
    while (quoted ? quoted_char(s[n]) : letter(s[n])) n++;
    if (n >= TZ_ABBR_MAX || (quoted ? n < 1 || s[n] != '>' : n < 3)) return -1;
    for (int i = 0; i < n; i++) out[i] = s[i];
    out[n] = 0;
    *pp = s + n + quoted;
    return 0;
}

// Mm.w.d, Jn or n, then an optional /time (default 02:00).
static int date(const char **pp, tz_date *o)
{
    const char *s = *pp;
    int64_t a = 0, b = 0, c = 0;
    o->m = o->w = o->d = o->n = 0;
    if (*s == 'M') {
        s++;
        if (digits(&s, 1, 2, 1, 12, &a) || *s++ != '.' || digits(&s, 1, 1, 1, 5, &b) || *s++ != '.' || digits(&s, 1, 1, 0, 6, &c)) return -1;
        o->kind = 'M';
        o->m = (int)a;
        o->w = (int)b;
        o->d = (int)c;
    } else if (*s == 'J') {
        s++;
        if (digits(&s, 1, 3, 1, 365, &a)) return -1;
        o->kind = 'J';
        o->n = (int)a;
    } else {
        if (digits(&s, 1, 3, 0, 365, &a)) return -1;
        o->kind = 'N';
        o->n = (int)a;
    }
    o->time_s = 2 * 3600;
    if (*s == '/') {
        s++;
        if (hms(&s, 167, &o->time_s)) return -1;
    }
    *pp = s;
    return 0;
}

int tz_rule_parse(const char *rule, tz_rule *out)
{
    tz_rule r = { 0 };
    const char *s = rule;
    int32_t v;
    if (!s || name(&s, r.std) || hms(&s, 24, &v)) return -1;
    r.std_off = -v;
    r.dst_off = r.std_off;
    if (*s) {
        r.has_dst = 1;
        if (name(&s, r.dst)) return -1;
        r.dst_off = r.std_off + 3600;
        if (*s && *s != ',') {
            if (hms(&s, 24, &v)) return -1;
            r.dst_off = -v;
        }
        if (!*s) {   // the POSIX default dates
            r.start = (tz_date){ 'M', 3, 2, 0, 0, 2 * 3600 };
            r.end = (tz_date){ 'M', 11, 1, 0, 0, 2 * 3600 };
        } else if (*s++ != ',' || date(&s, &r.start) || *s++ != ',' || date(&s, &r.end) || *s) {
            return -1;
        }
    }
    *out = r;
    return 0;
}

// ---- transitions --------------------------------------------------------------------------------

// The local day (days since the epoch) a date falls on in `year`.
static int32_t date_day(const tz_date *t, int64_t year)
{
    if (t->kind == 'M') {
        int32_t first = tz_days_from_civil(year, t->m, 1);
        int32_t day = first + (t->d - tz_weekday(first) + 7) % 7 + (t->w - 1) * 7;
        if (day >= first + month_days(year, t->m)) day -= 7;   // week 5 is the last, not the fifth
        return day;
    }
    int32_t jan1 = tz_days_from_civil(year, 1, 1);
    if (t->kind == 'J') return jan1 + t->n - 1 + (leap(year) && t->n >= 60);
    return jan1 + t->n;
}

// The two transitions of `year` in UTC seconds. The start is read in the standard wall clock, the end in the DST one.
static void year_trans(const tz_rule *r, int64_t year, int64_t s[2])
{
    s[0] = (int64_t)date_day(&r->start, year) * 86400 + r->start.time_s - r->std_off;
    s[1] = (int64_t)date_day(&r->end, year) * 86400 + r->end.time_s - r->dst_off;
}

int tz_rule_transitions(const tz_rule *r, int year, int64_t out[2])
{
    if (!r->has_dst) return 0;
    int64_t s[2];
    year_trans(r, year, s);
    out[0] = s[0] * 1000;
    out[1] = s[1] * 1000;
    return 2;
}

// Whether the UTC second t is in DST: the last transition at or before it starts it or ends it.
static int in_dst(const tz_rule *r, int64_t t)
{
    if (!r->has_dst) return 0;
    int64_t y;
    int m, d;
    tz_civil_from_days(fdiv(t + r->std_off, 86400), &y, &m, &d);
    int64_t best = INT64_MIN;
    int dst = 0;
    for (int k = -1; k <= 1; k++) {
        int64_t s[2];
        year_trans(r, y + k, s);
        if (s[0] <= t && s[0] > best) { best = s[0]; dst = 1; }
        if (s[1] <= t && s[1] > best) { best = s[1]; dst = 0; }
    }
    return dst;
}

int tz_rule_utc_ms(const tz_rule *r, int32_t day, int minute, int64_t *utc_ms)
{
    if (minute < 0 || minute > 1439) return TZ_ERR;
    int64_t local = (int64_t)day * 86400 + (int64_t)minute * 60;
    int64_t t = local - r->std_off;
    int rc = TZ_OK;
    if (r->has_dst) {
        int64_t t_dst = local - r->dst_off;
        int std_ok = !in_dst(r, t), dst_ok = in_dst(r, t_dst);
        if (std_ok && dst_ok) {          // twice: the earlier instant
            t = t_dst < t ? t_dst : t;
            rc = TZ_OVERLAP;
        } else if (dst_ok) {
            t = t_dst;
        } else if (!std_ok) {            // never: forward by the gap, read in the offset in force before it
            t = local - (r->dst_off < r->std_off ? r->dst_off : r->std_off);
            rc = TZ_GAP;
        }
    }
    *utc_ms = t * 1000;
    return rc;
}

int tz_rule_local(const tz_rule *r, int64_t utc_ms, int32_t *day, int *minute, char *abbr, int *offset_min)
{
    int64_t t = fdiv(utc_ms, 1000);
    int dst = in_dst(r, t);
    int32_t off = dst ? r->dst_off : r->std_off;
    int64_t local = t + off;
    int64_t days = fdiv(local, 86400);
    if (day) *day = (int32_t)days;
    if (minute) *minute = (int)((local - days * 86400) / 60);
    if (offset_min) *offset_min = off / 60;
    if (abbr) {
        const char *a = dst ? r->dst : r->std;
        int i = 0;
        for (; a[i]; i++) abbr[i] = a[i];
        abbr[i] = 0;
    }
    return 0;
}

// ---- the zones ----------------------------------------------------------------------------------

int tz_find(const char *name)
{
    if (!name) return -1;
    for (int i = 0; i < tz_zone_count; i++) {
        const char *a = tz_zones[i].name, *b = name;
        while (*a && *a == *b) a++, b++;
        if (*a == *b) return i;
    }
    return -1;
}

const char *tz_zone_name(int zone) { return zone >= 0 && zone < tz_zone_count ? tz_zones[zone].name : 0; }
const char *tz_zone_rule(int zone) { return zone >= 0 && zone < tz_zone_count ? tz_zones[zone].rule : 0; }

int tz_utc_ms(int zone, int32_t day, int minute, int64_t *utc_ms)
{
    tz_rule r;
    if (!utc_ms || tz_rule_parse(tz_zone_rule(zone), &r)) return TZ_ERR;
    return tz_rule_utc_ms(&r, day, minute, utc_ms);
}

int tz_local(int zone, int64_t utc_ms, int32_t *day, int *minute, char *abbr, int *offset_min)
{
    tz_rule r;
    if (tz_rule_parse(tz_zone_rule(zone), &r)) return -1;
    return tz_rule_local(&r, utc_ms, day, minute, abbr, offset_min);
}
