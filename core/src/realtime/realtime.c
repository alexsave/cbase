// realtime.c - the push stream's publish side (realtime.h): a target's topic
// from its family's pattern, the batch, and delivery through the transport.
#include "realtime.h"

#include "rt_deps.h"
#include "stream_mqtt.h"
#include "store/store.h"
#include "util/err.h"
#include "util/log.h"
#include "util/str.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- the live dependencies (rt_deps.h) ----------------------------------------------

// The token, its session and its account (auth.c cb_auth_verify_token reads
// all three): a deleted account's or a signed-out session's token is signed
// and unexpired for up to its TTL, and must not open a connection.
static int live_verify(cb_ctx *ctx, const char *jwt, cb_user *out) {
    if (!ctx->auth) return CB_E_STATE;
    return cb_auth_verify_token(ctx->auth, jwt, out);
}

cb_rt_deps cb_rt_deps_g = {
    .verify_token = live_verify,
};

// ---- publishing ------------------------------------------------------------------------

static _Thread_local int t_failed;

void cb_rt_failures_add(int n) { t_failed += n; }

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


int cb_rt_topic(const cb_rt_to *to, char *out, size_t cap) {
    if (!to || !out || !cap) return -1;
    out[0] = 0;
    if (!to->family || !to->family->pattern) return -1;
    size_t n = 0;
    for (const char *p = to->family->pattern; *p;) {
        const char *put;
        size_t k;
        if (strncmp(p, "{user}", 6) == 0 || strncmp(p, "{doc}", 5) == 0) {
            bool user = p[1] == 'u';
            put = user ? to->user_id : to->doc_id;
            if (!level_ok(put)) { out[0] = 0; return -1; }
            k = strlen(put);
            p += user ? 6 : 5;
        } else {
            put = p++;
            k = 1;
        }
        if (n + k >= cap) { out[0] = 0; return -1; }
        memcpy(out + n, put, k);
        n += k;
    }
    out[n] = 0;
    return n ? (int)n : -1;
}

int cb_rt_families_check(const cb_topic_family *const *f, int n, char *err, size_t cap) {
    static const char USER[] = CB_MQTT_USER_PREFIX "{user}/";
    for (int i = 0; i < n; i++) {
        const char *p = f[i] && f[i]->pattern ? f[i]->pattern : "";
        bool ok = strncmp(p, USER, sizeof USER - 1) == 0 || strncmp(p, CB_MQTT_WATCH_PREFIX, 2) == 0;
        for (const char *q = p; ok && *q; q++) {
            if (*q == '+' || *q == '#' || *q == '}') ok = false;
            else if (*q == '{') {
                ok = strncmp(q, "{user}", 6) == 0 || strncmp(q, "{doc}", 5) == 0;
                q += q[1] == 'u' ? 5 : 4;
            }
        }
        if (!ok) {
            snprintf(err, cap, "topic family %s: \"%s\" is not under \"%s\" or \"%s\", or names more than {user} and {doc}",
                     f[i] && f[i]->name ? f[i]->name : "?", p, USER, CB_MQTT_WATCH_PREFIX);
            return CB_E_INVALID;
        }
    }
    return CB_OK;
}

// ---- publishing ----------------------------------------------------------------------------

// ---- the batch (realtime.h cb_rt_batch_begin) ------------------------------------------

#define RT_BATCH_MAX 32   // a commit of 8 humans: 8 views, 8 casts, the spectators, up to 8 seats left
#define RT_ID_MAX    64   // level_ok's longest id: a longer one is never a topic

typedef struct {
    const cb_topic_family *family;
    bool         has_user, has_doc;
    char         user[RT_ID_MAX + 1], doc[RT_ID_MAX + 1];
    uint8_t     *frame;
    size_t       len;
} rt_held;

static _Thread_local struct {
    cb_rt  *rt;
    int     depth, n;
    rt_held items[RT_BATCH_MAX];
} t_batch;

static bool batching(cb_rt *rt) { return rt->t && rt->t->deliver_lanes && t_batch.depth > 0 && t_batch.rt == rt; }

static bool copy_id(char *out, const char *id, bool *has) {
    *has = id != NULL;
    if (!id) return true;
    size_t n = strlen(id);
    if (n > RT_ID_MAX) return false;
    memcpy(out, id, n + 1);
    return true;
}

// Holds a delivery for the batch's end: false when it cannot (the caller delivers at once).
static bool hold(const cb_rt_to *to, const uint8_t *frame, size_t len) {
    if (t_batch.n >= RT_BATCH_MAX) return false;
    rt_held *h = &t_batch.items[t_batch.n];
    if (!copy_id(h->user, to->user_id, &h->has_user) || !copy_id(h->doc, to->doc_id, &h->has_doc)) return false;
    h->frame = malloc(len ? len : 1);
    if (!h->frame) return false;
    if (len) memcpy(h->frame, frame, len);
    h->len = len;
    h->family = to->family;
    t_batch.n++;
    return true;
}

void cb_rt_batch_begin(cb_rt *rt) {
    if (!rt || !rt->t || !rt->t->deliver_lanes) return;
    if (t_batch.depth > 0 && t_batch.rt != rt) return;   // another transport's batch is open: this one is not held
    t_batch.rt = rt;
    t_batch.depth++;
}

void cb_rt_batch_end(cb_rt *rt) {
    if (!batching(rt) || --t_batch.depth > 0) return;
    int n = t_batch.n;
    // Zeroed: gcc cannot see that only the first n are read.
    cb_rt_to to[RT_BATCH_MAX] = { { 0 } };
    const uint8_t *frames[RT_BATCH_MAX] = { 0 };
    size_t lens[RT_BATCH_MAX] = { 0 };
    int lane[RT_BATCH_MAX] = { 0 }, n_lanes = 0;
    const char *lane_user[RT_BATCH_MAX] = { 0 };   // NULL: the spectators' lane
    for (int i = 0; i < n; i++) {
        rt_held *h = &t_batch.items[i];
        to[i] = (cb_rt_to){ h->family, h->has_user ? h->user : NULL, h->has_doc ? h->doc : NULL };
        frames[i] = h->frame;
        lens[i] = h->len;
        int l = 0;
        while (l < n_lanes && !(lane_user[l] == NULL ? to[i].user_id == NULL
                                                     : to[i].user_id && strcmp(lane_user[l], to[i].user_id) == 0))
            l++;
        if (l == n_lanes) lane_user[n_lanes++] = to[i].user_id;
        lane[i] = l;
    }
    if (n) t_failed += rt->t->deliver_lanes(rt, to, frames, lens, lane, n);
    for (int i = 0; i < n; i++) free(t_batch.items[i].frame);
    t_batch.n = 0;
    t_batch.rt = NULL;
}

static void deliver(cb_rt *rt, const cb_rt_to *to, const uint8_t *frame, size_t len) {
    if (batching(rt) && hold(to, frame, len)) return;
    if (rt->t && rt->t->deliver) t_failed += rt->t->deliver(rt, to, frame, len);
}

void cb_rt_publish(cb_rt *rt, const cb_rt_to *to, int n, const uint8_t *frame, size_t len) {
    if (!rt || !rt->t) return;
    if (n > 1) cb_rt_batch_begin(rt);   // the targets hear it at once
    for (int i = 0; i < n; i++) deliver(rt, &to[i], frame, len);
    if (n > 1) cb_rt_batch_end(rt);
}

void cb_rt_end_sessions(cb_rt *rt, const char *user_id, const char *session_id, const char *keep_session) {
    if (rt && user_id && *user_id && rt->t && rt->t->forget) rt->t->forget(rt, user_id, session_id, keep_session);
}

void cb_rt_forget_user(cb_rt *rt, const char *user_id) { cb_rt_end_sessions(rt, user_id, NULL, NULL); }

void cb_rt_free(cb_rt *rt) {
    if (rt && rt->t && rt->t->free) rt->t->free(rt);
}
