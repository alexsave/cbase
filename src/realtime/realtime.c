// realtime.c - the publish calls (realtime.h): each builds its stream message
// (netwire.h) once and hands it to the live transport.
#include "realtime.h"

#include "rt_deps.h"
#include "aws/iot_data.h"
#include "util/log.h"

#include "util/str.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- the live dependencies (rt_deps.h) ----------------------------------------------

// The token, and the account it names: a deleted account's token is still
// signed and unexpired for up to its TTL, and must not open a subscription
// (cb_rt_forget_user ends the ones it had).
static int live_verify(cb_ctx *ctx, const char *jwt, cb_user *out) {
    if (!ctx->auth) return CB_E_STATE;
    int rc = cb_auth_verify_token(ctx->auth, jwt, out);
    if (rc != CB_OK || !ctx->store) return rc;
    cb_user_row row;
    rc = CB_STORE_CALL(ctx->store, user_get, out->id, &row);
    return rc == CB_E_NOT_FOUND ? CB_E_AUTH : rc;
}

cb_rt_deps cb_rt_deps_g = {
    .verify_token = live_verify,
    .iot_publish = cb_iot_publish,
};

// ---- publishing ------------------------------------------------------------------------

static _Thread_local int t_failed;

int cb_rt_failures_take(void) {
    int n = t_failed;
    t_failed = 0;
    return n;
}

// ---- topics ------------------------------------------------------------------------------

// An id a topic level can carry: 1 to 64 bytes of A-Z a-z 0-9 '-' '_'.
static bool level_ok(const char *id) {
    if (!id || !*id) return false;
    size_t n = 0;
    for (const char *p = id; *p; p++, n++) {
        char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_'))
            return false;
    }
    return n <= 64;
}

static int printed(int n, size_t cap) { return n > 0 && (size_t)n < cap ? n : -1; }

int cb_rt_topic(const cb_rt_to *to, char *out, size_t cap) {
    if (!to || !out || !cap) return -1;
    out[0] = 0;
    switch (to->family) {
    case CB_RT_VIEWS:
        if (!level_ok(to->user_id)) return -1;
        return printed(snprintf(out, cap, "u/%s/v", to->user_id), cap);
    case CB_RT_SEAT:
        if (!level_ok(to->user_id) || !level_ok(to->game_id)) return -1;
        return printed(snprintf(out, cap, "u/%s/c/%s", to->user_id, to->game_id), cap);
    case CB_RT_WATCH:
        if (!level_ok(to->game_id)) return -1;
        return printed(snprintf(out, cap, "s/%s", to->game_id), cap);
    case CB_RT_CHAT:
        if (!level_ok(to->user_id) || !level_ok(to->game_id)) return -1;
        return printed(snprintf(out, cap, "u/%s/m/%s", to->user_id, to->game_id), cap);
    }
    return -1;
}

// ---- publishing ----------------------------------------------------------------------------

static void deliver(cb_rt *rt, const cb_rt_to *to, const uint8_t *frame, size_t len) {
    if (rt->t && rt->t->deliver) t_failed += rt->t->deliver(rt, to, frame, len);
}

// The frame, encoded once, to each of `to[0..n)`.
static void publish(cb_rt *rt, const cb_rt_to *to, int n, const NwFrame *f) {
    if (!rt || !rt->t) return;
    size_t cap = 96 + (size_t)f->blob.len + (f->kind == NW_WS_CHAT ? NW_CHAT_MAX + 2 * NW_ID_MAX : 0);
    cb_buf b;
    cb_buf_init(&b);
    int len = cb_buf_reserve(&b, cap) ? nw_frame_encode(f, b.p, (int)cap) : -1;
    if (len < 0) {
        CB_ERROR("realtime", "a push of kind %d for game %.*s was not written (%d)", f->kind, (int)f->gid_len, f->gid, len);
        t_failed += n;
    } else {
        for (int i = 0; i < n; i++) deliver(rt, &to[i], b.p, (size_t)len);
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

void cb_rt_forget_user(cb_rt *rt, const char *user_id) {
    if (rt && user_id && *user_id && rt->t && rt->t->forget) rt->t->forget(rt, user_id);
}

void cb_rt_free(cb_rt *rt) {
    if (rt && rt->t && rt->t->free) rt->t->free(rt);
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
