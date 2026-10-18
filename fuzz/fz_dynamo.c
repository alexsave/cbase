// fz_dynamo.c - every reader of what DynamoDB answers (json_surface.h dw_):
// the error classification, the transaction's cancellation reasons, the
// response's Item / Items / Responses / LastEvaluatedKey / NextToken, and
// every item decoder the store turns a row back into a struct with. In
// production the body comes over TLS from AWS; a decoder that trusts it is one
// bad answer (or one item a buggy writer stored) from a crash.
#include "fz.h"

#include "json_surface.h"
#include "store/store.h"
#include "util/b64.h"
#include "util/json.h"

#include <ctype.h>

// ---- the old decode, kept as the oracle ----------------------------------------
//
// json_surface.c reads an item through a one-pass index and a response through
// its top level, crossing strings with memchr. What follows is the decode it
// replaced, as it was (2026-10-18): a byte-at-a-time string skip, every
// attribute found by walking the item from its start, every response member
// found by cb_json_find. The new decode must answer exactly what this answers:
// on any input for the walkers and the item decoders, and on any valid document
// for the response readers (cb_json_find validates as it walks; the new ones
// are as tolerant as the item walkers, which only matters for a body that is
// not JSON).

static size_t ref_skip_ws(cb_span s, size_t i) {
    while (i < s.n && (s.p[i] == ' ' || s.p[i] == '\t' || s.p[i] == '\n' || s.p[i] == '\r')) i++;
    return i;
}

static size_t ref_skip_value(cb_span s, size_t i, int depth) {
    i = ref_skip_ws(s, i);
    if (i >= s.n || depth > 64) return 0;
    char c = s.p[i];
    if (c == '"') {
        for (i++; i < s.n; i++) {
            if (s.p[i] == '\\') i++;
            else if (s.p[i] == '"') return i + 1;
        }
        return 0;
    }
    if (c == '{' || c == '[') {
        char close = c == '{' ? '}' : ']';
        i = ref_skip_ws(s, i + 1);
        if (i < s.n && s.p[i] == close) return i + 1;
        for (;;) {
            if (c == '{') {
                size_t e = ref_skip_value(s, i, depth + 1);
                if (!e) return 0;
                i = ref_skip_ws(s, e);
                if (i >= s.n || s.p[i] != ':') return 0;
                i++;
            }
            size_t e = ref_skip_value(s, i, depth + 1);
            if (!e) return 0;
            i = ref_skip_ws(s, e);
            if (i >= s.n) return 0;
            if (s.p[i] == close) return i + 1;
            if (s.p[i] != ',') return 0;
            i++;
        }
    }
    size_t j = i;
    while (j < s.n && (isalnum((unsigned char)s.p[j]) || s.p[j] == '-' || s.p[j] == '+' || s.p[j] == '.')) j++;
    return j > i ? j : 0;
}

static bool ref_array_next(cb_span arr, size_t *pos, cb_span *elem) {
    size_t i = *pos;
    if (i == 0) {
        i = ref_skip_ws(arr, 0);
        if (i >= arr.n || arr.p[i] != '[') return false;
        i = ref_skip_ws(arr, i + 1);
    } else {
        i = ref_skip_ws(arr, i);
        if (i >= arr.n || arr.p[i] != ',') return false;
        i = ref_skip_ws(arr, i + 1);
    }
    if (i >= arr.n || arr.p[i] == ']') return false;
    size_t e = ref_skip_value(arr, i, 0);
    if (!e) return false;
    *elem = cb_span_of(arr.p + i, e - i);
    *pos = e;
    return true;
}

static bool ref_object_next(cb_span obj, size_t *pos, cb_span *key, cb_span *val) {
    size_t i = *pos;
    if (i == 0) {
        i = ref_skip_ws(obj, 0);
        if (i >= obj.n || obj.p[i] != '{') return false;
        i = ref_skip_ws(obj, i + 1);
    } else {
        i = ref_skip_ws(obj, i);
        if (i >= obj.n || obj.p[i] != ',') return false;
        i = ref_skip_ws(obj, i + 1);
    }
    if (i >= obj.n || obj.p[i] != '"') return false;
    size_t e = ref_skip_value(obj, i, 0);
    if (!e) return false;
    *key = cb_span_of(obj.p + i, e - i);
    i = ref_skip_ws(obj, e);
    if (i >= obj.n || obj.p[i] != ':') return false;
    i = ref_skip_ws(obj, i + 1);
    e = ref_skip_value(obj, i, 0);
    if (!e) return false;
    *val = cb_span_of(obj.p + i, e - i);
    *pos = e;
    return true;
}

static bool ref_key_is(cb_span key, const char *name) {
    size_t n = strlen(name);
    return key.n == n + 2 && memcmp(key.p + 1, name, n) == 0;
}

static bool ref_member(cb_span obj, const char *name, cb_span *val) {
    size_t pos = 0;
    cb_span key;
    while (ref_object_next(obj, &pos, &key, val))
        if (ref_key_is(key, name)) return true;
    return false;
}

static bool ref_attr(cb_span item, const char *name, const char *type, cb_span *tok) {
    cb_span v;
    return ref_member(item, name, &v) && ref_member(v, type, tok);
}

static bool ref_get_s(cb_span item, const char *name, char *out, size_t cap) {
    cb_span tok;
    if (!ref_attr(item, name, "S", &tok) || tok.n < 2 || tok.p[0] != '"') return false;
    return cb_json_unescape(tok, out, cap) >= 0;
}

static bool ref_get_n(cb_span item, const char *name, int64_t *out) {
    char t[32];
    cb_span tok;
    if (!ref_attr(item, name, "N", &tok) || cb_json_unescape(tok, t, sizeof t) < 1) return false;
    char *end;
    long long v = strtoll(t, &end, 10);
    if (*end) return false;
    *out = v;
    return true;
}

static bool ref_get_bool(cb_span item, const char *name, bool *out) {
    cb_span tok;
    if (!ref_attr(item, name, "BOOL", &tok)) return false;
    if (tok.n == 4 && memcmp(tok.p, "true", 4) == 0) *out = true;
    else if (tok.n == 5 && memcmp(tok.p, "false", 5) == 0) *out = false;
    else return false;
    return true;
}

static bool ref_get_b(cb_span item, const char *name, uint8_t **out, size_t *len) {
    *out = NULL;
    *len = 0;
    cb_span tok;
    if (!ref_attr(item, name, "B", &tok)) return true;
    if (tok.n < 2 || tok.p[0] != '"') return false;
    char *text = malloc(tok.n);
    if (!text) return false;
    int tn = cb_json_unescape(tok, text, tok.n);
    if (tn < 0) { free(text); return false; }
    size_t cap = (size_t)tn / 4 * 3 + 3;
    uint8_t *p = malloc(cap);
    int n = p ? cb_b64_decode(text, (size_t)tn, p, cap) : -1;
    free(text);
    if (n < 0) { free(p); return false; }
    if (n == 0) { free(p); return true; }
    *out = p;
    *len = (size_t)n;
    return true;
}

static int64_t ref_n_or(cb_span item, const char *name, int64_t none) {
    int64_t v;
    return ref_get_n(item, name, &v) ? v : none;
}

static bool ref_item_user(cb_span it, cb_user_row *u) {
    memset(u, 0, sizeof *u);
    if (!ref_get_s(it, "id", u->id, sizeof u->id) || !ref_get_s(it, "email", u->email, sizeof u->email) ||
        !ref_get_s(it, "password_hash", u->password_hash, sizeof u->password_hash))
        return false;
    ref_get_s(it, "username", u->username, sizeof u->username);
    u->created_at = ref_n_or(it, "created_at", 0);
    u->last_sign_in_at = ref_n_or(it, "last_sign_in_at", 0);
    u->session_gen = ref_n_or(it, "session_gen", 0);
    ref_get_s(it, "kept_session", u->kept_session, sizeof u->kept_session);
    return true;
}

static bool ref_item_session(cb_span it, int64_t now_ms, cb_session_row *s) {
    memset(s, 0, sizeof *s);
    if (!ref_get_s(it, "id", s->id, sizeof s->id)) return false;
    ref_get_s(it, "user_id", s->user_id, sizeof s->user_id);
    s->created_at = ref_n_or(it, "created_at", 0);
    s->expires_at = ref_n_or(it, "expires_ms", 0);
    s->gen = ref_n_or(it, "gen", 0);
    ref_get_bool(it, "revoked", &s->revoked);
    return s->expires_at > now_ms;
}

static bool ref_item_refresh(cb_span it, cb_refresh_row *r) {
    memset(r, 0, sizeof *r);
    if (!ref_get_s(it, "token_hash", r->token_hash, sizeof r->token_hash) ||
        !ref_get_s(it, "user_id", r->user_id, sizeof r->user_id))
        return false;
    ref_get_s(it, "session_id", r->session_id, sizeof r->session_id);
    r->created_at = ref_n_or(it, "created_at", 0);
    ref_get_bool(it, "revoked", &r->revoked);
    r->revoked_at = ref_n_or(it, "revoked_at", 0);
    r->expires_at = ref_n_or(it, "expires_ms", 0);
    return true;
}

static bool ref_item_game(cb_span it, cb_game_row *g) {
    memset(g, 0, sizeof *g);
    int64_t status, version;
    if (!ref_get_s(it, "id", g->id, sizeof g->id) || !ref_get_n(it, "status", &status) ||
        !ref_get_n(it, "version", &version))
        return false;
    g->status = (int)status;
    g->version = version;
    uint8_t *seed;
    size_t seed_len;
    if (!ref_get_b(it, "state", &g->state, &g->state_len) || !ref_get_b(it, "roster", &g->roster, &g->roster_len) ||
        !ref_get_b(it, "logs_packed", &g->logs, &g->logs_len) || !ref_get_b(it, "game_seed", &seed, &seed_len)) {
        cb_game_row_free(g);
        return false;
    }
    if (seed && seed_len == CB_SEED_LEN) {
        g->has_seed = true;
        memcpy(g->seed, seed, CB_SEED_LEN);
    }
    free(seed);
    ref_get_bool(it, "needs_bots", &g->needs_bots);
    g->round_epoch = ref_n_or(it, "round_epoch", 0);
    g->created_at = ref_n_or(it, "created_at", 0);
    g->updated_at = ref_n_or(it, "updated_at", 0);
    g->last_commit_at = ref_n_or(it, "last_commit_at", 0);
    return true;
}

static bool ref_item_member(cb_span it, cb_member_row *m) {
    memset(m, 0, sizeof *m);
    m->updated_at = ref_n_or(it, "updated_at", 0);
    return ref_get_s(it, "game_id", m->game_id, sizeof m->game_id);
}

static bool ref_item_bot(cb_span it, cb_bot_row *b) {
    memset(b, 0, sizeof *b);
    if (!ref_get_s(it, "id", b->id, sizeof b->id) || !ref_get_s(it, "nickname", b->nickname, sizeof b->nickname) ||
        !ref_get_s(it, "strategy_key", b->strategy_key, sizeof b->strategy_key))
        return false;
    b->elo_rating = (int32_t)ref_n_or(it, "elo_rating", 1000);
    b->previous_elo = (int32_t)ref_n_or(it, "previous_elo", -1);
    b->games_played = (int32_t)ref_n_or(it, "games_played", 0);
    b->created_at = ref_n_or(it, "created_at", 0);
    b->updated_at = ref_n_or(it, "updated_at", 0);
    return true;
}

static bool ref_item_rating(cb_span it, cb_rating_row *r) {
    memset(r, 0, sizeof *r);
    if (!ref_get_s(it, "user_id", r->user_id, sizeof r->user_id)) return false;
    ref_get_s(it, "username", r->username, sizeof r->username);
    r->elo_rating = (int32_t)ref_n_or(it, "elo_rating", 1000);
    r->previous_elo = (int32_t)ref_n_or(it, "previous_elo", -1);
    r->games_played = (int32_t)ref_n_or(it, "games_played", 0);
    r->updated_at = ref_n_or(it, "updated_at", 0);
    return true;
}

static bool ref_item_snapshot(cb_span it, cb_snapshot_row *s) {
    memset(s, 0, sizeof *s);
    if (!ref_get_s(it, "id", s->id, sizeof s->id)) return false;
    ref_get_s(it, "game_id", s->game_id, sizeof s->game_id);
    cb_span players, list, e;
    size_t pos = 0;
    if (ref_member(it, "player_ids", &players) && ref_member(players, "L", &list))
        while (s->n_players < CB_MAX_SEATS && ref_array_next(list, &pos, &e)) {
            cb_span tok;
            if (!ref_member(e, "S", &tok) || cb_json_unescape(tok, s->player_ids[s->n_players], sizeof(cb_uuid)) < 0)
                return false;
            s->n_players++;
        }
    if (!ref_get_b(it, "moves", &s->moves, &s->moves_len) || !ref_get_b(it, "extras", &s->extras, &s->extras_len)) {
        cb_snapshot_row_free(s);
        return false;
    }
    s->created_at = ref_n_or(it, "created_at", 0);
    return true;
}

static bool ref_item_chat(cb_span it, cb_chat_row *c) {
    memset(c, 0, sizeof *c);
    cb_span tok;
    if (!ref_get_s(it, "id", c->id, sizeof c->id) || !ref_get_s(it, "game_id", c->game_id, sizeof c->game_id) ||
        !ref_get_s(it, "user_id", c->user_id, sizeof c->user_id) || !ref_attr(it, "message", "S", &tok))
        return false;
    c->message = malloc(tok.n);
    if (!c->message || cb_json_unescape(tok, c->message, tok.n) < 0) {
        free(c->message);
        c->message = NULL;
        return false;
    }
    ref_get_bool(it, "is_system", &c->is_system);
    c->created_at = ref_n_or(it, "created_at", 0);
    return true;
}

static bool ref_item_signin_fails(cb_span it, int64_t now_ms, cb_signin_fails *f) {
    memset(f, 0, sizeof f[0]);
    int64_t n, expires;
    if (!ref_get_n(it, "n", &n) || !ref_get_n(it, "expires_ms", &expires) || expires <= now_ms) return false;
    f->n = n < 0 ? 0 : n > INT32_MAX ? INT32_MAX : (int32_t)n;
    f->last_at = ref_n_or(it, "last_at", 0);
    f->locked_until = ref_n_or(it, "locked_until", 0);
    return true;
}

static bool ref_item_scan(cb_span it, char game_id[CB_GAME_ID_MAX + 1], int64_t *updated_at, int64_t *last_commit_at) {
    *updated_at = ref_n_or(it, "updated_at", 0);
    *last_commit_at = ref_n_or(it, "last_commit_at", 0);
    return ref_get_s(it, "game_id", game_id, CB_GAME_ID_MAX + 1);
}

// The response readers as they were: cb_json_find from the top.
static double ref_consumed(const char *body, size_t n) {
    static const char *const paths[] = { "ConsumedCapacity.CapacityUnits", "ConsumedCapacity.0.CapacityUnits" };
    for (int i = 0; i < 2; i++) {
        cb_span v;
        if (cb_json_find(body, n, paths[i], &v) == CB_JSON_NUM) {
            char t[32];
            if (v.n >= sizeof t) return 0;
            memcpy(t, v.p, v.n);
            t[v.n] = 0;
            return strtod(t, NULL);
        }
    }
    return 0;
}

static bool ref_nonempty(const char *body, size_t n, const char *path) {
    cb_span s, k, v;
    size_t pos = 0;
    return cb_json_find(body, n, path, &s) == CB_JSON_OBJ && ref_object_next(s, &pos, &k, &v);
}

static bool same_span(cb_span a, cb_span b) { return a.n == b.n && (a.n == 0 || memcmp(a.p, b.p, a.n) == 0); }
static bool same_bytes(const uint8_t *a, size_t an, const uint8_t *b, size_t bn) {
    return an == bn && (an == 0 || memcmp(a, b, an) == 0);
}

// Every walker on a span, step for step against the old one.
static void same_walks(cb_span s) {
    size_t p1 = 0, p2 = 0;
    cb_span k1, v1, k2, v2;
    for (int i = 0; i < 256; i++) {
        bool a = dw_object_next(s, &p1, &k1, &v1), b = ref_object_next(s, &p2, &k2, &v2);
        FZ_CHECK(a == b);
        if (!a) break;
        FZ_CHECK(p1 == p2 && same_span(k1, k2) && same_span(v1, v2));
    }
    p1 = p2 = 0;
    for (int i = 0; i < 256; i++) {
        bool a = dw_array_next(s, &p1, &k1), b = ref_array_next(s, &p2, &k2);
        FZ_CHECK(a == b);
        if (!a) break;
        FZ_CHECK(p1 == p2 && same_span(k1, k2));
    }
}

// Every item decoder on one item against the old one: the same answer, and
// the same struct (a partly filled one included).
static void same_items(cb_span it) {
    same_walks(it);
    { cb_user_row a, b; FZ_CHECK(dw_item_user(it, &a) == ref_item_user(it, &b)); FZ_CHECK(!memcmp(&a, &b, sizeof a)); }
    { cb_session_row a, b; FZ_CHECK(dw_item_session(it, 1000, &a) == ref_item_session(it, 1000, &b)); FZ_CHECK(!memcmp(&a, &b, sizeof a)); }
    { cb_refresh_row a, b; FZ_CHECK(dw_item_refresh(it, &a) == ref_item_refresh(it, &b)); FZ_CHECK(!memcmp(&a, &b, sizeof a)); }
    { cb_member_row a, b; FZ_CHECK(dw_item_member(it, &a) == ref_item_member(it, &b)); FZ_CHECK(!memcmp(&a, &b, sizeof a)); }
    { cb_bot_row a, b; FZ_CHECK(dw_item_bot(it, &a) == ref_item_bot(it, &b)); FZ_CHECK(!memcmp(&a, &b, sizeof a)); }
    { cb_rating_row a, b; FZ_CHECK(dw_item_rating(it, &a) == ref_item_rating(it, &b)); FZ_CHECK(!memcmp(&a, &b, sizeof a)); }
    { cb_signin_fails a, b; FZ_CHECK(dw_item_signin_fails(it, 1000, &a) == ref_item_signin_fails(it, 1000, &b)); FZ_CHECK(!memcmp(&a, &b, sizeof a)); }
    {
        char ga[CB_GAME_ID_MAX + 1] = "", gb[CB_GAME_ID_MAX + 1] = "";
        int64_t ua, ub, la, lb;
        FZ_CHECK(dw_item_scan(it, ga, &ua, &la) == ref_item_scan(it, gb, &ub, &lb));
        FZ_CHECK(ua == ub && la == lb && !memcmp(ga, gb, sizeof ga));
    }
    {
        cb_game_row a, b;
        bool ra = dw_item_game(it, &a), rb = ref_item_game(it, &b);
        FZ_CHECK(ra == rb);
        FZ_CHECK(!strcmp(a.id, b.id) && a.status == b.status && a.version == b.version && a.has_seed == b.has_seed &&
                 !memcmp(a.seed, b.seed, sizeof a.seed) && a.needs_bots == b.needs_bots && a.round_epoch == b.round_epoch &&
                 a.created_at == b.created_at && a.updated_at == b.updated_at && a.last_commit_at == b.last_commit_at);
        FZ_CHECK(same_bytes(a.state, a.state_len, b.state, b.state_len) &&
                 same_bytes(a.roster, a.roster_len, b.roster, b.roster_len) &&
                 same_bytes(a.logs, a.logs_len, b.logs, b.logs_len));
        cb_game_row_free(&a);
        cb_game_row_free(&b);
    }
    {
        cb_snapshot_row a, b;
        bool ra = dw_item_snapshot(it, &a), rb = ref_item_snapshot(it, &b);
        FZ_CHECK(ra == rb);
        FZ_CHECK(!strcmp(a.id, b.id) && !strcmp(a.game_id, b.game_id) && a.n_players == b.n_players &&
                 !memcmp(a.player_ids, b.player_ids, sizeof a.player_ids) && a.created_at == b.created_at);
        FZ_CHECK(same_bytes(a.moves, a.moves_len, b.moves, b.moves_len) &&
                 same_bytes(a.extras, a.extras_len, b.extras, b.extras_len));
        cb_snapshot_row_free(&a);
        cb_snapshot_row_free(&b);
    }
    {
        cb_chat_row a, b;
        bool ra = dw_item_chat(it, &a), rb = ref_item_chat(it, &b);
        FZ_CHECK(ra == rb);
        FZ_CHECK(!strcmp(a.id, b.id) && !strcmp(a.game_id, b.game_id) && !strcmp(a.user_id, b.user_id) &&
                 a.is_system == b.is_system && a.created_at == b.created_at);
        FZ_CHECK((a.message == NULL) == (b.message == NULL) && (!a.message || !strcmp(a.message, b.message)));
        cb_chat_row_free(&a);
        cb_chat_row_free(&b);
    }
    static const char *const names[] = { "pk", "sk", "id", "version", "state", "needs_bots", "message", "n", "" };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        char sa[64], sb[64];
        int64_t na = 0, nb = 0;
        uint8_t *pa, *pb;
        size_t la, lb;
        FZ_CHECK(dw_get_s(it, names[i], sa, sizeof sa) == ref_get_s(it, names[i], sb, sizeof sb));
        FZ_CHECK(dw_get_n(it, names[i], &na) == ref_get_n(it, names[i], &nb) && na == nb);
        FZ_CHECK(dw_get_b(it, names[i], &pa, &la) == ref_get_b(it, names[i], &pb, &lb) && same_bytes(pa, la, pb, lb));
        free(pa);
        free(pb);
    }
}

// The response readers against cb_json_find, on a valid document.
static void same_responses(const char *body, size_t n) {
    if (!cb_json_valid(body, n)) return;
    cb_span a, b;
    bool ha = dw_item(body, n, &a), hb = cb_json_find(body, n, "Item", &b) == CB_JSON_OBJ;
    FZ_CHECK(ha == hb && (!ha || same_span(a, b)));
    FZ_CHECK(dw_cond_failed_has_item(body, n) == hb);
    ha = dw_attributes(body, n, &a), hb = cb_json_find(body, n, "Attributes", &b) == CB_JSON_OBJ;
    FZ_CHECK(ha == hb && (!ha || same_span(a, b)));
    a = dw_items(body, n);
    if (cb_json_find(body, n, "Items", &b) != CB_JSON_ARR) b = CB_SPAN_LIT("[]");
    FZ_CHECK(same_span(a, b));
    a = dw_last_key(body, n);
    if (cb_json_find(body, n, "LastEvaluatedKey", &b) != CB_JSON_OBJ) b = cb_span_of("", 0);
    FZ_CHECK(same_span(a, b));
    FZ_CHECK(dw_unprocessed(body, n) == (ref_nonempty(body, n, "UnprocessedItems") || ref_nonempty(body, n, "UnprocessedKeys")));
    double ca = dw_consumed(body, n), cb = ref_consumed(body, n);
    FZ_CHECK(ca == cb || (ca != ca && cb != cb));
    cb_span resp, key, val;
    size_t pos = 0;
    b = CB_SPAN_LIT("[]");
    if (cb_json_find(body, n, "Responses", &resp) == CB_JSON_OBJ)
        while (ref_object_next(resp, &pos, &key, &val))
            if (ref_key_is(key, "cbase")) { b = val; break; }
    FZ_CHECK(same_span(dw_batch_items(body, n, "cbase"), b));
}

// Every item decoder on one item.
static void items_of(cb_span item) {
    same_items(item);
    cb_user_row u; (void)dw_item_user(item, &u);
    cb_refresh_row rr; (void)dw_item_refresh(item, &rr);
    cb_game_row g; memset(&g, 0, sizeof g);
    if (dw_item_game(item, &g)) FZ_CHECK(!g.state_len || g.state);
    cb_game_row_free(&g);
    cb_member_row m; (void)dw_item_member(item, &m);
    cb_uuid id; bool bot; (void)dw_seat_key(item, id, &bot);
    cb_bot_row b; (void)dw_item_bot(item, &b);
    cb_rating_row rt; (void)dw_item_rating(item, &rt);
    cb_snapshot_row s; memset(&s, 0, sizeof s);
    (void)dw_item_snapshot(item, &s);
    cb_snapshot_row_free(&s);
    cb_chat_row c; memset(&c, 0, sizeof c);
    (void)dw_item_chat(item, &c);
    cb_chat_row_free(&c);
    char gid[CB_GAME_ID_MAX + 1]; int64_t a, z; (void)dw_item_scan(item, gid, &a, &z);
    (void)dw_item_email_user(item, id);
    cb_buf k; cb_buf_init(&k); (void)dw_item_key(item, &k); cb_buf_free(&k);
    cb_buf st; cb_buf_init(&st);
    const char *names[64], *values[64];
    int n = dw_item_text(item, &st, names, values, 64);
    FZ_CHECK(n <= 64);
    for (int i = 0; i < n; i++) { (void)strlen(names[i]); if (values[i]) (void)strlen(values[i]); }
    cb_buf_free(&st);
    char sv[64]; int64_t nv; uint8_t *bp = NULL; size_t bl = 0;
    (void)dw_get_s(item, "pk", sv, sizeof sv);
    (void)dw_get_n(item, "version", &nv);
    if (dw_get_b(item, "state", &bp, &bl)) free(bp);
    char exp[64]; (void)dw_export_kind(item, exp, sizeof exp);
}

static void walk(cb_span arr) {
    size_t pos = 0;
    cb_span e;
    for (int i = 0; i < 64 && dw_array_next(arr, &pos, &e); i++) {
        FZ_CHECK(e.p >= arr.p && e.p + e.n <= arr.p + arr.n);
        items_of(e);
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 1) return 0;
    int status = data[0] < 128 ? 200 : 400 + (data[0] & 0x7f);
    size_t n = size - 1;
    const char *body = (const char *)fz_dup(data + 1, n);

    (void)dw_classify(status, body, n);
    (void)dw_throttled(body, n);
    char text[256]; dw_error_text(body, n, text, sizeof text); FZ_CHECK(strlen(text) < sizeof text);
    bool has;
    for (int i = 0; i < 4; i++) (void)dw_cancel_reason(body, n, i, &has);
    (void)dw_cond_failed_has_item(body, n);
    same_responses(body, n);
    same_walks(cb_span_of(body, n));

    cb_span item;
    if (dw_item(body, n, &item)) items_of(item);
    if (dw_attributes(body, n, &item)) items_of(item);
    walk(dw_items(body, n));
    walk(dw_batch_items(body, n, "cbase"));
    (void)dw_last_key(body, n);
    (void)dw_unprocessed(body, n);
    char tok[128]; dw_next_token(body, n, tok, sizeof tok); FZ_CHECK(strlen(tok) < sizeof tok);
    int64_t rcu, wcu; (void)dw_provisioned(body, n, &rcu, &wcu);
    (void)dw_consumed(body, n);

    // An object walk over the whole body, as dw_item_text does over an item.
    size_t pos = 0; cb_span k, v;
    for (int i = 0; i < 256 && dw_object_next(cb_span_of(body, n), &pos, &k, &v); i++)
        FZ_CHECK(v.p >= body && v.p + v.n <= body + n);

    const char *ids[3] = { "11111111-1111-4111-8111-111111111111", "22222222-2222-4222-8222-222222222222", "ab12cd" };
    const bool is_bot[3] = { false, true, false };
    dw_rating_read rr[6];
    dw_ratings_read(body, n, "cbase", ids, is_bot, 3, false, rr);
    dw_ratings_read(body, n, "cbase", ids, is_bot, 3, true, rr);
    const char *left[3];
    int u = dw_unprocessed_games(body, n, "cbase", ids, 3, left);
    FZ_CHECK(u >= 0 && u <= 3);
    free((void *)body);
    return 0;
}
