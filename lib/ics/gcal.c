// gcal.c - a Google Calendar "template" link (gcal.h).
#include "gcal.h"
#include "../tz/tz.h"

typedef struct {
    char *p;
    size_t cap, len;
} W;

static void put(W *w, char c)
{
    if (w->p && w->len + 1 < w->cap) w->p[w->len] = c;
    w->len++;
}
static void puts_(W *w, const char *s)
{
    while (*s) put(w, *s++);
}
static void finish(W *w)
{
    if (w->p && w->cap) w->p[w->len < w->cap ? w->len : w->cap - 1] = 0;
}

static void pct(W *w, const char *s, size_t n)
{
    static const char hex[] = "0123456789ABCDEF";
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~') {
            put(w, (char)c);
        } else {
            put(w, '%');
            put(w, hex[c >> 4]);
            put(w, hex[c & 15]);
        }
    }
}

static void pct_z(W *w, const char *s)
{
    size_t n = 0;
    while (s[n]) n++;
    pct(w, s, n);
}

int pct_encode(char *out, size_t cap, const char *s, size_t n)
{
    W w = { out, cap, 0 };
    pct(&w, s ? s : "", s ? n : 0);
    finish(&w);
    return (int)w.len;
}

static void field(W *w, const char *name, ics_str v)
{
    puts_(w, name);
    pct(w, v.p ? v.p : "", v.p ? v.n : 0);
}

static int local_of(const tz_rule *r, int64_t ms, int32_t *day, int *sec)
{
    int minute;
    tz_rule_local(r, ms, day, &minute, NULL, NULL);
    int64_t s = ms / 1000 % 60;   // offsets are whole minutes, so the seconds are UTC's
    *sec = minute * 60 + (int)(s < 0 ? s + 60 : s);
    return 0;
}

int gcal_url(char *out, size_t cap, const ics_event *ev)
{
    W w = { out, cap, 0 };
    char zone[64], stamp[ICS_UTC_LEN];   // ICS_UTC_LEN holds a local stamp too
    tz_rule rule;
    int local = 0;
    int rc = 0;
    if (ev->end_ms < ev->start_ms) rc = ICS_EINVAL;
    if (!rc && ev->weekly && ev->zone.n) {
        if (ev->zone.n >= sizeof zone) rc = ICS_EZONE;
        else {
            for (size_t i = 0; i < ev->zone.n; i++) zone[i] = ev->zone.p[i];
            zone[ev->zone.n] = 0;
            int z = tz_find(zone);
            if (z < 0 || tz_rule_parse(tz_zone_rule(z), &rule)) rc = ICS_EZONE;
            local = 1;
        }
    }
    if (rc) {
        if (out && cap) out[0] = 0;
        return rc;
    }
    puts_(&w, "https://calendar.google.com/calendar/render?action=TEMPLATE");
    field(&w, "&text=", ev->summary);
    puts_(&w, "&dates=");
    for (int e = 0; e < 2; e++) {
        int64_t ms = e ? ev->end_ms : ev->start_ms;
        if (local) {
            int32_t day;
            int sec;
            local_of(&rule, ms, &day, &sec);
            if (ics_fmt_local(stamp, day, sec)) {
                if (out && cap) out[0] = 0;
                return ICS_EINVAL;
            }
            puts_(&w, stamp);
        } else {
            if (ics_fmt_utc(stamp, ms)) {
                if (out && cap) out[0] = 0;
                return ICS_EINVAL;
            }
            puts_(&w, stamp);
        }
        if (!e) put(&w, '/');
    }
    if (ev->description.n) field(&w, "&details=", ev->description);
    if (ev->location.n) field(&w, "&location=", ev->location);
    if (local) {
        puts_(&w, "&ctz=");
        pct(&w, ev->zone.p, ev->zone.n);
    }
    if (ev->weekly) {
        static const char *const DAYS[7] = { "MO", "TU", "WE", "TH", "FR", "SA", "SU" };
        puts_(&w, "&recur=");
        pct_z(&w, "RRULE:FREQ=WEEKLY");
        if (ev->byday & 0x7F) {
            pct_z(&w, ";BYDAY=");
            int first = 1;
            for (int i = 0; i < 7; i++) {
                if (!(ev->byday & (1u << i))) continue;
                if (!first) pct_z(&w, ",");
                pct_z(&w, DAYS[i]);
                first = 0;
            }
        }
    }
    finish(&w);
    return (int)w.len;
}
