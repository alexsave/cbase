// tz.h - time zones from one POSIX TZ rule per zone, transitions computed on demand.
//
// A zone is its IANA name and one rule string: the footer of the zone's TZif file (RFC 8536
// section 3.3), POSIX TZ with RFC 8536's two extensions:
//
//     std offset [dst [offset] [,start[/time],end[/time]]]
//
//   * names are plain (CET) or quoted (<+08>);
//   * offsets are [+-]hh[:mm[:ss]] with POSIX's inverted sign (EST5 is UTC-5);
//   * dates are Mm.w.d (day d of week w of month m, w = 5 the last), Jn (1..365, February 29 never
//     counted) or n (0..365);
//   * times are [+-]hh[:mm[:ss]] with hours -167..167 (tzdata writes /25 and /-1), default 02:00;
//   * a rule with no DST part is a fixed offset. A DST part with no dates takes the POSIX default
//     dates (M3.2.0, M11.1.0); lib/tz/tzdb.tsv never holds that form.
//
// Every number is read by num_parse_i64. Freestanding: no allocation, no libc, no clock and no
// libc time function (core/test/unit_time_gate.c holds that); the time comes from the caller.
//
// WHERE THE ZONES COME FROM. An app names its zones in its Makefile (APP_TZ_ZONES); cbase.mk writes
// build/gen/tz_zones.c from lib/tz/tzdb.tsv with exactly those, in that order, and the app links
// only those. The IANA name is a zone's key: tz_find(name) is how an app reaches a zone, and an app
// stores the name, never the index. The index is a handle for one process (a position in
// APP_TZ_ZONES), valid until the next build.
//
// LOCAL TIME. A local time is a day (days since 1970-01-01) and a minute of that day (0..1439).
//   * In a spring-forward gap (a local time that never happens) the time moves forward by the gap:
//     02:30 on the day New York springs forward is 03:30 EDT, 07:30 UTC.
//   * In a fall-back overlap (a local time that happens twice) the first one is taken, the earlier
//     UTC instant: 01:30 on the day New York falls back is 01:30 EDT, 05:30 UTC.
//
// THE STATED LIMITS.
//   * Four zones of tzdata 2026f are not in lib/tz/tzdb.tsv (tools/tz_skip.txt): Asia/Gaza and Asia/Hebron
//     (DST suspended for Ramadan in explicit transitions to 2086) and America/Havana and Cuba (no DST end
//     in 2026), whose files hold transitions their footer rule cannot say. An app that names one is
//     stopped at build time. Supporting them (keeping their explicit future transitions) is a later decision.
//   * Past transitions are not represented. A time before a zone adopted its current
// rule is computed with today's rule, which can be wrong (New York before 2007, for example). That
// is fine for upcoming events and wrong for history.
#ifndef SHARED_TZ_H
#define SHARED_TZ_H

#include <stdint.h>

#define TZ_ABBR_MAX 8   // an abbreviation and its NUL

// A zone: what build/gen/tz_zones.c holds.
typedef struct {
    const char *name;   // the IANA name
    const char *rule;   // the POSIX rule
} tz_zone;

// One end of a DST period.
typedef struct {
    char kind;          // 'M' (m, w, d), 'J' (n: 1..365) or 'N' (n: 0..365)
    int  m, w, d, n;
    int32_t time_s;     // seconds after local midnight, wall clock in force before it; may be negative or past a day
} tz_date;

// A parsed rule. Offsets are seconds east of UTC.
typedef struct {
    char    std[TZ_ABBR_MAX], dst[TZ_ABBR_MAX];
    int32_t std_off, dst_off;
    int     has_dst;
    tz_date start, end;   // the DST period begins at start and ends at end
} tz_rule;

// tz_utc_ms answers.
enum { TZ_OK = 0, TZ_GAP = 1, TZ_OVERLAP = 2, TZ_ERR = -1 };

// The zones this build links (build/gen/tz_zones.c, or a test's own).
extern const tz_zone tz_zones[];
extern const int     tz_zone_count;

// The handle of an IANA name, or -1.
int tz_find(const char *name);
// A zone's IANA name and rule string, or NULL for a handle that is not one.
const char *tz_zone_name(int zone);
const char *tz_zone_rule(int zone);

// A local day and minute in a zone to a UTC instant in ms. TZ_OK for a time that happens once,
// TZ_GAP when it fell in a spring-forward gap and was moved forward, TZ_OVERLAP when it happens
// twice and the first was taken; TZ_ERR (nothing written) for a bad zone or a minute outside 0..1439.
int tz_utc_ms(int zone, int32_t day, int minute, int64_t *utc_ms);
// A UTC instant (ms) in a zone to its local day, local minute, abbreviation (TZ_ABBR_MAX bytes,
// NUL-terminated) and offset east of UTC in whole minutes. Any output may be NULL. 0, or -1 for a bad zone.
// (The design gave abbr as const char **: a rule is parsed per call and has no string to point into.)
int tz_local(int zone, int64_t utc_ms, int32_t *day, int *minute, char *abbr, int *offset_min);

// The same two over a parsed rule, and the rule's parts, for the tests and for a caller with a rule in hand.
int tz_rule_parse(const char *rule, tz_rule *out);   // 0, or -1 (out untouched) for text that is not a rule
// The two transitions of `year` in UTC ms: out[0] the start of DST, out[1] its end (in a southern zone
// out[0] is the later one). Returns 2, or 0 for a fixed-offset rule (out untouched).
int tz_rule_transitions(const tz_rule *r, int year, int64_t out[2]);
int tz_rule_utc_ms(const tz_rule *r, int32_t day, int minute, int64_t *utc_ms);
int tz_rule_local(const tz_rule *r, int64_t utc_ms, int32_t *day, int *minute, char *abbr, int *offset_min);

// The proleptic Gregorian calendar (Howard Hinnant's algorithms), days since 1970-01-01.
int32_t tz_days_from_civil(int64_t y, int m, int d);
void    tz_civil_from_days(int64_t days, int64_t *y, int *m, int *d);
// 0 = Sunday .. 6 = Saturday.
int     tz_weekday(int64_t days);

#endif
