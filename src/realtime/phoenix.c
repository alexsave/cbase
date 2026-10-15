// phoenix.c - the Phoenix channel protocol (phoenix.h).
//
// Ground truth for every shape here: @supabase/realtime-js 2.x
// (RealtimeChannel.js: the join payload, _updatePostgresBindings, the
// postgres_changes filter by `ids`), its vendored phoenix (Channel.isMember:
// a push with a join_ref other than the channel's is dropped, so pushes carry
// null), supabase-swift's RealtimeV2 (RealtimeMessageV2, PostgresJoinConfig; 2.55.4
// speaks vsn 2.0.0, earlier releases the v1 object keys), and foolish/e2e/helpers/ws_phoenix.ts.
#include "phoenix.h"

#include "json_surface.h"
#include "realtime.h"
#include "rt_deps.h"
#include "util/log.h"

#include <stdio.h>
#include <string.h>

#define WIRE_PREFIX "realtime:"

// ---- helpers -----------------------------------------------------------------------

int32_t cb_phx_binding_id(const char *event, const char *schema, const char *table, const char *filter) {
    const char *parts[4] = { event, schema, table, filter };
    uint32_t h = 2166136261u;
    for (int i = 0; i < 4; i++) {
        for (const char *s = parts[i] ? parts[i] : ""; *s; s++) { h ^= (uint8_t)*s; h *= 16777619u; }
        h ^= 0xff;   // a separator no string contains, so ("ab","") differs from ("a","b")
        h *= 16777619u;
    }
    h &= 0x7fffffffu;
    return h ? (int32_t)h : 1;
}

typedef struct { int vsn; bool seen; } vsn_scan;

static bool vsn_fn(const char *key, const char *value, void *ud) {
    vsn_scan *v = ud;
    if (strcmp(key, "vsn") != 0) return true;
    v->seen = true;
    v->vsn = strcmp(value, "2.0.0") == 0 ? 2 : strcmp(value, "1.0.0") == 0 ? 1 : CB_E_INVALID;
    return false;
}

int cb_phx_vsn_of_query(const char *query) {
    vsn_scan v = { 1, false };
    if (cb_http_query_each(query ? query : "", vsn_fn, &v) < 0) return CB_E_INVALID;
    return v.vsn;
}

static const char *bare_of(const char *wire) {
    return cb_starts_with(wire, WIRE_PREFIX) ? wire + strlen(WIRE_PREFIX) : NULL;
}

static int topic_index(const cb_phx_conn *c, const char *bare) {
    for (int i = 0; i < c->n_topics; i++)
        if (strcmp(c->topics[i], bare) == 0) return i;
    return -1;
}

static void topic_remove(cb_phx_conn *c, int i) {
    for (int k = i; k + 1 < c->n_topics; k++) {
        memcpy(c->topics[k], c->topics[k + 1], sizeof c->topics[k]);
        memcpy(c->join_refs[k], c->join_refs[k + 1], sizeof c->join_refs[k]);
    }
    c->n_topics--;
    memset(c->topics[c->n_topics], 0, sizeof c->topics[0]);
    memset(c->join_refs[c->n_topics], 0, sizeof c->join_refs[0]);
}

// phx_close for the channel that joined topic i on `jr`, as Phoenix sends it
// when a channel ends (ref = join_ref).
static void send_close_ref(const cb_phx_conn *c, int i, cb_span jr, cb_phx_send_fn send, void *ud) {
    char wire[CB_TOPIC_MAX + sizeof WIRE_PREFIX];
    snprintf(wire, sizeof wire, WIRE_PREFIX "%s", c->topics[i]);
    cb_buf f;
    cb_buf_init(&f);
    cb_phx_close(&f, c->vsn, jr, wire);
    send(ud, cb_buf_cstr(&f), f.len);
    cb_buf_free(&f);
}

// phx_close for the channel on topic i now.
static void send_close(const cb_phx_conn *c, int i, cb_phx_send_fn send, void *ud) {
    send_close_ref(c, i, cb_span_cstr(c->join_refs[i]), send, ud);
}

static void reply(const cb_phx_conn *c, const cb_phx_msg *m, bool ok, const char *response, cb_phx_send_fn send, void *ud) {
    cb_buf f;
    cb_buf_init(&f);
    cb_phx_reply(&f, c->vsn, m, ok, response);
    send(ud, cb_buf_cstr(&f), f.len);
    cb_buf_free(&f);
}

static void refuse(const cb_phx_conn *c, const cb_phx_msg *m, const char *reason, cb_phx_send_fn send, void *ud) {
    cb_buf r;
    cb_buf_init(&r);
    cb_phx_refusal(&r, reason);
    reply(c, m, false, cb_buf_cstr(&r), send, ud);
    cb_buf_free(&r);
}

// The user a token names. 1 a user (in *u), 0 nobody (no token, or the anon
// key), CB_E_AUTH a token that does not verify, another CB_E_* on failure.
static int user_of_token(cb_ctx *ctx, const char *payload, size_t n, cb_user *u) {
    char tok[4096];
    int has = cb_phx_access_token(payload, n, tok, sizeof tok);
    if (has == 0) return 0;
    if (has < 0) return CB_E_AUTH;
    if (!tok[0]) return 0;
    if (ctx->cfg && ctx->cfg->anon_key[0] && strcmp(tok, ctx->cfg->anon_key) == 0) return 0;
    int rc = cb_rt_deps_g.verify_token(ctx, tok, u);
    return rc == CB_OK ? 1 : rc;
}

static void handle_join(cb_ctx *ctx, cb_phx_conn *c, const cb_phx_msg *m, cb_phx_send_fn send, void *ud) {
    const char *bare = bare_of(m->topic);
    if (!bare || !*bare) { refuse(c, m, "topic must be realtime:<topic>", send, ud); return; }
    if (strlen(bare) >= CB_TOPIC_MAX) { refuse(c, m, "topic too long", send, ud); return; }
    if (m->join_ref.n >= sizeof c->join_refs[0]) { refuse(c, m, "join_ref too long", send, ud); return; }
    cb_user u;
    memset(&u, 0, sizeof u);
    int who = user_of_token(ctx, m->payload.p, m->payload.n, &u);
    if (who == CB_E_AUTH) { refuse(c, m, "Invalid token", send, ud); return; }
    if (who < 0) { CB_WARN("realtime", "join %s: token check failed: %s", bare, cb_strerror(who)); refuse(c, m, "Internal error", send, ud); return; }
    int may = cb_phx_may_join(ctx, who == 1 ? u.id : NULL, bare);
    if (may < 0) { CB_WARN("realtime", "join %s: policy check failed: %s", bare, cb_strerror(may)); refuse(c, m, "Internal error", send, ud); return; }
    if (may == 0) {
        cb_buf why;   // grown to fit: the whole topic is always in the reason
        cb_buf_init(&why);
        cb_buf_printf(&why, "Unauthorized: You do not have permissions to read from this Channel topic: %s", bare);
        refuse(c, m, cb_buf_cstr(&why), send, ud);
        cb_buf_free(&why);
        return;
    }
    cb_buf resp;
    cb_buf_init(&resp);
    int rc = cb_phx_join_response(m->payload.p, m->payload.n, &resp);
    if (rc != CB_OK) {
        refuse(c, m, rc == CB_E_FULL ? "too many postgres_changes bindings" : "malformed postgres_changes binding", send, ud);
        cb_buf_free(&resp);
        return;
    }
    // One user per connection: a join as someone else ends the old user's channels.
    if (c->user_id[0] && strcmp(c->user_id, u.id) != 0) {
        while (c->n_topics > 0) { send_close(c, 0, send, ud); topic_remove(c, 0); }
    }
    int i = topic_index(c, bare);
    if (i < 0) {
        if (c->n_topics >= CB_CONN_TOPICS) { refuse(c, m, "too many channels on one connection", send, ud); cb_buf_free(&resp); return; }
        i = c->n_topics++;
        cb_strlcpy(c->topics[i], bare, sizeof c->topics[i]);
    }
    cb_strncpy0(c->join_refs[i], sizeof c->join_refs[i], m->join_ref.p ? m->join_ref.p : "", m->join_ref.n);
    cb_strlcpy(c->user_id, u.id, sizeof c->user_id);
    reply(c, m, true, cb_buf_cstr(&resp), send, ud);
    cb_buf_free(&resp);
}

static void handle_access_token(cb_ctx *ctx, cb_phx_conn *c, const cb_phx_msg *m, cb_phx_send_fn send, void *ud) {
    const char *bare = bare_of(m->topic);
    int i = bare ? topic_index(c, bare) : -1;
    if (i < 0) return;
    cb_user u;
    memset(&u, 0, sizeof u);
    int who = user_of_token(ctx, m->payload.p, m->payload.n, &u);
    char tok[4096];
    if (who == 0 && cb_phx_access_token(m->payload.p, m->payload.n, tok, sizeof tok) == 0) return;   // nothing presented
    if (who == 1 && strcmp(u.id, c->user_id) == 0) return;   // the same user, a fresh token
    // A bad token, the anon key, or someone else: this channel ends.
    send_close(c, i, send, ud);
    topic_remove(c, i);
}

int cb_phx_on_frame(cb_ctx *ctx, cb_phx_conn *conn, const char *text, size_t len, cb_phx_send_fn send, void *ud) {
    cb_phx_msg m;
    if (cb_phx_decode(text, len, conn->vsn, &m) != CB_OK) return CB_E_INVALID;
    if (strcmp(m.topic, "phoenix") == 0) {
        if (strcmp(m.event, "heartbeat") == 0) reply(conn, &m, true, NULL, send, ud);
        return CB_OK;
    }
    if (strcmp(m.event, "phx_join") == 0) {
        handle_join(ctx, conn, &m, send, ud);
    } else if (strcmp(m.event, "phx_leave") == 0) {
        const char *bare = bare_of(m.topic);
        int i = bare ? topic_index(conn, bare) : -1;
        reply(conn, &m, true, NULL, send, ud);
        if (i >= 0 && m.join_ref.n && !cb_span_eq(m.join_ref, conn->join_refs[i])) {
            // A leave of an earlier join of this topic, run after the rejoin
            // that followed it (API Gateway runs a connection's frames
            // concurrently, so a page's leave-then-rejoin can land in either
            // order). It ends that earlier channel only: the close carries the
            // leave's join_ref, which the client routes to the channel it
            // left, and the rejoined topic stays subscribed.
            send_close_ref(conn, i, m.join_ref, send, ud);
        } else if (i >= 0) {
            send_close(conn, i, send, ud);
            topic_remove(conn, i);
        }
    } else if (strcmp(m.event, "access_token") == 0) {
        handle_access_token(ctx, conn, &m, send, ud);
    }
    return CB_OK;
}

// ---- the topic policy ----------------------------------------------------------------
//
// seed.sql's realtime.messages policies, in C. The gu- topic is rebuilt from
// the member and compared whole, never split on '-' (a user id is a hyphenated
// UUID), exactly as migrations/20260917120000_realtime_channel_exact_topics.sql
// does it.
int cb_phx_may_join(cb_ctx *ctx, const char *user_id, const char *topic) {
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
