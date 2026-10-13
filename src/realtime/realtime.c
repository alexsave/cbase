// realtime.c - the publish calls (realtime.h): each builds its Phoenix frame
// once per serializer through phoenix.h and hands both to the live transport.
//
// The payload shapes are Supabase Realtime's own, keys in the order its
// encoder (Elixir's Jason over small maps) writes them, which is sorted:
//   postgres_changes  {"data":{"columns":[..],"commit_timestamp":"..Z",
//                      "errors":null,["old_record":{pk}],["record":{row}],
//                      "schema":"public","table":"player_views","type":"UPDATE"},
//                      "ids":[..]}
//                     INSERT has no old_record, DELETE no record, and an
//                     old_record is the primary key only (player_views has the
//                     default replica identity). Every object's keys are
//                     sorted, the record's too.
//   broadcast         {"event":"animation_events","payload":{"b":".."},"type":"broadcast"}
//   chat              {"event":"INSERT","payload":{"old_record":null,"operation":"INSERT",
//                      "record":{row},"schema":"public","table":"chat_messages"},
//                      "type":"broadcast"}  (realtime.broadcast_changes)
#include "realtime.h"

#include "phoenix.h"
#include "rt_deps.h"
#include "rt_rows.h"
#include "aws/apigw_mgmt.h"
#include "util/b64.h"
#include "util/clock.h"
#include "util/json.h"
#include "util/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- the live dependencies (rt_deps.h) ----------------------------------------------

static int live_verify(cb_ctx *ctx, const char *jwt, cb_user *out) {
    if (!ctx->auth) return CB_E_STATE;
    return cb_auth_verify_token(ctx->auth, jwt, out);
}

cb_rt_deps cb_rt_deps_g = {
    .verify_token = live_verify,
    .table_view = cb_table_view,
    .table_views_of_user = cb_table_views_of_user,
    .apigw_post = cb_apigw_post,
    .apigw_delete = cb_apigw_delete,
};

// ---- publishing ------------------------------------------------------------------------

static void publish(cb_rt *rt, const char *topic, const char *event, const char *payload) {
    if (!rt || !rt->t || !rt->t->deliver) return;
    char wire[CB_TOPIC_MAX + 16];
    snprintf(wire, sizeof wire, "realtime:%s", topic);
    cb_buf f1, f2;
    cb_buf_init(&f1);
    cb_buf_init(&f2);
    cb_phx_push(&f1, 1, wire, event, payload);
    cb_phx_push(&f2, 2, wire, event, payload);
    if (f1.oom || f2.oom) CB_ERROR("realtime", "out of memory building a push to %s", topic);
    else {
        const char *frames[2] = { cb_buf_cstr(&f1), cb_buf_cstr(&f2) };
        const size_t lens[2] = { f1.len, f2.len };
        rt->t->deliver(rt, topic, frames, lens);
    }
    cb_buf_free(&f1);
    cb_buf_free(&f2);
}

// The columns player_views reports (realtime's "columns", in table order).
static const char *const PV_COLUMNS =
    "[{\"name\":\"game_id\",\"type\":\"text\"},{\"name\":\"player_id\",\"type\":\"uuid\"},"
    "{\"name\":\"view\",\"type\":\"bytea\"},{\"name\":\"version\",\"type\":\"int8\"},"
    "{\"name\":\"status\",\"type\":\"text\"},{\"name\":\"updated_at\",\"type\":\"timestamptz\"}]";

static void pv_pk(cb_jw *w, const cb_view_row *row) {
    cb_jw_obj(w);
    cb_jw_kstr(w, "game_id", row->game_id);
    cb_jw_kstr(w, "player_id", row->player_id);
    cb_jw_end(w);
}

void cb_rt_pv_payload(cb_buf *out, int event, const cb_view_row *row) {
    static const char *const TYPES[4] = { "", "INSERT", "UPDATE", "DELETE" };
    const char *type = TYPES[event];
    char commit[40], ts[40];
    cb_rt_commit_ts(row->updated_at, commit, sizeof commit);
    cb_rt_ts(row->updated_at, true, ts, sizeof ts);
    char filter[64 + CB_UUID_LEN];
    snprintf(filter, sizeof filter, "player_id=eq.%s", row->player_id);

    cb_jw w;
    cb_jw_init(&w, out);
    cb_jw_obj(&w);
    cb_jw_key(&w, "data");
    cb_jw_obj(&w);
    cb_jw_key(&w, "columns"); cb_jw_raw(&w, PV_COLUMNS, strlen(PV_COLUMNS));
    cb_jw_kstr(&w, "commit_timestamp", commit);
    cb_jw_knull(&w, "errors");
    if (event != CB_RT_INSERT) { cb_jw_key(&w, "old_record"); pv_pk(&w, row); }
    if (event != CB_RT_DELETE) {
        cb_jw_key(&w, "record");
        cb_jw_obj(&w);
        cb_jw_kstr(&w, "game_id", row->game_id);
        cb_jw_kstr(&w, "player_id", row->player_id);
        cb_jw_kstr(&w, "status", row->status);
        cb_jw_kstr(&w, "updated_at", ts);
        cb_jw_kint(&w, "version", row->version);
        cb_jw_key(&w, "view"); cb_jw_bytea(&w, row->view, row->view_len);
        cb_jw_end(&w);
    }
    cb_jw_kstr(&w, "schema", "public");
    cb_jw_kstr(&w, "table", "player_views");
    cb_jw_kstr(&w, "type", type);
    cb_jw_end(&w);
    // Every binding this change satisfies: its own event or "*", with the
    // player's filter or none (the RLS scopes an unfiltered binding to the same rows).
    cb_jw_key(&w, "ids");
    cb_jw_arr(&w);
    const char *events[2] = { type, "*" };
    const char *filters[2] = { filter, "" };
    for (int e = 0; e < 2; e++)
        for (int f = 0; f < 2; f++) cb_jw_int(&w, cb_phx_binding_id(events[e], "public", "player_views", filters[f]));
    cb_jw_end(&w);
    cb_jw_end(&w);
}

void rt_publish_pv_update(cb_rt *rt, const char *user_id, int event, const cb_view_row *row) {
    if (!rt || !row || event < CB_RT_INSERT || event > CB_RT_DELETE) return;
    char topic[CB_TOPIC_MAX];
    snprintf(topic, sizeof topic, "pv-%s", user_id);
    cb_buf p;
    cb_buf_init(&p);
    cb_rt_pv_payload(&p, event, row);
    if (!p.oom) publish(rt, topic, "postgres_changes", cb_buf_cstr(&p));
    cb_buf_free(&p);
}

void cb_rt_cast_payload(cb_buf *out, const uint8_t *cast, size_t len) {
    size_t n = cb_b64_len(len) + 1;
    char *b = malloc(n);
    if (!b) { out->oom = true; return; }
    cb_b64_encode(cast, len, b, n);
    cb_buf_puts(out, "{\"event\":\"animation_events\",\"payload\":{\"b\":\"");
    cb_buf_puts(out, b);   // base64 needs no JSON escaping
    cb_buf_puts(out, "\"},\"type\":\"broadcast\"}");
    free(b);
}

void rt_publish_cast(cb_rt *rt, const char *topic, const uint8_t *cast, size_t len) {
    if (!rt || !topic) return;
    cb_buf p;
    cb_buf_init(&p);
    cb_rt_cast_payload(&p, cast, len);
    if (!p.oom) publish(rt, topic, "broadcast", cb_buf_cstr(&p));
    cb_buf_free(&p);
}

void cb_rt_chat_payload(cb_buf *out, const cb_chat_row *row) {
    cb_jw w;
    cb_jw_init(&w, out);
    cb_jw_obj(&w);
    cb_jw_kstr(&w, "event", "INSERT");
    cb_jw_key(&w, "payload");
    cb_jw_obj(&w);
    cb_jw_knull(&w, "old_record");
    cb_jw_kstr(&w, "operation", "INSERT");
    cb_jw_key(&w, "record");
    cb_rt_write_chat_row(&w, row, true);
    cb_jw_kstr(&w, "schema", "public");
    cb_jw_kstr(&w, "table", "chat_messages");
    cb_jw_end(&w);
    cb_jw_kstr(&w, "type", "broadcast");
    cb_jw_end(&w);
}

void rt_publish_chat(cb_rt *rt, const char *game_id, const cb_chat_row *row) {
    if (!rt || !game_id || !row) return;
    char topic[CB_TOPIC_MAX];
    snprintf(topic, sizeof topic, "chat:%s", game_id);
    cb_buf p;
    cb_buf_init(&p);
    cb_rt_chat_payload(&p, row);
    if (!p.oom) publish(rt, topic, "broadcast", cb_buf_cstr(&p));
    cb_buf_free(&p);
}

void cb_rt_free(cb_rt *rt) {
    if (rt && rt->t && rt->t->free) rt->t->free(rt);
}
