// dynamo_wire.c - the DynamoDB JSON (dynamo_wire.h). Every DynamoDB string in
// c_base is in this file: the targets, the item layout, the expressions.
#include "dynamo_wire.h"

#include "util/b64.h"
#include "util/hex.h"
#include "util/json.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define TARGET "DynamoDB_20120810."
#define GSI1 "gsi1"
#define GSI2 "gsi2"

void dw_req_init(dw_req *r) {
    r->target[0] = '\0';
    cb_buf_init(&r->body);
}

void dw_req_free(dw_req *r) { cb_buf_free(&r->body); }

void dw_host(const char *region, char *out, size_t cap) { snprintf(out, cap, "dynamodb.%s.amazonaws.com", region); }
const char *dw_content_type(void) { return "application/x-amz-json-1.0"; }
const char *dw_service(void) { return "dynamodb"; }

// Starts a request: the target, an empty body, the writer on it with the
// top-level object open.
static void begin(dw_req *r, cb_jw *w, const char *op) {
    snprintf(r->target, sizeof r->target, TARGET "%s", op);
    cb_buf_reset(&r->body);
    cb_jw_init(w, &r->body);
    cb_jw_obj(w);
}

static void finish(cb_jw *w) { cb_jw_end(w); }

// ---- keys -------------------------------------------------------------------------------

// "<prefix><id>" into out.
static const char *k(char *out, size_t cap, const char *prefix, const char *id) {
    snprintf(out, cap, "%s%s", prefix, id ? id : "");
    return out;
}

// "<15-digit ms>#<id>": sorts as time, then id.
static const char *tsk(char *out, size_t cap, int64_t ms, const char *id) {
    snprintf(out, cap, "%015" PRId64 "#%s", ms < 0 ? (int64_t)0 : ms, id ? id : "");
    return out;
}

#define KB 400   // room for any key we build (a topic is at most 128)

// ---- attribute values ------------------------------------------------------------------

static void av_s(cb_jw *w, const char *name, const char *v) {
    cb_jw_key(w, name);
    cb_jw_obj(w);
    cb_jw_kstr(w, "S", v ? v : "");
    cb_jw_end(w);
}

static void av_n(cb_jw *w, const char *name, int64_t v) {
    char t[24];
    snprintf(t, sizeof t, "%" PRId64, v);
    cb_jw_key(w, name);
    cb_jw_obj(w);
    cb_jw_kstr(w, "N", t);
    cb_jw_end(w);
}

static void av_bool(cb_jw *w, const char *name, bool v) {
    cb_jw_key(w, name);
    cb_jw_obj(w);
    cb_jw_kbool(w, "BOOL", v);
    cb_jw_end(w);
}

// Binary: base64 in the JSON. Zero bytes are a valid (empty) Binary value.
static void av_b(cb_jw *w, const char *name, const uint8_t *p, size_t n) {
    size_t len = cb_b64_len(n) + 1;
    char *t = malloc(len);
    cb_jw_key(w, name);
    cb_jw_obj(w);
    if (t && cb_b64_encode(p, n, t, len) >= 0) cb_jw_kstr(w, "B", t);
    else w->err = true;
    cb_jw_end(w);
    free(t);
}

static void key2(cb_jw *w, const char *pk, const char *sk) {
    cb_jw_key(w, "Key");
    cb_jw_obj(w);
    av_s(w, "pk", pk);
    av_s(w, "sk", sk);
    cb_jw_end(w);
}

// ---- expressions ------------------------------------------------------------------------
//
// An update or condition expression with its two maps. Every attribute name
// is aliased (#name), so no reserved word (status, state, version, ...) can
// break one, and a name enters the map only when an expression uses it:
// DynamoDB refuses an unused alias.

typedef struct {
    cb_buf set, rem, names_b, vals_b;
    cb_jw  names, vals;
    int    n_names, n_vals;
    char   used[32][32];
} ux;

static void ux_init(ux *u) {
    memset(u, 0, sizeof *u);
    cb_buf_init(&u->set);
    cb_buf_init(&u->rem);
    cb_buf_init(&u->names_b);
    cb_buf_init(&u->vals_b);
    cb_jw_init(&u->names, &u->names_b);
    cb_jw_obj(&u->names);
    cb_jw_init(&u->vals, &u->vals_b);
    cb_jw_obj(&u->vals);
}

static void ux_free(ux *u) {
    cb_buf_free(&u->set);
    cb_buf_free(&u->rem);
    cb_buf_free(&u->names_b);
    cb_buf_free(&u->vals_b);
}

// Registers #attr once.
static void ux_name(ux *u, const char *attr) {
    for (int i = 0; i < u->n_names; i++)
        if (strcmp(u->used[i], attr) == 0) return;
    if (u->n_names == 32) { u->names.err = true; return; }
    snprintf(u->used[u->n_names++], sizeof u->used[0], "%s", attr);
    char a[40];
    snprintf(a, sizeof a, "#%s", attr);
    cb_jw_kstr(&u->names, a, attr);
}

// The value :name's key is due next in vals.
static cb_jw *ux_val(ux *u, const char *name) {
    u->n_vals++;
    cb_jw_key(&u->vals, name);
    return &u->vals;
}

static void ux_val_s(ux *u, const char *name, const char *v) {
    cb_jw *w = ux_val(u, name);
    cb_jw_obj(w);
    cb_jw_kstr(w, "S", v ? v : "");
    cb_jw_end(w);
}

static void ux_val_n(ux *u, const char *name, int64_t v) {
    char t[24];
    snprintf(t, sizeof t, "%" PRId64, v);
    cb_jw *w = ux_val(u, name);
    cb_jw_obj(w);
    cb_jw_kstr(w, "N", t);
    cb_jw_end(w);
}

static void ux_val_bool(ux *u, const char *name, bool v) {
    cb_jw *w = ux_val(u, name);
    cb_jw_obj(w);
    cb_jw_kbool(w, "BOOL", v);
    cb_jw_end(w);
}

static void ux_val_b(ux *u, const char *name, const uint8_t *p, size_t n) {
    size_t len = cb_b64_len(n) + 1;
    char *t = malloc(len);
    cb_jw *w = ux_val(u, name);
    cb_jw_obj(w);
    if (t && cb_b64_encode(p, n, t, len) >= 0) cb_jw_kstr(w, "B", t);
    else w->err = true;
    cb_jw_end(w);
    free(t);
}

// "#attr = :attr" in the SET clause; the caller writes the value.
static void ux_set(ux *u, const char *attr) {
    ux_name(u, attr);
    if (u->set.len) cb_buf_puts(&u->set, ", ");
    cb_buf_printf(&u->set, "#%s = :%s", attr, attr);
}
static void ux_set_s(ux *u, const char *attr, const char *v) {
    char n[40];
    ux_set(u, attr);
    snprintf(n, sizeof n, ":%s", attr);
    ux_val_s(u, n, v);
}
static void ux_set_n(ux *u, const char *attr, int64_t v) {
    char n[40];
    ux_set(u, attr);
    snprintf(n, sizeof n, ":%s", attr);
    ux_val_n(u, n, v);
}
static void ux_set_bool(ux *u, const char *attr, bool v) {
    char n[40];
    ux_set(u, attr);
    snprintf(n, sizeof n, ":%s", attr);
    ux_val_bool(u, n, v);
}
static void ux_set_b(ux *u, const char *attr, const uint8_t *p, size_t len) {
    char n[40];
    ux_set(u, attr);
    snprintf(n, sizeof n, ":%s", attr);
    ux_val_b(u, n, p, len);
}
// A SET clause written out (its names registered by the caller).
static void ux_set_raw(ux *u, const char *clause) {
    if (u->set.len) cb_buf_puts(&u->set, ", ");
    cb_buf_puts(&u->set, clause);
}
static void ux_remove(ux *u, const char *attr) {
    ux_name(u, attr);
    if (u->rem.len) cb_buf_puts(&u->rem, ", ");
    cb_buf_printf(&u->rem, "#%s", attr);
}

// Writes UpdateExpression (when there is one), ConditionExpression (when
// given) and the two maps into w's open object.
static void ux_emit(cb_jw *w, ux *u, const char *cond) {
    if (u->set.len || u->rem.len) {
        cb_buf e;
        cb_buf_init(&e);
        if (u->set.len) cb_buf_printf(&e, "SET %s", cb_buf_cstr(&u->set));
        if (u->rem.len) cb_buf_printf(&e, "%sREMOVE %s", u->set.len ? " " : "", cb_buf_cstr(&u->rem));
        cb_jw_kstr(w, "UpdateExpression", cb_buf_cstr(&e));
        cb_buf_free(&e);
    }
    if (cond) cb_jw_kstr(w, "ConditionExpression", cond);
    cb_jw_end(&u->names);
    cb_jw_end(&u->vals);
    if (!cb_jw_ok(&u->names) || !cb_jw_ok(&u->vals)) w->err = true;
    if (u->n_names) {
        cb_jw_key(w, "ExpressionAttributeNames");
        cb_jw_raw(w, (const char *)u->names_b.p, u->names_b.len);
    }
    if (u->n_vals) {
        cb_jw_key(w, "ExpressionAttributeValues");
        cb_jw_raw(w, (const char *)u->vals_b.p, u->vals_b.len);
    }
}

// ---- reading JSON -------------------------------------------------------------------------

static size_t skip_ws(cb_span s, size_t i) {
    while (i < s.n && (s.p[i] == ' ' || s.p[i] == '\t' || s.p[i] == '\n' || s.p[i] == '\r')) i++;
    return i;
}

// The end of the value starting at i (after whitespace), or 0 when malformed.
static size_t skip_value(cb_span s, size_t i, int depth) {
    i = skip_ws(s, i);
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
        i = skip_ws(s, i + 1);
        if (i < s.n && s.p[i] == close) return i + 1;
        for (;;) {
            if (c == '{') {
                size_t e = skip_value(s, i, depth + 1);   // the key
                if (!e) return 0;
                i = skip_ws(s, e);
                if (i >= s.n || s.p[i] != ':') return 0;
                i++;
            }
            size_t e = skip_value(s, i, depth + 1);
            if (!e) return 0;
            i = skip_ws(s, e);
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

bool dw_array_next(cb_span arr, size_t *pos, cb_span *elem) {
    size_t i = *pos;
    if (i == 0) {
        i = skip_ws(arr, 0);
        if (i >= arr.n || arr.p[i] != '[') return false;
        i = skip_ws(arr, i + 1);
    } else {
        i = skip_ws(arr, i);
        if (i >= arr.n || arr.p[i] != ',') return false;
        i = skip_ws(arr, i + 1);
    }
    if (i >= arr.n || arr.p[i] == ']') return false;
    size_t e = skip_value(arr, i, 0);
    if (!e) return false;
    *elem = cb_span_of(arr.p + i, e - i);
    *pos = e;
    return true;
}

bool dw_object_next(cb_span obj, size_t *pos, cb_span *key, cb_span *val) {
    size_t i = *pos;
    if (i == 0) {
        i = skip_ws(obj, 0);
        if (i >= obj.n || obj.p[i] != '{') return false;
        i = skip_ws(obj, i + 1);
    } else {
        i = skip_ws(obj, i);
        if (i >= obj.n || obj.p[i] != ',') return false;
        i = skip_ws(obj, i + 1);
    }
    if (i >= obj.n || obj.p[i] != '"') return false;
    size_t e = skip_value(obj, i, 0);
    if (!e) return false;
    *key = cb_span_of(obj.p + i, e - i);
    i = skip_ws(obj, e);
    if (i >= obj.n || obj.p[i] != ':') return false;
    i = skip_ws(obj, i + 1);
    e = skip_value(obj, i, 0);
    if (!e) return false;
    *val = cb_span_of(obj.p + i, e - i);
    *pos = e;
    return true;
}

// A key token equals "name" (attribute and type names never need escapes).
static bool key_is(cb_span key, const char *name) {
    size_t n = strlen(name);
    return key.n == n + 2 && memcmp(key.p + 1, name, n) == 0;
}

// The member `name` of an object value.
static bool member(cb_span obj, const char *name, cb_span *val) {
    size_t pos = 0;
    cb_span key;
    while (dw_object_next(obj, &pos, &key, val))
        if (key_is(key, name)) return true;
    return false;
}

// Attribute `name` of an item, as the token of its `type` member.
static bool attr(cb_span item, const char *name, const char *type, cb_span *tok) {
    cb_span v;
    return member(item, name, &v) && member(v, type, tok);
}

bool dw_get_s(cb_span item, const char *name, char *out, size_t cap) {
    cb_span tok;
    if (!attr(item, name, "S", &tok) || tok.n < 2 || tok.p[0] != '"') return false;
    return cb_json_unescape(tok, out, cap) >= 0;
}

bool dw_get_n(cb_span item, const char *name, int64_t *out) {
    char t[32];
    cb_span tok;
    if (!attr(item, name, "N", &tok) || cb_json_unescape(tok, t, sizeof t) < 1) return false;
    char *end;
    long long v = strtoll(t, &end, 10);
    if (*end) return false;
    *out = v;
    return true;
}

bool dw_get_bool(cb_span item, const char *name, bool *out) {
    cb_span tok;
    if (!attr(item, name, "BOOL", &tok)) return false;
    if (tok.n == 4 && memcmp(tok.p, "true", 4) == 0) *out = true;
    else if (tok.n == 5 && memcmp(tok.p, "false", 5) == 0) *out = false;
    else return false;
    return true;
}

bool dw_get_b(cb_span item, const char *name, uint8_t **out, size_t *len) {
    *out = NULL;
    *len = 0;
    cb_span tok;
    if (!attr(item, name, "B", &tok)) return true;
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

static int64_t get_n_or(cb_span item, const char *name, int64_t none) {
    int64_t v;
    return dw_get_n(item, name, &v) ? v : none;
}

// ---- responses ----------------------------------------------------------------------------

static void error_type(const char *body, size_t n, char *out, size_t cap) {
    char t[160] = "";
    if (!cb_json_get_str(body, n, "__type", t, sizeof t)) out[0] = '\0';
    const char *h = strrchr(t, '#');
    snprintf(out, cap, "%s", h ? h + 1 : t);
}

dw_status dw_classify(int status, const char *body, size_t n) {
    if (status == 200) return DW_OK;
    char t[128];
    error_type(body, n, t, sizeof t);
    if (strcmp(t, "ConditionalCheckFailedException") == 0) return DW_COND_FAILED;
    if (strcmp(t, "TransactionCanceledException") == 0) return DW_TXN_CANCELED;
    if (strcmp(t, "ProvisionedThroughputExceededException") == 0 || strcmp(t, "ThrottlingException") == 0 ||
        strcmp(t, "RequestLimitExceeded") == 0 || strcmp(t, "InternalServerError") == 0 ||
        strcmp(t, "ServiceUnavailable") == 0 || strcmp(t, "TransactionInProgressException") == 0 ||
        strcmp(t, "TransactionConflictException") == 0 || status >= 500)
        return DW_RETRY;
    return DW_FAILED;
}

void dw_error_text(const char *body, size_t n, char *out, size_t cap) {
    char t[128], m[256] = "";
    error_type(body, n, t, sizeof t);
    if (!cb_json_get_str(body, n, "message", m, sizeof m)) cb_json_get_str(body, n, "Message", m, sizeof m);
    snprintf(out, cap, "%s: %s", t[0] ? t : "(no __type)", m);
}

dw_reason dw_cancel_reason(const char *body, size_t n, int index, bool *has_item) {
    char path[64], code[64];
    *has_item = false;
    snprintf(path, sizeof path, "CancellationReasons.%d.Code", index);
    if (!cb_json_get_str(body, n, path, code, sizeof code)) {
        snprintf(path, sizeof path, "CancellationReasons.%d", index);
        return cb_json_find(body, n, path, NULL) == CB_JSON_OBJ ? DW_REASON_NONE : DW_REASON_MISSING;
    }
    snprintf(path, sizeof path, "CancellationReasons.%d.Item", index);
    *has_item = cb_json_find(body, n, path, NULL) == CB_JSON_OBJ;
    if (strcmp(code, "None") == 0) return DW_REASON_NONE;
    if (strcmp(code, "ConditionalCheckFailed") == 0) return DW_REASON_COND;
    if (strcmp(code, "TransactionConflict") == 0) return DW_REASON_CONFLICT;
    return DW_REASON_OTHER;
}

bool dw_cond_failed_has_item(const char *body, size_t n) { return cb_json_find(body, n, "Item", NULL) == CB_JSON_OBJ; }

bool dw_item(const char *body, size_t n, cb_span *item) {
    return cb_json_find(body, n, "Item", item) == CB_JSON_OBJ;
}

bool dw_attributes(const char *body, size_t n, cb_span *item) {
    return cb_json_find(body, n, "Attributes", item) == CB_JSON_OBJ;
}

cb_span dw_items(const char *body, size_t n) {
    cb_span s;
    if (cb_json_find(body, n, "Items", &s) != CB_JSON_ARR) return CB_SPAN_LIT("[]");
    return s;
}

cb_span dw_batch_items(const char *body, size_t n, const char *table) {
    cb_span resp, key, val;
    size_t pos = 0;
    if (cb_json_find(body, n, "Responses", &resp) == CB_JSON_OBJ)
        while (dw_object_next(resp, &pos, &key, &val))
            if (key_is(key, table)) return val;
    return CB_SPAN_LIT("[]");
}

cb_span dw_last_key(const char *body, size_t n) {
    cb_span s;
    if (cb_json_find(body, n, "LastEvaluatedKey", &s) != CB_JSON_OBJ) return cb_span_of("", 0);
    return s;
}

static bool nonempty_object(const char *body, size_t n, const char *path) {
    cb_span s, k, v;
    size_t pos = 0;
    return cb_json_find(body, n, path, &s) == CB_JSON_OBJ && dw_object_next(s, &pos, &k, &v);
}

bool dw_unprocessed(const char *body, size_t n) {
    return nonempty_object(body, n, "UnprocessedItems") || nonempty_object(body, n, "UnprocessedKeys");
}

void dw_next_token(const char *body, size_t n, char *out, size_t cap) {
    if (!cb_json_get_str(body, n, "NextToken", out, cap)) out[0] = '\0';
}

// ---- items as rows --------------------------------------------------------------------------

bool dw_item_user(cb_span it, cb_user_row *u) {
    memset(u, 0, sizeof *u);
    if (!dw_get_s(it, "id", u->id, sizeof u->id) || !dw_get_s(it, "email", u->email, sizeof u->email) ||
        !dw_get_s(it, "password_hash", u->password_hash, sizeof u->password_hash))
        return false;
    dw_get_s(it, "username", u->username, sizeof u->username);
    u->created_at = get_n_or(it, "created_at", 0);
    u->last_sign_in_at = get_n_or(it, "last_sign_in_at", 0);
    return true;
}

bool dw_item_refresh(cb_span it, cb_refresh_row *r) {
    memset(r, 0, sizeof *r);
    if (!dw_get_s(it, "token_hash", r->token_hash, sizeof r->token_hash) ||
        !dw_get_s(it, "user_id", r->user_id, sizeof r->user_id))
        return false;
    dw_get_s(it, "session_id", r->session_id, sizeof r->session_id);
    r->created_at = get_n_or(it, "created_at", 0);
    dw_get_bool(it, "revoked", &r->revoked);
    return true;
}

bool dw_item_game(cb_span it, cb_game_row *g) {
    memset(g, 0, sizeof *g);
    int64_t status, version;
    if (!dw_get_s(it, "id", g->id, sizeof g->id) || !dw_get_n(it, "status", &status) ||
        !dw_get_n(it, "version", &version))
        return false;
    g->status = (int)status;
    g->version = version;
    uint8_t *seed;
    size_t seed_len;
    if (!dw_get_b(it, "state", &g->state, &g->state_len) || !dw_get_b(it, "roster", &g->roster, &g->roster_len) ||
        !dw_get_b(it, "logs_packed", &g->logs, &g->logs_len) || !dw_get_b(it, "game_seed", &seed, &seed_len)) {
        cb_game_row_free(g);
        return false;
    }
    if (seed && seed_len == CB_SEED_LEN) {
        g->has_seed = true;
        memcpy(g->seed, seed, CB_SEED_LEN);
    }
    free(seed);
    dw_get_bool(it, "needs_bots", &g->needs_bots);
    g->round_epoch = get_n_or(it, "round_epoch", 0);
    g->created_at = get_n_or(it, "created_at", 0);
    g->updated_at = get_n_or(it, "updated_at", 0);
    g->last_commit_at = get_n_or(it, "last_commit_at", 0);
    return true;
}

bool dw_item_member(cb_span it, cb_member_row *m) {
    memset(m, 0, sizeof *m);
    m->updated_at = get_n_or(it, "updated_at", 0);
    return dw_get_s(it, "game_id", m->game_id, sizeof m->game_id);
}

bool dw_item_seat_id(cb_span it, cb_uuid id) {
    return dw_get_s(it, "user_id", id, sizeof(cb_uuid)) || dw_get_s(it, "bot_id", id, sizeof(cb_uuid));
}

bool dw_item_bot(cb_span it, cb_bot_row *b) {
    memset(b, 0, sizeof *b);
    if (!dw_get_s(it, "id", b->id, sizeof b->id) || !dw_get_s(it, "nickname", b->nickname, sizeof b->nickname) ||
        !dw_get_s(it, "strategy_key", b->strategy_key, sizeof b->strategy_key))
        return false;
    b->elo_rating = (int32_t)get_n_or(it, "elo_rating", 1000);
    b->previous_elo = (int32_t)get_n_or(it, "previous_elo", -1);
    b->games_played = (int32_t)get_n_or(it, "games_played", 0);
    b->created_at = get_n_or(it, "created_at", 0);
    b->updated_at = get_n_or(it, "updated_at", 0);
    return true;
}

bool dw_item_rating(cb_span it, cb_rating_row *r) {
    memset(r, 0, sizeof *r);
    if (!dw_get_s(it, "user_id", r->user_id, sizeof r->user_id)) return false;
    dw_get_s(it, "username", r->username, sizeof r->username);
    r->elo_rating = (int32_t)get_n_or(it, "elo_rating", 1000);
    r->previous_elo = (int32_t)get_n_or(it, "previous_elo", -1);
    r->games_played = (int32_t)get_n_or(it, "games_played", 0);
    r->updated_at = get_n_or(it, "updated_at", 0);
    return true;
}

bool dw_item_snapshot(cb_span it, cb_snapshot_row *s) {
    memset(s, 0, sizeof *s);
    if (!dw_get_s(it, "id", s->id, sizeof s->id)) return false;
    dw_get_s(it, "game_id", s->game_id, sizeof s->game_id);
    cb_span players, list, e;
    size_t pos = 0;
    if (member(it, "player_ids", &players) && member(players, "L", &list))
        while (s->n_players < CB_MAX_SEATS && dw_array_next(list, &pos, &e)) {
            cb_span tok;
            if (!member(e, "S", &tok) || cb_json_unescape(tok, s->player_ids[s->n_players], sizeof(cb_uuid)) < 0)
                return false;
            s->n_players++;
        }
    if (!dw_get_b(it, "moves", &s->moves, &s->moves_len) || !dw_get_b(it, "extras", &s->extras, &s->extras_len)) {
        cb_snapshot_row_free(s);
        return false;
    }
    s->created_at = get_n_or(it, "created_at", 0);
    return true;
}

bool dw_item_chat(cb_span it, cb_chat_row *c) {
    memset(c, 0, sizeof *c);
    cb_span tok;
    if (!dw_get_s(it, "id", c->id, sizeof c->id) || !dw_get_s(it, "game_id", c->game_id, sizeof c->game_id) ||
        !dw_get_s(it, "user_id", c->user_id, sizeof c->user_id) || !attr(it, "message", "S", &tok))
        return false;
    c->message = malloc(tok.n);
    if (!c->message || cb_json_unescape(tok, c->message, tok.n) < 0) {
        free(c->message);
        c->message = NULL;
        return false;
    }
    dw_get_bool(it, "is_system", &c->is_system);
    c->created_at = get_n_or(it, "created_at", 0);
    return true;
}

bool dw_item_conn(cb_span it, cb_conn_row *c) {
    memset(c, 0, sizeof *c);
    if (!dw_get_s(it, "conn_id", c->conn_id, sizeof c->conn_id)) return false;
    dw_get_s(it, "user_id", c->user_id, sizeof c->user_id);
    c->vsn = (int)get_n_or(it, "vsn", 1);
    c->expires_at = get_n_or(it, "expires_ms", 0);
    cb_span topics, list, e;
    size_t pos = 0;
    if (member(it, "topics", &topics) && member(topics, "L", &list))
        while (c->n_topics < CB_CONN_TOPICS && dw_array_next(list, &pos, &e)) {
            cb_span m;
            if (!member(e, "M", &m) || !dw_get_s(m, "t", c->topics[c->n_topics], sizeof c->topics[0])) return false;
            dw_get_s(m, "r", c->join_refs[c->n_topics], sizeof c->join_refs[0]);
            c->n_topics++;
        }
    return true;
}

bool dw_item_scan(cb_span it, char game_id[CB_GAME_ID_MAX + 1], int64_t *updated_at, int64_t *last_commit_at) {
    *updated_at = get_n_or(it, "updated_at", 0);
    *last_commit_at = get_n_or(it, "last_commit_at", 0);
    return dw_get_s(it, "game_id", game_id, CB_GAME_ID_MAX + 1);
}

bool dw_item_email_user(cb_span it, cb_uuid id) { return dw_get_s(it, "user_id", id, sizeof(cb_uuid)); }

bool dw_item_key(cb_span it, cb_buf *out) {
    cb_span pk, sk;
    if (!member(it, "pk", &pk) || !member(it, "sk", &sk)) return false;
    cb_buf_puts(out, "{\"pk\":");
    cb_buf_append(out, pk.p, pk.n);
    cb_buf_puts(out, ",\"sk\":");
    cb_buf_append(out, sk.p, sk.n);
    cb_buf_putc(out, '}');
    return !out->oom;
}

int dw_item_text(cb_span it, cb_buf *st, const char **names, const char **values, int cap) {
    size_t noff[64], voff[64];
    bool vnull[64];
    int n = 0;
    size_t pos = 0;
    cb_span key, val;
    if (cap > 64) cap = 64;
    cb_buf_reset(st);
    while (n < cap && dw_object_next(it, &pos, &key, &val)) {
        size_t vpos = 0;
        cb_span type, tok;
        if (!dw_object_next(val, &vpos, &type, &tok)) return -1;
        noff[n] = st->len;
        char *tmp = malloc(key.n + 1);
        if (!tmp || cb_json_unescape(key, tmp, key.n + 1) < 0) { free(tmp); return -1; }
        cb_buf_puts(st, tmp);
        cb_buf_putc(st, '\0');
        free(tmp);
        voff[n] = st->len;
        vnull[n] = false;
        if ((key_is(type, "S") || key_is(type, "N")) && tok.n >= 2 && tok.p[0] == '"') {
            tmp = malloc(tok.n);
            if (!tmp || cb_json_unescape(tok, tmp, tok.n) < 0) { free(tmp); return -1; }
            cb_buf_puts(st, tmp);
            free(tmp);
        } else if (key_is(type, "B")) {
            uint8_t *b;
            size_t blen;
            // Reuse the attribute reader on a one-member item.
            cb_buf wrap;
            cb_buf_init(&wrap);
            cb_buf_puts(&wrap, "{\"v\":");
            cb_buf_append(&wrap, val.p, val.n);
            cb_buf_putc(&wrap, '}');
            bool ok = dw_get_b(cb_span_of((const char *)wrap.p, wrap.len), "v", &b, &blen);
            cb_buf_free(&wrap);
            if (!ok) return -1;
            char *hex = malloc(2 * blen + 3);
            if (!hex) { free(b); return -1; }
            cb_bytea_encode(b, blen, hex, 2 * blen + 3);
            cb_buf_puts(st, hex);
            free(hex);
            free(b);
        } else if (key_is(type, "NULL")) {
            vnull[n] = true;
        } else {
            cb_buf_append(st, tok.p, tok.n);   // BOOL's literal, or a list or map as JSON
        }
        cb_buf_putc(st, '\0');
        n++;
    }
    if (st->oom) return -1;
    for (int i = 0; i < n; i++) {
        names[i] = (const char *)st->p + noff[i];
        values[i] = vnull[i] ? NULL : (const char *)st->p + voff[i];
    }
    return n;
}

// ---- point reads ----------------------------------------------------------------------------

static void get_item(dw_req *r, const char *table, const char *pk, const char *sk) {
    cb_jw w;
    begin(r, &w, "GetItem");
    cb_jw_kstr(&w, "TableName", table);
    key2(&w, pk, sk);
    cb_jw_kbool(&w, "ConsistentRead", true);
    finish(&w);
}

void dw_get_user(dw_req *r, const char *table, const char *id) {
    char pk[KB];
    get_item(r, table, k(pk, sizeof pk, "USER#", id), "PROFILE");
}

void dw_get_email(dw_req *r, const char *table, const char *email) {
    char pk[KB];
    get_item(r, table, k(pk, sizeof pk, "EMAIL#", email), "USER");
}

void dw_get_game(dw_req *r, const char *table, const char *id) {
    char pk[KB];
    get_item(r, table, k(pk, sizeof pk, "GAME#", id), "ROW");
}

void dw_get_member(dw_req *r, const char *table, const char *game, const char *user) {
    char pk[KB], sk[KB];
    get_item(r, table, k(pk, sizeof pk, "GAME#", game), k(sk, sizeof sk, "MEMBER#", user));
}

void dw_get_conn(dw_req *r, const char *table, const char *conn_id) {
    char pk[KB];
    get_item(r, table, k(pk, sizeof pk, "CONN#", conn_id), "CONN");
}

// ---- queries ----------------------------------------------------------------------------------

void dw_query(dw_req *r, const char *table, dw_query_kind kind, const char *arg, bool forward, int limit, cb_span start) {
    const char *index = NULL, *pkattr = "pk", *skattr = "sk", *prefix = NULL, *below = NULL;
    const char *pfx = "";
    bool consistent = true, keys_only = false, fwd = true;
    switch (kind) {
    case DWQ_GAME_MEMBERS: pfx = "GAME#"; prefix = "MEMBER#"; break;
    case DWQ_GAME_BOTS: pfx = "GAME#"; prefix = "BOT#"; break;
    // Every seat item sorts before ROW ("BOT#..." < "MEMBER#..." < "ROW"); a
    // Query cannot filter on a key attribute, so the range does it.
    case DWQ_GAME_PARTITION: pfx = "GAME#"; below = "ROW"; keys_only = true; break;
    case DWQ_USER_GAMES: index = GSI1; pfx = "USERGAMES#"; fwd = false; break;
    case DWQ_USER_CHATS: index = GSI1; pfx = "USERCHAT#"; keys_only = true; break;
    case DWQ_USER_TOKENS: index = GSI1; pfx = "USERREFRESH#"; keys_only = true; break;
    case DWQ_SESSION_TOKENS: index = GSI2; pfx = "SESSION#"; keys_only = true; break;
    case DWQ_GAME_SNAPSHOTS: index = GSI1; pfx = "GAMESNAPS#"; keys_only = true; break;
    case DWQ_PLAYER_SNAPSHOTS: index = GSI2; pfx = "PLAYER#"; fwd = forward; break;
    case DWQ_SNAPSHOT_ITEMS: pfx = "SNAP#"; break;
    case DWQ_CHAT: pfx = "CHAT#"; fwd = false; break;
    case DWQ_TOPIC_CONNS: pfx = "TOPIC#"; break;
    case DWQ_SCAN: pfx = "SCAN"; arg = ""; break;
    case DWQ_BOTS: index = GSI2; pfx = "BOTS"; arg = ""; break;
    case DWQ_RATINGS: index = GSI2; pfx = "RATINGS"; arg = ""; break;
    case DWQ_USERS: index = GSI2; pfx = "USERS"; arg = ""; fwd = false; break;
    }
    if (index) {
        consistent = false;   // a GSI is eventually consistent; asking otherwise is an error
        pkattr = index[3] == '1' ? "gsi1pk" : "gsi2pk";
        skattr = index[3] == '1' ? "gsi1sk" : "gsi2sk";
    }
    char pk[KB];
    k(pk, sizeof pk, pfx, arg);
    cb_jw w;
    begin(r, &w, "Query");
    cb_jw_kstr(&w, "TableName", table);
    if (index) cb_jw_kstr(&w, "IndexName", index);
    ux u;
    ux_init(&u);
    ux_name(&u, pkattr);
    ux_val_s(&u, ":pk", pk);
    char kc[160];
    if (prefix) {
        ux_name(&u, skattr);
        ux_val_s(&u, ":sk", prefix);
        snprintf(kc, sizeof kc, "#%s = :pk AND begins_with(#%s, :sk)", pkattr, skattr);
    } else if (below) {
        ux_name(&u, skattr);
        ux_val_s(&u, ":sk", below);
        snprintf(kc, sizeof kc, "#%s = :pk AND #%s < :sk", pkattr, skattr);
    } else snprintf(kc, sizeof kc, "#%s = :pk", pkattr);
    cb_jw_kstr(&w, "KeyConditionExpression", kc);
    if (keys_only) {
        ux_name(&u, "pk");
        ux_name(&u, "sk");
        cb_jw_kstr(&w, "ProjectionExpression", "#pk, #sk");
    }
    ux_emit(&w, &u, NULL);
    ux_free(&u);
    if (!fwd) cb_jw_kbool(&w, "ScanIndexForward", false);
    if (limit > 0) cb_jw_kint(&w, "Limit", limit);
    if (consistent) cb_jw_kbool(&w, "ConsistentRead", true);
    if (start.n) {
        cb_jw_key(&w, "ExclusiveStartKey");
        cb_jw_raw(&w, start.p, start.n);
    }
    finish(&w);
}

// ---- items --------------------------------------------------------------------------------

static void item_profile(cb_jw *w, const cb_user_row *u) {
    char pk[KB], gsk[KB];
    av_s(w, "pk", k(pk, sizeof pk, "USER#", u->id));
    av_s(w, "sk", "PROFILE");
    av_s(w, "gsi2pk", "USERS");
    av_s(w, "gsi2sk", tsk(gsk, sizeof gsk, u->created_at, u->id));
    av_s(w, "id", u->id);
    av_s(w, "email", u->email);
    av_s(w, "password_hash", u->password_hash);
    if (u->username[0]) av_s(w, "username", u->username);
    av_n(w, "created_at", u->created_at);
    if (u->last_sign_in_at) av_n(w, "last_sign_in_at", u->last_sign_in_at);
}

static void item_email(cb_jw *w, const cb_user_row *u) {
    char pk[KB];
    av_s(w, "pk", k(pk, sizeof pk, "EMAIL#", u->email));
    av_s(w, "sk", "USER");
    av_s(w, "user_id", u->id);
}

static void item_rating(cb_jw *w, const cb_rating_row *rt) {
    char pk[KB];
    av_s(w, "pk", k(pk, sizeof pk, "USER#", rt->user_id));
    av_s(w, "sk", "RATING");
    av_s(w, "gsi2pk", "RATINGS");
    av_s(w, "gsi2sk", rt->user_id);
    av_s(w, "user_id", rt->user_id);
    if (rt->username[0]) av_s(w, "username", rt->username);
    av_n(w, "elo_rating", rt->elo_rating);
    if (rt->previous_elo >= 0) av_n(w, "previous_elo", rt->previous_elo);
    av_n(w, "games_played", rt->games_played);
    av_n(w, "updated_at", rt->updated_at);
}

static void item_game(cb_jw *w, const cb_game_row *g) {
    char pk[KB];
    av_s(w, "pk", k(pk, sizeof pk, "GAME#", g->id));
    av_s(w, "sk", "ROW");
    av_s(w, "id", g->id);
    av_n(w, "status", g->status);
    av_b(w, "state", g->state, g->state_len);
    av_b(w, "roster", g->roster, g->roster_len);
    if (g->logs_len) av_b(w, "logs_packed", g->logs, g->logs_len);
    if (g->has_seed) av_b(w, "game_seed", g->seed, CB_SEED_LEN);
    av_bool(w, "needs_bots", g->needs_bots);
    av_n(w, "version", g->version);
    av_n(w, "round_epoch", g->round_epoch);
    av_n(w, "created_at", g->created_at);
    av_n(w, "updated_at", g->updated_at);
    av_n(w, "last_commit_at", g->last_commit_at);
}

static void item_bot(cb_jw *w, const cb_bot_row *b) {
    char pk[KB];
    av_s(w, "pk", k(pk, sizeof pk, "BOT#", b->id));
    av_s(w, "sk", "BOT");
    av_s(w, "gsi2pk", "BOTS");
    av_s(w, "gsi2sk", b->id);
    av_s(w, "id", b->id);
    av_s(w, "nickname", b->nickname);
    av_s(w, "strategy_key", b->strategy_key);
    av_n(w, "elo_rating", b->elo_rating);
    if (b->previous_elo >= 0) av_n(w, "previous_elo", b->previous_elo);
    av_n(w, "games_played", b->games_played);
    av_n(w, "created_at", b->created_at);
    av_n(w, "updated_at", b->updated_at);
}

// ---- transactions ------------------------------------------------------------------------------

static void txn_begin(dw_req *r, cb_jw *w) {
    begin(r, w, "TransactWriteItems");
    cb_jw_key(w, "TransactItems");
    cb_jw_arr(w);
}

static void txn_end(cb_jw *w) {
    cb_jw_end(w);   // TransactItems
    finish(w);
}

// {"Put":{"TableName":..,"Item":{ (the caller writes the attributes, then calls put_end)
static void put_begin(cb_jw *w, const char *table, const char *op_wrapper) {
    if (op_wrapper) {
        cb_jw_obj(w);
        cb_jw_key(w, op_wrapper);
    }
    cb_jw_obj(w);
    cb_jw_kstr(w, "TableName", table);
    cb_jw_key(w, "Item");
    cb_jw_obj(w);
}

static void put_end(cb_jw *w, const char *cond, bool wrapped) {
    cb_jw_end(w);   // Item
    if (cond) cb_jw_kstr(w, "ConditionExpression", cond);
    cb_jw_end(w);
    if (wrapped) cb_jw_end(w);
}

static void txn_delete(cb_jw *w, const char *table, const char *pk, const char *sk) {
    cb_jw_obj(w);
    cb_jw_key(w, "Delete");
    cb_jw_obj(w);
    cb_jw_kstr(w, "TableName", table);
    key2(w, pk, sk);
    cb_jw_end(w);
    cb_jw_end(w);
}

static void txn_delete_raw(cb_jw *w, const char *table, const char *key, size_t n) {
    cb_jw_obj(w);
    cb_jw_key(w, "Delete");
    cb_jw_obj(w);
    cb_jw_kstr(w, "TableName", table);
    cb_jw_key(w, "Key");
    cb_jw_raw(w, key, n);
    cb_jw_end(w);
    cb_jw_end(w);
}

static void txn_check_exists(cb_jw *w, const char *table, const char *pk, const char *sk) {
    cb_jw_obj(w);
    cb_jw_key(w, "ConditionCheck");
    cb_jw_obj(w);
    cb_jw_kstr(w, "TableName", table);
    key2(w, pk, sk);
    cb_jw_kstr(w, "ConditionExpression", "attribute_exists(pk)");
    cb_jw_end(w);
    cb_jw_end(w);
}

// {"Update":{"TableName","Key", expressions}} (or a bare UpdateItem body when
// `wrapped` is false: the caller has the object open).
static void update_open(cb_jw *w, const char *table, const char *pk, const char *sk, bool wrapped) {
    if (wrapped) {
        cb_jw_obj(w);
        cb_jw_key(w, "Update");
        cb_jw_obj(w);
    }
    cb_jw_kstr(w, "TableName", table);
    key2(w, pk, sk);
}

static void update_close(cb_jw *w, bool wrapped) {
    if (wrapped) {
        cb_jw_end(w);
        cb_jw_end(w);
    }
}

// ---- users ----------------------------------------------------------------------------------------

void dw_user_create(dw_req *r, const char *table, const cb_user_row *u, int32_t start) {
    cb_jw w;
    txn_begin(r, &w);
    put_begin(&w, table, "Put");
    item_email(&w, u);
    put_end(&w, "attribute_not_exists(pk)", true);
    put_begin(&w, table, "Put");
    item_profile(&w, u);
    put_end(&w, "attribute_not_exists(pk)", true);
    // The auth.users trigger's row: the start rating, previous_elo the
    // column default (the start too), the username.
    cb_rating_row rt = { .elo_rating = start, .previous_elo = start, .updated_at = u->created_at };
    memcpy(rt.user_id, u->id, sizeof rt.user_id);
    memcpy(rt.username, u->username, sizeof rt.username);
    put_begin(&w, table, "Put");
    item_rating(&w, &rt);
    put_end(&w, NULL, true);
    txn_end(&w);
}

void dw_user_import(dw_req *r, const char *table, const cb_user_row *u) {
    cb_jw w;
    txn_begin(r, &w);
    put_begin(&w, table, "Put");
    item_email(&w, u);
    put_end(&w, NULL, true);
    put_begin(&w, table, "Put");
    item_profile(&w, u);
    put_end(&w, NULL, true);
    txn_end(&w);
}

static void user_update(dw_req *r, const char *table, const char *id, const char *attr_name, const char *s, int64_t n) {
    char pk[KB];
    cb_jw w;
    begin(r, &w, "UpdateItem");
    update_open(&w, table, k(pk, sizeof pk, "USER#", id), "PROFILE", false);
    ux u;
    ux_init(&u);
    if (s) ux_set_s(&u, attr_name, s);
    else ux_set_n(&u, attr_name, n);
    ux_emit(&w, &u, "attribute_exists(pk)");
    ux_free(&u);
    finish(&w);
}

void dw_user_set_password(dw_req *r, const char *table, const char *id, const char *hash) {
    user_update(r, table, id, "password_hash", hash, 0);
}

void dw_user_touch(dw_req *r, const char *table, const char *id, int64_t at) {
    user_update(r, table, id, "last_sign_in_at", NULL, at);
}

void dw_user_delete_txn(dw_req *r, const char *table, const cb_user_row *u) {
    char pk[KB], epk[KB];
    cb_jw w;
    txn_begin(r, &w);
    k(pk, sizeof pk, "USER#", u->id);
    txn_delete(&w, table, pk, "PROFILE");
    txn_delete(&w, table, pk, "RATING");
    txn_delete(&w, table, k(epk, sizeof epk, "EMAIL#", u->email), "USER");
    txn_end(&w);
}

// ---- refresh tokens -----------------------------------------------------------------------------

void dw_refresh_put(dw_req *r, const char *table, const cb_refresh_row *t) {
    char pk[KB], g1[KB], g2[KB];
    cb_jw w;
    begin(r, &w, "PutItem");
    cb_jw_kstr(&w, "TableName", table);
    cb_jw_key(&w, "Item");
    cb_jw_obj(&w);
    av_s(&w, "pk", k(pk, sizeof pk, "REFRESH#", t->token_hash));
    av_s(&w, "sk", "TOKEN");
    av_s(&w, "gsi1pk", k(g1, sizeof g1, "USERREFRESH#", t->user_id));
    av_s(&w, "gsi1sk", t->token_hash);
    av_s(&w, "gsi2pk", k(g2, sizeof g2, "SESSION#", t->session_id));
    av_s(&w, "gsi2sk", t->token_hash);
    av_s(&w, "token_hash", t->token_hash);
    av_s(&w, "user_id", t->user_id);
    av_s(&w, "session_id", t->session_id);
    av_n(&w, "created_at", t->created_at);
    av_bool(&w, "revoked", t->revoked);
    cb_jw_end(&w);
    cb_jw_kstr(&w, "ConditionExpression", "attribute_not_exists(pk)");
    finish(&w);
}

void dw_refresh_take(dw_req *r, const char *table, const char *hash) {
    char pk[KB];
    cb_jw w;
    begin(r, &w, "UpdateItem");
    update_open(&w, table, k(pk, sizeof pk, "REFRESH#", hash), "TOKEN", false);
    ux u;
    ux_init(&u);
    ux_set_bool(&u, "revoked", true);
    ux_emit(&w, &u, "attribute_exists(pk)");
    ux_free(&u);
    cb_jw_kstr(&w, "ReturnValues", "ALL_OLD");
    finish(&w);
}

void dw_refresh_revoke(dw_req *r, const char *table, cb_span key) {
    cb_jw w;
    begin(r, &w, "UpdateItem");
    cb_jw_kstr(&w, "TableName", table);
    cb_jw_key(&w, "Key");
    cb_jw_raw(&w, key.p, key.n);
    ux u;
    ux_init(&u);
    ux_set_bool(&u, "revoked", true);
    ux_emit(&w, &u, "attribute_exists(pk)");
    ux_free(&u);
    finish(&w);
}

// ---- games ----------------------------------------------------------------------------------------

// A seated human's item: the dashboard's GSI1 entry, ordered by the game's
// updated_at. joined_at is kept from the first time the seat was written.
static void member_update(cb_jw *w, const char *table, const char *game, const char *user, int64_t at) {
    char pk[KB], sk[KB], g1[KB], g1s[KB];
    update_open(w, table, k(pk, sizeof pk, "GAME#", game), k(sk, sizeof sk, "MEMBER#", user), true);
    ux u;
    ux_init(&u);
    ux_set_s(&u, "game_id", game);
    ux_set_s(&u, "user_id", user);
    ux_set_n(&u, "updated_at", at);
    ux_set_s(&u, "gsi1pk", k(g1, sizeof g1, "USERGAMES#", user));
    ux_set_s(&u, "gsi1sk", tsk(g1s, sizeof g1s, at, game));
    ux_name(&u, "joined_at");
    ux_set_raw(&u, "#joined_at = if_not_exists(#joined_at, :updated_at)");
    ux_emit(w, &u, NULL);
    ux_free(&u);
    update_close(w, true);
}

static void bot_seat_put(cb_jw *w, const char *table, const char *game, const char *bot) {
    char pk[KB], sk[KB];
    put_begin(w, table, "Put");
    av_s(w, "pk", k(pk, sizeof pk, "GAME#", game));
    av_s(w, "sk", k(sk, sizeof sk, "BOT#", bot));
    av_s(w, "game_id", game);
    av_s(w, "bot_id", bot);
    put_end(w, NULL, true);
}

// The SCAN marker: present (with the times the scan filters on) while the
// game needs bots, absent otherwise.
static void scan_marker(cb_jw *w, const char *table, const cb_game_row *g) {
    char sk[KB];
    k(sk, sizeof sk, "GAME#", g->id);
    if (!g->needs_bots) {
        txn_delete(w, table, "SCAN", sk);
        return;
    }
    put_begin(w, table, "Put");
    av_s(w, "pk", "SCAN");
    av_s(w, "sk", sk);
    av_s(w, "game_id", g->id);
    av_n(w, "updated_at", g->updated_at);
    av_n(w, "last_commit_at", g->last_commit_at);
    put_end(w, NULL, true);
}

void dw_game_create(dw_req *r, const char *table, const cb_game_row *g, const char *creator) {
    cb_jw w;
    txn_begin(r, &w);
    cb_game_row row = *g;
    row.last_commit_at = g->updated_at;   // a new row's last commit is its creation
    put_begin(&w, table, "Put");
    item_game(&w, &row);
    put_end(&w, "attribute_not_exists(pk)", true);
    member_update(&w, table, g->id, creator, g->updated_at);
    if (g->needs_bots) scan_marker(&w, table, &row);
    txn_end(&w);
}

static bool listed(const cb_uuid *ids, int n, const char *id) {
    for (int i = 0; i < n; i++)
        if (strcmp(ids[i], id) == 0) return true;
    return false;
}

// One rated seat's write, fenced on the rating it read. Returns the items it
// added (0 when the seat has nothing to rate: a bot or account that is gone).
static int rating_write(cb_jw *w, const char *table, const cb_game_commit *c, int i, const dw_rating_read *rd,
                        int64_t at) {
    char pk[KB];
    const char *id = c->rated_ids[i];
    bool bot = c->rated_is_bot[i];
    k(pk, sizeof pk, bot ? "BOT#" : "USER#", id);
    const char *sk = bot ? "BOT" : "RATING";
    int32_t cur = rd->elo, delta = c->rating_delta[i];
    if (!rd->exists) {
        if (bot || !rd->account) return 0;
        // A human with no row yet starts from the start rating, if the account
        // is still there (checked in the same transaction).
        int64_t v = (int64_t)1000 + delta;
        cb_rating_row rt = { .elo_rating = (int32_t)(v < 0 ? 0 : v), .previous_elo = 1000, .games_played = 1,
                             .updated_at = at };
        snprintf(rt.user_id, sizeof rt.user_id, "%s", id);
        snprintf(rt.username, sizeof rt.username, "%s", c->rated_names[i]);
        put_begin(w, table, "Put");
        item_rating(w, &rt);
        put_end(w, "attribute_not_exists(pk)", true);
        txn_check_exists(w, table, pk, "PROFILE");
        return 2;
    }
    int64_t v = (int64_t)cur + delta;
    update_open(w, table, pk, sk, true);
    ux u;
    ux_init(&u);
    ux_set_n(&u, "elo_rating", v < 0 ? 0 : v);
    ux_set_n(&u, "previous_elo", cur);
    ux_set_n(&u, "updated_at", at);
    ux_name(&u, "games_played");
    ux_val_n(&u, ":one", 1);
    ux_set_raw(&u, "#games_played = #games_played + :one");
    ux_val_n(&u, ":read", cur);
    ux_emit(w, &u, "#elo_rating = :read");
    ux_free(&u);
    update_close(w, true);
    return 1;
}

bool dw_game_commit(dw_req *r, const char *table, const cb_game_commit *c, const dw_seats *stored,
                    const dw_rating_read *ratings, bool import) {
    const cb_game_row *g = c->row;
    cb_game_row row = *g;
    row.last_commit_at = import ? g->last_commit_at : g->updated_at;   // a commit's time is its last commit
    int items = 0;
    cb_jw w;
    txn_begin(r, &w);
    char pk[KB], sk[KB];
    k(pk, sizeof pk, "GAME#", g->id);
    if (import) {
        put_begin(&w, table, "Put");
        item_game(&w, &row);
        put_end(&w, NULL, true);
    } else {
        // Item 0, THE CAS: every scalar and blob, fenced on the version read.
        update_open(&w, table, pk, "ROW", true);
        ux u;
        ux_init(&u);
        ux_set_n(&u, "status", g->status);
        ux_set_b(&u, "state", g->state, g->state_len);
        ux_set_b(&u, "roster", g->roster, g->roster_len);
        if (g->logs_len) ux_set_b(&u, "logs_packed", g->logs, g->logs_len);
        else ux_remove(&u, "logs_packed");
        if (g->has_seed) ux_set_b(&u, "game_seed", g->seed, CB_SEED_LEN);
        else ux_remove(&u, "game_seed");
        ux_set_bool(&u, "needs_bots", g->needs_bots);
        ux_set_n(&u, "version", g->version);
        ux_set_n(&u, "round_epoch", g->round_epoch);
        ux_set_n(&u, "updated_at", g->updated_at);
        ux_set_n(&u, "last_commit_at", row.last_commit_at);
        ux_val_n(&u, ":expected", c->expected_version);
        ux_emit(&w, &u, "#version = :expected");
        ux_free(&u);
        cb_jw_kstr(&w, "ReturnValuesOnConditionCheckFailure", "ALL_OLD");
        update_close(&w, true);
    }
    items++;
    for (int i = 0; i < c->n_humans; i++, items++) member_update(&w, table, g->id, c->humans[i], g->updated_at);
    for (int i = 0; stored && i < stored->n_humans; i++)
        if (!listed(c->humans, c->n_humans, stored->humans[i])) {
            txn_delete(&w, table, pk, k(sk, sizeof sk, "MEMBER#", stored->humans[i]));
            items++;
        }
    for (int i = 0; i < c->n_bots; i++)
        if (!stored || !listed(stored->bots, stored->n_bots, c->bots[i])) {
            bot_seat_put(&w, table, g->id, c->bots[i]);
            items++;
        }
    for (int i = 0; stored && i < stored->n_bots; i++)
        if (!listed(c->bots, c->n_bots, stored->bots[i])) {
            txn_delete(&w, table, pk, k(sk, sizeof sk, "BOT#", stored->bots[i]));
            items++;
        }
    for (int i = 0; ratings && i < c->n_rated; i++) {
        bool dup = false;   // one write per item: a repeated id is rated once
        for (int j = 0; j < i; j++) dup |= strcmp(c->rated_ids[j], c->rated_ids[i]) == 0;
        if (!dup) items += rating_write(&w, table, c, i, &ratings[i], g->updated_at);
    }
    scan_marker(&w, table, &row);
    items++;
    txn_end(&w);
    return items <= 100 && cb_jw_ok(&w);
}

void dw_ratings_get(dw_req *r, const char *table, const char *const *ids, const bool *is_bot, int n, bool both) {
    char pk[KB];
    cb_jw w;
    begin(r, &w, "BatchGetItem");
    cb_jw_key(&w, "RequestItems");
    cb_jw_obj(&w);
    cb_jw_key(&w, table);
    cb_jw_obj(&w);
    cb_jw_key(&w, "Keys");
    cb_jw_arr(&w);
    for (int i = 0; i < n; i++) {
        bool dup = false;   // BatchGetItem refuses a repeated key
        for (int j = 0; j < i; j++) dup |= strcmp(ids[j], ids[i]) == 0 && (both || is_bot[j] == is_bot[i]);
        if (dup) continue;
        if (both || is_bot[i]) {
            cb_jw_obj(&w);
            av_s(&w, "pk", k(pk, sizeof pk, "BOT#", ids[i]));
            av_s(&w, "sk", "BOT");
            cb_jw_end(&w);
        }
        if (both || !is_bot[i]) {
            k(pk, sizeof pk, "USER#", ids[i]);
            cb_jw_obj(&w);
            av_s(&w, "pk", pk);
            av_s(&w, "sk", "RATING");
            cb_jw_end(&w);
            if (!both) {
                cb_jw_obj(&w);
                av_s(&w, "pk", pk);
                av_s(&w, "sk", "PROFILE");
                cb_jw_end(&w);
            }
        }
    }
    cb_jw_end(&w);
    cb_jw_kbool(&w, "ConsistentRead", true);
    cb_jw_kstr(&w, "ProjectionExpression", "pk, sk, elo_rating");
    cb_jw_end(&w);
    cb_jw_end(&w);
    finish(&w);
}

void dw_ratings_read(const char *body, size_t n, const char *table, const char *const *ids, const bool *is_bot,
                     int count, bool both, dw_rating_read *out) {
    cb_span items = dw_batch_items(body, n, table);
    bool from_bot[64] = { false };
    for (int i = 0; i < count; i++) memset(&out[i], 0, sizeof out[i]);
    size_t pos = 0;
    cb_span it;
    while (dw_array_next(items, &pos, &it)) {
        char pk[KB], sk[16];
        int64_t elo = 0;
        if (!dw_get_s(it, "pk", pk, sizeof pk) || !dw_get_s(it, "sk", sk, sizeof sk) || !strchr(pk, '#')) continue;
        bool has_elo = dw_get_n(it, "elo_rating", &elo);
        bool bot = strncmp(pk, "BOT#", 4) == 0;
        const char *id = strchr(pk, '#') + 1;
        for (int i = 0; i < count && i < 64; i++) {
            if (strcmp(ids[i], id) != 0 || (!both && bot != is_bot[i])) continue;
            if (strcmp(sk, "PROFILE") == 0) {
                out[i].account = true;
                continue;
            }
            // Both kinds asked: a bot's row wins over a user's, whichever came
            // first (seat_ratings' COALESCE(b.elo_rating, u.elo_rating) order).
            if (!has_elo || (both && from_bot[i] && !bot)) continue;
            out[i].exists = true;
            out[i].elo = (int32_t)elo;
            from_bot[i] = bot;
        }
    }
}

bool dw_game_delete(dw_req *r, const char *table, const char *id, int64_t expected, const cb_buf *keys, int n_keys) {
    char pk[KB], sk[KB];
    cb_jw w;
    txn_begin(r, &w);
    // Item 0: the row, fenced on the version (ALL_OLD tells gone from moved).
    cb_jw_obj(&w);
    cb_jw_key(&w, "Delete");
    cb_jw_obj(&w);
    cb_jw_kstr(&w, "TableName", table);
    key2(&w, k(pk, sizeof pk, "GAME#", id), "ROW");
    ux u;
    ux_init(&u);
    ux_name(&u, "version");
    ux_val_n(&u, ":expected", expected);
    ux_emit(&w, &u, "#version = :expected");
    ux_free(&u);
    cb_jw_kstr(&w, "ReturnValuesOnConditionCheckFailure", "ALL_OLD");
    cb_jw_end(&w);
    cb_jw_end(&w);
    for (int i = 0; i < n_keys; i++) txn_delete_raw(&w, table, (const char *)keys[i].p, keys[i].len);
    txn_delete(&w, table, "SCAN", k(sk, sizeof sk, "GAME#", id));
    txn_end(&w);
    return n_keys + 2 <= 100 && cb_jw_ok(&w);
}

void dw_snapshot_unlink(dw_req *r, const char *table, cb_span key) {
    cb_jw w;
    begin(r, &w, "UpdateItem");
    cb_jw_kstr(&w, "TableName", table);
    cb_jw_key(&w, "Key");
    cb_jw_raw(&w, key.p, key.n);
    ux u;
    ux_init(&u);
    ux_set_s(&u, "game_id", "");
    ux_remove(&u, "gsi1pk");
    ux_remove(&u, "gsi1sk");
    ux_emit(&w, &u, "attribute_exists(pk)");
    ux_free(&u);
    finish(&w);
}

// ---- the lease ------------------------------------------------------------------------------------

static void lease_update(dw_req *r, const char *table, const char *game, cb_jw *w, ux *u) {
    char pk[KB];
    begin(r, w, "UpdateItem");
    update_open(w, table, k(pk, sizeof pk, "GAME#", game), "ROW", false);
    ux_init(u);
}

void dw_lease_acquire(dw_req *r, const char *table, const char *game, const char *token, int64_t now, int64_t ttl) {
    cb_jw w;
    ux u;
    lease_update(r, table, game, &w, &u);
    ux_set_s(&u, "lease_token", token);
    ux_set_n(&u, "lease_until", now + ttl);
    ux_set_n(&u, "updated_at", now);
    // Free when never taken, released (removed) or run out: lease_until < now.
    ux_emit(&w, &u, "attribute_exists(pk) AND (attribute_not_exists(#lease_until) OR #lease_until < :updated_at)");
    ux_free(&u);
    cb_jw_kstr(&w, "ReturnValuesOnConditionCheckFailure", "ALL_OLD");
    finish(&w);
}

void dw_lease_renew(dw_req *r, const char *table, const char *game, const char *token, int64_t now, int64_t ttl) {
    cb_jw w;
    ux u;
    lease_update(r, table, game, &w, &u);
    ux_set_n(&u, "lease_until", now + ttl);
    ux_set_n(&u, "updated_at", now);
    ux_name(&u, "lease_token");
    ux_val_s(&u, ":token", token);
    ux_emit(&w, &u, "#lease_token = :token");
    ux_free(&u);
    finish(&w);
}

void dw_lease_release(dw_req *r, const char *table, const char *game, const char *token) {
    cb_jw w;
    ux u;
    lease_update(r, table, game, &w, &u);
    ux_remove(&u, "lease_until");
    ux_name(&u, "lease_token");
    ux_val_s(&u, ":token", token);
    ux_emit(&w, &u, "#lease_token = :token");
    ux_free(&u);
    finish(&w);
}

void dw_scan_touch(dw_req *r, const char *table, const char *game, int64_t now) {
    char sk[KB];
    cb_jw w;
    begin(r, &w, "UpdateItem");
    update_open(&w, table, "SCAN", k(sk, sizeof sk, "GAME#", game), false);
    ux u;
    ux_init(&u);
    ux_set_n(&u, "updated_at", now);
    ux_emit(&w, &u, "attribute_exists(pk)");
    ux_free(&u);
    finish(&w);
}

// ---- ratings and bots --------------------------------------------------------------------------------

void dw_rating_set_username(dw_req *r, const char *table, const char *user, const char *username) {
    char pk[KB];
    cb_jw w;
    begin(r, &w, "UpdateItem");
    update_open(&w, table, k(pk, sizeof pk, "USER#", user), "RATING", false);
    ux u;
    ux_init(&u);
    if (username && *username) ux_set_s(&u, "username", username);
    else ux_remove(&u, "username");
    ux_emit(&w, &u, "attribute_exists(pk)");
    ux_free(&u);
    finish(&w);
}

void dw_rating_import(dw_req *r, const char *table, const cb_rating_row *rt) {
    cb_jw w;
    begin(r, &w, "PutItem");
    cb_jw_kstr(&w, "TableName", table);
    cb_jw_key(&w, "Item");
    cb_jw_obj(&w);
    item_rating(&w, rt);
    cb_jw_end(&w);
    finish(&w);
}

bool dw_bots_replace(dw_req *r, const char *table, const cb_bot_row *rows, int n, const cb_uuid *stored, int n_stored) {
    char pk[KB];
    int items = 0;
    cb_jw w;
    txn_begin(r, &w);
    for (int i = 0; i < n_stored; i++) {
        bool kept = false;
        for (int j = 0; j < n && !kept; j++) kept = strcmp(rows[j].id, stored[i]) == 0;
        if (kept) continue;
        txn_delete(&w, table, k(pk, sizeof pk, "BOT#", stored[i]), "BOT");
        items++;
    }
    for (int i = 0; i < n; i++, items++) {
        put_begin(&w, table, "Put");
        item_bot(&w, &rows[i]);
        put_end(&w, NULL, true);
    }
    txn_end(&w);
    return items <= 100 && cb_jw_ok(&w);
}

// ---- snapshots, chat, connections ----------------------------------------------------------------------

static void snapshot_attrs(cb_jw *w, const cb_snapshot_row *s) {
    char g1[KB], ts[KB];
    if (s->game_id[0]) {
        av_s(w, "gsi1pk", k(g1, sizeof g1, "GAMESNAPS#", s->game_id));
        av_s(w, "gsi1sk", tsk(ts, sizeof ts, s->created_at, s->id));
    }
    av_s(w, "id", s->id);
    av_s(w, "game_id", s->game_id);
    cb_jw_key(w, "player_ids");
    cb_jw_obj(w);
    cb_jw_key(w, "L");
    cb_jw_arr(w);
    for (int i = 0; i < s->n_players; i++) {
        cb_jw_obj(w);
        cb_jw_kstr(w, "S", s->player_ids[i]);
        cb_jw_end(w);
    }
    cb_jw_end(w);
    cb_jw_end(w);
    av_b(w, "moves", s->moves, s->moves_len);
    if (s->extras_len) av_b(w, "extras", s->extras, s->extras_len);
    av_n(w, "created_at", s->created_at);
}

// The replay and a copy per player, in one transaction (so a reader never
// sees half of one), the main item refusing a reused id.
void dw_snapshot_put(dw_req *r, const char *table, const cb_snapshot_row *s) {
    char pk[KB], sk[KB], g2[KB], ts[KB];
    cb_jw w;
    txn_begin(r, &w);
    k(pk, sizeof pk, "SNAP#", s->id);
    put_begin(&w, table, "Put");
    av_s(&w, "pk", pk);
    av_s(&w, "sk", "SNAP");
    snapshot_attrs(&w, s);
    put_end(&w, "attribute_not_exists(pk)", true);
    for (int i = 0; i < s->n_players; i++) {
        bool dup = false;
        for (int j = 0; j < i; j++) dup |= strcmp(s->player_ids[j], s->player_ids[i]) == 0;
        if (dup) continue;
        put_begin(&w, table, "Put");
        av_s(&w, "pk", pk);
        av_s(&w, "sk", k(sk, sizeof sk, "PLAYER#", s->player_ids[i]));
        av_s(&w, "gsi2pk", k(g2, sizeof g2, "PLAYER#", s->player_ids[i]));
        av_s(&w, "gsi2sk", tsk(ts, sizeof ts, s->created_at, s->id));
        snapshot_attrs(&w, s);
        put_end(&w, NULL, true);
    }
    txn_end(&w);
}

void dw_chat_put(dw_req *r, const char *table, const cb_chat_row *c) {
    char pk[KB], sk[KB], g1[KB], gpk[KB], upk[KB];
    cb_jw w;
    txn_begin(r, &w);
    put_begin(&w, table, "Put");
    av_s(&w, "pk", k(pk, sizeof pk, "CHAT#", c->game_id));
    av_s(&w, "sk", tsk(sk, sizeof sk, c->created_at, c->id));
    av_s(&w, "gsi1pk", k(g1, sizeof g1, "USERCHAT#", c->user_id));
    av_s(&w, "gsi1sk", sk);
    av_s(&w, "id", c->id);
    av_s(&w, "game_id", c->game_id);
    av_s(&w, "user_id", c->user_id);
    av_s(&w, "message", c->message ? c->message : "");
    av_bool(&w, "is_system", c->is_system);
    av_n(&w, "created_at", c->created_at);
    put_end(&w, "attribute_not_exists(pk)", true);
    txn_check_exists(&w, table, k(gpk, sizeof gpk, "GAME#", c->game_id), "ROW");
    txn_check_exists(&w, table, k(upk, sizeof upk, "USER#", c->user_id), "PROFILE");
    txn_end(&w);
}

static void conn_attrs(cb_jw *w, const cb_conn_row *c) {
    av_s(w, "conn_id", c->conn_id);
    if (c->user_id[0]) av_s(w, "user_id", c->user_id);
    av_n(w, "vsn", c->vsn);
    cb_jw_key(w, "topics");
    cb_jw_obj(w);
    cb_jw_key(w, "L");
    cb_jw_arr(w);
    for (int i = 0; i < c->n_topics; i++) {
        cb_jw_obj(w);
        cb_jw_key(w, "M");
        cb_jw_obj(w);
        av_s(w, "t", c->topics[i]);
        av_s(w, "r", c->join_refs[i]);
        cb_jw_end(w);
        cb_jw_end(w);
    }
    cb_jw_end(w);
    cb_jw_end(w);
    av_n(w, "expires_ms", c->expires_at);
    av_n(w, "expires_at", c->expires_at / 1000);   // the TTL attribute is in seconds
}

static bool has_topic(const cb_conn_row *c, const char *t) {
    for (int i = 0; c && i < c->n_topics; i++)
        if (strcmp(c->topics[i], t) == 0) return true;
    return false;
}

// BatchWriteItem: the CONN item, a full copy per topic (a fan-out reads one
// partition and has all it needs), deletes for the topics left.
void dw_conn_put(dw_req *r, const char *table, const cb_conn_row *c, const cb_conn_row *old) {
    char pk[KB], tpk[KB], sk[KB];
    cb_jw w;
    begin(r, &w, "BatchWriteItem");
    cb_jw_key(&w, "RequestItems");
    cb_jw_obj(&w);
    cb_jw_key(&w, table);
    cb_jw_arr(&w);
    k(pk, sizeof pk, "CONN#", c->conn_id);
    cb_jw_obj(&w);
    cb_jw_key(&w, "PutRequest");
    cb_jw_obj(&w);
    cb_jw_key(&w, "Item");
    cb_jw_obj(&w);
    av_s(&w, "pk", pk);
    av_s(&w, "sk", "CONN");
    conn_attrs(&w, c);
    cb_jw_end(&w);
    cb_jw_end(&w);
    cb_jw_end(&w);
    for (int i = 0; i < c->n_topics; i++) {
        bool dup = false;
        for (int j = 0; j < i; j++) dup |= strcmp(c->topics[j], c->topics[i]) == 0;
        if (dup) continue;
        cb_jw_obj(&w);
        cb_jw_key(&w, "PutRequest");
        cb_jw_obj(&w);
        cb_jw_key(&w, "Item");
        cb_jw_obj(&w);
        av_s(&w, "pk", k(tpk, sizeof tpk, "TOPIC#", c->topics[i]));
        av_s(&w, "sk", pk);
        conn_attrs(&w, c);
        cb_jw_end(&w);
        cb_jw_end(&w);
        cb_jw_end(&w);
    }
    for (int i = 0; old && i < old->n_topics; i++) {
        if (has_topic(c, old->topics[i])) continue;
        cb_jw_obj(&w);
        cb_jw_key(&w, "DeleteRequest");
        cb_jw_obj(&w);
        key2(&w, k(tpk, sizeof tpk, "TOPIC#", old->topics[i]), k(sk, sizeof sk, "CONN#", c->conn_id));
        cb_jw_end(&w);
        cb_jw_end(&w);
    }
    cb_jw_end(&w);
    cb_jw_end(&w);
    finish(&w);
}

void dw_conn_delete(dw_req *r, const char *table, const cb_conn_row *c) {
    char pk[KB], tpk[KB];
    cb_jw w;
    begin(r, &w, "BatchWriteItem");
    cb_jw_key(&w, "RequestItems");
    cb_jw_obj(&w);
    cb_jw_key(&w, table);
    cb_jw_arr(&w);
    k(pk, sizeof pk, "CONN#", c->conn_id);
    cb_jw_obj(&w);
    cb_jw_key(&w, "DeleteRequest");
    cb_jw_obj(&w);
    key2(&w, pk, "CONN");
    cb_jw_end(&w);
    cb_jw_end(&w);
    for (int i = 0; i < c->n_topics; i++) {
        bool dup = false;
        for (int j = 0; j < i; j++) dup |= strcmp(c->topics[j], c->topics[i]) == 0;
        if (dup) continue;
        cb_jw_obj(&w);
        cb_jw_key(&w, "DeleteRequest");
        cb_jw_obj(&w);
        key2(&w, k(tpk, sizeof tpk, "TOPIC#", c->topics[i]), pk);
        cb_jw_end(&w);
        cb_jw_end(&w);
    }
    cb_jw_end(&w);
    cb_jw_end(&w);
    finish(&w);
}

void dw_batch_delete(dw_req *r, const char *table, const cb_buf *keys, int n) {
    cb_jw w;
    begin(r, &w, "BatchWriteItem");
    cb_jw_key(&w, "RequestItems");
    cb_jw_obj(&w);
    cb_jw_key(&w, table);
    cb_jw_arr(&w);
    for (int i = 0; i < n; i++) {
        cb_jw_obj(&w);
        cb_jw_key(&w, "DeleteRequest");
        cb_jw_obj(&w);
        cb_jw_key(&w, "Key");
        cb_jw_raw(&w, (const char *)keys[i].p, keys[i].len);
        cb_jw_end(&w);
        cb_jw_end(&w);
    }
    cb_jw_end(&w);
    cb_jw_end(&w);
    finish(&w);
}

// ---- admin -----------------------------------------------------------------------------------------------

static const char *const FAMILIES[] = { "USER", "EMAIL", "REFRESH", "GAME", "BOT", "SNAP", "CHAT", "CONN", "TOPIC", "SCAN" };
#define N_FAMILIES (int)(sizeof FAMILIES / sizeof FAMILIES[0])

const char *dw_collection(int i) { return i >= 0 && i < N_FAMILIES ? FAMILIES[i] : NULL; }

bool dw_collection_valid(const char *c) {
    for (int i = 0; c && i < N_FAMILIES; i++)
        if (strcmp(FAMILIES[i], c) == 0) return true;
    return false;
}

void dw_scan_family(dw_req *r, const char *table, const char *family, const char *prefix, int limit, cb_span start) {
    char p[KB];
    // SCAN is a whole partition; every other family is "<FAMILY>#...".
    snprintf(p, sizeof p, "%s%s%s", family, strcmp(family, "SCAN") == 0 ? "" : "#", prefix ? prefix : "");
    cb_jw w;
    begin(r, &w, "Scan");
    cb_jw_kstr(&w, "TableName", table);
    ux u;
    ux_init(&u);
    ux_name(&u, "pk");
    ux_val_s(&u, ":p", p);
    cb_jw_kstr(&w, "FilterExpression", "begins_with(#pk, :p)");
    ux_emit(&w, &u, NULL);
    ux_free(&u);
    if (limit > 0) cb_jw_kint(&w, "Limit", limit);
    if (start.n) {
        cb_jw_key(&w, "ExclusiveStartKey");
        cb_jw_raw(&w, start.p, start.n);
    }
    finish(&w);
}

void dw_execute(dw_req *r, const char *statement, const char *next_token) {
    cb_jw w;
    begin(r, &w, "ExecuteStatement");
    cb_jw_kstr(&w, "Statement", statement);
    cb_jw_kbool(&w, "ConsistentRead", false);
    if (next_token && *next_token) cb_jw_kstr(&w, "NextToken", next_token);
    finish(&w);
}

bool dw_statement_reads(const char *s) {
    while (s && isspace((unsigned char)*s)) s++;
    return s && strncasecmp(s, "SELECT", 6) == 0 && (s[6] == '\0' || isspace((unsigned char)s[6]));
}
