// sigv4.c - AWS Signature Version 4 (sigv4.h).
//
// The algorithm, from AWS's "Create a signed AWS API request":
//   canonical request = METHOD \n canonical URI \n canonical query \n
//                       canonical headers (name:value\n, sorted) \n
//                       signed header names (;-joined) \n hex(sha256(payload))
//   string to sign    = "AWS4-HMAC-SHA256" \n <amz date> \n <scope> \n hex(sha256(canonical request))
//   key               = HMAC chain "AWS4"+secret -> date -> region -> service -> "aws4_request"
//   signature         = hex(HMAC(key, string to sign))
#include "sigv4.h"

#include "util/err.h"
#include "util/hex.h"

#include <openssl/hmac.h>
#include <openssl/sha.h>

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_SIGNED 32

static bool env_copy(const char *name, char *out, size_t cap, bool required) {
    const char *v = getenv(name);
    if (!v || !*v) { out[0] = 0; return !required; }
    return cb_strlcpy(out, v, cap) < cap;
}

int cb_aws_creds_from_env(cb_aws_creds *out) {
    memset(out, 0, sizeof *out);
    if (!env_copy("AWS_ACCESS_KEY_ID", out->access_key, sizeof out->access_key, true)) return CB_E_INVALID;
    if (!env_copy("AWS_SECRET_ACCESS_KEY", out->secret_key, sizeof out->secret_key, true)) return CB_E_INVALID;
    if (!env_copy("AWS_SESSION_TOKEN", out->session_token, sizeof out->session_token, false)) return CB_E_INVALID;
    if (!env_copy("AWS_REGION", out->region, sizeof out->region, true) &&
        !env_copy("AWS_DEFAULT_REGION", out->region, sizeof out->region, true)) return CB_E_INVALID;
    return CB_OK;
}

static void sha256_hex(const void *p, size_t n, char out[65]) {
    uint8_t d[SHA256_DIGEST_LENGTH];
    SHA256(p, n, d);
    cb_hex_encode(d, sizeof d, out, 65);
}

static void hmac(const void *key, size_t klen, const void *msg, size_t mlen, uint8_t out[32]) {
    unsigned int len = 32;
    HMAC(EVP_sha256(), key, (int)klen, msg, mlen, out, &len);
}

static bool unreserved(unsigned char c) {
    return isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~';
}

// Percent-encodes every byte but the unreserved ones and (when keep_slash) '/'.
static void uri_encode(cb_buf *b, const char *s, size_t n, bool keep_slash) {
    static const char H[] = "0123456789ABCDEF";
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (unreserved(c) || (keep_slash && c == '/')) cb_buf_putc(b, (char)c);
        else { cb_buf_putc(b, '%'); cb_buf_putc(b, H[c >> 4]); cb_buf_putc(b, H[c & 15]); }
    }
}

typedef struct {
    char name[64];
    const char *value;  // trimmed start
    size_t vlen;        // trimmed length
} hdr;

static int hdr_cmp(const void *a, const void *b) { return strcmp(((const hdr *)a)->name, ((const hdr *)b)->name); }

typedef struct {
    const char *p;
    size_t n;
} qpair;

static int q_cmp(const void *a, const void *b) {
    const qpair *x = a, *y = b;
    // Sort by key, then value: compare up to '=' first.
    const char *xe = memchr(x->p, '=', x->n), *ye = memchr(y->p, '=', y->n);
    size_t xk = xe ? (size_t)(xe - x->p) : x->n, yk = ye ? (size_t)(ye - y->p) : y->n;
    int c = memcmp(x->p, y->p, xk < yk ? xk : yk);
    if (c) return c;
    if (xk != yk) return xk < yk ? -1 : 1;
    size_t m = x->n < y->n ? x->n : y->n;
    c = memcmp(x->p, y->p, m);
    return c ? c : (x->n < y->n ? -1 : x->n > y->n);
}

// Appends a header value with leading/trailing blanks dropped and inner runs
// of blanks collapsed to one space (SigV4's "trimall").
static void put_trimmed(cb_buf *b, const char *v, size_t n) {
    bool space = false, any = false;
    for (size_t i = 0; i < n; i++) {
        char c = v[i];
        if (c == ' ' || c == '\t') { space = any; continue; }
        if (space) cb_buf_putc(b, ' ');
        space = false;
        any = true;
        cb_buf_putc(b, c);
    }
}

static void amz_date(int64_t now_ms, char out[17]) {
    time_t t = (time_t)(now_ms / 1000);
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(out, 17, "%Y%m%dT%H%M%SZ", &tm);
}

int cb_sigv4_debug(const cb_aws_creds *c, const cb_sigv4_req *r, cb_buf *canonical, cb_buf *sts, char sig[65]) {
    if (r->n_headers < 0 || r->n_headers + 3 > MAX_SIGNED) return CB_E_INVALID;
    char date[17];
    amz_date(r->now_ms, date);

    hdr h[MAX_SIGNED];
    int nh = 0;
    for (int i = 0; i < r->n_headers; i++) {
        const char *line = r->headers[i];
        const char *colon = strchr(line, ':');
        if (!colon || colon == line || (size_t)(colon - line) >= sizeof h[0].name) return CB_E_INVALID;
        size_t k = 0;
        for (const char *p = line; p < colon; p++) h[nh].name[k++] = (char)tolower((unsigned char)*p);
        while (k > 0 && (h[nh].name[k - 1] == ' ' || h[nh].name[k - 1] == '\t')) k--;
        h[nh].name[k] = 0;
        h[nh].value = colon + 1;
        h[nh].vlen = strlen(colon + 1);
        nh++;
    }
    cb_strlcpy(h[nh].name, "host", sizeof h[nh].name);
    h[nh].value = r->host;
    h[nh++].vlen = strlen(r->host);
    cb_strlcpy(h[nh].name, "x-amz-date", sizeof h[nh].name);
    h[nh].value = date;
    h[nh++].vlen = strlen(date);
    if (c->session_token[0]) {
        cb_strlcpy(h[nh].name, "x-amz-security-token", sizeof h[nh].name);
        h[nh].value = c->session_token;
        h[nh++].vlen = strlen(c->session_token);
    }
    qsort(h, (size_t)nh, sizeof h[0], hdr_cmp);
    for (int i = 1; i < nh; i++)
        if (strcmp(h[i].name, h[i - 1].name) == 0) return CB_E_INVALID;   // one line per signed name

    // The canonical request.
    cb_buf_reset(canonical);
    cb_buf_puts(canonical, r->method);
    cb_buf_putc(canonical, '\n');
    const char *path = r->path && *r->path ? r->path : "/";
    uri_encode(canonical, path, strlen(path), true);
    cb_buf_putc(canonical, '\n');
    const char *q = r->query ? r->query : "";
    if (*q) {
        qpair qp[64];
        size_t nq = 0, qn = strlen(q);
        for (size_t s = 0; s <= qn;) {
            const char *amp = memchr(q + s, '&', qn - s);
            size_t e = amp ? (size_t)(amp - q) : qn;
            if (e > s) {
                if (nq == 64) return CB_E_INVALID;
                qp[nq++] = (qpair){ q + s, e - s };
            }
            s = e + 1;
        }
        qsort(qp, nq, sizeof qp[0], q_cmp);
        for (size_t i = 0; i < nq; i++) {
            if (i) cb_buf_putc(canonical, '&');
            cb_buf_append(canonical, qp[i].p, qp[i].n);
            if (!memchr(qp[i].p, '=', qp[i].n)) cb_buf_putc(canonical, '=');
        }
    }
    cb_buf_putc(canonical, '\n');
    for (int i = 0; i < nh; i++) {
        cb_buf_puts(canonical, h[i].name);
        cb_buf_putc(canonical, ':');
        put_trimmed(canonical, h[i].value, h[i].vlen);
        cb_buf_putc(canonical, '\n');
    }
    cb_buf_putc(canonical, '\n');
    for (int i = 0; i < nh; i++) {
        if (i) cb_buf_putc(canonical, ';');
        cb_buf_puts(canonical, h[i].name);
    }
    cb_buf_putc(canonical, '\n');
    char ph[65];
    sha256_hex(r->payload ? r->payload : "", r->payload ? r->payload_len : 0, ph);
    cb_buf_puts(canonical, ph);
    if (canonical->oom) return CB_E_NOMEM;

    // The string to sign.
    char ch[65];
    sha256_hex(canonical->p, canonical->len, ch);
    cb_buf_reset(sts);
    cb_buf_printf(sts, "AWS4-HMAC-SHA256\n%s\n%.8s/%s/%s/aws4_request\n%s", date, date, c->region, r->service, ch);
    if (sts->oom) return CB_E_NOMEM;

    // The signing key and the signature.
    char k0[4 + sizeof c->secret_key];
    size_t sl = strlen(c->secret_key);
    memcpy(k0, "AWS4", 4);
    memcpy(k0 + 4, c->secret_key, sl);
    uint8_t kd[32], kr[32], ks[32], kk[32], sg[32];
    hmac(k0, 4 + sl, date, 8, kd);
    hmac(kd, 32, c->region, strlen(c->region), kr);
    hmac(kr, 32, r->service, strlen(r->service), ks);
    hmac(ks, 32, "aws4_request", 12, kk);
    hmac(kk, 32, sts->p, sts->len, sg);
    cb_hex_encode(sg, 32, sig, 65);
    return CB_OK;
}

int cb_sigv4_sign(const cb_aws_creds *c, const cb_sigv4_req *r, cb_buf *out) {
    cb_buf canonical, sts;
    cb_buf_init(&canonical);
    cb_buf_init(&sts);
    char sig[65];
    int rc = cb_sigv4_debug(c, r, &canonical, &sts, sig);
    if (rc == CB_OK) {
        // The signed header names are the canonical request's second-to-last line.
        const char *end = (const char *)canonical.p + canonical.len;
        const char *last = end;
        while (last > (const char *)canonical.p && last[-1] != '\n') last--;
        const char *signed_end = last - 1;
        const char *signed_start = signed_end;
        while (signed_start > (const char *)canonical.p && signed_start[-1] != '\n') signed_start--;
        // The string to sign's second line is the date, its third the scope.
        const char *d = strchr(cb_buf_cstr(&sts), '\n') + 1;
        const char *scope = strchr(d, '\n') + 1;
        const char *scope_end = strchr(scope, '\n');
        cb_buf_printf(out, "Authorization: AWS4-HMAC-SHA256 Credential=%s/%.*s, SignedHeaders=%.*s, Signature=%s\r\n",
                      c->access_key, (int)(scope_end - scope), scope, (int)(signed_end - signed_start), signed_start, sig);
        cb_buf_printf(out, "X-Amz-Date: %.16s\r\n", d);
        if (c->session_token[0]) cb_buf_printf(out, "X-Amz-Security-Token: %s\r\n", c->session_token);
        if (out->oom) rc = CB_E_NOMEM;
    }
    cb_buf_free(&canonical);
    cb_buf_free(&sts);
    return rc;
}
