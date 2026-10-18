// fz_dynamo.c - every reader of what DynamoDB answers (json_surface.h dw_):
// the error classification, the transaction's cancellation reasons, the
// response's Item / Items / Responses / LastEvaluatedKey / NextToken, and
// every item decoder the store turns a row back into a struct with. In
// production the body comes over TLS from AWS; a decoder that trusts it is one
// bad answer (or one item a buggy writer stored) from a crash.
#include "fz.h"

#include "aws_json.h"
#include "store/rows_foolish.h"
#include "store/store.h"
#include "../../../../shared/c/b64/b64.h"
#include "util/json.h"
#include "../../../../shared/c/num_parse.h"

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
    // The number rule itself is num_parse_i64's (strtoll's leniency went on
    // 2026-10-18), and fz_num holds that against its own model; this oracle
    // is about finding the attribute.
    return num_parse_i64_cstr(t, INT64_MIN, INT64_MAX, out);
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
    int n = p ? b64_decode(text, (size_t)tn, p, cap) : -1;
    free(text);
    if (n < 0) { free(p); return false; }
    if (n == 0) { free(p); return true; }
    *out = p;
    *len = (size_t)n;
    return true;
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

// The attribute readers on one item against the old ones: the same answer.
// (The row decoders over JSON, which this held against their predecessors,
// went in stage 6: rows are read from cb_item now, items_of below.)
// A Binary attribute the item layer read is the old reader's bytes.
static void same_items(cb_span it) {
    same_walks(it);
    cb_item ii;
    cb_item_init(&ii);
    bool read = dw_item_read(it, &ii);
    static const char *const names[] = { "pk", "sk", "id", "version", "state", "needs_bots", "message", "n", "" };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        char sa[64], sb[64];
        int64_t na = 0, nb = 0;
        FZ_CHECK(dw_get_s(it, names[i], sa, sizeof sa) == ref_get_s(it, names[i], sb, sizeof sb));
        FZ_CHECK(dw_get_n(it, names[i], &na) == ref_get_n(it, names[i], &nb) && na == nb);
        cb_av v;
        if (read && cb_item_get(&ii, names[i], &v) && v.kind == CB_AV_B) {
            uint8_t *pb;
            size_t lb;
            FZ_CHECK(ref_get_b(it, names[i], &pb, &lb) && same_bytes(v.bytes, v.len, pb, lb));
            free(pb);
        }
    }
    cb_item_free(&ii);
}

// The response readers against cb_json_find, on a valid document.
static void same_responses(const char *body, size_t n) {
    if (!cb_json_valid(body, n)) return;
    cb_span a, b;
    bool ha = dw_item(body, n, &a), hb = cb_json_find(body, n, "Item", &b) == CB_JSON_OBJ;
    FZ_CHECK(ha == hb && (!ha || same_span(a, b)));
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

// Every reader on one item: the item layer's (any item, every attribute),
// its record round trip, and every row decoder over what it read.
static void items_of(cb_span item) {
    same_items(item);
    cb_item ii;
    cb_item_init(&ii);
    if (dw_item_read(item, &ii)) {
        cb_buf rec;
        cb_buf_init(&rec);
        // What SQLite would keep reads back as the same attributes.
        if (cb_item_record_encode(&ii, &rec)) {
            cb_item back;
            cb_item_init(&back);
            FZ_CHECK(cb_item_record_decode(rec.p, rec.len, &back) && back.n == ii.n);
            cb_item_free(&back);
        }
        cb_buf_free(&rec);
        cb_user_row u; (void)cb_rows_user_of(&ii, &u);
        cb_game_row g; memset(&g, 0, sizeof g);
        if (cb_rows_game_of(&ii, &g)) FZ_CHECK(!g.state_len || g.state);
        cb_game_row_free(&g);
        cb_bot_row b; (void)cb_rows_bot_of(&ii, &b);
        cb_rating_row rt; (void)cb_rows_rating_of(&ii, &rt);
        cb_snapshot_row sn; memset(&sn, 0, sizeof sn);
        if (cb_rows_snapshot_of(&ii, &sn)) FZ_CHECK(sn.n_players >= 0 && sn.n_players <= CB_MAX_SEATS);
        cb_snapshot_row_free(&sn);
        cb_chat_row c; memset(&c, 0, sizeof c);
        (void)cb_rows_chat_of(&ii, &c);
        cb_chat_row_free(&c);
        cb_buf st; cb_buf_init(&st);
        const char *nm[CB_ITEM_ATTRS], *vl[CB_ITEM_ATTRS];
        int k = cb_item_text(&ii, &st, nm, vl, CB_ITEM_ATTRS);
        for (int i = 0; i < k; i++) { (void)strlen(nm[i]); if (vl[i]) (void)strlen(vl[i]); }
        cb_buf_free(&st);
    }
    cb_item_free(&ii);
    char pk[CB_ITEM_KEY_MAX], sk[CB_ITEM_KEY_MAX];
    (void)dw_item_keys(item, pk, sizeof pk, sk, sizeof sk);
    char sv[64]; int64_t nv;
    (void)dw_get_s(item, "pk", sv, sizeof sv);
    (void)dw_get_n(item, "version", &nv);
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

    // A BatchGetItem's unprocessed keys, by index into the keys asked for.
    const cb_ikey keys[3] = { { "USER#11111111-1111-4111-8111-111111111111", "RATING" },
                              { "BOT#22222222-2222-4222-8222-222222222222", "BOT" }, { "GAME#ab12cd", "ROW" } };
    int left[3];
    int u = dw_ibatch_unprocessed(body, n, "cbase", keys, 3, left);
    FZ_CHECK(u >= 0 && u <= 3);
    for (int i = 0; i < u; i++) FZ_CHECK(left[i] >= 0 && left[i] < 3);
    free((void *)body);
    return 0;
}
