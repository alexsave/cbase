// The HTTP/1.1 parser and response writer - see http.h.
//
// LIFTED from foolish/server/impls/native/httpd.c (find_headers_end and
// parse_request_line_and_headers): the same single pass over the header block
// with fixed-width name compares. What changed:
//   * it reads a buffer and can be called again (CB_HTTP_MORE) instead of
//     looping on a blocking socket read;
//   * it records every header instead of picking out four, since the facade
//     reads apikey, Prefer, Accept, Range, Origin and the WebSocket ones;
//   * the native Content-Length scan accumulated digits into an int with no
//     bound (a 20-digit length was signed overflow); this one stops at the
//     body limit, and refuses a length that is not all digits, a second
//     differing Content-Length, and any Transfer-Encoding;
//   * fixed-size path/query/token copies are gone: they point into the buffer.
#include "http.h"

#include "util/err.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

const char *cb_http_header_get(const cb_http_req *r, const char *name) {
    for (int i = 0; i < r->n_headers; i++)
        if (strcasecmp(r->headers[i].name, name) == 0) return r->headers[i].value;
    return NULL;
}

const char *cb_http_bearer(const cb_http_req *r) {
    const char *v = cb_http_header_get(r, "authorization");
    if (!v || strncasecmp(v, "Bearer ", 7) != 0) return NULL;
    v += 7;
    while (*v == ' ') v++;
    return *v ? v : NULL;
}

int cb_http_query_each(const char *query, cb_query_fn fn, void *ud) {
    char key[1024], val[1024];
    int count = 0;
    const char *p = query ? query : "";
    while (*p) {
        const char *amp = strchr(p, '&');
        size_t n = amp ? (size_t)(amp - p) : strlen(p);
        if (n > 0) {
            const char *eq = memchr(p, '=', n);
            size_t kn = eq ? (size_t)(eq - p) : n;
            if (cb_url_decode(p, kn, key, sizeof key, true) < 0) return CB_E_INVALID;
            if (eq) { if (cb_url_decode(eq + 1, n - kn - 1, val, sizeof val, true) < 0) return CB_E_INVALID; }
            else val[0] = 0;
            count++;
            if (!fn(key, val, ud)) return count;
        }
        if (!amp) break;
        p = amp + 1;
    }
    return count;
}

typedef struct { const char *key; char *out; size_t cap; bool found; bool ok; } qget;

static bool qget_fn(const char *key, const char *value, void *ud) {
    qget *q = ud;
    if (strcmp(key, q->key) != 0) return true;
    q->found = true;
    q->ok = cb_strlcpy(q->out, value, q->cap) < q->cap;
    return false;
}

bool cb_http_query_get(const cb_http_req *r, const char *key, char *out, size_t cap) {
    qget q = { key, out, cap, false, false };
    if (cap == 0) return false;
    if (cb_http_query_each(r->query, qget_fn, &q) < 0) return false;
    return q.found && q.ok;
}

// ---- parse ---------------------------------------------------------------------

// The blank line ending the header block, by a plain byte scan (native httpd.c:
// strstr's two-way search measured as real cost even for this literal). The
// offset just past it, or 0 when not seen yet.
static size_t find_headers_end(const char *buf, size_t n) {
    for (size_t i = 0; i + 3 < n; i++)
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') return i + 4;
    return 0;
}

static bool is_tchar(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || strchr("!#$%&'*+-.^_`|~", c);
}

// The first pass: nothing is written. Finds where the head ends and how long
// the body is, so an incomplete request can be re-read later unchanged.
static int scan_head(const char *buf, size_t len, size_t max_body, size_t *head_len, size_t *body_len) {
    size_t he = find_headers_end(buf, len < CB_HTTP_MAX_HEAD ? len : CB_HTTP_MAX_HEAD);
    if (!he) return len >= CB_HTTP_MAX_HEAD ? -431 : CB_HTTP_MORE;
    size_t cl = 0;
    bool have_cl = false;
    int lines = 0;
    // Skip the request line.
    size_t i = 0;
    while (i < he && buf[i] != '\n') i++;
    i++;
    while (i < he - 2) {
        size_t ls = i;
        while (i < he && buf[i] != '\r') i++;
        size_t le = i;
        i += 2;   // CRLF
        if (le == ls) break;
        if (++lines > CB_HTTP_MAX_HEADERS) return -431;
        const char *colon = memchr(buf + ls, ':', le - ls);
        if (!colon) return -400;
        size_t nl = (size_t)(colon - (buf + ls));
        const char *v = colon + 1;
        while (v < buf + le && (*v == ' ' || *v == '\t')) v++;
        size_t vl = (size_t)(buf + le - v);
        while (vl > 0 && (v[vl - 1] == ' ' || v[vl - 1] == '\t')) vl--;
        if (nl == 14 && strncasecmp(buf + ls, "content-length", 14) == 0) {
            if (vl == 0) return -400;
            size_t x = 0;
            for (size_t k = 0; k < vl; k++) {
                if (v[k] < '0' || v[k] > '9') return -400;
                if (x > max_body) return -413;   // stop before it can overflow
                x = x * 10 + (size_t)(v[k] - '0');
            }
            if (x > max_body) return -413;
            if (have_cl && x != cl) return -400;
            cl = x;
            have_cl = true;
        } else if (nl == 17 && strncasecmp(buf + ls, "transfer-encoding", 17) == 0) {
            return -411;
        }
    }
    *head_len = he;
    *body_len = cl;
    return 1;
}

int cb_http_parse(char *buf, size_t len, size_t max_body, cb_http_req *out) {
    size_t he = 0, bl = 0;
    int rc = scan_head(buf, len, max_body, &he, &bl);
    if (rc <= 0) return rc;
    if (len - he < bl) return CB_HTTP_MORE;

    // The second pass writes NULs into the head.
    memset(out, 0, sizeof *out);
    char *p = buf, *end = buf + he;
    char *s = p;
    while (p < end && is_tchar((unsigned char)*p)) p++;
    if (p == s || p >= end || *p != ' ' || (size_t)(p - s) >= sizeof out->method) return -400;
    memcpy(out->method, s, (size_t)(p - s));
    p++;
    char *target = p;
    while (p < end && *p != ' ' && *p != '\r') p++;
    if (p == target || p >= end || *p != ' ') return -400;
    char *target_end = p;
    p++;
    if (end - p < 10 || memcmp(p, "HTTP/1.", 7) != 0 || p[7] < '0' || p[7] > '9') return -505;
    if (p[8] != '\r' || p[9] != '\n') return -400;
    out->minor = p[7] - '0';
    p += 10;   // "HTTP/1.x\r\n"
    *target_end = 0;
    if (target[0] != '/') return -400;   // origin-form only; Caddy never sends absolute-form
    char *q = strchr(target, '?');
    if (q) { *q = 0; out->query = q + 1; } else out->query = "";
    out->path = target;

    bool conn_close = false, conn_keep = false;
    while (p < end - 2) {
        char *ls = p;
        while (p < end && *p != '\r') p++;
        char *le = p;
        if (le == ls) break;
        // A bare LF inside a line, or a CR without its LF, is not HTTP.
        if (memchr(ls, '\n', (size_t)(le - ls)) || le + 1 >= end || le[1] != '\n') return -400;
        p += 2;
        char *colon = memchr(ls, ':', (size_t)(le - ls));
        if (!colon || colon == ls) return -400;
        for (char *c = ls; c < colon; c++) if (!is_tchar((unsigned char)*c)) return -400;
        char *v = colon + 1;
        while (v < le && (*v == ' ' || *v == '\t')) v++;
        char *ve = le;
        while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t')) ve--;
        *colon = 0;
        *ve = 0;
        cb_http_header *h = &out->headers[out->n_headers++];
        h->name = ls;
        h->value = v;
        if (strcasecmp(ls, "connection") == 0) {
            // A comma list; only the two tokens that decide keep-alive matter.
            for (char *t = v; *t; t++) {
                if (strncasecmp(t, "close", 5) == 0) conn_close = true;
                if (strncasecmp(t, "keep-alive", 10) == 0) conn_keep = true;
            }
        }
    }
    out->keep_alive = out->minor >= 1 ? !conn_close : conn_keep;
    out->body = bl ? (const uint8_t *)buf + he : NULL;
    out->body_len = bl;
    return (int)(he + bl);
}

// ---- respond -------------------------------------------------------------------

void cb_res_init(cb_http_res *res) {
    memset(res, 0, sizeof *res);
    res->status = 200;
    cb_buf_init(&res->headers);
    cb_buf_init(&res->body);
}

void cb_res_free(cb_http_res *res) {
    cb_buf_free(&res->headers);
    cb_buf_free(&res->body);
}

void cb_res_status(cb_http_res *res, int status) { res->status = status; }

void cb_res_header(cb_http_res *res, const char *name, const char *value) {
    if (strpbrk(name, "\r\n:") || strpbrk(value, "\r\n")) return;
    cb_buf_printf(&res->headers, "%s: %s\r\n", name, value);
}

bool cb_res_has_header(const cb_http_res *res, const char *name) {
    size_t nl = strlen(name);
    const char *h = cb_buf_cstr(&res->headers);
    while (*h) {
        if (strncasecmp(h, name, nl) == 0 && h[nl] == ':') return true;
        const char *nx = strstr(h, "\r\n");
        if (!nx) break;
        h = nx + 2;
    }
    return false;
}

void cb_res_body(cb_http_res *res, const void *p, size_t n) {
    cb_buf_reset(&res->body);
    cb_buf_append(&res->body, p, n);
}

void cb_res_append(cb_http_res *res, const void *p, size_t n) { cb_buf_append(&res->body, p, n); }

void cb_res_send(cb_http_res *res, int status, const char *content_type, const void *body, size_t n) {
    res->status = status;
    if (content_type) cb_res_header(res, "Content-Type", content_type);
    cb_res_body(res, body, n);
}

void cb_res_text(cb_http_res *res, int status, const char *text) {
    cb_res_send(res, status, "text/plain; charset=utf-8", text, strlen(text));
}

const char *cb_http_reason(int status) {
    switch (status) {
    case 101: return "Switching Protocols";
    case 200: return "OK";
    case 201: return "Created";
    case 202: return "Accepted";
    case 204: return "No Content";
    case 206: return "Partial Content";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 406: return "Not Acceptable";
    case 408: return "Request Timeout";
    case 409: return "Conflict";
    case 411: return "Length Required";
    case 413: return "Content Too Large";
    case 415: return "Unsupported Media Type";
    case 422: return "Unprocessable Content";
    case 426: return "Upgrade Required";
    case 429: return "Too Many Requests";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 503: return "Service Unavailable";
    case 505: return "HTTP Version Not Supported";
    default:  return "Unknown";
    }
}

void cb_res_serialize(const cb_http_res *res, bool keep_alive, cb_buf *out) {
    cb_buf_printf(out, "HTTP/1.1 %d %s\r\n", res->status, cb_http_reason(res->status));
    cb_buf_append(out, res->headers.p ? res->headers.p : (const uint8_t *)"", res->headers.len);
    // 1xx and 204 carry no body and no Content-Length (RFC 9110 8.6).
    if (res->status >= 200 && res->status != 204) cb_buf_printf(out, "Content-Length: %zu\r\n", res->body.len);
    if (res->status != 101) cb_buf_puts(out, keep_alive ? "Connection: keep-alive\r\n" : "Connection: close\r\n");
    cb_buf_puts(out, "\r\n");
    if (res->status >= 200 && res->status != 204 && res->body.len)
        cb_buf_append(out, res->body.p, res->body.len);
}
