// grpc.c - gRPC unary calls: see grpc.h.
#include "net/grpc.h"

#include "b64/b64.h"
#include "util/err.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_META 32

void cb_grpc_reply_init(cb_grpc_reply *r) {
    memset(r, 0, sizeof *r);
    r->code = -1;
    cb_buf_init(&r->message);
    cb_buf_init(&r->details);
    cb_h2_response_init(&r->resp);
}

void cb_grpc_reply_free(cb_grpc_reply *r) {
    cb_buf_free(&r->message);
    cb_buf_free(&r->details);
    cb_h2_response_free(&r->resp);
    cb_grpc_reply_init(r);
}

// Each coarser unit rounds up, so the server never sees a deadline earlier than
// the call's; the unit is chosen on the rounded value, which must fit 8 digits.
void cb_grpc_timeout(int64_t ms, char *out, size_t cap) {
    if (ms < 1) ms = 1;
    int64_t s = ms / 1000 + (ms % 1000 != 0), h = ms / 3600000 + (ms % 3600000 != 0);
    if (ms <= 99999999) snprintf(out, cap, "%lldm", (long long)ms);
    else if (s <= 99999999) snprintf(out, cap, "%lldS", (long long)s);
    else snprintf(out, cap, "%lldH", (long long)(h > 99999999 ? 99999999 : h));
}

// On CB_E_INVALID nothing is handed back, so a refused reply never shows a message.
int cb_grpc_unframe(const uint8_t *p, size_t n, const uint8_t **msg, size_t *len, bool *has) {
    const uint8_t *m = NULL;
    size_t ml_found = 0;
    bool found = false;
    *msg = NULL;
    *len = 0;
    *has = false;
    size_t at = 0;
    while (at < n) {
        if (n - at < 5) return CB_E_INVALID;   // a prefix cut short
        uint8_t flag = p[at];
        uint32_t ml = (uint32_t)p[at + 1] << 24 | (uint32_t)p[at + 2] << 16 | (uint32_t)p[at + 3] << 8 | p[at + 4];
        if (flag != 0) return CB_E_INVALID;     // compressed (never asked for), or a reserved bit
        if (ml > n - at - 5) return CB_E_INVALID;
        if (found) return CB_E_INVALID;         // a unary reply holds one message
        m = p + at + 5;
        ml_found = ml;
        found = true;
        at += 5 + (size_t)ml;
    }
    *msg = m;
    *len = ml_found;
    *has = found;
    return CB_OK;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

void cb_grpc_percent_decode(const char *p, size_t n, cb_buf *out) {
    for (size_t i = 0; i < n; i++) {
        int hi, lo;
        if (p[i] == '%' && i + 2 < n && (hi = hexval(p[i + 1])) >= 0 && (lo = hexval(p[i + 2])) >= 0) {
            cb_buf_putc(out, (char)(hi << 4 | lo));
            i += 2;
        } else {
            cb_buf_putc(out, p[i]);
        }
    }
}

// A field of the answer: the trailers first (a normal answer), then the headers
// (a trailers-only one carries everything there).
static bool field(const cb_h2_response *r, const char *name, cb_span *v) {
    return cb_h2_response_get(r, true, name, v) || cb_h2_response_get(r, false, name, v);
}

static int invalid(cb_grpc_reply *r, const char *why) {
    snprintf(r->resp.err, sizeof r->resp.err, "%s", why);
    return CB_E_INVALID;
}

int cb_grpc_unary(cb_h2_channel *ch, const char *path, const cb_h2_field *meta, int n_meta, const void *req,
                  size_t req_len, int timeout_ms, size_t max_reply, cb_grpc_reply *r) {
    cb_grpc_reply_free(r);
    if (n_meta < 0 || n_meta > MAX_META) return invalid(r, "too many metadata fields");
    if (req_len > 0xffffffffu) return invalid(r, "a request message over 4 GB");
    char timeout[16];
    cb_grpc_timeout(timeout_ms > 0 ? timeout_ms : 10000, timeout, sizeof timeout);
    cb_h2_field f[3 + MAX_META] = {
        { "content-type", "application/grpc", 12, 16, false },
        { "te", "trailers", 2, 8, false },
        { "grpc-timeout", timeout, 12, strlen(timeout), false },
    };
    for (int i = 0; i < n_meta; i++) f[3 + i] = meta[i];
    uint8_t prefix[5] = { 0, (uint8_t)(req_len >> 24), (uint8_t)(req_len >> 16), (uint8_t)(req_len >> 8),
                          (uint8_t)req_len };
    cb_h2_iov body[2] = { { prefix, 5 }, { req, req_len } };
    int rc = cb_h2_request(ch, "POST", path, f, 3 + n_meta, body, req_len ? 2 : 1, timeout_ms,
                           max_reply ? max_reply + 5 : 0, &r->resp);
    if (rc != CB_OK) return rc;
    r->http_status = r->resp.status;
    cb_span v;
    if (!field(&r->resp, "grpc-status", &v)) {
        r->code = -1;   // not a gRPC answer: only the HTTP status means anything
        return CB_OK;
    }
    int code = 0;
    if (v.n < 1 || v.n > 3) return invalid(r, "a grpc-status that does not parse");
    for (size_t i = 0; i < v.n; i++) {
        if (v.p[i] < '0' || v.p[i] > '9') return invalid(r, "a grpc-status that does not parse");
        code = code * 10 + (v.p[i] - '0');
    }
    r->code = code;
    if (field(&r->resp, "grpc-message", &v)) cb_grpc_percent_decode(v.p, v.n, &r->message);
    cb_buf_putc(&r->message, 0);
    r->message.len--;   // NUL-terminated, not counted
    if (field(&r->resp, "grpc-status-details-bin", &v)) {
        if (!cb_buf_reserve(&r->details, v.n)) return invalid(r, "no memory for the status details");
        int k = b64_decode(v.p, v.n, r->details.p, v.n + 1);
        if (k < 0) return invalid(r, "grpc-status-details-bin is not base64");
        r->details.len = (size_t)k;
    }
    if (r->http_status != 200) return CB_OK;   // a gRPC status on an HTTP error: no message to read
    if (cb_grpc_unframe(r->resp.body.p, r->resp.body.len, &r->msg, &r->msg_len, &r->has_msg) != CB_OK)
        return invalid(r, "the reply's message framing is malformed");
    if (code == CB_GRPC_OK && !r->has_msg) return invalid(r, "an OK unary reply without a message");
    if (code != CB_GRPC_OK) { r->msg = NULL; r->msg_len = 0; r->has_msg = false; }
    return CB_OK;
}
