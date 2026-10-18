#include "json.h"

#include "../../../../shared/c/b64/b64.h"
#include "../../../../shared/c/hex/hex.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- writer -------------------------------------------------------------------

void cb_jw_init(cb_jw *w, cb_buf *b) {
    memset(w, 0, sizeof *w);
    w->b = b;
}

bool cb_jw_ok(const cb_jw *w) { return !w->err && w->depth == 0 && !w->b->oom; }

// Writes the separator a new value needs, and refuses a value where a key is due.
static bool value_start(cb_jw *w) {
    if (w->err) return false;
    if (w->depth > 0) {
        int d = w->depth - 1;
        if (w->is_obj[d]) {
            if (!w->key_pending) { w->err = true; return false; }
            w->key_pending = false;
            return true;
        }
        if (!w->first[d]) cb_buf_putc(w->b, ',');
        w->first[d] = 0;
    }
    return true;
}

static void jw_open(cb_jw *w, char c, bool obj) {
    if (!value_start(w)) return;
    if (w->depth >= CB_JW_DEPTH) { w->err = true; return; }
    cb_buf_putc(w->b, c);
    w->first[w->depth] = 1;
    w->is_obj[w->depth] = obj;
    w->depth++;
}

void cb_jw_obj(cb_jw *w) { jw_open(w, '{', true); }
void cb_jw_arr(cb_jw *w) { jw_open(w, '[', false); }

void cb_jw_end(cb_jw *w) {
    if (w->err) return;
    if (w->depth == 0 || w->key_pending) { w->err = true; return; }
    w->depth--;
    cb_buf_putc(w->b, w->is_obj[w->depth] ? '}' : ']');
}

void cb_jw_key(cb_jw *w, const char *k) {
    if (w->err) return;
    if (w->depth == 0 || !w->is_obj[w->depth - 1] || w->key_pending) { w->err = true; return; }
    int d = w->depth - 1;
    if (!w->first[d]) cb_buf_putc(w->b, ',');
    w->first[d] = 0;
    cb_buf_putc(w->b, '"');
    cb_json_escape(w->b, k, strlen(k));
    cb_buf_puts(w->b, "\":");
    w->key_pending = true;
}

void cb_json_escape(cb_buf *b, const char *s, size_t n) {
    size_t run = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        const char *esc = NULL;
        char u[8];
        switch (c) {
        case '"':  esc = "\\\""; break;
        case '\\': esc = "\\\\"; break;
        case '\n': esc = "\\n"; break;
        case '\r': esc = "\\r"; break;
        case '\t': esc = "\\t"; break;
        case '\b': esc = "\\b"; break;
        case '\f': esc = "\\f"; break;
        default:
            if (c < 0x20) { snprintf(u, sizeof u, "\\u%04x", c); esc = u; }
        }
        if (esc) {
            cb_buf_append(b, s + run, i - run);
            cb_buf_puts(b, esc);
            run = i + 1;
        }
    }
    cb_buf_append(b, s + run, n - run);
}

void cb_jw_strn(cb_jw *w, const char *s, size_t n) {
    if (!value_start(w)) return;
    cb_buf_putc(w->b, '"');
    cb_json_escape(w->b, s, n);
    cb_buf_putc(w->b, '"');
}

void cb_jw_str(cb_jw *w, const char *s) {
    if (!s) { cb_jw_null(w); return; }
    cb_jw_strn(w, s, strlen(s));
}

// Base64's alphabet (A-Z a-z 0-9 + / =) never needs escaping: the encoding
// is written straight into the buffer between its quotes, with no copy and no
// pass of cb_json_escape over it.
void cb_jw_b64(cb_jw *w, const uint8_t *p, size_t n) {
    if (!value_start(w)) return;
    size_t k = b64_len(n);
    if (k < n || !cb_buf_reserve(w->b, k + 2)) { w->err = true; return; }
    char *o = (char *)w->b->p + w->b->len;
    o[0] = '"';
    b64_encode(p, n, o + 1, k + 1);   // k + 1 is exactly what it needs, the NUL included
    o[1 + k] = '"';
    w->b->len += k + 2;
    w->b->p[w->b->len] = 0;
}

void cb_jw_int(cb_jw *w, int64_t v) {
    if (!value_start(w)) return;
    cb_buf_printf(w->b, "%lld", (long long)v);
}

void cb_jw_bool(cb_jw *w, bool v) {
    if (!value_start(w)) return;
    cb_buf_puts(w->b, v ? "true" : "false");
}

void cb_jw_null(cb_jw *w) {
    if (!value_start(w)) return;
    cb_buf_puts(w->b, "null");
}

void cb_jw_raw(cb_jw *w, const char *json, size_t n) {
    if (!value_start(w)) return;
    cb_buf_append(w->b, json, n);
}

void cb_jw_kstr(cb_jw *w, const char *k, const char *s) { cb_jw_key(w, k); cb_jw_str(w, s); }
void cb_jw_kint(cb_jw *w, const char *k, int64_t v) { cb_jw_key(w, k); cb_jw_int(w, v); }
void cb_jw_kbool(cb_jw *w, const char *k, bool v) { cb_jw_key(w, k); cb_jw_bool(w, v); }
void cb_jw_kb64(cb_jw *w, const char *k, const uint8_t *p, size_t n) { cb_jw_key(w, k); cb_jw_b64(w, p, n); }
void cb_jw_knull(cb_jw *w, const char *k) { cb_jw_key(w, k); cb_jw_null(w); }

// ---- reader -------------------------------------------------------------------

#define MAX_DEPTH 64

typedef struct {
    const char *s;
    size_t n, i;
} rd;

static void ws(rd *r) {
    while (r->i < r->n && (r->s[r->i] == ' ' || r->s[r->i] == '\t' || r->s[r->i] == '\n' || r->s[r->i] == '\r')) r->i++;
}

static bool lit(rd *r, const char *w) {
    size_t k = strlen(w);
    if (r->n - r->i < k || memcmp(r->s + r->i, w, k) != 0) return false;
    r->i += k;
    return true;
}

static bool skip_string(rd *r) {
    if (r->i >= r->n || r->s[r->i] != '"') return false;
    r->i++;
    while (r->i < r->n) {
        unsigned char c = (unsigned char)r->s[r->i++];
        if (c == '"') return true;
        if (c < 0x20) return false;
        if (c == '\\') {
            if (r->i >= r->n) return false;
            char e = r->s[r->i++];
            if (e == 'u') {
                for (int k = 0; k < 4; k++) {
                    if (r->i >= r->n) return false;
                    char h = r->s[r->i++];
                    if (!((h >= '0' && h <= '9') || (h >= 'a' && h <= 'f') || (h >= 'A' && h <= 'F'))) return false;
                }
            } else if (!strchr("\"\\/bfnrt", e) || e == 0) {
                return false;
            }
        }
    }
    return false;
}

static bool skip_number(rd *r) {
    size_t st = r->i;
    if (r->i < r->n && r->s[r->i] == '-') r->i++;
    if (r->i >= r->n) return false;
    if (r->s[r->i] == '0') r->i++;
    else if (r->s[r->i] >= '1' && r->s[r->i] <= '9') { while (r->i < r->n && r->s[r->i] >= '0' && r->s[r->i] <= '9') r->i++; }
    else return false;
    if (r->i < r->n && r->s[r->i] == '.') {
        r->i++;
        size_t d = r->i;
        while (r->i < r->n && r->s[r->i] >= '0' && r->s[r->i] <= '9') r->i++;
        if (r->i == d) return false;
    }
    if (r->i < r->n && (r->s[r->i] == 'e' || r->s[r->i] == 'E')) {
        r->i++;
        if (r->i < r->n && (r->s[r->i] == '+' || r->s[r->i] == '-')) r->i++;
        size_t d = r->i;
        while (r->i < r->n && r->s[r->i] >= '0' && r->s[r->i] <= '9') r->i++;
        if (r->i == d) return false;
    }
    return r->i > st;
}

static cb_json_type skip_value(rd *r, int depth);

static bool skip_container(rd *r, int depth, bool obj) {
    if (depth >= MAX_DEPTH) return false;
    r->i++;   // the bracket
    ws(r);
    char close = obj ? '}' : ']';
    if (r->i < r->n && r->s[r->i] == close) { r->i++; return true; }
    for (;;) {
        ws(r);
        if (obj) {
            if (!skip_string(r)) return false;
            ws(r);
            if (r->i >= r->n || r->s[r->i] != ':') return false;
            r->i++;
            ws(r);
        }
        if (skip_value(r, depth + 1) == CB_JSON_ERR) return false;
        ws(r);
        if (r->i >= r->n) return false;
        if (r->s[r->i] == ',') { r->i++; continue; }
        if (r->s[r->i] == close) { r->i++; return true; }
        return false;
    }
}

static cb_json_type skip_value(rd *r, int depth) {
    ws(r);
    if (r->i >= r->n) return CB_JSON_ERR;
    switch (r->s[r->i]) {
    case '{': return skip_container(r, depth, true) ? CB_JSON_OBJ : CB_JSON_ERR;
    case '[': return skip_container(r, depth, false) ? CB_JSON_ARR : CB_JSON_ERR;
    case '"': return skip_string(r) ? CB_JSON_STR : CB_JSON_ERR;
    case 't': return lit(r, "true") ? CB_JSON_BOOL : CB_JSON_ERR;
    case 'f': return lit(r, "false") ? CB_JSON_BOOL : CB_JSON_ERR;
    case 'n': return lit(r, "null") ? CB_JSON_NULL : CB_JSON_ERR;
    default:  return skip_number(r) ? CB_JSON_NUM : CB_JSON_ERR;
    }
}

bool cb_json_valid(const char *json, size_t n) {
    rd r = { json, n, 0 };
    if (skip_value(&r, 0) == CB_JSON_ERR) return false;
    ws(&r);
    return r.i == n;
}

// Is the string token at r->i (with quotes) equal, unescaped, to key[0..klen)?
static bool key_matches(cb_span tok, const char *key, size_t klen) {
    char buf[256];
    if (klen >= sizeof buf) return false;
    int k = cb_json_unescape(tok, buf, sizeof buf);
    return k >= 0 && (size_t)k == klen && memcmp(buf, key, klen) == 0;
}

cb_json_type cb_json_find(const char *json, size_t n, const char *path, cb_span *out) {
    rd r = { json, n, 0 };
    const char *seg = path ? path : "";
    for (;;) {
        ws(&r);
        if (*seg == 0) {
            size_t st = r.i;
            cb_json_type t = skip_value(&r, 0);
            if (t != CB_JSON_ERR && out) *out = cb_span_of(json + st, r.i - st);
            return t;
        }
        const char *dot = strchr(seg, '.');
        size_t klen = dot ? (size_t)(dot - seg) : strlen(seg);
        const char *next = dot ? dot + 1 : seg + klen;
        if (r.i >= r.n) return CB_JSON_ERR;
        char c = r.s[r.i];
        if (c == '{') {
            r.i++;
            ws(&r);
            if (r.i < r.n && r.s[r.i] == '}') return CB_JSON_MISSING;
            bool found = false;
            for (;;) {
                ws(&r);
                size_t ks = r.i;
                if (!skip_string(&r)) return CB_JSON_ERR;
                cb_span tok = cb_span_of(json + ks, r.i - ks);
                ws(&r);
                if (r.i >= r.n || r.s[r.i] != ':') return CB_JSON_ERR;
                r.i++;
                ws(&r);
                if (key_matches(tok, seg, klen)) { found = true; break; }
                if (skip_value(&r, 1) == CB_JSON_ERR) return CB_JSON_ERR;
                ws(&r);
                if (r.i >= r.n) return CB_JSON_ERR;
                if (r.s[r.i] == ',') { r.i++; continue; }
                if (r.s[r.i] == '}') return CB_JSON_MISSING;
                return CB_JSON_ERR;
            }
            if (!found) return CB_JSON_MISSING;
        } else if (c == '[') {
            // The segment must be a non-negative decimal index.
            if (klen == 0 || klen > 9) return CB_JSON_MISSING;
            long idx = 0;
            for (size_t k = 0; k < klen; k++) {
                if (seg[k] < '0' || seg[k] > '9') return CB_JSON_MISSING;
                idx = idx * 10 + (seg[k] - '0');
            }
            r.i++;
            ws(&r);
            if (r.i < r.n && r.s[r.i] == ']') return CB_JSON_MISSING;
            for (long k = 0; k < idx; k++) {
                if (skip_value(&r, 1) == CB_JSON_ERR) return CB_JSON_ERR;
                ws(&r);
                if (r.i >= r.n) return CB_JSON_ERR;
                if (r.s[r.i] == ']') return CB_JSON_MISSING;
                if (r.s[r.i] != ',') return CB_JSON_ERR;
                r.i++;
            }
        } else {
            // A scalar has no children.
            if (skip_value(&r, 0) == CB_JSON_ERR) return CB_JSON_ERR;
            return CB_JSON_MISSING;
        }
        seg = next;
    }
}

static int hex4(const char *p) {
    int v = 0;
    for (int k = 0; k < 4; k++) {
        char h = p[k];
        int d = hex_nibble((unsigned char)h);
        if (d < 0) return -1;
        v = v << 4 | d;
    }
    return v;
}

static size_t utf8_put(uint32_t cp, char *o) {
    if (cp < 0x80) { o[0] = (char)cp; return 1; }
    if (cp < 0x800) { o[0] = (char)(0xC0 | cp >> 6); o[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) {
        o[0] = (char)(0xE0 | cp >> 12); o[1] = (char)(0x80 | (cp >> 6 & 0x3F)); o[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    o[0] = (char)(0xF0 | cp >> 18); o[1] = (char)(0x80 | (cp >> 12 & 0x3F));
    o[2] = (char)(0x80 | (cp >> 6 & 0x3F)); o[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

int cb_json_unescape(cb_span tok, char *out, size_t cap) {
    if (tok.n < 2 || tok.p[0] != '"' || tok.p[tok.n - 1] != '"' || cap == 0) return -1;
    size_t o = 0;
    for (size_t i = 1; i + 1 < tok.n; i++) {
        char tmp[4];
        size_t k;
        unsigned char c = (unsigned char)tok.p[i];
        if (c < 0x20 || c == '"') return -1;
        if (c != '\\') { tmp[0] = (char)c; k = 1; }
        else {
            if (i + 1 >= tok.n - 1) return -1;
            char e = tok.p[++i];
            switch (e) {
            case '"': tmp[0] = '"'; k = 1; break;
            case '\\': tmp[0] = '\\'; k = 1; break;
            case '/': tmp[0] = '/'; k = 1; break;
            case 'b': tmp[0] = '\b'; k = 1; break;
            case 'f': tmp[0] = '\f'; k = 1; break;
            case 'n': tmp[0] = '\n'; k = 1; break;
            case 'r': tmp[0] = '\r'; k = 1; break;
            case 't': tmp[0] = '\t'; k = 1; break;
            case 'u': {
                if (i + 4 > tok.n - 2) return -1;
                int hi = hex4(tok.p + i + 1);
                if (hi < 0) return -1;
                i += 4;
                uint32_t cp = (uint32_t)hi;
                if (hi >= 0xD800 && hi <= 0xDBFF) {
                    // A high surrogate must be followed by \u and a low one.
                    if (i + 6 > tok.n - 2 || tok.p[i + 1] != '\\' || tok.p[i + 2] != 'u') return -1;
                    int lo = hex4(tok.p + i + 3);
                    if (lo < 0xDC00 || lo > 0xDFFF) return -1;
                    i += 6;
                    cp = 0x10000 + (((uint32_t)hi - 0xD800) << 10) + ((uint32_t)lo - 0xDC00);
                } else if (hi >= 0xDC00 && hi <= 0xDFFF) {
                    return -1;
                }
                k = utf8_put(cp, tmp);
                break;
            }
            default: return -1;
            }
        }
        if (o + k + 1 > cap) return -1;
        memcpy(out + o, tmp, k);
        o += k;
    }
    out[o] = 0;
    return (int)o;
}

bool cb_json_get_str(const char *json, size_t n, const char *path, char *out, size_t cap) {
    cb_span v;
    bool ok = cb_json_find(json, n, path, &v) == CB_JSON_STR && cb_json_unescape(v, out, cap) >= 0;
    // A failed unescape may have written part of the string: leave none.
    if (!ok && out && cap) out[0] = 0;
    return ok;
}

bool cb_json_get_i64(const char *json, size_t n, const char *path, int64_t *out) {
    cb_span v;
    if (cb_json_find(json, n, path, &v) != CB_JSON_NUM) return false;
    size_t i = 0;
    bool neg = false;
    if (v.p[0] == '-') { neg = true; i = 1; }
    uint64_t acc = 0;
    for (; i < v.n; i++) {
        char c = v.p[i];
        if (c < '0' || c > '9') return false;   // a fraction or an exponent
        if (acc > (UINT64_MAX - 9) / 10) return false;
        acc = acc * 10 + (uint64_t)(c - '0');
    }
    if (!neg && acc > (uint64_t)INT64_MAX) return false;
    if (neg && acc > (uint64_t)INT64_MAX + 1) return false;
    *out = neg ? (int64_t)(0 - acc) : (int64_t)acc;
    return true;
}

bool cb_json_get_bool(const char *json, size_t n, const char *path, bool *out) {
    cb_span v;
    if (cb_json_find(json, n, path, &v) != CB_JSON_BOOL) return false;
    *out = v.p[0] == 't';
    return true;
}

int cb_json_array_len(const char *json, size_t n, const char *path) {
    cb_span v;
    if (cb_json_find(json, n, path, &v) != CB_JSON_ARR) return -1;
    rd r = { v.p, v.n, 1 };
    ws(&r);
    if (r.i < r.n && r.s[r.i] == ']') return 0;
    int count = 0;
    for (;;) {
        if (skip_value(&r, 1) == CB_JSON_ERR) return -1;
        count++;
        ws(&r);
        if (r.i >= r.n) return -1;
        if (r.s[r.i] == ']') return count;
        if (r.s[r.i] != ',') return -1;
        r.i++;
    }
}
