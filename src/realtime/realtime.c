// realtime.c - the publish calls (realtime.h): each builds its stream message
// (netwire.h) once and hands it to the live transport.
#include "realtime.h"

#include "rt_deps.h"
#include "aws/apigw_mgmt.h"
#include "util/log.h"

#include "stream.h"

#include <stdio.h>
#include <string.h>

// ---- the live dependencies (rt_deps.h) ----------------------------------------------

static int live_verify(cb_ctx *ctx, const char *jwt, cb_user *out) {
    if (!ctx->auth) return CB_E_STATE;
    return cb_auth_verify_token(ctx->auth, jwt, out);
}

cb_rt_deps cb_rt_deps_g = {
    .verify_token = live_verify,
    .table_view = cb_table_view,
    .apigw_post = cb_apigw_post,
    .apigw_delete = cb_apigw_delete,
};

// ---- publishing ------------------------------------------------------------------------

static void publish(cb_rt *rt, const char *topic, const NwFrame *f) {
    if (!rt || !rt->t || !rt->t->deliver) return;
    size_t cap = 96 + (size_t)f->blob.len + (f->kind == NW_WS_CHAT ? NW_CHAT_MAX + 2 * NW_ID_MAX : 0);
    cb_buf b;
    cb_buf_init(&b);
    int n = cb_buf_reserve(&b, cap) ? nw_frame_encode(f, b.p, (int)cap) : -1;
    if (n < 0) CB_ERROR("realtime", "a push to %s was not written (%d)", topic, n);
    else rt->t->deliver(rt, topic, b.p, (size_t)n);
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
    char topic[CB_TOPIC_MAX];
    snprintf(topic, sizeof topic, "pv-%s", user_id);
    static _Thread_local NwFrame f;
    memset(&f, 0, sizeof f);
    f.kind = NW_WS_VIEW;
    frame_gid(&f, row->game_id);
    if (!gone) {
        f.version = (uint32_t)row->version;
        f.blob.p = row->view;
        f.blob.len = (uint32_t)row->view_len;
    }
    publish(rt, topic, &f);
}

void rt_publish_cast(cb_rt *rt, const char *topic, const char *game_id, int64_t version, const uint8_t *cast, size_t len) {
    if (!rt || !topic || !game_id) return;
    static _Thread_local NwFrame f;
    memset(&f, 0, sizeof f);
    f.kind = NW_WS_CAST;
    frame_gid(&f, game_id);
    f.version = (uint32_t)version;
    f.blob.p = cast;
    f.blob.len = (uint32_t)len;
    publish(rt, topic, &f);
}

void rt_publish_chat(cb_rt *rt, const char *game_id, const cb_chat_row *row) {
    if (!rt || !game_id || !row) return;
    char topic[CB_TOPIC_MAX];
    snprintf(topic, sizeof topic, "chat:%s", game_id);
    static _Thread_local NwFrame f;
    memset(&f, 0, sizeof f);
    f.kind = NW_WS_CHAT;
    frame_gid(&f, game_id);
    cb_chat_to_nw(row, &f.chat);
    publish(rt, topic, &f);
}

void cb_rt_free(cb_rt *rt) {
    if (rt && rt->t && rt->t->free) rt->t->free(rt);
}
