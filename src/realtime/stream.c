// stream.c - the binary push stream (stream.h).
#include "stream.h"

#include "rt_deps.h"
#include "util/log.h"

#include <stdio.h>
#include <string.h>

typedef struct { const char *key; bool found; } proto_scan;

static bool proto_fn(const char *key, const char *value, void *ud) {
    if (strcmp(key, "proto") != 0) return true;
    proto_scan *p = ud;
    p->found = true;
    p->key = value;
    return false;
}

int cb_nw_proto_of_query(const char *query) {
    char v[16] = "";
    proto_scan p = { NULL, false };
    if (cb_http_query_each(query ? query : "", proto_fn, &p) < 0) return CB_E_INVALID;
    if (!p.found) return 0;
    cb_strlcpy(v, p.key ? p.key : "", sizeof v);
    return strcmp(v, "nw1") == 0 ? CB_VSN_NW : CB_E_INVALID;
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

static void send_frame(const NwFrame *f, cb_phx_send_fn send, void *ud) {
    uint8_t small[64];
    int n = nw_frame_encode(f, small, sizeof small);
    if (n == NW_E_CAP) {
        // A view: as long as its envelope.
        size_t cap = 64 + (size_t)f->blob.len;
        cb_buf b;
        cb_buf_init(&b);
        if (cb_buf_reserve(&b, cap) && (n = nw_frame_encode(f, b.p, (int)cap)) > 0) send(ud, (const char *)b.p, (size_t)n);
        else CB_ERROR("stream", "a frame of kind %d was not written (%d)", f->kind, n);
        cb_buf_free(&b);
        return;
    }
    if (n > 0) send(ud, (const char *)small, (size_t)n);
    else CB_ERROR("stream", "a frame of kind %d was not written (%d)", f->kind, n);
}

static void ack(uint16_t ref, int why, bool seated, cb_phx_send_fn send, void *ud) {
    NwFrame f;
    memset(&f, 0, sizeof f);
    f.kind = NW_WS_ACK;
    f.ref = ref;
    f.why = (uint8_t)why;
    f.seated = seated ? 1 : 0;
    send_frame(&f, send, ud);
}

static int topic_index(const cb_conn_row *c, const char *topic) {
    for (int i = 0; i < c->n_topics; i++)
        if (strcmp(c->topics[i], topic) == 0) return i;
    return -1;
}

static void topic_remove(cb_conn_row *c, int i) {
    for (int k = i; k + 1 < c->n_topics; k++) {
        memcpy(c->topics[k], c->topics[k + 1], sizeof c->topics[k]);
        memcpy(c->join_refs[k], c->join_refs[k + 1], sizeof c->join_refs[k]);
    }
    c->n_topics--;
    memset(c->topics[c->n_topics], 0, sizeof c->topics[0]);
    memset(c->join_refs[c->n_topics], 0, sizeof c->join_refs[0]);
}

static void topic_drop(cb_conn_row *c, const char *topic) {
    int i = topic_index(c, topic);
    if (i >= 0) topic_remove(c, i);
}

static bool topic_add(cb_conn_row *c, const char *topic) {
    if (topic_index(c, topic) >= 0) return true;
    if (c->n_topics >= CB_CONN_TOPICS) return false;
    cb_strlcpy(c->topics[c->n_topics++], topic, sizeof c->topics[0]);
    return true;
}

// The game's id as a string, refused when it is not one a topic can carry.
static bool gid_of(const NwFrame *f, char *gid, size_t cap) {
    if (f->gid_len == 0 || f->gid_len >= cap || f->gid_len > CB_GAME_ID_MAX) return false;
    for (int i = 0; i < f->gid_len; i++) {
        char ch = f->gid[i];
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_')) return false;
    }
    memcpy(gid, f->gid, f->gid_len);
    gid[f->gid_len] = 0;
    return true;
}

// The caller's current view of the game when it is newer than `since`.
static void catch_up(cb_ctx *ctx, const char *gid, const char *viewer, uint32_t since, cb_phx_send_fn send, void *ud) {
    cb_view_row v;
    memset(&v, 0, sizeof v);
    int rc = cb_rt_deps_g.table_view(ctx, gid, viewer, &v);
    if (rc == CB_OK && v.version > (int64_t)since && v.view_len) {
        NwFrame f;
        memset(&f, 0, sizeof f);
        f.kind = NW_WS_VIEW;
        f.gid_len = (uint8_t)strlen(gid);
        memcpy(f.gid, gid, f.gid_len);
        f.version = (uint32_t)v.version;
        f.blob.p = v.view;
        f.blob.len = (uint32_t)v.view_len;
        send_frame(&f, send, ud);
    } else if (rc != CB_OK && rc != CB_E_NOT_FOUND) {
        CB_WARN("stream", "catch-up of %s: %s", gid, cb_strerror(rc));
    }
    cb_view_row_free(&v);
}

static void on_sub(cb_ctx *ctx, cb_conn_row *c, const NwFrame *f, cb_phx_send_fn send, void *ud) {
    if (!c->user_id[0]) { ack(f->ref, ASK_WHY_AUTH, false, send, ud); return; }
    char gid[CB_GAME_ID_MAX + 1] = "";
    char topic[CB_TOPIC_MAX];
    bool seated = false;
    if (f->what == NW_SUB_GAMES) {
        if (f->gid_len) { ack(f->ref, ASK_WHY_MALFORMED, false, send, ud); return; }
        snprintf(topic, sizeof topic, "pv-%s", c->user_id);
    } else {
        if (!gid_of(f, gid, sizeof gid)) { ack(f->ref, ASK_WHY_MALFORMED, false, send, ud); return; }
        if (f->what == NW_SUB_CHAT) {
            snprintf(topic, sizeof topic, "chat:%s", gid);
        } else {
            // A member hears their own seat; anyone else the spectators' stream.
            snprintf(topic, sizeof topic, "gu-%s-%s", gid, c->user_id);
            int m = cb_phx_may_join(ctx, c->user_id, topic);
            if (m < 0) { CB_WARN("stream", "sub %s: %s", topic, cb_strerror(m)); ack(f->ref, ASK_WHY_INTERNAL, false, send, ud); return; }
            seated = m == 1;
            if (!seated) snprintf(topic, sizeof topic, "game-%s", gid);
        }
    }
    int may = cb_phx_may_join(ctx, c->user_id, topic);
    if (may < 0) { CB_WARN("stream", "sub %s: %s", topic, cb_strerror(may)); ack(f->ref, ASK_WHY_INTERNAL, false, send, ud); return; }
    if (may == 0) { ack(f->ref, ASK_WHY_FORBIDDEN, false, send, ud); return; }
    if (f->what == NW_SUB_GAME) {
        // One stream per game: a seat taken or given up switches it.
        char other[CB_TOPIC_MAX];
        if (seated) snprintf(other, sizeof other, "game-%s", gid);
        else snprintf(other, sizeof other, "gu-%s-%s", gid, c->user_id);
        topic_drop(c, other);
    }
    if (!topic_add(c, topic)) { ack(f->ref, ASK_WHY_INTERNAL, false, send, ud); return; }
    ack(f->ref, 0, seated, send, ud);
    if (f->what == NW_SUB_GAME) catch_up(ctx, gid, seated ? c->user_id : NULL, f->version, send, ud);
}

static void on_unsub(cb_conn_row *c, const NwFrame *f) {
    char gid[CB_GAME_ID_MAX + 1];
    char topic[CB_TOPIC_MAX];
    if (f->what == NW_SUB_GAMES) {
        snprintf(topic, sizeof topic, "pv-%s", c->user_id);
        topic_drop(c, topic);
        return;
    }
    if (!gid_of(f, gid, sizeof gid)) return;
    if (f->what == NW_SUB_CHAT) {
        snprintf(topic, sizeof topic, "chat:%s", gid);
        topic_drop(c, topic);
        return;
    }
    snprintf(topic, sizeof topic, "gu-%s-%s", gid, c->user_id);
    topic_drop(c, topic);
    snprintf(topic, sizeof topic, "game-%s", gid);
    topic_drop(c, topic);
}

int cb_nw_on_frame(cb_ctx *ctx, cb_conn_row *conn, const uint8_t *msg, size_t len, cb_phx_send_fn send, void *ud) {
    static _Thread_local NwFrame f;
    if (len > (size_t)INT32_MAX || nw_frame_decode(msg, (int)len, &f) != NW_OK) return CB_E_INVALID;
    switch (f.kind) {
    case NW_WS_AUTH: {
        char tok[NW_ACCESS_MAX + 1];
        memcpy(tok, f.token, f.token_len);
        tok[f.token_len] = 0;
        cb_user u;
        memset(&u, 0, sizeof u);
        int rc = f.token_len ? cb_rt_deps_g.verify_token(ctx, tok, &u) : CB_E_AUTH;
        if (rc != CB_OK) {
            // Not someone: every subscription made as the old user ends.
            if (rc != CB_E_AUTH) CB_WARN("stream", "token check failed: %s", cb_strerror(rc));
            conn->user_id[0] = 0;
            while (conn->n_topics) topic_remove(conn, 0);
            return CB_OK;
        }
        if (conn->user_id[0] && strcmp(conn->user_id, u.id) != 0)
            while (conn->n_topics) topic_remove(conn, 0);
        cb_strlcpy(conn->user_id, u.id, sizeof conn->user_id);
        return CB_OK;
    }
    case NW_WS_SUB: on_sub(ctx, conn, &f, send, ud); return CB_OK;
    case NW_WS_UNSUB: on_unsub(conn, &f); return CB_OK;
    case NW_WS_PING: {
        NwFrame p;
        memset(&p, 0, sizeof p);
        p.kind = NW_WS_PONG;
        send_frame(&p, send, ud);
        return CB_OK;
    }
    default:
        return CB_E_INVALID;   // a server frame sent by a client
    }
}
