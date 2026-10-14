// phoenix.c - the Phoenix channel protocol (phoenix.h).
//
// Ground truth for every shape here: @supabase/realtime-js 2.x
// (RealtimeChannel.js: the join payload, _updatePostgresBindings, the
// postgres_changes filter by `ids`), its vendored phoenix (Channel.isMember:
// a push with a join_ref other than the channel's is dropped, so pushes carry
// null), supabase-swift's RealtimeV2 (RealtimeMessageV2, PostgresJoinConfig; 2.55.4
// speaks vsn 2.0.0, earlier releases the v1 object keys), and foolish/e2e/helpers/ws_phoenix.ts.
#include "phoenix.h"

#include "realtime.h"
#include "rt_deps.h"
#include "util/json.h"
#include "util/log.h"

#include <stdio.h>
#include <string.h>

#define WIRE_PREFIX "realtime:"
#define MAX_BINDINGS 16

// ---- frames ----------------------------------------------------------------------

// One string element of a frame into `out`: a ref (no escapes allowed, empty
// for null or missing) or the topic/event (unescaped into a fixed buffer).
static bool ref_of(const char *t, size_t n, const char *path, cb_span *out) {
    cb_span v;
    cb_json_type ty = cb_json_find(t, n, path, &v);
    if (ty == CB_JSON_NULL || ty == CB_JSON_MISSING) { *out = cb_span_of("", 0); return true; }
    if (ty != CB_JSON_STR || v.n < 2) return false;
    if (memchr(v.p, '\\', v.n)) return false;   // refs are counters; an escape is not one
    *out = cb_span_of(v.p + 1, v.n - 2);
    return true;
}

static bool str_of(const char *t, size_t n, const char *path, char *out, size_t cap) {
    return cb_json_find(t, n, path, NULL) == CB_JSON_STR && cb_json_get_str(t, n, path, out, cap);
}

int cb_phx_decode(const char *text, size_t len, int vsn, cb_phx_msg *out) {
    memset(out, 0, sizeof *out);
    if (!text || !cb_json_valid(text, len)) return CB_E_INVALID;
    cb_json_type top = cb_json_find(text, len, "", NULL);
    const char *k_jr, *k_ref, *k_topic, *k_event, *k_payload;
    if (vsn == 2) {
        if (top != CB_JSON_ARR || cb_json_array_len(text, len, "") < 5) return CB_E_INVALID;
        k_jr = "0"; k_ref = "1"; k_topic = "2"; k_event = "3"; k_payload = "4";
    } else if (vsn == 1) {
        if (top != CB_JSON_OBJ) return CB_E_INVALID;
        k_jr = "join_ref"; k_ref = "ref"; k_topic = "topic"; k_event = "event"; k_payload = "payload";
    } else {
        return CB_E_INVALID;
    }
    if (!ref_of(text, len, k_jr, &out->join_ref) || !ref_of(text, len, k_ref, &out->ref)) return CB_E_INVALID;
    if (!str_of(text, len, k_topic, out->topic, sizeof out->topic)) return CB_E_INVALID;
    if (!str_of(text, len, k_event, out->event, sizeof out->event)) return CB_E_INVALID;
    cb_json_type pt = cb_json_find(text, len, k_payload, &out->payload);
    if (pt == CB_JSON_MISSING || pt == CB_JSON_ERR) return CB_E_INVALID;
    return CB_OK;
}

static void put_ref(cb_jw *w, cb_span s) {
    if (s.n) cb_jw_strn(w, s.p, s.n);
    else cb_jw_null(w);
}

// The one frame writer: [join_ref, ref, topic, event, payload] for vsn 2, the
// same five as an object for vsn 1. `payload` is JSON text.
static void frame(cb_buf *out, int vsn, cb_span join_ref, cb_span ref, const char *topic, const char *event,
                  const char *payload, size_t payload_n) {
    cb_buf_reset(out);
    cb_jw w;
    cb_jw_init(&w, out);
    if (vsn == 2) {
        cb_jw_arr(&w);
        put_ref(&w, join_ref);
        put_ref(&w, ref);
        cb_jw_str(&w, topic);
        cb_jw_str(&w, event);
        cb_jw_raw(&w, payload, payload_n);
    } else {
        cb_jw_obj(&w);
        cb_jw_key(&w, "join_ref"); put_ref(&w, join_ref);
        cb_jw_key(&w, "ref"); put_ref(&w, ref);
        cb_jw_kstr(&w, "topic", topic);
        cb_jw_kstr(&w, "event", event);
        cb_jw_key(&w, "payload"); cb_jw_raw(&w, payload, payload_n);
    }
    cb_jw_end(&w);
}

void cb_phx_reply(cb_buf *out, int vsn, const cb_phx_msg *to, bool ok, const char *response_json) {
    cb_buf p;
    cb_buf_init(&p);
    cb_buf_printf(&p, "{\"response\":%s,\"status\":\"%s\"}", response_json && *response_json ? response_json : "{}",
                  ok ? "ok" : "error");
    frame(out, vsn, to->join_ref, to->ref, to->topic, "phx_reply", cb_buf_cstr(&p), p.len);
    cb_buf_free(&p);
}

void cb_phx_push(cb_buf *out, int vsn, const char *wire_topic, const char *event, const char *payload_json) {
    frame(out, vsn, cb_span_of("", 0), cb_span_of("", 0), wire_topic, event, payload_json, strlen(payload_json));
}

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

// phx_close for topic i, as Phoenix sends it when a channel ends (ref = join_ref).
static void send_close(const cb_phx_conn *c, int i, cb_phx_send_fn send, void *ud) {
    char wire[CB_TOPIC_MAX + sizeof WIRE_PREFIX];
    snprintf(wire, sizeof wire, WIRE_PREFIX "%s", c->topics[i]);
    cb_span jr = cb_span_cstr(c->join_refs[i]);
    cb_buf f;
    cb_buf_init(&f);
    frame(&f, c->vsn, jr, jr, wire, "phx_close", "{}", 2);
    send(ud, cb_buf_cstr(&f), f.len);
    cb_buf_free(&f);
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
    cb_jw w;
    cb_jw_init(&w, &r);
    cb_jw_obj(&w);
    cb_jw_kstr(&w, "reason", reason);
    cb_jw_end(&w);
    reply(c, m, false, cb_buf_cstr(&r), send, ud);
    cb_buf_free(&r);
}

// The user a token names. 1 a user (in *u), 0 nobody (no token, or the anon
// key), CB_E_AUTH a token that does not verify, another CB_E_* on failure.
static int user_of_token(cb_ctx *ctx, const char *payload, size_t n, cb_user *u) {
    char tok[4096];
    if (cb_json_find(payload, n, "access_token", NULL) != CB_JSON_STR) return 0;
    if (!cb_json_get_str(payload, n, "access_token", tok, sizeof tok)) return CB_E_AUTH;
    if (!tok[0]) return 0;
    if (ctx->cfg && ctx->cfg->anon_key[0] && strcmp(tok, ctx->cfg->anon_key) == 0) return 0;
    int rc = cb_rt_deps_g.verify_token(ctx, tok, u);
    return rc == CB_OK ? 1 : rc;
}

// The join reply's response: {} for a broadcast channel, or every
// postgres_changes binding echoed whole with its id appended (realtime-js and
// supabase-swift compare the echo field by field and error the channel on a
// mismatch, so the request's own text is the only safe answer).
static int join_response(const char *payload, size_t n, cb_buf *out) {
    int nb = cb_json_array_len(payload, n, "config.postgres_changes");
    if (nb <= 0) { cb_buf_puts(out, "{}"); return CB_OK; }
    if (nb > MAX_BINDINGS) return CB_E_FULL;
    cb_buf_puts(out, "{\"postgres_changes\":[");
    for (int i = 0; i < nb; i++) {
        char path[64], f[4][256];
        static const char *const keys[4] = { "event", "schema", "table", "filter" };
        cb_span obj;
        snprintf(path, sizeof path, "config.postgres_changes.%d", i);
        if (cb_json_find(payload, n, path, &obj) != CB_JSON_OBJ) return CB_E_INVALID;
        for (int k = 0; k < 4; k++) {
            char kp[96];
            snprintf(kp, sizeof kp, "%s.%s", path, keys[k]);
            cb_json_type t = cb_json_find(payload, n, kp, NULL);
            if (t == CB_JSON_MISSING || t == CB_JSON_NULL) f[k][0] = 0;
            else if (!str_of(payload, n, kp, f[k], sizeof f[k])) return CB_E_INVALID;
        }
        int32_t id = cb_phx_binding_id(f[0], f[1], f[2], f[3]);
        if (i) cb_buf_putc(out, ',');
        // The object minus its closing brace, then the id. obj.p[0] is '{'.
        size_t body = obj.n - 1;
        bool empty = true;
        for (size_t k = 1; k < body; k++)
            if (obj.p[k] != ' ' && obj.p[k] != '\t' && obj.p[k] != '\n' && obj.p[k] != '\r') { empty = false; break; }
        if (empty) cb_buf_printf(out, "{\"id\":%d}", (int)id);
        else {
            cb_buf_append(out, obj.p, body);
            cb_buf_printf(out, ",\"id\":%d}", (int)id);
        }
    }
    cb_buf_puts(out, "]}");
    return out->oom ? CB_E_NOMEM : CB_OK;
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
    int rc = join_response(m->payload.p, m->payload.n, &resp);
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
    if (who == 0 && cb_json_find(m->payload.p, m->payload.n, "access_token", NULL) != CB_JSON_STR) return;   // nothing presented
    if (who == 1 && strcmp(u.id, c->user_id) == 0) return;   // the same user, a fresh token
    // A bad token, the anon key, or someone else: this channel ends.
    send_close(c, i, send, ud);
    topic_remove(c, i);
}

int cb_phx_on_frame(cb_ctx *ctx, cb_phx_conn *conn, const char *text, size_t len, cb_phx_send_fn send, void *ud) {
    cb_phx_msg m;
    if (cb_phx_decode(text, len, conn->vsn, &m) != CB_OK) return CB_E_INVALID;
    if (strcmp(m.topic, "phoenix") == 0) {
        if (strcmp(m.event, "heartbeat") == 0) reply(conn, &m, true, "{}", send, ud);
        return CB_OK;
    }
    if (strcmp(m.event, "phx_join") == 0) {
        handle_join(ctx, conn, &m, send, ud);
    } else if (strcmp(m.event, "phx_leave") == 0) {
        const char *bare = bare_of(m.topic);
        int i = bare ? topic_index(conn, bare) : -1;
        reply(conn, &m, true, "{}", send, ud);
        if (i >= 0) {
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
