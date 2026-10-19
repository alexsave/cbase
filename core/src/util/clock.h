// clock.h - the two clocks, and the timestamp text Postgres writes.
//
// Every kernel call takes now_ms from the host (the kernel never reads a
// clock), and that number is cb_now_ms: wall-clock milliseconds since the Unix
// epoch, the same unit the TypeScript server passed (Date.now()). Durations and
// timers use cb_mono_ms, which never jumps when the wall clock is set.
//
// Stored timestamps are INTEGER milliseconds since the epoch; the facade turns
// them into the text PostgREST and GoTrue answer with.
#ifndef CB_CLOCK_H
#define CB_CLOCK_H

#include <stddef.h>
#include <stdint.h>

int64_t cb_now_ms(void);    // CLOCK_REALTIME, ms since 1970-01-01T00:00:00Z
int64_t cb_mono_ms(void);   // CLOCK_MONOTONIC, ms since an arbitrary start
int64_t cb_mono_us(void);   // CLOCK_MONOTONIC, microseconds
void    cb_sleep_ms(int64_t ms);

// "2026-10-13T12:34:56.789000+00:00": UTC, microsecond precision, the way
// PostgREST prints a timestamptz. Returns the characters written (32), or -1
// when cap < 33.
int cb_iso8601(int64_t ms, char *out, size_t cap);

// Reads the timestamp texts c_base meets: ISO-8601 with 'T' or ' ' between
// date and time, an optional fraction of any length, and 'Z', "+HH", "+HHMM"
// or "+HH:MM" (or none, read as UTC). This covers psql's CSV export
// ("2026-01-05 10:11:12.345678+00"). 0 and *ms set, or CB_E_INVALID.
int cb_iso8601_parse(const char *s, size_t n, int64_t *ms);

#endif
