// aws_call.c - one signed AWS request (aws_call.h).
#include "aws_call.h"

#include "sigv4.h"

#include "util/clock.h"
#include "util/err.h"
#include "util/log.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int cb_aws_url_parse(const char *url, char *host, size_t host_cap, int *port, char *base, size_t base_cap) {
    if (!url) return CB_E_INVALID;
    if (cb_starts_with(url, "https://")) url += 8;
    else if (cb_starts_with(url, "wss://")) url += 6;
    else return CB_E_INVALID;
    const char *slash = strchr(url, '/');
    const char *hend = slash ? slash : url + strlen(url);
    const char *colon = memchr(url, ':', (size_t)(hend - url));
    *port = 443;
    if (colon) {
        char *e;
        long p = strtol(colon + 1, &e, 10);
        if (e != hend || p <= 0 || p > 65535) return CB_E_INVALID;
        *port = (int)p;
    } else {
        colon = hend;
    }
    if (colon == url || !cb_strncpy0(host, host_cap, url, (size_t)(colon - url))) return CB_E_INVALID;
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

int cb_aws_call(const char *service, const char *method, const char *host, int port, const char *path,
                const char *const *headers, int n_headers, const void *body, size_t body_len, int timeout_ms,
                cb_https_response *out) {
    memset(out, 0, sizeof *out);
    cb_aws_creds c;
    if (cb_aws_creds_from_env(&c) != CB_OK) {
        CB_ERROR("aws", "%s: no AWS credentials (AWS_ACCESS_KEY_ID, AWS_SECRET_ACCESS_KEY, AWS_REGION)", service);
        return CB_E_IO;
    }
    char hosthdr[300];
    if (port == 443) cb_strlcpy(hosthdr, host, sizeof hosthdr);
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
    if (rc == CB_OK) rc = cb_https_request(host, port, method, path, cb_buf_cstr(&h), body, body_len, timeout_ms, out);
    cb_buf_free(&h);
    return rc;
}
