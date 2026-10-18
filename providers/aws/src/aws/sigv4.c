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
#include "../../../shared/c/hex/hex.h"

#include <openssl/evp.h>
#include <openssl/opensslv.h>

#include <ctype.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if OPENSSL_VERSION_MAJOR < 3
#error "sigv4.c fetches SHA-256 once through OpenSSL 3's EVP_MD_fetch"
#endif

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

// ---- SHA-256 and HMAC-SHA256 ---------------------------------------------------
//
// OpenSSL 3's one-shot SHA256() and HMAC() fetch their algorithm by name on
// every call (evp_generic_fetch), which was a fifth of the signer's time. The
// digest is fetched once per process, and HMAC (RFC 2104) is written over it:
// two digests, the key padded to SHA-256's 64-byte block.

static EVP_MD *g_sha256;
static pthread_once_t g_sha256_once = PTHREAD_ONCE_INIT;

static void sha256_fetch(void) { g_sha256 = EVP_MD_fetch(NULL, "SHA256", NULL); }

static const EVP_MD *sha256_md(void) {
    pthread_once(&g_sha256_once, sha256_fetch);
    return g_sha256;
}

// The digest of the parts in order. false: no memory, or no SHA-256 in libcrypto.
static bool sha256_parts(const void *const *p, const size_t *n, int k, uint8_t out[32]) {
    const EVP_MD *md = sha256_md();
    EVP_MD_CTX *c = md ? EVP_MD_CTX_new() : NULL;
    bool ok = c && EVP_DigestInit_ex(c, md, NULL) == 1;
    for (int i = 0; ok && i < k; i++) ok = EVP_DigestUpdate(c, p[i], n[i]) == 1;
    unsigned int len = 0;
    ok = ok && EVP_DigestFinal_ex(c, out, &len) == 1 && len == 32;
    EVP_MD_CTX_free(c);
    return ok;
}

static bool sha256_hex(const void *p, size_t n, char out[65]) {
    uint8_t d[32];
    if (!sha256_parts(&p, &n, 1, d)) return false;
    hex_encode(d, sizeof d, out, 65);
    return true;
}

static bool hmac(const void *key, size_t klen, const void *msg, size_t mlen, uint8_t out[32]) {
    uint8_t k[64] = { 0 }, ipad[64], opad[64], inner[32];
    if (klen > sizeof k) {
        if (!sha256_parts(&key, &klen, 1, k)) return false;
    } else {
        memcpy(k, key, klen);
    }
    for (int i = 0; i < 64; i++) {
        ipad[i] = k[i] ^ 0x36;
        opad[i] = k[i] ^ 0x5c;
    }
    const void *ip[2] = { ipad, msg }, *op[2] = { opad, inner };
    size_t in[2] = { 64, mlen }, on[2] = { 64, 32 };
    return sha256_parts(ip, in, 2, inner) && sha256_parts(op, on, 2, out);
}

// ---- the signing key -------------------------------------------------------------
//
// The key is the HMAC chain "AWS4"+secret -> date -> region -> service ->
// "aws4_request": four HMACs that only change with the day, the region, the
// service or the credentials. Each thread keeps the last few it derived,
// keyed by all four inputs, so a request signs with one HMAC. A secret that
// changes (Lambda's session credentials rotate under a warm container) or a
// date that rolls over is a different key: it is derived, and the oldest
// entry gives way.

#define KEYS 4

typedef struct {
    bool    used;
    char    secret[sizeof(((cb_aws_creds *)0)->secret_key)];
    char    date[9];
    char    region[sizeof(((cb_aws_creds *)0)->region)];
    char    service[32];
    uint8_t key[32];
} signing_key;

static _Thread_local signing_key t_keys[KEYS];
static _Thread_local int t_keys_next;
static _Thread_local uint64_t t_derived;

uint64_t cb_sigv4_keys_derived(void) { return t_derived; }

static bool derive(const char *secret, const char *date, const char *region, const char *service, uint8_t out[32]) {
    char k0[4 + sizeof(((cb_aws_creds *)0)->secret_key)];
    size_t sl = strlen(secret);
    memcpy(k0, "AWS4", 4);
    memcpy(k0 + 4, secret, sl);
    uint8_t kd[32], kr[32], ks[32];
    bool ok = hmac(k0, 4 + sl, date, 8, kd) && hmac(kd, 32, region, strlen(region), kr) &&
              hmac(kr, 32, service, strlen(service), ks) && hmac(ks, 32, "aws4_request", 12, out);
    memset(k0, 0, sizeof k0);
    t_derived++;
    return ok;
}

// The key for (secret, the date's first 8 characters, region, service).
static bool signing_key_for(const char *secret, const char *date, const char *region, const char *service,
                            uint8_t out[32]) {
    if (strlen(secret) >= sizeof t_keys[0].secret || strlen(region) >= sizeof t_keys[0].region ||
        strlen(service) >= sizeof t_keys[0].service)
        return derive(secret, date, region, service, out);   // too long to keep: derived each time
    for (int i = 0; i < KEYS; i++) {
        const signing_key *e = &t_keys[i];
        if (e->used && memcmp(e->date, date, 8) == 0 && strcmp(e->secret, secret) == 0 &&
            strcmp(e->region, region) == 0 && strcmp(e->service, service) == 0) {
            memcpy(out, e->key, 32);
            return true;
        }
    }
    if (!derive(secret, date, region, service, out)) return false;
    signing_key *e = &t_keys[t_keys_next];
    t_keys_next = (t_keys_next + 1) % KEYS;
    memset(e, 0, sizeof *e);
    e->used = true;
    strcpy(e->secret, secret);
    memcpy(e->date, date, 8);
    strcpy(e->region, region);
    strcpy(e->service, service);
    memcpy(e->key, out, 32);
    return true;
}

static bool unreserved(unsigned char c) {
    return isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~';
}

void cb_sigv4_uri_encode(cb_buf *b, const char *s, size_t n, bool keep_slash) {
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (unreserved(c) || (keep_slash && c == '/')) cb_buf_putc(b, (char)c);
        else { cb_buf_putc(b, '%'); cb_buf_putc(b, hex_digit_upper(c >> 4)); cb_buf_putc(b, hex_digit_upper(c)); }
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
    cb_sigv4_uri_encode(canonical, path, strlen(path), true);
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
    if (!sha256_hex(r->payload ? r->payload : "", r->payload ? r->payload_len : 0, ph)) return CB_E_NOMEM;
    cb_buf_puts(canonical, ph);
    if (canonical->oom) return CB_E_NOMEM;

    // The string to sign.
    char ch[65];
    if (!sha256_hex(canonical->p, canonical->len, ch)) return CB_E_NOMEM;
    cb_buf_reset(sts);
    cb_buf_printf(sts, "AWS4-HMAC-SHA256\n%s\n%.8s/%s/%s/aws4_request\n%s", date, date, c->region, r->service, ch);
    if (sts->oom) return CB_E_NOMEM;

    // The signing key and the signature.
    uint8_t kk[32], sg[32];
    if (!signing_key_for(c->secret_key, date, c->region, r->service, kk) || !hmac(kk, 32, sts->p, sts->len, sg))
        return CB_E_NOMEM;
    hex_encode(sg, 32, sig, 65);
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
