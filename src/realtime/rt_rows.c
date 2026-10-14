// rt_rows.c - (rt_rows.h).
#include "rt_rows.h"

#include "util/clock.h"

#include <string.h>

int cb_rt_ts(int64_t ms, char *out, size_t cap) {
    char iso[40];
    if (cap < 40 || cb_iso8601(ms, iso, sizeof iso) < 0) return -1;
    // iso is "YYYY-MM-DDTHH:MM:SS.ffffff+00:00": keep the fraction's
    // significant digits only, as Postgres prints it.
    size_t n = 19;
    memcpy(out, iso, n);
    int last = -1;
    for (int k = 0; k < 6; k++)
        if (iso[20 + k] != '0') last = k;
    if (last >= 0) {
        out[n++] = '.';
        memcpy(out + n, iso + 20, (size_t)last + 1);
        n += (size_t)last + 1;
    }
    memcpy(out + n, "+00:00", 6);
    n += 6;
    out[n] = 0;
    return (int)n;
}

int cb_rt_commit_ts(int64_t ms, char *out, size_t cap) {
    char iso[40];
    if (cap < 25 || cb_iso8601(ms, iso, sizeof iso) < 0) return -1;
    memcpy(out, iso, 23);   // "YYYY-MM-DDTHH:MM:SS.mmm"
    out[23] = 'Z';
    out[24] = 0;
    return 24;
}

void cb_rt_write_chat_row(cb_jw *w, const cb_chat_row *r, bool sorted) {
    char ts[40];
    cb_rt_ts(r->created_at, ts, sizeof ts);
    cb_jw_obj(w);
    if (sorted) {
        cb_jw_kstr(w, "created_at", ts);
        cb_jw_kstr(w, "game_id", r->game_id);
        cb_jw_kstr(w, "id", r->id);
        cb_jw_kbool(w, "is_system", r->is_system);
        cb_jw_kstr(w, "message", r->message ? r->message : "");
        cb_jw_kstr(w, "user_id", r->user_id);
    } else {
        cb_jw_kstr(w, "id", r->id);
        cb_jw_kstr(w, "game_id", r->game_id);
        cb_jw_kstr(w, "user_id", r->user_id);
        cb_jw_kstr(w, "message", r->message ? r->message : "");
        cb_jw_kbool(w, "is_system", r->is_system);
        cb_jw_kstr(w, "created_at", ts);
    }
    cb_jw_end(w);
}
