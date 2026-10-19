#include "clock.h"

#include "err.h"
#include "../../../../shared/c/clock.h"

#include <errno.h>
#include <stdio.h>
#include <time.h>

int64_t cb_now_ms(void) { return host_real_ms(); }

int64_t cb_mono_ms(void) { return host_mono_ms(); }

void cb_sleep_ms(int64_t ms) { host_sleep_ms(ms); }

// Days since 1970-01-01 for a proleptic Gregorian date (Howard Hinnant's
// days_from_civil), and its inverse. No time zone database, no timegm().
static int64_t days_from_civil(int64_t y, int m, int d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static void civil_from_days(int64_t z, int64_t *y, int *m, int *d) {
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = yoe + era * 400 + (*m <= 2);
}

int cb_iso8601(int64_t ms, char *out, size_t cap) {
    if (cap < 33) return -1;
    int64_t days = ms >= 0 ? ms / 86400000 : -((-ms + 86399999) / 86400000);
    int64_t rem = ms - days * 86400000;
    int64_t y; int mo, d;
    civil_from_days(days, &y, &mo, &d);
    int h = (int)(rem / 3600000), mi = (int)(rem / 60000 % 60), s = (int)(rem / 1000 % 60);
    int frac = (int)(rem % 1000);
    int n = snprintf(out, cap, "%04lld-%02d-%02dT%02d:%02d:%02d.%03d000+00:00",
                     (long long)y, mo, d, h, mi, s, frac);
    return n == 32 ? n : -1;
}

static int digits(const char *s, size_t n, size_t at, int count, int *out) {
    if (at + (size_t)count > n) return -1;
    int v = 0;
    for (int i = 0; i < count; i++) {
        char c = s[at + (size_t)i];
        if (c < '0' || c > '9') return -1;
        v = v * 10 + (c - '0');
    }
    *out = v;
    return 0;
}

int cb_iso8601_parse(const char *s, size_t n, int64_t *ms) {
    int y, mo, d, h, mi, sec;
    if (digits(s, n, 0, 4, &y) || n < 19 || s[4] != '-' || digits(s, n, 5, 2, &mo) || s[7] != '-' ||
        digits(s, n, 8, 2, &d) || (s[10] != 'T' && s[10] != ' ') || digits(s, n, 11, 2, &h) ||
        s[13] != ':' || digits(s, n, 14, 2, &mi) || s[16] != ':' || digits(s, n, 17, 2, &sec))
        return CB_E_INVALID;
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || sec > 60) return CB_E_INVALID;
    size_t at = 19;
    int frac_ms = 0;
    if (at < n && s[at] == '.') {
        at++;
        int k = 0;
        while (at < n && s[at] >= '0' && s[at] <= '9') {
            if (k < 3) frac_ms = frac_ms * 10 + (s[at] - '0');
            k++; at++;
        }
        if (k == 0) return CB_E_INVALID;
        for (; k < 3; k++) frac_ms *= 10;
    }
    int off_min = 0;
    if (at < n) {
        if (s[at] == 'Z') { at++; }
        else if (s[at] == '+' || s[at] == '-') {
            int sign = s[at] == '-' ? -1 : 1, oh, om = 0;
            at++;
            if (digits(s, n, at, 2, &oh)) return CB_E_INVALID;
            at += 2;
            if (at < n && s[at] == ':') at++;
            if (at < n) { if (digits(s, n, at, 2, &om)) return CB_E_INVALID; at += 2; }
            off_min = sign * (oh * 60 + om);
        } else return CB_E_INVALID;
    }
    if (at != n) return CB_E_INVALID;
    int64_t days = days_from_civil(y, mo, d);
    *ms = ((days * 24 + h) * 60 + mi) * 60000LL + sec * 1000LL + frac_ms - off_min * 60000LL;
    return CB_OK;
}
