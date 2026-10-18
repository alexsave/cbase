// aws_call.c - one signed AWS request (aws_call.h).
#include "aws_call.h"

#include "sigv4.h"

#include "util/clock.h"
#include "util/err.h"
#include "util/log.h"
#include "../../../shared/c/num_parse.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// Is `host` AWS's, however spelled (finding L12): amazonaws.com or
// amazonaws.com.cn, or a name under either, any case, with or without the
// trailing dot DNS allows. Plain HTTP is never sent to one.
static bool aws_host(const char *host) {
    size_t hl = strlen(host);
    if (hl && host[hl - 1] == '.') hl--;
    static const char *const roots[] = { "amazonaws.com", "amazonaws.com.cn" };
    for (size_t i = 0; i < sizeof roots / sizeof roots[0]; i++) {
        size_t rl = strlen(roots[i]);
        if (hl < rl || strncasecmp(host + hl - rl, roots[i], rl) != 0) continue;
        if (hl == rl || host[hl - rl - 1] == '.') return true;
    }
    return false;
}

int cb_aws_url_parse(const char *url, char *host, size_t host_cap, int *port, bool *tls, char *base, size_t base_cap) {
    if (!url) return CB_E_INVALID;
    *tls = true;
    if (cb_starts_with(url, "https://")) url += 8;
    else if (cb_starts_with(url, "wss://")) url += 6;
    else if (cb_starts_with(url, "http://")) { url += 7; *tls = false; }
    else return CB_E_INVALID;
    const char *slash = strchr(url, '/');
    const char *hend = slash ? slash : url + strlen(url);
    const char *colon = memchr(url, ':', (size_t)(hend - url));
    *port = *tls ? 443 : 80;
    if (colon) {
        int64_t p;
        if (!num_parse_i64(colon + 1, (size_t)(hend - colon - 1), 1, 65535, &p)) return CB_E_INVALID;
        *port = (int)p;
    } else {
        colon = hend;
    }
    if (colon == url || !cb_strncpy0(host, host_cap, url, (size_t)(colon - url))) return CB_E_INVALID;
    if (!*tls && aws_host(host)) return CB_E_INVALID;
    size_t bl = slash ? strlen(slash) : 0;
    while (bl > 0 && slash[bl - 1] == '/') bl--;
    if (!cb_strncpy0(base, base_cap, slash ? slash : "", bl)) return CB_E_INVALID;
    return CB_OK;
}

void cb_aws_path_segment(cb_buf *b, const char *s) {
    static const char H[] = "0123456789ABCDEF";
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') cb_buf_putc(b, (char)c);
        else { cb_buf_putc(b, '%'); cb_buf_putc(b, H[c >> 4]); cb_buf_putc(b, H[c & 15]); }
    }
}

int cb_aws_call(const char *service, const char *method, const char *host, int port, bool tls, const char *path,
                const char *const *headers, int n_headers, const void *body, size_t body_len, int timeout_ms,
                cb_https_response *out) {
    return cb_aws_call_region(NULL, service, method, host, port, tls, path, headers, n_headers, body, body_len,
                              timeout_ms, out);
}

int cb_aws_call_region(const char *region, const char *service, const char *method, const char *host, int port, bool tls,
                       const char *path, const char *const *headers, int n_headers, const void *body, size_t body_len,
                       int timeout_ms, cb_https_response *out) {
    memset(out, 0, sizeof *out);
    cb_aws_creds c;
    if (cb_aws_creds_from_env(&c) != CB_OK) {
        CB_ERROR("aws", "%s: no AWS credentials (AWS_ACCESS_KEY_ID, AWS_SECRET_ACCESS_KEY, AWS_REGION)", service);
        return CB_E_IO;
    }
    if (region && !cb_strncpy0(c.region, sizeof c.region, region, strlen(region))) return CB_E_INVALID;
    char hosthdr[300];
    if (port == (tls ? 443 : 80)) cb_strlcpy(hosthdr, host, sizeof hosthdr);
    else snprintf(hosthdr, sizeof hosthdr, "%s:%d", host, port);
    const char *q = strchr(path, '?');
    char p[2048];
    if (!cb_strncpy0(p, sizeof p, path, q ? (size_t)(q - path) : strlen(path))) return CB_E_INVALID;
    cb_sigv4_req r = {
        .method = method, .host = hosthdr, .path = p, .query = q ? q + 1 : "", .service = service,
        .headers = headers, .n_headers = n_headers, .payload = body, .payload_len = body_len, .now_ms = cb_now_ms(),
    };
    cb_buf h;
    cb_buf_init(&h);
    for (int i = 0; i < n_headers; i++) {
        cb_buf_puts(&h, headers[i]);
        cb_buf_puts(&h, "\r\n");
    }
    int rc = cb_sigv4_sign(&c, &r, &h);
    if (rc == CB_OK)
        rc = (tls ? cb_https_request : cb_http_plain_request)(host, port, method, path, cb_buf_cstr(&h), body, body_len,
                                                              timeout_ms, out);
    cb_buf_free(&h);
    return rc;
}
