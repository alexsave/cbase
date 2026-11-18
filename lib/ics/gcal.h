// gcal.h - a Google Calendar "template" link for an event (ics.h's ics_event), into the caller's buffer.
#ifndef SHARED_GCAL_H
#define SHARED_GCAL_H

#include "ics.h"

// A percent-encoded copy of the n bytes at s (RFC 3986 unreserved bytes pass through, every other byte,
// including each byte of a UTF-8 sequence, becomes %XX). Returns the length needed, snprintf-style.
int pct_encode(char *out, size_t cap, const char *s, size_t n);

// The link: action=TEMPLATE, text, dates, details, location, and recur for a weekly event. Uses
// summary, description, location, start_ms, end_ms, weekly, byday and zone of ev. A repeating event with
// a zone gets its local times and ctz=<name>, so the recurrence keeps its local time across DST; any
// other is in UTC (dates ending in Z). The length needed, or a negative ICS_E* code.
int gcal_url(char *out, size_t cap, const ics_event *ev);

#endif
