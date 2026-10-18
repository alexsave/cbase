// json_surface.c - every JSON shape c_base speaks (json_surface.h says why
// they are all here). The only file that includes util/json.h.
#include "json_surface.h"
#include "store/store_priv.h"

#include "jobs.h"
#include "config.h"

#include "util/err.h"
#include "util/log.h"
#include "../../shared/c/num_parse.h"
#include "util/clock.h"
#include "realtime/realtime.h"
#include "realtime/stream_mqtt.h"
#include "../../shared/c/b64/b64.h"
#include "net/http.h"
#include "util/bytea.h"
#include "util/json.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// =====================================================================================
// DynamoDB. Every DynamoDB string in c_base is in this section: the targets, the
// item layout, the expressions.

#define TARGET "DynamoDB_20120810."
#define GSI1 DW_GSI1
#define GSI2 DW_GSI2

void dw_req_init(dw_req *r) {
    r->target[0] = '\0';
    cb_buf_init(&r->body);
}

void dw_req_free(dw_req *r) { cb_buf_free(&r->body); }

void dw_host(const char *region, char *out, size_t cap) { snprintf(out, cap, "dynamodb.%s.amazonaws.com", region); }
const char *dw_content_type(void) { return "application/x-amz-json-1.0"; }
const char *dw_service(void) { return "dynamodb"; }

// Starts a request: the target, an empty body, the writer on it with the
// top-level object open. Every request asks for its ConsumedCapacity, which
// the per-thread tally adds up (store.h cb_store_tally): what each action
// costs in capacity units, in the debug log, at no charge.
static void begin(dw_req *r, cb_jw *w, const char *op) {
    snprintf(r->target, sizeof r->target, TARGET "%s", op);
    cb_buf_reset(&r->body);
    cb_jw_init(w, &r->body);
    cb_jw_obj(w);
    cb_jw_kstr(w, "ReturnConsumedCapacity", "TOTAL");
}

static void finish(cb_jw *w) { cb_jw_end(w); }

// ---- keys -------------------------------------------------------------------------------

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
    cb_jw_key(w, name);
    cb_jw_obj(w);
    cb_jw_kb64(w, "B", p, n);
    cb_jw_end(w);
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
    cb_jw *w = ux_val(u, name);
    cb_jw_obj(w);
    cb_jw_kb64(w, "B", p, n);
    cb_jw_end(w);
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

// The end of the string token whose opening quote is at i, or 0 when it is
// not closed. A quote closes the string unless an odd run of backslashes ends
// right before it (each backslash escapes the byte after it, so a run pairs off
// from its start): memchr finds the candidates, and a base64 blob, the bulk of
// every game row, is crossed at memchr's speed instead of a byte at a time.
// Linear: every backslash belongs to the run before one quote.
static size_t skip_string_at(cb_span s, size_t i) {
    size_t from = i + 1;
    while (from < s.n) {
        const char *q = memchr(s.p + from, '"', s.n - from);
        if (!q) return 0;
        size_t at = (size_t)(q - s.p), b = at;
        while (b > i + 1 && s.p[b - 1] == '\\') b--;
        if (((at - b) & 1) == 0) return at + 1;
        from = at + 1;
    }
    return 0;
}

// The end of the value starting at i (after whitespace), or 0 when malformed.
static size_t skip_value(cb_span s, size_t i, int depth) {
    i = skip_ws(s, i);
    if (i >= s.n || depth > 64) return 0;
    char c = s.p[i];
    if (c == '"') return skip_string_at(s, i);
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

// ---- the key-capture proof (json_surface.h dw_shape) ------------------------------------------

// THE TEMPLATES each key attribute's values are written from (the item
// layout, "THE ITEM LAYOUT" in json_surface.h): '#'-separated slots, each a
// literal word or "{}" for a value the row supplies. A value is normalized to
// the template whose literal slots it matches, its other slots printed as
// "{}" without ever being looked at: a game id is a short code that may be
// all digits, all letters, or a word (FOOLISH.CARDS/ABC123), so no class may
// come from what an id looks like. Several templates may share a shape
// ("{}#{}": a chat's sort key, a seat's dashboard key); they read the same.
static const char *const KEY_TEMPLATES[][16] = {
    /* pk */     { "USER#{}", "EMAIL#{}", "REFRESH#{}", "SESSION#{}", "AUTHFAIL#{}", "GAME#{}", "BOT#{}", "SNAP#{}", "CHAT#{}", "SCAN" },
    /* sk */     { "PROFILE", "RATING", "USER", "TOKEN", "SESSION", "FAILS", "ROW", "BOT", "SNAP", "MEMBER#{}", "BOT#{}", "PLAYER#{}", "GAME#{}", "{}#{}" },
    /* gsi1pk */ { "USERGAMES#{}", "USERCHAT#{}", "USERREFRESH#{}", "GAMESNAPS#{}" },
    /* gsi1sk */ { "{}#{}", "{}" },
    /* gsi2pk */ { "RATINGS", "BOTS", "USERS", "PLAYER#{}" },
    /* gsi2sk */ { "E#{}#{}", "{}#{}", "{}" },
};
static const char *const KEY_ATTRS[] = { "pk", "sk", "gsi1pk", "gsi1sk", "gsi2pk", "gsi2sk" };
#define N_KEY_ATTRS 6

// Which key attribute (an index into KEY_ATTRS) a name is, or -1.
static int key_attr_of(const char *name, size_t n) {
    for (int i = 0; i < N_KEY_ATTRS; i++)
        if (strlen(KEY_ATTRS[i]) == n && memcmp(KEY_ATTRS[i], name, n) == 0) return i;
    return -1;
}

// Does value v[0..n) fill template tp, slot by slot? `prefix`: v may stop
// part way (a begins_with), its last slot cut short.
static bool fits(const char *tp, const char *v, size_t n, bool prefix) {
    size_t vi = 0;
    while (*tp) {
        const char *te = strchr(tp, '#');
        size_t tl = te ? (size_t)(te - tp) : strlen(tp);
        const char *ve = memchr(v + vi, '#', n - vi);
        size_t vl = ve ? (size_t)(ve - (v + vi)) : n - vi;
        bool last_v = !ve;
        bool var = tl == 2 && tp[0] == '{';
        if (!var) {
            if (prefix && last_v) { if (vl > tl || memcmp(tp, v + vi, vl) != 0) return false; }
            else if (vl != tl || memcmp(tp, v + vi, tl) != 0) return false;
        }
        if (last_v) return prefix || !te;   // the value ends: so must the template, unless it is a prefix
        if (!te) return false;              // the template ends, the value goes on
        vi += vl + 1;
        tp = te + 1;
    }
    return false;
}

static int literals(const char *tp) {
    int n = 0;
    for (const char *s = tp; *s;) {
        const char *e = strchr(s, '#');
        size_t l = e ? (size_t)(e - s) : strlen(s);
        n += !(l == 2 && s[0] == '{');
        s = e ? e + 1 : s + l;
    }
    return n;
}

// The template a key attribute's value fills, into o ("?" when none does: a
// key the layout does not know, which the proof then shows). With `prefix`
// (a begins_with or a bound) the value may stop part way. Of several that
// fit, the one with the most literal slots: "E#" begins a rating's rank key,
// not a two-slot "{}#{}".
static void shape_value(cb_buf *o, int attr, cb_span tok, bool prefix) {
    char v[1024];
    int n = cb_json_unescape(tok, v, sizeof v);
    if (n < 0 || attr < 0) { cb_buf_puts(o, "?"); return; }
    int best = -1, best_lit = -1;
    bool best_part = false;
    for (int i = 0; i < 16 && KEY_TEMPLATES[attr][i]; i++) {
        const char *tp = KEY_TEMPLATES[attr][i];
        bool whole = fits(tp, v, (size_t)n, false), part = !whole && prefix && fits(tp, v, (size_t)n, true);
        int lit = literals(tp);
        if ((whole || part) && lit > best_lit) {
            best = i;
            best_lit = lit;
            best_part = part;
        }
    }
    if (best < 0) cb_buf_puts(o, "?");
    else cb_buf_printf(o, "%s%s", KEY_TEMPLATES[attr][best], best_part ? "(prefix)" : "");
}

// A sorted, de-duplicated set of short strings, joined by ','.
typedef struct {
    char s[64][160];
    int  n;
} shape_set;

static void set_add(shape_set *z, const char *v, size_t n) {
    if (n >= sizeof z->s[0]) n = sizeof z->s[0] - 1;
    char t[160];
    memcpy(t, v, n);
    t[n] = 0;
    for (int i = 0; i < z->n; i++)
        if (strcmp(z->s[i], t) == 0) return;
    if (z->n < 64) memcpy(z->s[z->n++], t, n + 1);
}

static int set_cmp(const void *a, const void *b) { return strcmp(a, b); }

static void set_put(cb_buf *o, const char *label, shape_set *z) {
    qsort(z->s, (size_t)z->n, sizeof z->s[0], set_cmp);
    cb_buf_printf(o, " %s=", label);
    for (int i = 0; i < z->n; i++) cb_buf_printf(o, "%s%s", i ? "," : "", z->s[i]);
}

// An item's attributes ({"name":{"T":v},...}): name:type, and for a key
// attribute the template its value fills.
static void shape_item(shape_set *z, cb_span item) {
    size_t pos = 0;
    cb_span k, v;
    while (dw_object_next(item, &pos, &k, &v)) {
        size_t tp = 0;
        cb_span tk, tv;
        if (!dw_object_next(v, &tp, &tk, &tv)) continue;
        cb_buf e;
        cb_buf_init(&e);
        cb_buf_printf(&e, "%.*s:%.*s", (int)k.n - 2, k.p + 1, (int)tk.n - 2, tk.p + 1);
        int attr = key_attr_of(k.p + 1, k.n - 2);
        if (attr >= 0 && key_is(tk, "S")) {
            cb_buf_putc(&e, '=');
            shape_value(&e, attr, tv, false);
        }
        set_add(z, cb_buf_cstr(&e), e.len);
        cb_buf_free(&e);
    }
}

// The expression names (their values) and values: each value's type, and
// for one that holds a key attribute its template. Which attribute a value
// holds is read from its placeholder: ":<attribute>" for a set (both the
// item layer and the builders before it name a set's value after its
// attribute), ":pk" and ":sk" for a Query's key condition (the index's pair),
// ":p" for a Scan's pk prefix; any other value is its type alone.
static void shape_exprs(shape_set *names, shape_set *vals, cb_span req, int index) {
    cb_span m, k, v;
    size_t pos = 0;
    if (member(req, "ExpressionAttributeNames", &m))
        while (dw_object_next(m, &pos, &k, &v)) set_add(names, v.p + 1, v.n - 2);
    pos = 0;
    bool query = member(req, "KeyConditionExpression", &m);
    if (member(req, "ExpressionAttributeValues", &m))
        while (dw_object_next(m, &pos, &k, &v)) {
            size_t tp = 0;
            cb_span tk, tv;
            if (!dw_object_next(v, &tp, &tk, &tv)) continue;
            cb_buf e;
            cb_buf_init(&e);
            cb_buf_printf(&e, "%.*s", (int)tk.n - 2, tk.p + 1);
            const char *ph = k.p + 2;   // past the quote and the ':'
            size_t phn = k.n >= 3 ? k.n - 3 : 0;
            int attr = -1;
            bool prefix = false;
            if (query && phn == 2 && memcmp(ph, "pk", 2) == 0) attr = index == 1 ? 2 : index == 2 ? 4 : 0;
            else if (query && phn == 2 && memcmp(ph, "sk", 2) == 0) attr = index == 1 ? 3 : index == 2 ? 5 : 1, prefix = true;
            else if (!query && phn == 1 && ph[0] == 'p') attr = 0, prefix = true;
            else attr = key_attr_of(ph, phn);
            if (attr >= 0 && key_is(tk, "S")) {
                cb_buf_putc(&e, ':');
                shape_value(&e, attr, tv, prefix);
            }
            set_add(vals, cb_buf_cstr(&e), e.len);
            cb_buf_free(&e);
        }
}

// One request (or one item of a transaction or batch) as one line.
static void shape_one(cb_buf *out, const char *op, cb_span req) {
    shape_set *key = calloc(1, sizeof *key), *item = calloc(1, sizeof *item), *names = calloc(1, sizeof *names),
              *vals = calloc(1, sizeof *vals);
    if (key && item && names && vals) {
        cb_span v;
        cb_buf_puts(out, op);
        int index = 0;
        if (member(req, "IndexName", &v)) {
            cb_buf_printf(out, " index=%.*s", (int)v.n - 2, v.p + 1);
            index = v.n >= 3 && v.p[v.n - 2] == '1' ? 1 : 2;
        }
        if (member(req, "Key", &v)) shape_item(key, v);
        if (member(req, "Item", &v)) shape_item(item, v);
        shape_exprs(names, vals, req, index);
        if (key->n) set_put(out, "key", key);
        if (item->n) set_put(out, "item", item);
        if (names->n) set_put(out, "names", names);
        if (vals->n) set_put(out, "vals", vals);
        cb_buf_putc(out, '\n');
    }
    free(key);
    free(item);
    free(names);
    free(vals);
}

void dw_shape(const char *target, cb_span body, cb_buf *out) {
    const char *op = strchr(target, '.');
    op = op ? op + 1 : target;
    cb_span m, e, k, v;
    size_t pos = 0;
    if (strcmp(op, "TransactWriteItems") == 0 && member(body, "TransactItems", &m)) {
        while (dw_array_next(m, &pos, &e)) {
            size_t ip = 0;
            if (!dw_object_next(e, &ip, &k, &v)) continue;
            char sub[64];
            snprintf(sub, sizeof sub, "Txn.%.*s", (int)k.n - 2, k.p + 1);
            shape_one(out, sub, v);
        }
    } else if ((strcmp(op, "BatchWriteItem") == 0 || strcmp(op, "BatchGetItem") == 0) && member(body, "RequestItems", &m)) {
        size_t tp = 0;
        cb_span tk, tv;
        while (dw_object_next(m, &tp, &tk, &tv)) {
            if (strcmp(op, "BatchGetItem") == 0) {
                cb_span keys;
                if (!member(tv, "Keys", &keys)) continue;
                while (dw_array_next(keys, &pos, &e)) {
                    cb_buf w;
                    cb_buf_init(&w);
                    cb_buf_puts(&w, "{\"Key\":");
                    cb_buf_append(&w, e.p, e.n);
                    cb_buf_putc(&w, '}');
                    shape_one(out, "BatchGet", cb_span_of((const char *)w.p, w.len));
                    cb_buf_free(&w);
                }
            } else {
                while (dw_array_next(tv, &pos, &e)) {
                    size_t ip = 0;
                    if (!dw_object_next(e, &ip, &k, &v)) continue;
                    char sub[64];
                    snprintf(sub, sizeof sub, "Batch.%.*s", (int)k.n - 2, k.p + 1);
                    shape_one(out, sub, v);
                }
            }
            pos = 0;
        }
    } else {
        shape_one(out, op, body);
    }
}

// ---- one pass over an item ------------------------------------------------------------------
//
// An item decoder reads ten or more attributes, and `attr` finds each one by
// walking the item from its start, crossing every blob before it: the game
// row's state, roster and session log were walked a dozen times per read. The
// index walks the item once, keeping each attribute's name, its typed value
// and that value's first member ({"S": ...}: the type and its token), and the
// decoders read the index.
//
// It answers exactly as `attr` does, on any input (fz_dynamo.c holds the
// old decoders and checks it): the walk stops where dw_object_next stops, the
// first attribute of a name is the one read, a value that is not an object
// or whose first key is not a string has no type, a value whose first type
// is not the one asked for is searched with `member` (DynamoDB writes one),
// and an item longer than the index is searched with `attr` past it.

#define IX_MAX 32

typedef enum { IX_UNTYPED = 0, IX_TYPED } ix_first;

typedef struct {
    cb_span  key, val;
    cb_span  type, tok;   // the value's first member, when IX_TYPED
    ix_first first;
} ix_attr;

typedef struct {
    cb_span item;
    int     n;
    bool    full;          // IX_MAX attributes indexed and the walk stopped there
    ix_attr a[IX_MAX];
} ix;

// ---- typed values ---------------------------------------------------------------------------

static bool tok_s(cb_span tok, char *out, size_t cap) {
    if (tok.n < 2 || tok.p[0] != '"') return false;
    return cb_json_unescape(tok, out, cap) >= 0;
}

static bool tok_n(cb_span tok, int64_t *out) {
    char t[32];
    if (cb_json_unescape(tok, t, sizeof t) < 1) return false;
    return num_parse_i64_cstr(t, INT64_MIN, INT64_MAX, out);
}

// A B token, base64-decoded into a malloc'd buffer (NULL for none). A token
// with no backslash is decoded where it lies: unescaping it could only copy it
// or refuse a control byte, which the decoder refuses as well.
static bool tok_b(cb_span tok, uint8_t **out, size_t *len) {
    *out = NULL;
    *len = 0;
    if (tok.n < 2 || tok.p[0] != '"') return false;
    const char *text = tok.p + 1;
    char *copy = NULL;
    int tn;
    if (tok.p[tok.n - 1] == '"' && !memchr(tok.p + 1, '\\', tok.n - 2)) {
        tn = (int)(tok.n - 2);
    } else {
        copy = malloc(tok.n);
        if (!copy) return false;
        tn = cb_json_unescape(tok, copy, tok.n);
        if (tn < 0) { free(copy); return false; }
        text = copy;
    }
    size_t cap = (size_t)tn / 4 * 3 + 3;
    uint8_t *p = malloc(cap);
    int n = p ? b64_decode(text, (size_t)tn, p, cap) : -1;
    free(copy);
    if (n < 0) { free(p); return false; }
    if (n == 0) { free(p); return true; }
    *out = p;
    *len = (size_t)n;
    return true;
}

bool dw_get_s(cb_span item, const char *name, char *out, size_t cap) {
    cb_span tok;
    return attr(item, name, "S", &tok) && tok_s(tok, out, cap);
}

bool dw_get_n(cb_span item, const char *name, int64_t *out) {
    cb_span tok;
    return attr(item, name, "N", &tok) && tok_n(tok, out);
}

bool dw_get_b(cb_span item, const char *name, uint8_t **out, size_t *len) {
    *out = NULL;
    *len = 0;
    cb_span tok;
    if (!attr(item, name, "B", &tok)) return true;
    return tok_b(tok, out, len);
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

bool dw_throttled(const char *body, size_t n) {
    char t[128];
    error_type(body, n, t, sizeof t);
    return strcmp(t, "ProvisionedThroughputExceededException") == 0 || strcmp(t, "ThrottlingException") == 0 ||
           strcmp(t, "RequestLimitExceeded") == 0;
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
    if (strcmp(code, "ThrottlingError") == 0 || strcmp(code, "ProvisionedThroughputExceeded") == 0)
        return DW_REASON_THROTTLED;
    return DW_REASON_OTHER;
}

// ---- a response's top-level members ---------------------------------------------------------
//
// What a response's reader wants (Item, Items, ConsumedCapacity, ...) is a
// member of its top-level object, and every response is read at least twice
// (dw_consumed in `call`, then the operation's reader). cb_json_find walked
// to each one validating every byte it passed, a game row's blobs included.
// dw_top_scan walks the top level once (dw_object_next, which crosses a blob
// at memchr's speed) and the dw_top_ readers read the index; the body-taking
// readers scan for themselves. On a valid document a reader finds what
// cb_json_find found (the first member whose key unescapes to `name`, typed
// by its first byte), which fz_dynamo.c checks; on an invalid one it is as
// tolerant as the item readers.

static bool key_eq(cb_span key, const char *name) {
    if (!memchr(key.p, '\\', key.n)) return key_is(key, name);
    char buf[256];
    size_t n = strlen(name);
    int k = n < sizeof buf ? cb_json_unescape(key, buf, sizeof buf) : -1;
    return k >= 0 && (size_t)k == n && memcmp(buf, name, n) == 0;
}

static cb_json_type token_type(cb_span v) {
    if (!v.n) return CB_JSON_ERR;
    switch (v.p[0]) {
    case '{': return CB_JSON_OBJ;
    case '[': return CB_JSON_ARR;
    case '"': return CB_JSON_STR;
    case 't': case 'f': return CB_JSON_BOOL;
    case 'n': return CB_JSON_NULL;
    default:  return (v.p[0] == '-' || (v.p[0] >= '0' && v.p[0] <= '9')) ? CB_JSON_NUM : CB_JSON_ERR;
    }
}

// The member `name` of the object `obj`, and its type; CB_JSON_MISSING when absent.
static cb_json_type obj_get(cb_span obj, const char *name, cb_span *val) {
    size_t pos = 0;
    cb_span key, v;
    while (dw_object_next(obj, &pos, &key, &v))
        if (key_eq(key, name)) {
            *val = v;
            return token_type(v);
        }
    return CB_JSON_MISSING;
}

void dw_top_scan(const char *body, size_t n, dw_top *t) {
    t->body = cb_span_of(body, n);
    t->n = 0;
    size_t pos = 0;
    while (t->n < DW_TOP_MAX && dw_object_next(t->body, &pos, &t->m[t->n].key, &t->m[t->n].val)) t->n++;
    t->full = t->n == DW_TOP_MAX;
}

// obj_get(t->body, name, val), read from the index.
static cb_json_type top_get(const dw_top *t, const char *name, cb_span *val) {
    for (int i = 0; i < t->n; i++)
        if (key_eq(t->m[i].key, name)) {
            *val = t->m[i].val;
            return token_type(*val);
        }
    return t->full ? obj_get(t->body, name, val) : CB_JSON_MISSING;
}

static cb_json_type top(const char *body, size_t n, const char *name, cb_span *val) {
    dw_top t;
    dw_top_scan(body, n, &t);
    return top_get(&t, name, val);
}

bool dw_top_item(const dw_top *t, cb_span *item) {
    cb_span v;
    if (top_get(t, "Item", &v) != CB_JSON_OBJ) return false;
    *item = v;
    return true;
}

bool dw_item(const char *body, size_t n, cb_span *item) {
    dw_top t;
    dw_top_scan(body, n, &t);
    return dw_top_item(&t, item);
}

bool dw_attributes(const char *body, size_t n, cb_span *item) {
    cb_span v;
    if (top(body, n, "Attributes", &v) != CB_JSON_OBJ) return false;
    *item = v;
    return true;
}

cb_span dw_top_items(const dw_top *t) {
    cb_span s;
    if (top_get(t, "Items", &s) != CB_JSON_ARR) return CB_SPAN_LIT("[]");
    return s;
}

cb_span dw_items(const char *body, size_t n) {
    dw_top t;
    dw_top_scan(body, n, &t);
    return dw_top_items(&t);
}

cb_span dw_batch_items(const char *body, size_t n, const char *table) {
    cb_span resp, key, val;
    size_t pos = 0;
    if (top(body, n, "Responses", &resp) == CB_JSON_OBJ)
        while (dw_object_next(resp, &pos, &key, &val))
            if (key_is(key, table)) return val;
    return CB_SPAN_LIT("[]");
}

cb_span dw_top_last_key(const dw_top *t) {
    cb_span s;
    if (top_get(t, "LastEvaluatedKey", &s) != CB_JSON_OBJ) return cb_span_of("", 0);
    return s;
}

cb_span dw_last_key(const char *body, size_t n) {
    dw_top t;
    dw_top_scan(body, n, &t);
    return dw_top_last_key(&t);
}

static bool nonempty_object(const char *body, size_t n, const char *name) {
    cb_span s, k, v;
    size_t pos = 0;
    return top(body, n, name, &s) == CB_JSON_OBJ && dw_object_next(s, &pos, &k, &v);
}

bool dw_unprocessed(const char *body, size_t n) {
    return nonempty_object(body, n, "UnprocessedItems") || nonempty_object(body, n, "UnprocessedKeys");
}

void dw_next_token(const char *body, size_t n, char *out, size_t cap) {
    if (!cb_json_get_str(body, n, "NextToken", out, cap)) out[0] = '\0';
}

// ---- the admin console's item text ---------------------------------------------------

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

// ---- the item layer (json_surface.h dw_iget ..., store/items.h) ------------------------------

static void txn_begin(dw_req *r, cb_jw *w);
static void txn_end(cb_jw *w);

void dw_iget(dw_req *r, const char *table, cb_ikey key, bool consistent) {
    cb_jw w;
    begin(r, &w, "GetItem");
    cb_jw_kstr(&w, "TableName", table);
    key2(&w, key.pk, key.sk);
    if (consistent) cb_jw_kbool(&w, "ConsistentRead", true);
    finish(&w);
}

static void av_list(cb_jw *w, const char *const *list, int n) {
    cb_jw_obj(w);
    cb_jw_key(w, "L");
    cb_jw_arr(w);
    for (int i = 0; i < n; i++) {
        cb_jw_obj(w);
        cb_jw_kstr(w, "S", list[i] ? list[i] : "");
        cb_jw_end(w);
    }
    cb_jw_end(w);
    cb_jw_end(w);
}

static void av_any(cb_jw *w, const cb_av *v) {
    switch (v->kind) {
    case CB_AV_S: av_s(w, v->name, v->s); break;
    case CB_AV_N: av_n(w, v->name, v->n); break;
    case CB_AV_BOOL: av_bool(w, v->name, v->b); break;
    case CB_AV_B: av_b(w, v->name, v->bytes, v->len); break;
    case CB_AV_SL:
        cb_jw_key(w, v->name);
        av_list(w, v->list, v->n_list);
        break;
    }
}

// The value v under the placeholder `vn`.
static void ux_val_any(ux *u, const char *vn, const cb_av *v) {
    switch (v->kind) {
    case CB_AV_S: ux_val_s(u, vn, v->s); break;
    case CB_AV_N: ux_val_n(u, vn, v->n); break;
    case CB_AV_BOOL: ux_val_bool(u, vn, v->b); break;
    case CB_AV_B: ux_val_b(u, vn, v->bytes, v->len); break;
    case CB_AV_SL: av_list(ux_val(u, vn), v->list, v->n_list); break;
    }
}

// One set of an update, as a SET or REMOVE clause with its value.
static bool ux_iset(ux *u, const cb_av *v, cb_set_how how) {
    char vn[48], v0[48], clause[160];
    if (!v->name || strlen(v->name) > 30) return false;
    if (how == CB_SET_REMOVE) {
        ux_remove(u, v->name);
        return true;
    }
    snprintf(vn, sizeof vn, ":%s", v->name);
    ux_name(u, v->name);
    ux_val_any(u, vn, v);
    if (how == CB_SET_TO) snprintf(clause, sizeof clause, "#%s = %s", v->name, vn);
    else if (how == CB_SET_IF_ABSENT) snprintf(clause, sizeof clause, "#%s = if_not_exists(#%s, %s)", v->name, v->name, vn);
    else {
        if (v->kind != CB_AV_N) return false;
        snprintf(v0, sizeof v0, ":%s0", v->name);
        ux_val_n(u, v0, 0);
        snprintf(clause, sizeof clause, "#%s = if_not_exists(#%s, %s) + %s", v->name, v->name, v0, vn);
    }
    ux_set_raw(u, clause);
    return true;
}

// The condition, its names and values registered in u, into out. false on a
// clause that cannot be spelled.
static bool icond(ux *u, const cb_iwrite *x, cb_buf *out) {
    for (int i = 0; i < x->n_cond; i++) {
        const cb_iclause *c = &x->cond[i];
        char vn[16], a[40];
        snprintf(vn, sizeof vn, ":c%d", i);
        if (out->len) cb_buf_puts(out, " AND ");
        if (c->check == CB_C_EXISTS) { cb_buf_puts(out, "attribute_exists(pk)"); continue; }
        if (c->check == CB_C_ABSENT) { cb_buf_puts(out, "attribute_not_exists(pk)"); continue; }
        if (!c->v.name || strlen(c->v.name) > 30) return false;
        ux_name(u, c->v.name);
        snprintf(a, sizeof a, "#%s", c->v.name);
        if (c->check != CB_C_ATTR_EXISTS) ux_val_any(u, vn, &c->v);
        switch (c->check) {
        case CB_C_EQ: cb_buf_printf(out, "%s = %s", a, vn); break;
        case CB_C_NE: cb_buf_printf(out, "%s <> %s", a, vn); break;
        case CB_C_LT: cb_buf_printf(out, "%s < %s", a, vn); break;
        case CB_C_ATTR_EXISTS: cb_buf_printf(out, "attribute_exists(%s)", a); break;
        case CB_C_ABSENT_OR_EQ: cb_buf_printf(out, "(attribute_not_exists(%s) OR %s = %s)", a, a, vn); break;
        case CB_C_ABSENT_OR_LT: cb_buf_printf(out, "(attribute_not_exists(%s) OR %s < %s)", a, a, vn); break;
        default: return false;
        }
    }
    return true;
}

// The body of one write (the caller has its object open).
static bool iwrite_body(cb_jw *w, const char *table, const cb_iwrite *x, bool old) {
    cb_jw_kstr(w, "TableName", table);
    if (x->n_cond < 0 || x->n_cond > CB_ICOND_MAX) return false;
    ux u;
    ux_init(&u);
    bool ok = true;
    if (x->op == CB_IW_PUT) {
        cb_jw_key(w, "Item");
        cb_jw_obj(w);
        av_s(w, "pk", x->key.pk);
        av_s(w, "sk", x->key.sk);
        for (int i = 0; i < x->n_attrs; i++) av_any(w, &x->attrs[i]);
        if (x->ttl_ms > 0) {
            av_n(w, CB_ITEM_TTL_MS, x->ttl_ms);
            av_n(w, CB_ITEM_TTL_S, x->ttl_ms / 1000);   // the TTL attribute is in seconds
        }
        cb_jw_end(w);
    } else {
        key2(w, x->key.pk, x->key.sk);
    }
    if (x->op == CB_IW_UPDATE) {
        for (int i = 0; ok && i < x->n_sets; i++) ok = ux_iset(&u, &x->sets[i].v, x->sets[i].how);
        if (ok && x->ttl_ms > 0) {
            cb_av ms = CB_AVN(CB_ITEM_TTL_MS, x->ttl_ms), s = CB_AVN(CB_ITEM_TTL_S, x->ttl_ms / 1000);
            ok = ux_iset(&u, &ms, x->ttl_how) && ux_iset(&u, &s, x->ttl_how);
        }
    }
    cb_buf cond;
    cb_buf_init(&cond);
    ok = ok && icond(&u, x, &cond);
    if (ok) ux_emit(w, &u, cond.len ? cb_buf_cstr(&cond) : NULL);
    cb_buf_free(&cond);
    ux_free(&u);
    if (ok && old && x->n_cond > 0) cb_jw_kstr(w, "ReturnValuesOnConditionCheckFailure", "ALL_OLD");
    return ok;
}

bool dw_iwrite(dw_req *r, const char *table, const cb_iwrite *x, bool old) {
    const char *op = x->op == CB_IW_PUT ? "PutItem" : x->op == CB_IW_UPDATE ? "UpdateItem" : x->op == CB_IW_DELETE ? "DeleteItem" : NULL;
    if (!op) return false;
    cb_jw w;
    begin(r, &w, op);
    bool ok = iwrite_body(&w, table, x, old);
    if (old && x->op != CB_IW_PUT) cb_jw_kstr(&w, "ReturnValues", "ALL_OLD");
    finish(&w);
    return ok && cb_jw_ok(&w);
}

bool dw_itxn(dw_req *r, const char *table, const cb_iwrite *x, int n) {
    if (n < 1 || n > CB_ITEM_TXN_MAX) return false;
    cb_jw w;
    txn_begin(r, &w);
    bool ok = true;
    for (int i = 0; ok && i < n; i++) {
        static const char *const OPS[] = { "", "Put", "Update", "Delete", "ConditionCheck" };
        if (x[i].op < CB_IW_PUT || x[i].op > CB_IW_CHECK) return false;
        if (x[i].op == CB_IW_CHECK && x[i].n_cond == 0) return false;
        cb_jw_obj(&w);
        cb_jw_key(&w, OPS[x[i].op]);
        cb_jw_obj(&w);
        ok = iwrite_body(&w, table, &x[i], x[i].old_on_fail);
        cb_jw_end(&w);
        cb_jw_end(&w);
    }
    txn_end(&w);
    return ok && cb_jw_ok(&w);
}

void dw_iquery(dw_req *r, const char *table, const cb_iquery *q, cb_span start) {
    const char *pkattr = q->index == 1 ? "gsi1pk" : q->index == 2 ? "gsi2pk" : "pk";
    const char *skattr = q->index == 1 ? "gsi1sk" : q->index == 2 ? "gsi2sk" : "sk";
    cb_jw w;
    begin(r, &w, "Query");
    cb_jw_kstr(&w, "TableName", table);
    if (q->index) cb_jw_kstr(&w, "IndexName", q->index == 1 ? GSI1 : GSI2);
    ux u;
    ux_init(&u);
    ux_name(&u, pkattr);
    ux_val_s(&u, ":pk", q->pk);
    char kc[160];
    if (q->sk_prefix || q->sk_below) {
        ux_name(&u, skattr);
        ux_val_s(&u, ":sk", q->sk_prefix ? q->sk_prefix : q->sk_below);
        snprintf(kc, sizeof kc, q->sk_prefix ? "#%s = :pk AND begins_with(#%s, :sk)" : "#%s = :pk AND #%s < :sk", pkattr,
                 skattr);
    } else snprintf(kc, sizeof kc, "#%s = :pk", pkattr);
    cb_jw_kstr(&w, "KeyConditionExpression", kc);
    if (q->keys_only) {
        ux_name(&u, "pk");
        ux_name(&u, "sk");
        cb_jw_kstr(&w, "ProjectionExpression", "#pk, #sk");
    }
    ux_emit(&w, &u, NULL);
    ux_free(&u);
    if (!q->forward) cb_jw_kbool(&w, "ScanIndexForward", false);
    if (q->page > 0) cb_jw_kint(&w, "Limit", q->page);
    if (!q->index) cb_jw_kbool(&w, "ConsistentRead", true);   // a GSI is eventually consistent; asking otherwise is an error
    if (start.n) {
        cb_jw_key(&w, "ExclusiveStartKey");
        cb_jw_raw(&w, start.p, start.n);
    }
    finish(&w);
}

bool dw_ibatch(dw_req *r, const char *table, const cb_ikey *keys, const int *which, int n, const char *const *proj,
               int n_proj) {
    if (n < 1 || n > 100 || n_proj < 0 || n_proj > 32) return false;
    cb_jw w;
    begin(r, &w, "BatchGetItem");
    cb_jw_key(&w, "RequestItems");
    cb_jw_obj(&w);
    cb_jw_key(&w, table);
    cb_jw_obj(&w);
    cb_jw_key(&w, "Keys");
    cb_jw_arr(&w);
    for (int i = 0; i < n; i++) {
        const cb_ikey *k1 = &keys[which[i]];
        bool dup = false;   // BatchGetItem refuses a repeated key
        for (int j = 0; j < i && !dup; j++)
            dup = strcmp(keys[which[j]].pk, k1->pk) == 0 && strcmp(keys[which[j]].sk, k1->sk) == 0;
        if (dup) continue;
        cb_jw_obj(&w);
        av_s(&w, "pk", k1->pk);
        av_s(&w, "sk", k1->sk);
        cb_jw_end(&w);
    }
    cb_jw_end(&w);
    cb_jw_kbool(&w, "ConsistentRead", true);
    if (proj && n_proj) {
        // Every name aliased: status and state are reserved words.
        char pe[1024] = "";
        size_t off = 0;
        // The keys always: the answer is matched back to the keys asked by them.
        off += (size_t)snprintf(pe, sizeof pe, "pk, sk");
        for (int i = 0; i < n_proj && off < sizeof pe; i++)
            off += (size_t)snprintf(pe + off, sizeof pe - off, ", #p%d", i);
        cb_jw_kstr(&w, "ProjectionExpression", pe);
        cb_jw_key(&w, "ExpressionAttributeNames");
        cb_jw_obj(&w);
        for (int i = 0; i < n_proj; i++) {
            char nm[16];
            snprintf(nm, sizeof nm, "#p%d", i);
            cb_jw_kstr(&w, nm, proj[i]);
        }
        cb_jw_end(&w);
    }
    cb_jw_end(&w);
    cb_jw_end(&w);
    finish(&w);
    return cb_jw_ok(&w);
}

bool dw_ibatch_delete(dw_req *r, const char *table, const cb_ikey *keys, int n) {
    if (n < 1 || n > 25) return false;
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
        key2(&w, keys[i].pk, keys[i].sk);
        cb_jw_end(&w);
        cb_jw_end(&w);
    }
    cb_jw_end(&w);
    cb_jw_end(&w);
    finish(&w);
    return cb_jw_ok(&w);
}

int dw_ibatch_unprocessed(const char *body, size_t n, const char *table, const cb_ikey *keys, int count, int *out) {
    cb_span un, key, val, ks, it;
    size_t pos = 0;
    int m = 0;
    if (cb_json_find(body, n, "UnprocessedKeys", &un) != CB_JSON_OBJ) return 0;
    while (dw_object_next(un, &pos, &key, &val)) {
        if (!key_is(key, table) || cb_json_find(val.p, val.n, "Keys", &ks) != CB_JSON_ARR) continue;
        size_t kp = 0;
        while (dw_array_next(ks, &kp, &it)) {
            char pk[KB], sk[KB];
            if (!dw_get_s(it, "pk", pk, sizeof pk) || !dw_get_s(it, "sk", sk, sizeof sk)) continue;
            for (int i = 0; i < count; i++)
                if (strcmp(keys[i].pk, pk) == 0 && strcmp(keys[i].sk, sk) == 0) {
                    out[m++] = i;   // the first with this key: the one that was asked
                    break;
                }
        }
    }
    return m;
}

bool dw_item_keys(cb_span item, char *pk, size_t pk_cap, char *sk, size_t sk_cap) {
    return dw_get_s(item, "pk", pk, pk_cap) && dw_get_s(item, "sk", sk, sk_cap);
}

bool dw_item_read(cb_span item, cb_item *out) {
    size_t pos = 0;
    cb_span k, v;
    char name[256];
    while (dw_object_next(item, &pos, &k, &v)) {
        if (cb_json_unescape(k, name, sizeof name) < 1) return false;
        size_t tp = 0;
        cb_span tk, tv;
        if (!dw_object_next(v, &tp, &tk, &tv)) return false;
        cb_av a = { .name = name };
        bool ok = true;
        char *text = NULL;
        uint8_t *bytes = NULL;
        char list_s[CB_ITEM_LIST_MAX][64];
        const char *list_p[CB_ITEM_LIST_MAX];
        if (key_is(tk, "S")) {
            text = malloc(tv.n);
            int n = text ? cb_json_unescape(tv, text, tv.n) : -1;
            ok = n >= 0 && (size_t)n == strlen(text);
            a.kind = CB_AV_S;
            a.s = text;
        } else if (key_is(tk, "N")) {
            char num[32];
            int n = cb_json_unescape(tv, num, sizeof num);
            ok = n > 0 && num_parse_i64_cstr(num, INT64_MIN, INT64_MAX, &a.n);
            a.kind = CB_AV_N;
        } else if (key_is(tk, "BOOL")) {
            a.kind = CB_AV_BOOL;
            ok = (tv.n == 4 && memcmp(tv.p, "true", 4) == 0) || (tv.n == 5 && memcmp(tv.p, "false", 5) == 0);
            a.b = tv.n == 4;
        } else if (key_is(tk, "B")) {
            a.kind = CB_AV_B;
            char *b64 = malloc(tv.n);
            int n = b64 ? cb_json_unescape(tv, b64, tv.n) : -1;
            bytes = n >= 0 ? malloc((size_t)n + 1) : NULL;
            int m = bytes ? b64_decode(b64, (size_t)n, bytes, (size_t)n + 1) : -1;
            free(b64);
            ok = m >= 0;
            a.bytes = bytes;
            a.len = ok ? (size_t)m : 0;
        } else if (key_is(tk, "L")) {
            // A list of strings (snapshots' player_ids); anything else in it is refused.
            a.kind = CB_AV_SL;
            size_t lp = 0;
            cb_span e, et;
            while (ok && dw_array_next(tv, &lp, &e)) {
                ok = a.n_list < CB_ITEM_LIST_MAX && member(e, "S", &et) &&
                     cb_json_unescape(et, list_s[a.n_list], sizeof list_s[0]) >= 0;
                if (ok) {
                    list_p[a.n_list] = list_s[a.n_list];
                    a.n_list++;
                }
            }
            a.list = list_p;
        } else {
            ok = false;
        }
        ok = ok && cb_item_add(out, &a);
        free(text);
        free(bytes);
        if (!ok) return false;
    }
    return true;
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

// The bench's fixture (json_surface.h).
bool dw_get_answer_of_put(const dw_req *put, cb_buf *out) {
    cb_span item;
    if (cb_json_find((const char *)put->body.p, put->body.len, "Item", &item) != CB_JSON_OBJ) return false;
    cb_buf_puts(out, "{\"Item\":");
    cb_buf_append(out, item.p, item.n);
    cb_buf_puts(out, "}");
    return true;
}

// ---- admin -----------------------------------------------------------------------------------------------

const char *dw_collection(int i) { return cb_store_collection(i); }

bool dw_collection_valid(const char *c) {
    char p[KB];
    return cb_store_collection_pk(c, NULL, p, sizeof p);
}

void dw_scan_family(dw_req *r, const char *table, const char *family, const char *prefix, int limit, cb_span start) {
    char p[KB];
    if (!cb_store_collection_pk(family, prefix, p, sizeof p)) p[0] = 0;
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

// Each export table, as the items it was written to: pk family, sk exact or
// (ending in '#', or empty) a prefix.
static const struct { const char *export_name, *pk, *sk; } COUNTS[] = {
    { "users", "USER#", "PROFILE" },
    { "bots", "BOT#", "BOT" },
    { "user_elo_ratings", "USER#", "RATING" },
    { "games", "GAME#", "ROW" },
    { "player_hands", "GAME#", "MEMBER#" },
    { "bot_hands", "GAME#", "BOT#" },
    { "game_snapshots", "SNAP#", "SNAP" },
    { "chat_messages", "CHAT#", "" },
};

void dw_scan_keys(dw_req *r, const char *table, int limit, cb_span start) {
    cb_jw w;
    begin(r, &w, "Scan");
    cb_jw_kstr(&w, "TableName", table);
    cb_jw_kbool(&w, "ConsistentRead", true);
    ux u;
    ux_init(&u);
    ux_name(&u, "pk");
    ux_name(&u, "sk");
    cb_jw_kstr(&w, "ProjectionExpression", "#pk, #sk");
    ux_emit(&w, &u, NULL);
    ux_free(&u);
    if (limit > 0) cb_jw_kint(&w, "Limit", limit);
    if (start.n) {
        cb_jw_key(&w, "ExclusiveStartKey");
        cb_jw_raw(&w, start.p, start.n);
    }
    finish(&w);
}

const char *dw_export_kind(cb_span item, char *id, size_t cap) {
    char pk[KB], sk[KB];
    if (!dw_get_s(item, "pk", pk, sizeof pk) || !dw_get_s(item, "sk", sk, sizeof sk)) return NULL;
    for (size_t i = 0; i < sizeof COUNTS / sizeof COUNTS[0]; i++) {
        size_t pl = strlen(COUNTS[i].pk), sl = strlen(COUNTS[i].sk);
        bool prefix = sl == 0 || COUNTS[i].sk[sl - 1] == '#';
        if (strncmp(pk, COUNTS[i].pk, pl) != 0) continue;
        if (prefix ? strncmp(sk, COUNTS[i].sk, sl) != 0 : strcmp(sk, COUNTS[i].sk) != 0) continue;
        snprintf(id, cap, "%s", pk + pl);
        return COUNTS[i].export_name;
    }
    return NULL;
}

void dw_describe_table(dw_req *r, const char *table) {
    cb_jw w;
    snprintf(r->target, sizeof r->target, TARGET "DescribeTable");
    cb_buf_reset(&r->body);
    cb_jw_init(&w, &r->body);
    cb_jw_obj(&w);   // DescribeTable takes no ReturnConsumedCapacity: it costs none
    cb_jw_kstr(&w, "TableName", table);
    finish(&w);
}

// One "<path>.ProvisionedThroughput.<Read|Write>CapacityUnits" into *min
// when it is smaller; false when the path is not there.
static bool lower_to(const char *body, size_t n, const char *path, const char *what, int64_t *min) {
    char full[160];
    int64_t v;
    snprintf(full, sizeof full, "%s.ProvisionedThroughput.%sCapacityUnits", path, what);
    if (!cb_json_get_i64(body, n, full, &v)) return false;
    if (v > 0 && (*min == 0 || v < *min)) *min = v;
    return true;
}

bool dw_provisioned(const char *body, size_t n, int64_t *rcu, int64_t *wcu) {
    *rcu = *wcu = 0;
    if (!lower_to(body, n, "Table", "Read", rcu) || !lower_to(body, n, "Table", "Write", wcu)) return false;
    for (int i = 0; i < 64; i++) {
        char path[64];
        snprintf(path, sizeof path, "Table.GlobalSecondaryIndexes.%d", i);
        if (!lower_to(body, n, path, "Read", rcu)) break;
        lower_to(body, n, path, "Write", wcu);
    }
    return true;
}

double dw_consumed(const char *body, size_t n) {
    dw_top t;
    dw_top_scan(body, n, &t);
    return dw_top_consumed(&t);
}

double dw_top_consumed(const dw_top *t) {
    // One table: an object (GetItem, Query, ...), or a one-entry array
    // (BatchGetItem, BatchWriteItem, TransactWriteItems). The two paths
    // cb_json_find was asked, "ConsumedCapacity.CapacityUnits" and then
    // "ConsumedCapacity.0.CapacityUnits" (on an object, "0" is a key).
    cb_span cc, v;
    cb_json_type ty = top_get(t, "ConsumedCapacity", &cc);
    bool found = false;
    if (ty == CB_JSON_OBJ) {
        found = obj_get(cc, "CapacityUnits", &v) == CB_JSON_NUM;
        cb_span z;
        if (!found && obj_get(cc, "0", &z) == CB_JSON_OBJ) found = obj_get(z, "CapacityUnits", &v) == CB_JSON_NUM;
    } else if (ty == CB_JSON_ARR) {
        size_t pos = 0;
        cb_span e;
        if (dw_array_next(cc, &pos, &e)) found = obj_get(e, "CapacityUnits", &v) == CB_JSON_NUM;
    }
    if (!found) return 0;
    char tx[32];
    if (v.n >= sizeof tx) return 0;
    memcpy(tx, v.p, v.n);
    tx[v.n] = 0;
    return strtod(tx, NULL);
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

// =====================================================================================
// The Lambda event envelope.

// ---- small JSON walking --------------------------------------------------------

typedef bool (*member_fn)(cb_span key, cb_span value, cb_json_type t, void *ud);

// Calls fn for each member of an object (key as its quoted token) or each
// element of an array (key empty). The document has been validated already.
static bool walk(cb_span v, member_fn fn, void *ud) {
    if (v.n < 2 || (v.p[0] != '{' && v.p[0] != '[')) return false;
    bool obj = v.p[0] == '{';
    const char *p = v.p + 1, *end = v.p + v.n - 1;
    for (;;) {
        while (p < end && isspace((unsigned char)*p)) p++;
        if (p >= end) return true;
        cb_span key = { "", 0 };
        if (obj) {
            if (cb_json_find(p, (size_t)(end - p), "", &key) != CB_JSON_STR) return false;
            p = key.p + key.n;
            while (p < end && isspace((unsigned char)*p)) p++;
            if (p >= end || *p != ':') return false;
            p++;
        }
        cb_span val;
        cb_json_type t = cb_json_find(p, (size_t)(end - p), "", &val);
        if (t == CB_JSON_ERR || t == CB_JSON_MISSING) return false;
        if (!fn(key, val, t, ud)) return true;
        p = val.p + val.n;
        while (p < end && isspace((unsigned char)*p)) p++;
        if (p < end && *p == ',') p++;
    }
}

// Unescapes a string token into storage with a NUL. The offset, or -1.
static long put_token(cb_buf *st, cb_span tok) {
    if (!cb_buf_reserve(st, tok.n + 1)) return -1;
    long off = (long)st->len;
    int k = cb_json_unescape(tok, (char *)st->p + st->len, st->cap - st->len);
    if (k < 0) return -1;
    st->len += (size_t)k + 1;
    return off;
}

static long put_bytes(cb_buf *st, const void *p, size_t n) {
    long off = (long)st->len;
    cb_buf_append(st, p, n);
    cb_buf_putc(st, 0);
    return st->oom ? -1 : off;
}

// A body field: unescaped text, or base64-decoded bytes. Offset and length.
static int put_body(cb_buf *st, const char *json, size_t len, long *off, size_t *n) {
    cb_span tok;
    cb_json_type t = cb_json_find(json, len, "body", &tok);
    *off = -1;
    *n = 0;
    if (t == CB_JSON_MISSING || t == CB_JSON_NULL) return CB_OK;
    if (t != CB_JSON_STR) return CB_E_INVALID;
    bool b64 = false;
    cb_json_get_bool(json, len, "isBase64Encoded", &b64);
    long o = put_token(st, tok);
    if (o < 0) return CB_E_INVALID;
    size_t tn = strlen((const char *)st->p + o);
    if (b64) {
        // Decoded in place: base64 never grows.
        int k = b64_decode((const char *)st->p + o, tn, st->p + o, tn + 1);
        if (k < 0) return CB_E_INVALID;
        st->len = (size_t)o + (size_t)k;
        cb_buf_putc(st, 0);
        tn = (size_t)k;
    }
    *off = o;
    *n = tn;
    return st->oom ? CB_E_NOMEM : CB_OK;
}

// ---- HTTP ------------------------------------------------------------------------

typedef struct {
    cb_buf *st;
    long    name[CB_HTTP_MAX_HEADERS], value[CB_HTTP_MAX_HEADERS];
    int     n;
    int     dropped;
    bool    bad;
    bool    cookie_started;
    long    cookie;
} hdr_acc;

static bool on_header(cb_span key, cb_span val, cb_json_type t, void *ud) {
    hdr_acc *a = ud;
    if (t != CB_JSON_STR) return true;   // API Gateway sends strings; anything else is not a header
    if (a->n == CB_HTTP_MAX_HEADERS) { a->dropped++; return true; }
    long k = put_token(a->st, key);
    if (k < 0) { a->bad = true; return false; }
    for (char *c = (char *)a->st->p + k; *c; c++) *c = (char)tolower((unsigned char)*c);
    long v = put_token(a->st, val);
    if (v < 0) { a->bad = true; return false; }
    a->name[a->n] = k;
    a->value[a->n] = v;
    a->n++;
    return true;
}

typedef struct {
    cb_buf joined;
    bool   bad;
} cookie_acc;

static bool on_cookie(cb_span key, cb_span val, cb_json_type t, void *ud) {
    cookie_acc *a = ud;
    if (t != CB_JSON_STR) return true;
    char tmp[4096];
    int k = cb_json_unescape(val, tmp, sizeof tmp);
    if (k < 0) { a->bad = true; return false; }
    if (a->joined.len) cb_buf_puts(&a->joined, "; ");
    cb_buf_append(&a->joined, tmp, (size_t)k);
    return true;
}

static int decode_http(const char *json, size_t len, cb_event *ev) {
    cb_http_req *r = &ev->http;
    cb_buf *st = &ev->storage;
    if (!cb_json_get_str(json, len, "requestContext.http.method", r->method, sizeof r->method)) return CB_E_INVALID;
    cb_span tok;
    if (cb_json_find(json, len, "rawPath", &tok) != CB_JSON_STR) return CB_E_INVALID;
    long path = put_token(st, tok);
    long query = cb_json_find(json, len, "rawQueryString", &tok) == CB_JSON_STR ? put_token(st, tok) : put_bytes(st, "", 0);
    if (path < 0 || query < 0) return CB_E_INVALID;

    hdr_acc h = { .st = st };
    cb_span obj;
    cb_json_type ht = cb_json_find(json, len, "headers", &obj);
    if (ht == CB_JSON_OBJ && !walk(obj, on_header, &h)) return CB_E_INVALID;
    if (ht != CB_JSON_OBJ && ht != CB_JSON_MISSING && ht != CB_JSON_NULL) return CB_E_INVALID;
    if (h.bad) return CB_E_INVALID;
    // Payload 2.0 moves the Cookie header into an array; put it back.
    cookie_acc ck;
    cb_buf_init(&ck.joined);
    ck.bad = false;
    if (cb_json_find(json, len, "cookies", &obj) == CB_JSON_ARR && !walk(obj, on_cookie, &ck)) ck.bad = true;
    if (ck.bad) { cb_buf_free(&ck.joined); return CB_E_INVALID; }
    if (ck.joined.len) {
        if (h.n == CB_HTTP_MAX_HEADERS) h.dropped++;
        else {
            h.name[h.n] = put_bytes(st, "cookie", 6);
            h.value[h.n] = put_bytes(st, ck.joined.p, ck.joined.len);
            h.n++;
        }
    }
    cb_buf_free(&ck.joined);
    if (h.dropped) CB_WARN("event", "%d headers over %d dropped", h.dropped, CB_HTTP_MAX_HEADERS);

    long body;
    size_t body_len;
    int rc = put_body(st, json, len, &body, &body_len);
    if (rc != CB_OK) return rc;
    cb_json_get_str(json, len, "requestContext.http.sourceIp", r->peer, sizeof r->peer);
    if (st->oom) return CB_E_NOMEM;

    // The storage is final: point into it.
    const char *base = (const char *)st->p;
    r->path = base + path;
    r->query = base + query;
    for (int i = 0; i < h.n; i++) {
        r->headers[i].name = base + h.name[i];
        r->headers[i].value = base + h.value[i];
    }
    r->n_headers = h.n;
    r->body = body >= 0 && body_len ? (const uint8_t *)base + body : NULL;
    r->body_len = body_len;
    r->minor = 1;
    r->keep_alive = false;
    return CB_OK;
}

// ---- the IoT custom authorizer -------------------------------------------------------

static bool on_iot_header(cb_span key, cb_span val, cb_json_type t, void *ud) {
    cb_iot_auth_event *a = ud;
    if (t != CB_JSON_STR) return true;
    char k[32];
    int kn = cb_json_unescape(key, k, sizeof k);
    if (kn < 0 || (size_t)kn != strlen(CB_ORIGIN_HEADER) || strncasecmp(k, CB_ORIGIN_HEADER, (size_t)kn) != 0) return true;
    int vn = cb_json_unescape(val, a->origin, sizeof a->origin);
    a->has_origin = vn >= 0 && (size_t)vn == strlen(a->origin);
    if (!a->has_origin) a->origin[0] = 0;
    return false;
}

static bool iot_query_token(const char *key, const char *value, void *ud) {
    if (strcmp(key, "token") != 0) return true;
    cb_iot_auth_event *a = ud;
    size_t n = strlen(value);
    if (n < sizeof a->token) {
        memcpy(a->token, value, n + 1);
        a->token_len = n;
    }
    return false;
}

static int decode_iot_auth(const char *json, size_t len, cb_event *ev) {
    cb_iot_auth_event *a = &ev->iot;
    // A token that does not fit, or does not decode, is left out: no token is
    // a refusal, and a cut one would be a different string.
    char pw64[sizeof a->token * 4 / 3 + 8];
    if (cb_json_get_str(json, len, "protocolData.mqtt.password", pw64, sizeof pw64)) {
        int n = b64_decode(pw64, strlen(pw64), (uint8_t *)a->token, sizeof a->token - 1);
        if (n > 0 && !memchr(a->token, 0, (size_t)n)) {
            a->token[n] = 0;
            a->token_len = (size_t)n;
        } else {
            a->token[0] = 0;
        }
    }
    // IoT's copy of a query parameter. Kept only when it unescapes whole and
    // holds no NUL: a failed unescape may have written part of it, and a
    // \u0000 would cut it short, so either leaves no token at all.
    if (!a->token_len) {
        cb_span v;
        int n = cb_json_find(json, len, "token", &v) == CB_JSON_STR ? cb_json_unescape(v, a->token, sizeof a->token) : -1;
        if (n > 0 && (size_t)n == strlen(a->token)) a->token_len = (size_t)n;
        else a->token[0] = 0;
    }
    if (!a->token_len) {
        char qs[4096];
        if (cb_json_get_str(json, len, "protocolData.http.queryString", qs, sizeof qs))
            cb_http_query_each(qs[0] == '?' ? qs + 1 : qs, iot_query_token, a);
    }
    if (!cb_json_get_str(json, len, "protocolData.mqtt.clientId", a->client_id, sizeof a->client_id)) a->client_id[0] = 0;
    cb_span hdrs;
    if (cb_json_find(json, len, "protocolData.http.headers", &hdrs) == CB_JSON_OBJ) walk(hdrs, on_iot_header, a);
    return CB_OK;
}

// Plain text an ARN may carry: a region or an account id.
static bool arn_part_ok(const char *s) {
    if (!s || !*s || strlen(s) > 32) return false;
    for (; *s; s++)
        if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '-')) return false;
    return true;
}

// One Allow statement: `action` on each resource "<base>:<res[i]>".
static void iot_statement(cb_jw *w, const char *action, const char *base, const char *const *res, int n) {
    cb_jw_obj(w);
    cb_jw_kstr(w, "Effect", "Allow");
    cb_jw_kstr(w, "Action", action);
    cb_jw_key(w, "Resource");
    cb_jw_arr(w);
    for (int i = 0; i < n; i++) {
        char r[256];
        snprintf(r, sizeof r, "%s:%s", base, res[i]);
        cb_jw_str(w, r);
    }
    cb_jw_end(w);
    cb_jw_end(w);
}

int cb_iot_auth_encode(bool ok, const char *user_id, int64_t lifetime_s, const char *region, const char *account,
                       cb_buf *out) {
    if (ok && (!cb_mqtt_user_ok(user_id) || !arn_part_ok(region) || !arn_part_ok(account) || lifetime_s < 300 ||
               lifetime_s > 86400))
        return CB_E_INVALID;
    cb_jw w;
    cb_jw_init(&w, out);
    cb_jw_obj(&w);
    cb_jw_kbool(&w, "isAuthenticated", ok);
    if (ok) {
        char principal[40];
        size_t k = 0;
        for (const char *p = user_id; *p; p++)
            if (*p != '-') principal[k++] = *p;
        principal[k] = 0;
        cb_jw_kstr(&w, "principalId", principal);
        cb_jw_kint(&w, "disconnectAfterInSeconds", lifetime_s);
        cb_jw_kint(&w, "refreshAfterInSeconds", lifetime_s);
        char base[96], client[64], tf_user[64], t_user[64];
        snprintf(base, sizeof base, "arn:aws:iot:%s:%s", region, account);
        snprintf(client, sizeof client, "client/%s-*", user_id);
        snprintf(tf_user, sizeof tf_user, "topicfilter/u/%s/*", user_id);
        snprintf(t_user, sizeof t_user, "topic/u/%s/*", user_id);
        const char *connect[] = { client }, *subscribe[] = { tf_user, "topicfilter/s/*" }, *receive[] = { t_user, "topic/s/*" };
        cb_buf pol;
        cb_buf_init(&pol);
        cb_jw pw;
        cb_jw_init(&pw, &pol);
        cb_jw_obj(&pw);
        cb_jw_kstr(&pw, "Version", "2012-10-17");
        cb_jw_key(&pw, "Statement");
        cb_jw_arr(&pw);
        iot_statement(&pw, "iot:Connect", base, connect, 1);
        iot_statement(&pw, "iot:Subscribe", base, subscribe, 2);
        iot_statement(&pw, "iot:Receive", base, receive, 2);
        cb_jw_end(&pw);
        cb_jw_end(&pw);
        bool pok = cb_jw_ok(&pw);
        cb_jw_key(&w, "policyDocuments");
        cb_jw_arr(&w);
        cb_jw_strn(&w, (const char *)pol.p, pol.len);
        cb_jw_end(&w);
        cb_buf_free(&pol);
        if (!pok) return CB_E_NOMEM;
    }
    cb_jw_end(&w);
    return cb_jw_ok(&w) ? CB_OK : CB_E_NOMEM;
}

// ---- the string literals ------------------------------------------------------------

// "<schedule>" (EventBridge Scheduler's literal input: "scan") and
// "<job>:<key>" (a self-invoke): the quotes stripped, nothing else parsed;
// the names and the key as jobs.h allows them.
static int decode_literal(const char *s, size_t n, cb_event *ev) {
    if (n < 2 || s[n - 1] != '"') return CB_E_INVALID;
    s++;
    n -= 2;
    const char *colon = memchr(s, ':', n);
    if (!colon) {
        if (!cb_job_name_ok(s, n)) return CB_E_INVALID;
        memcpy(ev->schedule, s, n);
        ev->schedule[n] = 0;
        ev->kind = CB_EVENT_SCHEDULE;
        return CB_OK;
    }
    size_t jn = (size_t)(colon - s), kn = n - jn - 1;
    if (!cb_job_name_ok(s, jn) || !cb_job_key_ok(colon + 1, kn)) return CB_E_INVALID;
    memcpy(ev->job, s, jn);
    ev->job[jn] = 0;
    memcpy(ev->key, colon + 1, kn);
    ev->key[kn] = 0;
    ev->kind = CB_EVENT_JOB;
    return CB_OK;
}

int cb_event_job_payload(const char *job, const char *key, char *out, size_t cap) {
    if (!job || !key || !cb_job_name_ok(job, strlen(job)) || !cb_job_key_ok(key, strlen(key))) return CB_E_INVALID;
    int k = snprintf(out, cap, "\"%s:%s\"", job, key);
    return k > 0 && (size_t)k < cap ? k : CB_E_INVALID;
}

int cb_event_decode(const char *json, size_t len, cb_event *out) {
    memset(out, 0, sizeof *out);
    cb_buf_init(&out->storage);
    while (len && isspace((unsigned char)json[len - 1])) len--;
    while (len && isspace((unsigned char)*json)) { json++; len--; }
    if (len && *json == '"') return decode_literal(json, len, out);
    if (!cb_json_valid(json, len) || len == 0 || *json != '{') return CB_E_INVALID;

    // Reserve once for the common case (every string fits in about the event's size).
    cb_buf_reserve(&out->storage, len + 256);
    int rc;
    char src[32];
    if (cb_json_find(json, len, "requestContext.http.method", NULL) == CB_JSON_STR) {
        out->kind = CB_EVENT_HTTP;
        rc = decode_http(json, len, out);
    } else if (cb_json_find(json, len, "protocolData", NULL) == CB_JSON_OBJ) {
        out->kind = CB_EVENT_IOT_AUTH;
        rc = decode_iot_auth(json, len, out);
    } else if (cb_json_get_str(json, len, "source", src, sizeof src) && strcmp(src, "aws.events") == 0) {
        out->kind = CB_EVENT_SCHEDULE;
        rc = CB_OK;
    } else {
        rc = CB_E_INVALID;
    }
    if (rc != CB_OK) {
        cb_buf_free(&out->storage);
        cb_buf *keep = &out->storage;
        memset(out, 0, sizeof *out);
        cb_buf_init(keep);
        return rc == CB_E_NOMEM ? rc : CB_E_INVALID;
    }
    return CB_OK;
}

// ---- responses ----------------------------------------------------------------------

static bool utf8_valid(const uint8_t *p, size_t n) {
    size_t i = 0;
    while (i < n) {
        uint8_t c = p[i];
        size_t k;
        uint32_t cp;
        if (c < 0x80) { i++; continue; }
        if ((c & 0xE0) == 0xC0) { k = 1; cp = c & 0x1F; }
        else if ((c & 0xF0) == 0xE0) { k = 2; cp = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0) { k = 3; cp = c & 0x07; }
        else return false;
        for (size_t j = 1; j <= k; j++) {
            if (i + j >= n || (p[i + j] & 0xC0) != 0x80) return false;
            cp = cp << 6 | (p[i + j] & 0x3F);
        }
        // Overlong forms, surrogates and values past U+10FFFF.
        if ((k == 1 && cp < 0x80) || (k == 2 && cp < 0x800) || (k == 3 && cp < 0x10000) || cp > 0x10FFFF ||
            (cp >= 0xD800 && cp <= 0xDFFF))
            return false;
        i += k + 1;
    }
    return true;
}

typedef struct {
    const char *name;
    size_t      nlen;
    const char *value;
    size_t      vlen;
    bool        done;
} line;

int cb_event_encode_http(const cb_http_res *res, cb_buf *out) {
    // The header lines, "Name: value\r\n" each.
    enum { MAXL = 128 };
    line l[MAXL];
    int nl = 0;
    const char *p = cb_buf_cstr(&res->headers), *end = p + res->headers.len;
    while (p < end && nl < MAXL) {
        const char *e = strstr(p, "\r\n");
        if (!e) e = end;
        const char *colon = memchr(p, ':', (size_t)(e - p));
        if (colon) {
            const char *v = colon + 1;
            while (v < e && *v == ' ') v++;
            l[nl++] = (line){ p, (size_t)(colon - p), v, (size_t)(e - v), false };
        }
        p = e + 2;
    }
    const char *ctype = NULL;
    size_t ctlen = 0;
    for (int i = 0; i < nl; i++)
        if (l[i].nlen == 12 && strncasecmp(l[i].name, "content-type", 12) == 0) { ctype = l[i].value; ctlen = l[i].vlen; }

    cb_jw w;
    cb_jw_init(&w, out);
    cb_jw_obj(&w);
    cb_jw_kint(&w, "statusCode", res->status ? res->status : 200);
    cb_jw_key(&w, "headers");
    cb_jw_obj(&w);
    bool cookies = false;
    for (int i = 0; i < nl; i++) {
        if (l[i].done) continue;
        if (l[i].nlen == 10 && strncasecmp(l[i].name, "set-cookie", 10) == 0) { cookies = true; continue; }
        // One JSON member per name: later lines of the same name joined with ", ".
        char name[128];
        if (!cb_strncpy0(name, sizeof name, l[i].name, l[i].nlen)) continue;
        cb_buf joined;
        cb_buf_init(&joined);
        cb_buf_append(&joined, l[i].value, l[i].vlen);
        for (int j = i + 1; j < nl; j++) {
            if (l[j].nlen == l[i].nlen && strncasecmp(l[j].name, l[i].name, l[i].nlen) == 0) {
                cb_buf_puts(&joined, ", ");
                cb_buf_append(&joined, l[j].value, l[j].vlen);
                l[j].done = true;
            }
        }
        cb_jw_key(&w, name);
        cb_jw_strn(&w, (const char *)joined.p ? (const char *)joined.p : "", joined.len);
        cb_buf_free(&joined);
    }
    cb_jw_end(&w);
    if (cookies) {
        cb_jw_key(&w, "cookies");
        cb_jw_arr(&w);
        for (int i = 0; i < nl; i++)
            if (l[i].nlen == 10 && strncasecmp(l[i].name, "set-cookie", 10) == 0) cb_jw_strn(&w, l[i].value, l[i].vlen);
        cb_jw_end(&w);
    }
    const uint8_t *body = res->body.p ? res->body.p : (const uint8_t *)"";
    size_t n = res->body.len;
    bool octet = ctype && ctlen >= 24 && strncasecmp(ctype, "application/octet-stream", 24) == 0;
    bool b64 = n > 0 && (octet || !utf8_valid(body, n));
    if (b64) {
        cb_jw_kb64(&w, "body", body, n);
    } else {
        cb_jw_key(&w, "body");
        cb_jw_strn(&w, (const char *)body, n);
    }
    cb_jw_kbool(&w, "isBase64Encoded", b64);
    cb_jw_end(&w);
    return cb_jw_ok(&w) ? CB_OK : CB_E_NOMEM;
}

int cb_event_encode_none(cb_buf *out) {
    cb_jw w;
    cb_jw_init(&w, out);
    cb_jw_obj(&w);
    cb_jw_end(&w);
    return cb_jw_ok(&w) ? CB_OK : CB_E_NOMEM;
}

// ---- the raw event, for the log ----------------------------------------------------

// Keys whose string value never reaches a log: credentials, and the body
// (a signup body carries a password). Matched without regard to case.
static bool secret_key(const char *k, size_t n) {
    static const char *const S[] = { "authorization", "apikey", "cookie", "cookies", "body",
                                     "x-amz-security-token", "password", "access_token", "refresh_token" };
    for (size_t i = 0; i < sizeof S / sizeof S[0]; i++)
        if (strlen(S[i]) == n && strncasecmp(S[i], k, n) == 0) return true;
    return false;
}

// Copies a query string value with the apikey and token parameters' values cut.
static void redact_query(cb_buf *out, const char *s, size_t n) {
    size_t i = 0;
    while (i < n) {
        size_t j = i;
        while (j < n && s[j] != '&') j++;
        const char *eq = memchr(s + i, '=', j - i);
        size_t kn = eq ? (size_t)(eq - (s + i)) : j - i;
        if (eq && secret_key(s + i, kn)) {
            cb_buf_append(out, s + i, kn + 1);
            cb_buf_puts(out, "REDACTED");
        } else {
            cb_buf_append(out, s + i, j - i);
        }
        if (j < n) cb_buf_putc(out, '&');
        i = j + 1;
    }
}

int cb_event_redact(const char *json, size_t len, size_t max, cb_buf *out) {
    if (len > max) len = max;
    const char *key = NULL;    // the last string that could be a key
    size_t key_n = 0;
    bool after_colon = false;  // the next token is key's value
    bool secret_arr = false;   // inside an array that is a secret key's value (cookies)
    size_t i = 0;
    while (i < len) {
        char c = json[i];
        if (c != '"') {
            if (c == ':') after_colon = true;
            else if (c == '[') { secret_arr = after_colon && key && secret_key(key, key_n); after_colon = false; }
            else if (c == ']') secret_arr = false;
            else if (!isspace((unsigned char)c)) after_colon = false;
            cb_buf_putc(out, c);
            i++;
            continue;
        }
        size_t j = i + 1;
        while (j < len && json[j] != '"') j += json[j] == '\\' ? 2 : 1;
        if (j > len) j = len;
        const char *s = json + i + 1;
        size_t sn = j - (i + 1);
        bool is_value = after_colon;
        after_colon = false;
        cb_buf_putc(out, '"');
        if (secret_arr || (is_value && key && secret_key(key, key_n)))
            cb_buf_printf(out, "REDACTED %zu", sn);
        else if (is_value && key && key_n == 14 && strncmp(key, "rawQueryString", 14) == 0)
            redact_query(out, s, sn);
        else
            cb_buf_append(out, s, sn);
        if (j < len) cb_buf_putc(out, '"');
        if (!is_value && !secret_arr) { key = s; key_n = sn; }
        i = j + 1;
    }
    return out->oom ? CB_E_NOMEM : CB_OK;
}

// =====================================================================================
// The Lambda Runtime API.

void cb_runtime_error_body(cb_buf *b, const char *type, const char *message) {
    cb_jw w;
    cb_jw_init(&w, b);
    cb_jw_obj(&w);
    cb_jw_kstr(&w, "errorMessage", message);
    cb_jw_kstr(&w, "errorType", type);
    cb_jw_end(&w);
}

// =====================================================================================
// JWT.

static const char JWT_HEADER_JSON[] = "{\"alg\":\"HS256\",\"typ\":\"JWT\"}";

const char *cb_js_jwt_header(size_t *n) {
    *n = sizeof JWT_HEADER_JSON - 1;
    return JWT_HEADER_JSON;
}

static bool jwt_str_is(const char *json, size_t n, const char *path, const char *want) {
    char v[64];
    return cb_json_get_str(json, n, path, v, sizeof v) && strcmp(v, want) == 0;
}

bool cb_js_jwt_header_ok(const char *json, size_t n) {
    return cb_json_valid(json, n) && cb_json_find(json, n, "", NULL) == CB_JSON_OBJ && jwt_str_is(json, n, "alg", "HS256");
}

int cb_js_jwt_claims(cb_buf *out, const cb_jwt_claims *c, const char *issuer) {
    cb_jw w;
    cb_jw_init(&w, out);
    cb_jw_obj(&w);
    cb_jw_kstr(&w, "aud", "authenticated");
    cb_jw_kint(&w, "exp", c->exp);
    cb_jw_kint(&w, "iat", c->iat);
    if (issuer && issuer[0]) cb_jw_kstr(&w, "iss", issuer);
    cb_jw_kstr(&w, "sub", c->sub);
    cb_jw_kstr(&w, "email", c->email);
    cb_jw_kstr(&w, "role", "authenticated");
    cb_jw_kstr(&w, "session_id", c->session_id);
    cb_jw_key(&w, "user_metadata");
    cb_jw_obj(&w);
    cb_jw_kstr(&w, "username", c->username);
    cb_jw_end(&w);
    cb_jw_key(&w, "app_metadata");
    cb_jw_obj(&w);
    cb_jw_kstr(&w, "provider", "email");
    cb_jw_key(&w, "providers");
    cb_jw_arr(&w);
    cb_jw_str(&w, "email");
    cb_jw_end(&w);
    cb_jw_end(&w);
    cb_jw_kbool(&w, "is_anonymous", false);
    cb_jw_end(&w);
    return cb_jw_ok(&w) ? CB_OK : CB_E_NOMEM;
}

bool cb_js_jwt_claims_decode(const char *body, size_t bl, cb_jwt_claims *out, bool *has_nbf, int64_t *nbf) {
    memset(out, 0, sizeof *out);
    *has_nbf = false;
    *nbf = 0;
    if (!cb_json_valid(body, bl) || cb_json_find(body, bl, "", NULL) != CB_JSON_OBJ) return false;
    cb_jwt_claims c;
    memset(&c, 0, sizeof c);
    if (!jwt_str_is(body, bl, "aud", "authenticated") || !jwt_str_is(body, bl, "role", "authenticated")) return false;
    if (!cb_json_get_i64(body, bl, "exp", &c.exp)) return false;
    if (cb_json_find(body, bl, "nbf", NULL) != CB_JSON_MISSING) {
        if (!cb_json_get_i64(body, bl, "nbf", nbf)) return false;
        *has_nbf = true;
    }
    if (!cb_json_get_i64(body, bl, "iat", &c.iat)) c.iat = 0;
    if (!cb_json_get_str(body, bl, "sub", c.sub, sizeof c.sub)) return false;
    if (!cb_json_get_str(body, bl, "session_id", c.session_id, sizeof c.session_id)) return false;
    if (!cb_json_get_str(body, bl, "email", c.email, sizeof c.email)) c.email[0] = 0;
    if (!cb_json_get_str(body, bl, "user_metadata.username", c.username, sizeof c.username)) c.username[0] = 0;
    *out = c;
    return true;
}

// =====================================================================================
// The admin console's API.

struct cb_js_stream {
    cb_jw w;
};

static cb_js_stream *adm_open(cb_buf *out) {
    cb_js_stream *s = malloc(sizeof *s);
    if (!s) return NULL;
    cb_jw_init(&s->w, out);
    cb_jw_obj(&s->w);
    return s;
}

// Closes the document and frees the stream.
static int adm_close(cb_js_stream *s) {
    if (!s) return CB_E_NOMEM;
    cb_jw_end(&s->w);
    bool ok = cb_jw_ok(&s->w);
    free(s);
    return ok ? CB_OK : CB_E_NOMEM;
}

static void adm_kiso(cb_jw *w, const char *k, int64_t ms) {
    char t[40];
    if (ms <= 0 || cb_iso8601(ms, t, sizeof t) < 0) cb_jw_knull(w, k);
    else cb_jw_kstr(w, k, t);
}

static void adm_links(cb_jw *w, const cb_js_admin_links *l) {
    if (!l->lambda) { cb_jw_knull(w, "links"); return; }
    cb_jw_key(w, "links");
    cb_jw_obj(w);
    cb_jw_kstr(w, "lambda", l->lambda);
    cb_jw_kstr(w, "logs_insights", l->logs_insights);
    cb_jw_kstr(w, "dynamodb", l->dynamodb);
    cb_jw_kstr(w, "backups", l->backups);
    cb_jw_end(w);
}

static int adm_done(cb_jw *w) { return cb_jw_ok(w) ? CB_OK : CB_E_NOMEM; }

int cb_js_admin_overview_write(cb_buf *out, const cb_js_admin_overview *o) {
    cb_jw w;
    cb_jw_init(&w, out);
    cb_jw_obj(&w);
    cb_jw_kstr(&w, "version", o->version);
    cb_jw_kstr(&w, "mode", o->mode);
    cb_jw_kint(&w, "uptime_s", o->uptime_s);
    adm_kiso(&w, "started_at", o->started_ms);
    cb_jw_kstr(&w, "store", o->store);
    cb_jw_kstr(&w, "table", o->table);
    cb_jw_kstr(&w, "public_url", o->public_url);
    cb_jw_kint(&w, "requests", o->requests);
    cb_jw_kint(&w, "errors_5xx", o->errors_5xx);
    if (o->conns_known) {
        cb_jw_kint(&w, "http_conns", o->http_conns);
        cb_jw_kint(&w, "ws_conns", o->ws_conns);
    } else {
        cb_jw_knull(&w, "http_conns");
        cb_jw_knull(&w, "ws_conns");
    }
    if (o->games_needing_bots < 0) cb_jw_knull(&w, "games_needing_bots");
    else cb_jw_kint(&w, "games_needing_bots", o->games_needing_bots);
    cb_jw_key(&w, "store_throttles");
    cb_jw_obj(&w);
    cb_jw_kint(&w, "throttled", (int64_t)o->throttled);
    cb_jw_kint(&w, "gave_up", (int64_t)o->gave_up);
    cb_jw_end(&w);
    if (o->continuous_backup) cb_jw_kstr(&w, "last_backup", "continuous (point-in-time recovery)");
    else if (o->last_backup_ms) adm_kiso(&w, "last_backup", o->last_backup_ms);
    else cb_jw_knull(&w, "last_backup");
    cb_jw_key(&w, "process");
    cb_jw_obj(&w);
    cb_jw_kint(&w, "pid", o->pid);
    cb_jw_kint(&w, "max_rss_kb", o->max_rss_kb);
    cb_jw_kint(&w, "cpu_user_ms", o->cpu_user_ms);
    cb_jw_kint(&w, "cpu_sys_ms", o->cpu_sys_ms);
    cb_jw_end(&w);
    adm_links(&w, &o->links);
    cb_jw_end(&w);
    return adm_done(&w);
}

cb_js_stream *cb_js_admin_collections_begin(cb_buf *out, const char *store) {
    cb_js_stream *s = adm_open(out);
    if (!s) return NULL;
    cb_jw_kstr(&s->w, "store", store);
    cb_jw_key(&s->w, "collections");
    cb_jw_arr(&s->w);
    return s;
}

void cb_js_admin_collections_add(cb_js_stream *s, const char *name) { cb_jw_str(&s->w, name); }

int cb_js_admin_collections_end(cb_js_stream *s) {
    if (s) cb_jw_end(&s->w);
    return adm_close(s);
}

void cb_js_admin_row(cb_js_stream *s, int n, const char *const *names, const char *const *values) {
    cb_jw_obj(&s->w);
    for (int i = 0; i < n; i++) {
        cb_jw_key(&s->w, names[i] ? names[i] : "?");
        cb_jw_str(&s->w, values[i]);   // NULL writes null
    }
    cb_jw_end(&s->w);
}

cb_js_stream *cb_js_admin_browse_begin(cb_buf *out, const char *collection) {
    cb_js_stream *s = adm_open(out);
    if (!s) return NULL;
    cb_jw_kstr(&s->w, "collection", collection);
    cb_jw_key(&s->w, "rows");
    cb_jw_arr(&s->w);
    return s;
}

int cb_js_admin_browse_end(cb_js_stream *s, const char *next) {
    if (s) {
        cb_jw_end(&s->w);
        cb_jw_kstr(&s->w, "next", next);
    }
    return adm_close(s);
}

cb_js_stream *cb_js_admin_query_begin(cb_buf *out, const char *dialect, bool write_allowed) {
    cb_js_stream *s = adm_open(out);
    if (!s) return NULL;
    cb_jw_kstr(&s->w, "dialect", dialect);
    cb_jw_kbool(&s->w, "write_allowed", write_allowed);
    cb_jw_key(&s->w, "rows");
    cb_jw_arr(&s->w);
    return s;
}

int cb_js_admin_query_end(cb_js_stream *s, bool truncated) {
    if (s) {
        cb_jw_end(&s->w);
        cb_jw_kbool(&s->w, "truncated", truncated);
    }
    return adm_close(s);
}

bool cb_js_admin_query_body(const char *body, size_t n, char *stmt, size_t stmt_cap, char *confirm, size_t confirm_cap) {
    if (!body || !cb_json_get_str(body, n, "statement", stmt, stmt_cap)) return false;
    if (!cb_json_get_str(body, n, "confirm", confirm, confirm_cap)) confirm[0] = 0;
    return true;
}

cb_js_stream *cb_js_admin_users_begin(cb_buf *out) {
    cb_js_stream *s = adm_open(out);
    if (!s) return NULL;
    cb_jw_key(&s->w, "users");
    cb_jw_arr(&s->w);
    return s;
}

void cb_js_admin_users_add(cb_js_stream *s, const cb_user_row *u) {
    cb_jw *w = &s->w;
    cb_jw_obj(w);
    cb_jw_kstr(w, "id", u->id);
    cb_jw_kstr(w, "email", u->email);
    cb_jw_kstr(w, "username", u->username);
    adm_kiso(w, "created_at", u->created_at);
    adm_kiso(w, "last_sign_in_at", u->last_sign_in_at);
    cb_jw_end(w);
}

int cb_js_admin_users_end(cb_js_stream *s, int64_t offset, int64_t next_offset) {
    if (s) {
        cb_jw_end(&s->w);
        cb_jw_kint(&s->w, "offset", offset);
        if (next_offset >= 0) cb_jw_kint(&s->w, "next_offset", next_offset);
        else cb_jw_knull(&s->w, "next_offset");
    }
    return adm_close(s);
}

bool cb_js_admin_id_body(const char *body, size_t n, char *id, size_t cap) {
    return body && cb_json_get_str(body, n, "id", id, cap);
}

static int adm_one(cb_buf *out, const char *key, const char *value) {
    cb_jw w;
    cb_jw_init(&w, out);
    cb_jw_obj(&w);
    cb_jw_kstr(&w, key, value);
    cb_jw_end(&w);
    return adm_done(&w);
}

int cb_js_admin_deleted(cb_buf *out, const char *id) { return adm_one(out, "deleted", id); }
int cb_js_admin_written(cb_buf *out, const char *path) { return adm_one(out, "written", path); }

// One game's scalar columns and its seats, into an open object.
static void adm_game(cb_jw *w, const cb_js_admin_game *a) {
    const cb_game_row *g = a->g;
    cb_jw_kstr(w, "id", g->id);
    cb_jw_kint(w, "status", g->status);
    cb_jw_kstr(w, "status_label", a->status_label);
    cb_jw_kint(w, "version", g->version);
    cb_jw_kint(w, "round_epoch", g->round_epoch);
    cb_jw_kbool(w, "needs_bots", g->needs_bots);
    adm_kiso(w, "created_at", g->created_at);
    adm_kiso(w, "updated_at", g->updated_at);
    adm_kiso(w, "last_commit_at", g->last_commit_at);
    cb_jw_key(w, "seats");
    if (a->roster_rc != ROSTER_OK) {
        cb_jw_null(w);
        cb_jw_kint(w, "roster_error", a->roster_rc);
        return;
    }
    const Roster *r = a->roster;
    cb_jw_arr(w);
    for (int i = 0; i < r->n; i++) {
        cb_jw_obj(w);
        cb_jw_key(w, "name");
        cb_jw_strn(w, r->seats[i].name, r->seats[i].name_len);
        cb_jw_key(w, "id");
        cb_jw_strn(w, r->seats[i].id, r->seats[i].id_len);
        cb_jw_kbool(w, "bot", r->seats[i].brain_len > 0);
        cb_jw_end(w);
    }
    cb_jw_end(w);
    cb_jw_key(w, "title");
    cb_jw_strn(w, r->title, r->title_len);
}

cb_js_stream *cb_js_admin_games_begin(cb_buf *out) {
    cb_js_stream *s = adm_open(out);
    if (!s) return NULL;
    cb_jw_key(&s->w, "games");
    cb_jw_arr(&s->w);
    return s;
}

void cb_js_admin_games_add(cb_js_stream *s, const cb_js_admin_game *g) {
    cb_jw_obj(&s->w);
    adm_game(&s->w, g);
    cb_jw_end(&s->w);
}

int cb_js_admin_games_end(cb_js_stream *s, const char *next) {
    if (s) {
        cb_jw_end(&s->w);
        cb_jw_kstr(&s->w, "next", next);
    }
    return adm_close(s);
}

int cb_js_admin_game_write(cb_buf *out, const cb_js_admin_game *a, const cb_js_admin_envelope *e) {
    const cb_game_row *g = a->g;
    cb_jw w;
    cb_jw_init(&w, out);
    cb_jw_obj(&w);
    adm_game(&w, a);
    cb_jw_kint(&w, "state_len", (int64_t)g->state_len);
    cb_jw_kint(&w, "roster_len", (int64_t)g->roster_len);
    cb_jw_kint(&w, "logs_len", (int64_t)g->logs_len);
    cb_jw_kbool(&w, "has_seed", g->has_seed);
    if (e->view_ok) {
        if (e->envelope_hex) cb_jw_kstr(&w, "envelope_hex", e->envelope_hex);
        else cb_jw_knull(&w, "envelope_hex");
        cb_jw_kint(&w, "envelope_version", e->envelope_version);
    } else {
        cb_jw_knull(&w, "envelope_hex");
        cb_jw_kstr(&w, "envelope_error", e->envelope_error);
    }
    cb_jw_end(&w);
    return adm_done(&w);
}

cb_js_stream *cb_js_admin_bots_begin(cb_buf *out) {
    cb_js_stream *s = adm_open(out);
    if (!s) return NULL;
    cb_jw_key(&s->w, "bots");
    cb_jw_arr(&s->w);
    return s;
}

void cb_js_admin_bots_add(cb_js_stream *s, const cb_bot_row *r) {
    cb_jw *w = &s->w;
    cb_jw_obj(w);
    cb_jw_kstr(w, "id", r->id);
    cb_jw_kstr(w, "nickname", r->nickname);
    cb_jw_kstr(w, "strategy_key", r->strategy_key);
    cb_jw_kint(w, "elo_rating", r->elo_rating);
    if (r->previous_elo < 0) cb_jw_knull(w, "previous_elo");
    else cb_jw_kint(w, "previous_elo", r->previous_elo);
    cb_jw_kint(w, "games_played", r->games_played);
    adm_kiso(w, "updated_at", r->updated_at);
    cb_jw_end(w);
}

int cb_js_admin_bots_end(cb_js_stream *s) {
    if (s) cb_jw_end(&s->w);
    return adm_close(s);
}

cb_js_stream *cb_js_admin_logs_begin(cb_buf *out, uint64_t last_seq) {
    cb_js_stream *s = adm_open(out);
    if (!s) return NULL;
    cb_jw_kint(&s->w, "last_seq", (int64_t)last_seq);
    cb_jw_key(&s->w, "entries");
    cb_jw_arr(&s->w);
    return s;
}

void cb_js_admin_logs_add(cb_js_stream *s, uint64_t seq, int64_t at_ms, const char *level, const char *module,
                          const char *msg) {
    cb_jw *w = &s->w;
    cb_jw_obj(w);
    cb_jw_kint(w, "seq", (int64_t)seq);
    adm_kiso(w, "at", at_ms);
    cb_jw_kstr(w, "level", level);
    cb_jw_kstr(w, "module", module);
    cb_jw_kstr(w, "msg", msg);
    cb_jw_end(w);
}

int cb_js_admin_logs_end(cb_js_stream *s, const cb_js_admin_links *links) {
    if (s) {
        cb_jw_end(&s->w);
        adm_links(&s->w, links);
    }
    return adm_close(s);
}

cb_js_stream *cb_js_admin_config_begin(cb_buf *out, const cb_js_admin_config *c) {
    cb_js_stream *s = adm_open(out);
    if (!s) return NULL;
    cb_jw_kstr(&s->w, "mode", c->mode);
    cb_jw_kstr(&s->w, "store", c->store);
    cb_jw_kstr(&s->w, "table", c->table);
    cb_jw_kstr(&s->w, "allowed_origins", c->allowed_origins);
    cb_jw_kstr(&s->w, "aws_region", c->aws_region);
    cb_jw_kstr(&s->w, "function_name", c->function_name);
    cb_jw_key(&s->w, "settings");
    cb_jw_arr(&s->w);
    return s;
}

void cb_js_admin_config_add(cb_js_stream *s, const char *name, const char *value, bool secret) {
    cb_jw *w = &s->w;
    cb_jw_obj(w);
    cb_jw_kstr(w, "name", name);
    cb_jw_kstr(w, "value", value);   // cb_config_each has already redacted a secret
    cb_jw_kbool(w, "secret", secret);
    cb_jw_end(w);
}

int cb_js_admin_config_end(cb_js_stream *s) {
    if (s) cb_jw_end(&s->w);
    return adm_close(s);
}

int cb_js_admin_backups_write(cb_buf *out, const cb_js_admin_backups *b) {
    cb_jw w;
    cb_jw_init(&w, out);
    cb_jw_obj(&w);
    cb_jw_kstr(&w, "store", b->store);
    if (b->sqlite) {
        cb_jw_kstr(&w, "kind", "vacuum_into");
        cb_jw_kstr(&w, "dir", b->dir);
        cb_jw_key(&w, "files");
        cb_jw_arr(&w);
        for (int i = 0; i < b->n_files; i++) {
            cb_jw_obj(&w);
            cb_jw_kstr(&w, "name", b->files[i].name);
            cb_jw_kint(&w, "size", b->files[i].size);
            adm_kiso(&w, "at", b->files[i].mtime_ms);
            cb_jw_end(&w);
        }
        cb_jw_end(&w);
    } else {
        cb_jw_kstr(&w, "kind", "pitr");
        cb_jw_kstr(&w, "explain", b->explain);
    }
    adm_links(&w, &b->links);
    cb_jw_end(&w);
    return adm_done(&w);
}

// =====================================================================================
// The client's side (bench/).

void cb_js_client_function_url_event(cb_buf *out, const char *bearer, int content_length, const char *body_b64) {
    cb_buf_printf(out,
        "{\"version\":\"2.0\",\"routeKey\":\"$default\",\"rawPath\":\"/v1/action\",\"rawQueryString\":\"\","
        "\"headers\":{\"accept\":\"*/*\",\"accept-encoding\":\"gzip, deflate, br, zstd\",\"accept-language\":\"en-US,en;q=0.9\","
        "\"authorization\":\"Bearer %s\",\"content-length\":\"%d\",\"content-type\":\"application/octet-stream\","
        "\"host\":\"abcdefghijklmnopqrstuvwxyz012345.lambda-url.us-east-1.on.aws\",\"origin\":\"https://foolish.cards\","
        "\"referer\":\"https://foolish.cards/\",\"sec-fetch-dest\":\"empty\",\"sec-fetch-mode\":\"cors\",\"sec-fetch-site\":\"cross-site\","
        "\"user-agent\":\"Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/141.0.0.0 Safari/537.36\","
        "\"via\":\"2.0 0123456789abcdef0123456789abcdef.cloudfront.net (CloudFront)\",\"x-amz-cf-id\":\"AbCdEfGhIjKlMnOpQrStUvWxYz0123456789AbCdEfGhIjKlMnOpQr==\","
        "\"x-amzn-trace-id\":\"Root=1-670b1c2d-0123456789abcdef01234567\","
        "\"x-forwarded-for\":\"203.0.113.7, 130.176.0.1\",\"x-forwarded-port\":\"443\",\"x-forwarded-proto\":\"https\"},"
        "\"requestContext\":{\"accountId\":\"anonymous\",\"apiId\":\"abcdefghijklmnopqrstuvwxyz012345\","
        "\"domainName\":\"abcdefghijklmnopqrstuvwxyz012345.lambda-url.us-east-1.on.aws\",\"domainPrefix\":\"abcdefghijklmnopqrstuvwxyz012345\","
        "\"http\":{\"method\":\"POST\",\"path\":\"/v1/action\",\"protocol\":\"HTTP/1.1\",\"sourceIp\":\"130.176.0.1\","
        "\"userAgent\":\"Amazon CloudFront\"},\"requestId\":\"0123abcd-4567-89ab-cdef-0123456789ab\",\"routeKey\":\"$default\","
        "\"stage\":\"$default\",\"time\":\"13/Oct/2026:12:00:00 +0000\",\"timeEpoch\":1760356800000},"
        "\"body\":\"%s\",\"isBase64Encoded\":true}",
        bearer, content_length, body_b64);
}

// ---- CloudFront: the one XML shape (json_surface.h "CloudFront") --------------------------

static void xml_text(cb_buf *b, const char *s) {
    for (; *s; s++) {
        switch (*s) {
        case '&': cb_buf_puts(b, "&amp;"); break;
        case '<': cb_buf_puts(b, "&lt;"); break;
        case '>': cb_buf_puts(b, "&gt;"); break;
        case '"': cb_buf_puts(b, "&quot;"); break;
        case '\'': cb_buf_puts(b, "&apos;"); break;
        default: cb_buf_putc(b, *s);
        }
    }
}

bool cf_invalidation_xml(cb_buf *out, const char *const *paths, int n, const char *caller_ref) {
    cb_buf_puts(out, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                     "<InvalidationBatch xmlns=\"http://cloudfront.amazonaws.com/doc/2020-05-31/\">");
    cb_buf_printf(out, "<Paths><Quantity>%d</Quantity><Items>", n);
    for (int i = 0; i < n; i++) {
        cb_buf_puts(out, "<Path>");
        xml_text(out, paths[i]);
        cb_buf_puts(out, "</Path>");
    }
    cb_buf_puts(out, "</Items></Paths><CallerReference>");
    xml_text(out, caller_ref);
    cb_buf_puts(out, "</CallerReference></InvalidationBatch>");
    return !out->oom;
}

// ---- SSM Parameter Store -------------------------------------------------------------------

bool ssm_get_parameters_body(cb_buf *out, const char *const *names, int n) {
    cb_jw w;
    cb_jw_init(&w, out);
    cb_jw_obj(&w);
    cb_jw_key(&w, "Names");
    cb_jw_arr(&w);
    for (int i = 0; i < n; i++) cb_jw_str(&w, names[i]);
    cb_jw_end(&w);
    cb_jw_kbool(&w, "WithDecryption", true);
    cb_jw_end(&w);
    return cb_jw_ok(&w) && !out->oom;
}

typedef struct {
    const char *name;
    char       *out;
    size_t      cap;
    int         found;   // 1 a value, -1 listed but not one this process can hold, 0 not listed
} ssm_find;

static bool ssm_param(cb_span key, cb_span val, cb_json_type t, void *ud) {
    ssm_find *f = ud;
    if (t != CB_JSON_OBJ) return true;
    char got[1024];
    if (!cb_json_get_str(val.p, val.n, "Name", got, sizeof got) || strcmp(got, f->name) != 0) return true;
    cb_span v;
    int n = cb_json_find(val.p, val.n, "Value", &v) == CB_JSON_STR ? cb_json_unescape(v, f->out, f->cap) : -1;
    // Too long, not text, or a NUL in it (\u0000, which a C string would cut short).
    f->found = n >= 0 && (size_t)n == strlen(f->out) ? 1 : -1;
    return false;
}

int ssm_parameter_value(const char *body, size_t len, const char *name, char *out, size_t cap) {
    if (cap) out[0] = 0;
    cb_span arr;
    if (cb_json_find(body, len, "Parameters", &arr) != CB_JSON_ARR) return 0;
    ssm_find f = { name, out, cap, 0 };
    if (!walk(arr, ssm_param, &f)) f.found = -1;
    if (f.found != 1 && cap) out[0] = 0;
    return f.found;
}
