// stream.c - the binary push stream (stream.h).
#include "stream.h"

#include "rt_deps.h"
#include "net/http.h"
#include "util/str.h"
#include "util/log.h"

#include <stdio.h>
#include <string.h>

// The value is copied in the callback: cb_http_query_each hands it a buffer
// on its own stack, gone once it returns.
typedef struct { char value[16]; bool found; } proto_scan;

static bool proto_fn(const char *key, const char *value, void *ud) {
    if (strcmp(key, "proto") != 0) return true;
    proto_scan *p = ud;
    p->found = true;
    cb_strlcpy(p->value, value, sizeof p->value);
    return false;
}

int cb_nw_proto_of_query(const char *query) {
    proto_scan p = { "", false };
    if (cb_http_query_each(query ? query : "", proto_fn, &p) < 0 || !p.found) return CB_E_INVALID;
    const char *v = p.value;
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

static void send_frame(const NwFrame *f, cb_rt_send_fn send, void *ud) {
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

static void ack(uint16_t ref, int why, bool seated, cb_rt_send_fn send, void *ud) {
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
    for (int k = i; k + 1 < c->n_topics; k++) memcpy(c->topics[k], c->topics[k + 1], sizeof c->topics[k]);
    c->n_topics--;
    memset(c->topics[c->n_topics], 0, sizeof c->topics[0]);
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

// The caller's current view of the game when it is newer than `since`, as
// `kind`: NW_WS_CATCHUP for a client that subscribed with NW_WS_SUB_AS (it
// tells the catch-up from a dashboard push), NW_WS_VIEW for an older one.
static void catch_up(cb_ctx *ctx, const char *gid, const char *viewer, uint32_t since, uint8_t kind,
                     cb_rt_send_fn send, void *ud) {
    cb_view_row v;
    memset(&v, 0, sizeof v);
    int rc = cb_rt_deps_g.table_view(ctx, gid, viewer, &v);
    if (rc == CB_OK && v.version > (int64_t)since && v.view_len) {
        NwFrame f;
        memset(&f, 0, sizeof f);
        f.kind = kind;
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

static void on_sub(cb_ctx *ctx, cb_conn_row *c, const NwFrame *f, cb_rt_send_fn send, void *ud) {
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
            int m = cb_rt_may_hear(ctx, c->user_id, topic);
            if (m < 0) { CB_WARN("stream", "sub %s: %s", topic, cb_strerror(m)); ack(f->ref, ASK_WHY_INTERNAL, false, send, ud); return; }
            seated = m == 1;
            if (!seated) snprintf(topic, sizeof topic, "game-%s", gid);
        }
    }
    int may = cb_rt_may_hear(ctx, c->user_id, topic);
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
    if (f->what == NW_SUB_GAME)
        catch_up(ctx, gid, seated ? c->user_id : NULL, f->version, f->kind == NW_WS_SUB_AS ? NW_WS_CATCHUP : NW_WS_VIEW, send, ud);
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

// The token's user, made the connection's: true, or false (rc in *why) when the
// token names nobody. A different user than the connection had ends every
// subscription made as the old one.
static bool take_user(cb_ctx *ctx, cb_conn_row *conn, const NwFrame *f, int *why) {
    char tok[NW_ACCESS_MAX + 1];
    memcpy(tok, f->token, f->token_len);
    tok[f->token_len] = 0;
    cb_user u;
    memset(&u, 0, sizeof u);
    int rc = f->token_len ? cb_rt_deps_g.verify_token(ctx, tok, &u) : CB_E_AUTH;
    if (rc != CB_OK) {
        if (rc != CB_E_AUTH) CB_WARN("stream", "token check failed: %s", cb_strerror(rc));
        *why = rc;
        return false;
    }
    if (conn->user_id[0] && strcmp(conn->user_id, u.id) != 0)
        while (conn->n_topics) topic_remove(conn, 0);
    cb_strlcpy(conn->user_id, u.id, sizeof conn->user_id);
    return true;
}

// One decoded client frame (never a batch).
static int on_one(cb_ctx *ctx, cb_conn_row *conn, NwFrame *f, cb_rt_send_fn send, void *ud) {
    int why = 0;
    switch (f->kind) {
    case NW_WS_AUTH:
        if (!take_user(ctx, conn, f, &why)) {
            // Not someone: every subscription made as the old user ends.
            conn->user_id[0] = 0;
            while (conn->n_topics) topic_remove(conn, 0);
        }
        return CB_OK;
    case NW_WS_SUB_AS:
        // Who asks rides with the ask: it never depends on an AUTH frame having
        // been stored first, which API Gateway does not promise.
        // A token that does not verify is AUTH; one whose check could not run
        // (no memory, the store) is the server's: INTERNAL, and the client asks again.
        if (!take_user(ctx, conn, f, &why)) { ack(f->ref, why == CB_E_AUTH ? ASK_WHY_AUTH : ASK_WHY_INTERNAL, false, send, ud); return CB_OK; }
        on_sub(ctx, conn, f, send, ud);
        return CB_OK;
    case NW_WS_SUB: on_sub(ctx, conn, f, send, ud); return CB_OK;
    case NW_WS_UNSUB: on_unsub(conn, f); return CB_OK;
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

static bool client_kind(uint8_t k) {
    return k == NW_WS_AUTH || k == NW_WS_SUB || k == NW_WS_UNSUB || k == NW_WS_PING || k == NW_WS_SUB_AS;
}

// A batch's replies, held as batch entries (u32 length, frame) until the batch
// has run.
typedef struct {
    cb_buf entries;
    int    n;
} held_replies;

static void hold(void *ud, const char *frame, size_t len) {
    held_replies *h = ud;
    size_t at = h->entries.len;
    if (len > (size_t)INT32_MAX - 8 || !cb_buf_reserve(&h->entries, at + len + 4)) {
        CB_ERROR("stream", "a batch reply of %zu bytes was not held", len);
        return;
    }
    int n = nw_batch_add((const uint8_t *)frame, (int)len, h->entries.p + at, (int)(len + 4));
    if (n < 0) { CB_ERROR("stream", "a batch reply was not held (%d)", n); return; }
    h->entries.len = at + (size_t)n;
    h->n++;
}

// Sends held replies as NW_WS_BATCH messages of at most NW_BATCH_MAX entries
// and CB_NW_REPLY_BYTES bytes each (a lone reply as itself).
void cb_nw_send_batched(const uint8_t *entries, size_t len, int n, cb_rt_send_fn send, void *ud) {
    NwBlob all = { entries, (uint32_t)len }, one;
    if (n == 1) {
        uint32_t pos = 0;
        if (nw_batch_next(all, &pos, &one) == 1) send(ud, (const char *)one.p, one.len);
        return;
    }
    uint32_t pos = 0, start = 0;
    int count = 0;
    for (;;) {
        uint32_t at = pos;
        int rc = nw_batch_next(all, &pos, &one);
        bool full = count == NW_BATCH_MAX || (count > 0 && pos - start > CB_NW_REPLY_BYTES);
        if (rc != 1 || full) {
            if (count) {
                NwFrame b;
                memset(&b, 0, sizeof b);
                b.kind = NW_WS_BATCH;
                b.blob.p = entries + start;
                b.blob.len = at - start;
                if (count == 1) {
                    uint32_t p0 = 0;
                    NwBlob lone;
                    if (nw_batch_next(b.blob, &p0, &lone) == 1) send(ud, (const char *)lone.p, lone.len);
                } else {
                    send_frame(&b, send, ud);
                }
            }
            if (rc != 1) return;
            start = at;
            count = 0;
        }
        count++;
    }
}

int cb_nw_on_frame(cb_ctx *ctx, cb_conn_row *conn, const uint8_t *msg, size_t len, cb_rt_send_fn send, void *ud) {
    static _Thread_local NwFrame f;
    if (len > (size_t)INT32_MAX || nw_frame_decode(msg, (int)len, &f) != NW_OK) return CB_E_INVALID;
    if (f.kind != NW_WS_BATCH) return on_one(ctx, conn, &f, send, ud);
    // A batch: every entry a client frame, or none of it runs (the decoder has
    // checked the list's shape; the entries are checked here, before anything
    // changes).
    NwBlob all = f.blob, one;
    uint32_t pos = 0;
    while (nw_batch_next(all, &pos, &one) == 1)
        if (nw_frame_decode(one.p, (int)one.len, &f) != NW_OK || !client_kind(f.kind)) return CB_E_INVALID;
    held_replies h;
    cb_buf_init(&h.entries);
    h.n = 0;
    pos = 0;
    while (nw_batch_next(all, &pos, &one) == 1) {
        nw_frame_decode(one.p, (int)one.len, &f);
        on_one(ctx, conn, &f, hold, &h);
    }
    if (h.n) cb_nw_send_batched(h.entries.p, h.entries.len, h.n, send, ud);
    cb_buf_free(&h.entries);
    return CB_OK;
}

bool cb_nw_is_ping(const uint8_t *msg, size_t len) {
    return len == 2 && msg[0] == NW_V1 && msg[1] == NW_WS_PING;
}

// ---- the topic policy ----------------------------------------------------------------
//
// The gu- topic is rebuilt from the member and compared whole, never split on
// '-' (a user id is a hyphenated UUID).
int cb_rt_may_hear(cb_ctx *ctx, const char *user_id, const char *topic) {
    if (!user_id || !*user_id || !topic) return 0;
    if (cb_starts_with(topic, "pv-")) return strcmp(topic + 3, user_id) == 0;
    if (cb_starts_with(topic, "game-")) return topic[5] != 0;
    char game[CB_GAME_ID_MAX + 1];
    if (cb_starts_with(topic, "gu-")) {
        size_t tl = strlen(topic), ul = strlen(user_id);
        // "gu-" + game + "-" + uid, with a non-empty game.
        if (tl < 3 + 1 + 1 + ul || strcmp(topic + tl - ul, user_id) != 0 || topic[tl - ul - 1] != '-') return 0;
        size_t gl = tl - ul - 1 - 3;
        if (!cb_strncpy0(game, sizeof game, topic + 3, gl)) return 0;
    } else if (cb_starts_with(topic, "chat:")) {
        if (!topic[5] || cb_strlcpy(game, topic + 5, sizeof game) >= sizeof game) return 0;
    } else {
        return 0;
    }
    if (!ctx->store) return CB_E_STATE;
    int r = CB_STORE_CALL(ctx->store, is_member, game, user_id);
    return r < 0 ? r : r == 1;
}
