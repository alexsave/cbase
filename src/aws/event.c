// event.c - the Lambda envelope in and out (event.h).
//
// Decoding validates the whole event as JSON first (cb_json_valid), then reads
// only the fields event.h lists. Every string the request keeps is copied
// into ev->storage; pointers into it are set at the end, once the buffer has
// stopped growing, so a realloc can never leave one dangling.
#include "event.h"

#include "util/b64.h"
#include "util/err.h"
#include "util/json.h"
#include "util/log.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

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
        int k = cb_b64_decode((const char *)st->p + o, tn, st->p + o, tn + 1);
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

// ---- WebSocket ---------------------------------------------------------------------

static void url_encode(cb_buf *b, const char *s, size_t n) {
    static const char H[] = "0123456789ABCDEF";
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') cb_buf_putc(b, (char)c);
        else { cb_buf_putc(b, '%'); cb_buf_putc(b, H[c >> 4]); cb_buf_putc(b, H[c & 15]); }
    }
}

typedef struct {
    cb_buf q;
    bool   bad;
} query_acc;

static bool on_param(cb_span key, cb_span val, cb_json_type t, void *ud) {
    query_acc *a = ud;
    if (t != CB_JSON_STR) return true;
    char k[1024], v[4096];
    int kn = cb_json_unescape(key, k, sizeof k), vn = cb_json_unescape(val, v, sizeof v);
    if (kn < 0 || vn < 0) { a->bad = true; return false; }
    if (a->q.len) cb_buf_putc(&a->q, '&');
    url_encode(&a->q, k, (size_t)kn);
    cb_buf_putc(&a->q, '=');
    url_encode(&a->q, v, (size_t)vn);
    return true;
}

static int decode_ws(const char *json, size_t len, cb_event *ev) {
    cb_ws_event *w = &ev->ws;
    cb_buf *st = &ev->storage;
    if (!cb_json_get_str(json, len, "requestContext.routeKey", w->route_key, sizeof w->route_key) ||
        !cb_json_get_str(json, len, "requestContext.connectionId", w->connection_id, sizeof w->connection_id))
        return CB_E_INVALID;
    cb_json_get_str(json, len, "requestContext.domainName", w->domain, sizeof w->domain);
    cb_json_get_str(json, len, "requestContext.stage", w->stage, sizeof w->stage);

    query_acc qa;
    cb_buf_init(&qa.q);
    qa.bad = false;
    cb_span obj;
    if (cb_json_find(json, len, "queryStringParameters", &obj) == CB_JSON_OBJ && !walk(obj, on_param, &qa)) qa.bad = true;
    long query = qa.bad ? -1 : put_bytes(st, qa.q.p ? qa.q.p : (const uint8_t *)"", qa.q.len);
    cb_buf_free(&qa.q);
    if (query < 0) return CB_E_INVALID;

    long body;
    size_t body_len;
    int rc = put_body(st, json, len, &body, &body_len);
    if (rc != CB_OK) return rc;
    if (st->oom) return CB_E_NOMEM;
    const char *base = (const char *)st->p;
    w->query = base + query;
    w->body = body >= 0 ? (const uint8_t *)base + body : (const uint8_t *)base + query + strlen(base + query);
    w->body_len = body_len;
    return CB_OK;
}

// ---- the string literals ------------------------------------------------------------

static bool game_id_ok(const char *s, size_t n, size_t cap) {
    if (n == 0 || n >= cap) return false;
    for (size_t i = 0; i < n; i++)
        if (!isalnum((unsigned char)s[i]) && s[i] != '-' && s[i] != '_') return false;
    return true;
}

// "scan" and "bot_drive:<id>": the quotes stripped, nothing else parsed.
static int decode_literal(const char *s, size_t n, cb_event *ev) {
    if (n < 2 || s[n - 1] != '"') return CB_E_INVALID;
    s++;
    n -= 2;
    if (n == 4 && memcmp(s, "scan", 4) == 0) { ev->kind = CB_EVENT_SCHEDULE; return CB_OK; }
    static const char P[] = "bot_drive:";
    if (n > sizeof P - 1 && memcmp(s, P, sizeof P - 1) == 0) {
        const char *id = s + sizeof P - 1;
        size_t idn = n - (sizeof P - 1);
        if (!game_id_ok(id, idn, sizeof ev->game_id)) return CB_E_INVALID;
        memcpy(ev->game_id, id, idn);
        ev->game_id[idn] = 0;
        ev->kind = CB_EVENT_BOT_DRIVE;
        return CB_OK;
    }
    return CB_E_INVALID;
}

int cb_event_bot_drive_payload(const char *game_id, char *out, size_t cap) {
    size_t n = game_id ? strlen(game_id) : 0;
    if (!game_id_ok(game_id, n, sizeof ((cb_event *)0)->game_id)) return CB_E_INVALID;
    int k = snprintf(out, cap, "\"bot_drive:%s\"", game_id);
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
    if (cb_json_find(json, len, "requestContext.connectionId", NULL) == CB_JSON_STR) {
        out->kind = CB_EVENT_WS;
        rc = decode_ws(json, len, out);
    } else if (cb_json_find(json, len, "requestContext.http.method", NULL) == CB_JSON_STR) {
        out->kind = CB_EVENT_HTTP;
        rc = decode_http(json, len, out);
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

void cb_event_free(cb_event *ev) { cb_buf_free(&ev->storage); }

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
        size_t cap = cb_b64_len(n) + 1;
        char *enc = malloc(cap);
        if (!enc) return CB_E_NOMEM;
        cb_b64_encode(body, n, enc, cap);
        cb_jw_key(&w, "body");
        cb_jw_strn(&w, enc, cap - 1);
        free(enc);
    } else {
        cb_jw_key(&w, "body");
        cb_jw_strn(&w, (const char *)body, n);
    }
    cb_jw_kbool(&w, "isBase64Encoded", b64);
    cb_jw_end(&w);
    return cb_jw_ok(&w) ? CB_OK : CB_E_NOMEM;
}

int cb_event_encode_ws(int status, cb_buf *out) {
    cb_jw w;
    cb_jw_init(&w, out);
    cb_jw_obj(&w);
    cb_jw_kint(&w, "statusCode", status);
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

void cb_event_log_raw(const char *json, size_t len) {
    cb_buf r;
    cb_buf_init(&r);
    if (cb_event_redact(json, len, 4096, &r) == CB_OK) {
        // A log line holds CB_LOG_MSG_MAX bytes, so the event goes in pieces.
        enum { PIECE = 200 };
        const char *p = cb_buf_cstr(&r);
        size_t pieces = (r.len + PIECE - 1) / PIECE;
        for (size_t k = 0; k < pieces; k++) {
            size_t o = k * PIECE, n = r.len - o < PIECE ? r.len - o : PIECE;
            CB_WARN("event", "raw %zu/%zu (%zu bytes): %.*s", k + 1, pieces, len, (int)n, p + o);
        }
    }
    cb_buf_free(&r);
}
