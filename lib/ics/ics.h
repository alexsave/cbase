// ics.h - an RFC 5545 calendar file of one event, written into the caller's buffer.
//
// Plain C11, no allocation, no libc beyond what tz.h needs (none): ics_write and gcal_url write into
// a buffer the caller owns and return the length they need, snprintf-style, so a caller sizes the
// buffer with a first call (cap 0) or takes a fixed one and checks. The output is cut to fit and
// always NUL-terminated when cap > 0; a result >= cap means it did not fit. A negative result is an
// error (the buffer holds the empty string).
//
// TIME. An event's start and end are UTC instants (ms), and the clock (DTSTAMP) is the caller's.
//   * A one-off event, or a repeating one with no zone, is written in UTC (DTSTART:...Z). A repeating
//     event in UTC shows an hour off in a calendar app after each DST change of the zone its people
//     live in (a weekly 07:00 New York run shows at 06:00 or 08:00), so a repeating event takes a zone.
//   * A repeating event with a zone (IANA name, lib/tz) is written DTSTART;TZID=<name>:<local> and
//     DTEND likewise, with a VTIMEZONE made from the zone's POSIX rule (a STANDARD and a DAYLIGHT part,
//     each with RRULE:FREQ=YEARLY;BYMONTH=m;BYDAY=nSU read from Mm.w.d, w = 5 as -1), so the event keeps
//     its local time across DST. byday then names the days of the LOCAL week.
//   * The VTIMEZONE reads the rule straight, so it needs an Mm.w.d date whose time is within 0..24h;
//     a zone whose rule has other dates (tzdata has four rules with a /-1, /24, /26 time) answers
//     ICS_EZONE. Past transitions are not represented (tz.h).
//
// NO APP IS NAMED HERE: the UID's domain comes from the caller (uid_host), and PRODID names this library.
#ifndef SHARED_ICS_H
#define SHARED_ICS_H

#include <stddef.h>
#include <stdint.h>

// A string with an explicit length. p may be NULL when n is 0.
typedef struct {
    const char *p;
    size_t n;
} ics_str;

// Weekday bits for ics_event.byday, Monday first (BYDAY order MO..SU).
enum { ICS_MO = 1 << 0, ICS_TU = 1 << 1, ICS_WE = 1 << 2, ICS_TH = 1 << 3, ICS_FR = 1 << 4, ICS_SA = 1 << 5, ICS_SU = 1 << 6 };

typedef struct {
    ics_str id;            // event id
    ics_str occ_day;       // occurrence day, "yyyy-mm-dd"
    ics_str uid_host;      // the UID's domain (UID:<id>-<occ_day>@<uid_host>), required
    int64_t now_ms;        // DTSTAMP, UTC ms since the epoch; the caller's clock
    int64_t start_ms;      // DTSTART, UTC ms
    int64_t end_ms;        // DTEND, UTC ms, must be >= start_ms
    ics_str summary;
    ics_str location;
    ics_str description;
    ics_str url;           // optional, empty to omit
    int has_geo;           // nonzero to write GEO
    int32_t lat_udeg;      // micro-degrees
    int32_t lng_udeg;
    int weekly;            // nonzero to write RRULE:FREQ=WEEKLY
    unsigned byday;        // ICS_* mask; 0 writes no BYDAY
    ics_str zone;          // IANA name (tz_find) of the zone a repeating event keeps its local time in; empty for UTC
} ics_event;

enum {
    ICS_EINVAL = -1,   // end before start, a year outside 0..9999, an empty uid_host
    ICS_EZONE = -2     // an unknown zone, or one whose rule the VTIMEZONE cannot say (see above)
};

// "yyyymmddThhmmss" for a local day (days since 1970-01-01) and second of the day, and a NUL: 16 bytes. 0, or ICS_EINVAL.
#define ICS_LOCAL_LEN 16
int ics_fmt_local(char out[ICS_LOCAL_LEN], int32_t day, int sec);
// "yyyymmddThhmmssZ" for a UTC ms timestamp (floored to the second) and a NUL: 17 bytes. 0, or ICS_EINVAL.
#define ICS_UTC_LEN 17
int ics_fmt_utc(char out[ICS_UTC_LEN], int64_t ms);

// The whole VCALENDAR: the length needed (without the NUL), or a negative ICS_E* code.
int ics_write(char *out, size_t cap, const ics_event *ev);

#endif
