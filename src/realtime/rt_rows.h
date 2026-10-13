// rt_rows.h - the row and payload text realtime.c and postgrest.c share.
//
// Postgres prints a timestamp in JSON as ISO-8601 with a 'T', the fraction cut
// of its trailing zeros (none at all on a whole second), and "+00:00" only for
// a TIMESTAMPTZ column. The tables carry both kinds (seed.sql): player_views
// and spectator_views.updated_at are TIMESTAMPTZ, every other time column is a
// plain TIMESTAMP, and PostgREST and Realtime print each as Postgres does.
#ifndef CB_RT_ROWS_H
#define CB_RT_ROWS_H

#include "app/table_ops.h"
#include "store/store.h"
#include "util/json.h"
#include "util/str.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// "2026-10-13T12:34:56.789" (tz false) or "2026-10-13T12:34:56.789+00:00".
// The length, or -1 when cap < 40.
int cb_rt_ts(int64_t ms, bool tz, char *out, size_t cap);
// Realtime's commit_timestamp: "2026-10-13T12:34:56.789Z". The length, or -1.
int cb_rt_commit_ts(int64_t ms, char *out, size_t cap);

// A chat_messages row as an object: in table column order (PostgREST) or in
// sorted key order (Realtime's re-encoded record).
void cb_rt_write_chat_row(cb_jw *w, const cb_chat_row *r, bool sorted);

// The three push payloads (realtime.c), exposed so the tests compare them
// with recorded frames. Each appends to `out`.
void cb_rt_pv_payload(cb_buf *out, int event, const cb_view_row *row);
void cb_rt_cast_payload(cb_buf *out, const uint8_t *cast, size_t len);
void cb_rt_chat_payload(cb_buf *out, const cb_chat_row *row);

#endif
