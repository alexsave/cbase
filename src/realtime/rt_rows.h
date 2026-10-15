// rt_rows.h - the time text realtime.c, postgrest.c and json_surface.c share.
//
// Every timestamp is printed as Postgres prints a TIMESTAMPTZ in UTC: ISO-8601
// with a 'T', the fraction cut of its trailing zeros (none at all on a whole
// second), and "+00:00". Production (seed.sql) has plain TIMESTAMP columns too
// (chat_messages, game_snapshots, the ratings), which PostgREST prints with no
// offset, and a browser's new Date() reads such a string as LOCAL time: the
// chat clock was off by the viewer's UTC offset. Every value here is UTC, so
// every value carries the offset, whatever production's column type was.
#ifndef CB_RT_ROWS_H
#define CB_RT_ROWS_H

#include "app/table_ops.h"
#include "store/store.h"
#include "util/str.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// "2026-10-13T12:34:56.789+00:00". The length, or -1 when cap < 40.
int cb_rt_ts(int64_t ms, char *out, size_t cap);
// Realtime's commit_timestamp: "2026-10-13T12:34:56.789Z". The length, or -1.
int cb_rt_commit_ts(int64_t ms, char *out, size_t cap);

#endif
