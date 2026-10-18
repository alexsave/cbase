// pushes.c - foolish's topic families and stream messages (pushes.h): each
// publish builds its netwire.h frame once and hands it to the stream.
#include "pushes.h"

#include "util/log.h"
#include "util/str.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const cb_topic_family cb_fam_views = { "views", "u/{user}/v" };
const cb_topic_family cb_fam_seat = { "seat", "u/{user}/c/{doc}" };
const cb_topic_family cb_fam_watch = { "watch", "s/{doc}" };
const cb_topic_family cb_fam_chat = { "chat", "u/{user}/m/{doc}" };
const cb_topic_family *const cb_foolish_topics[4] = { &cb_fam_views, &cb_fam_seat, &cb_fam_watch, &cb_fam_chat };

// The frame, encoded once, to each of `to[0..n)`.
static void publish(cb_rt *rt, const cb_rt_to *to, int n, const NwFrame *f) {
    if (!rt || !rt->t) return;
    size_t cap = 96 + (size_t)f->blob.len + (f->kind == NW_WS_CHAT ? NW_CHAT_MAX + 2 * NW_ID_MAX : 0);
    cb_buf b;
    cb_buf_init(&b);
    int len = cb_buf_reserve(&b, cap) ? nw_frame_encode(f, b.p, (int)cap) : -1;
    if (len < 0) {
        CB_ERROR("realtime", "a push of kind %d for game %.*s was not written (%d)", f->kind, (int)f->gid_len, f->gid, len);
        cb_rt_failures_add(n);
    } else {
        cb_rt_publish(rt, to, n, b.p, (size_t)len);
    }
    cb_buf_free(&b);
}

static void frame_gid(NwFrame *f, const char *game_id) {
    size_t n = strlen(game_id);
    if (n > NW_GID_MAX) n = 0;   // never: game ids are a few bytes
    f->gid_len = (uint8_t)n;
    memcpy(f->gid, game_id, n);
}

void rt_publish_view(cb_rt *rt, const char *user_id, const cb_view_row *row, bool gone) {
    if (!rt || !row || !user_id) return;
    static _Thread_local NwFrame f;
    memset(&f, 0, sizeof f);
    f.kind = NW_WS_VIEW;
    frame_gid(&f, row->game_id);
    if (!gone) {
        f.version = (uint32_t)row->version;
        f.blob.p = row->view;
        f.blob.len = (uint32_t)row->view_len;
    }
    cb_rt_to to = { CB_RT_VIEWS, user_id, NULL };
    publish(rt, &to, 1, &f);
}

void rt_publish_cast(cb_rt *rt, const char *user_id, const char *game_id, int64_t version, const uint8_t *cast, size_t len) {
    if (!rt || !game_id) return;
    static _Thread_local NwFrame f;
    memset(&f, 0, sizeof f);
    f.kind = NW_WS_CAST;
    frame_gid(&f, game_id);
    f.version = (uint32_t)version;
    f.blob.p = cast;
    f.blob.len = (uint32_t)len;
    cb_rt_to to = { user_id ? CB_RT_SEAT : CB_RT_WATCH, user_id, game_id };
    publish(rt, &to, 1, &f);
}

void rt_publish_chat(cb_rt *rt, const char *game_id, const cb_chat_row *row, const cb_uuid *members, int n) {
    if (!rt || !game_id || !row) return;
    static _Thread_local NwFrame f;
    memset(&f, 0, sizeof f);
    f.kind = NW_WS_CHAT;
    frame_gid(&f, game_id);
    cb_chat_to_nw(row, &f.chat);
    // Each member's own topic.
    cb_rt_to to[CB_MAX_SEATS];
    int k = 0;
    for (int i = 0; i < n && i < CB_MAX_SEATS; i++)
        if (members[i][0]) to[k++] = (cb_rt_to){ CB_RT_CHAT, members[i], game_id };
    publish(rt, to, k, &f);
}

void cb_chat_to_nw(const cb_chat_row *r, NwChat *out) {
    memset(out, 0, sizeof *out);
    size_t n = strlen(r->id);
    out->id_len = (uint8_t)(n <= NW_ID_MAX ? n : 0);
    memcpy(out->id, r->id, out->id_len);
    n = strlen(r->user_id);
    out->user_id_len = (uint8_t)(n <= NW_ID_MAX ? n : 0);
    memcpy(out->user_id, r->user_id, out->user_id_len);
    out->created_ms = r->created_at > 0 ? (uint64_t)r->created_at : 0;
    out->is_system = r->is_system ? 1 : 0;
    n = r->message ? strlen(r->message) : 0;
    if (n > NW_CHAT_MAX) n = NW_CHAT_MAX;   // the write path refuses longer; an imported row is cut
    out->message_len = (uint16_t)n;
    if (n) memcpy(out->message, r->message, n);
}
