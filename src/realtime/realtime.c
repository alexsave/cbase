// realtime.c - the publish calls (realtime.h): each builds its Phoenix frame
// once per serializer and hands both to the live transport.
// The payload shapes (Supabase Realtime's own) are written in json_surface.c.
#include "realtime.h"

#include "json_surface.h"
#include "phoenix.h"
#include "rt_deps.h"
#include "aws/apigw_mgmt.h"
#include "util/clock.h"
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

void rt_publish_cast(cb_rt *rt, const char *topic, const uint8_t *cast, size_t len) {
    if (!rt || !topic) return;
    cb_buf p;
    cb_buf_init(&p);
    cb_rt_cast_payload(&p, cast, len);
    if (!p.oom) publish(rt, topic, "broadcast", cb_buf_cstr(&p));
    cb_buf_free(&p);
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
