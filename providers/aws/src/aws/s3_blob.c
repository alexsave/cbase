// s3_blob.c - see s3_blob.h and util/blob.h.
#include "aws/s3_blob.h"

#include "aws/aws_call.h"
#include "aws/s3_post.h"
#include "net/https.h"
#include "sigv4.h"
#include "util/blob.h"
#include "util/clock.h"
#include "util/err.h"
#include "util/log.h"
#include "hex/hex.h"
#include "num_parse.h"
#include "sha256.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MOD "s3"

// ---- the instance role ---------------------------------------------------------------------

// The string value of "key" in a flat document: "key" : "value" (no escapes in what the service sends).
static bool doc_str(const char *doc, const char *key, char *out, size_t cap) {
    char pat[48];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(doc, pat);
    if (!p) return false;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p++ != ':') return false;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p++ != '"') return false;
    const char *e = strchr(p, '"');
    if (!e || (size_t)(e - p) >= cap) return false;
    memcpy(out, p, (size_t)(e - p));
    out[e - p] = 0;
    return true;
}

int cb_imds_parse(const char *doc, cb_aws_creds *out, int64_t *expiry_ms) {
    char exp[40];
    if (!doc_str(doc, "AccessKeyId", out->access_key, sizeof out->access_key) ||
        !doc_str(doc, "SecretAccessKey", out->secret_key, sizeof out->secret_key) ||
        !doc_str(doc, "Token", out->session_token, sizeof out->session_token) || !doc_str(doc, "Expiration", exp, sizeof exp))
        return CB_E_INVALID;
    struct tm tm;
    memset(&tm, 0, sizeof tm);
    if (!strptime(exp, "%Y-%m-%dT%H:%M:%SZ", &tm)) return CB_E_INVALID;
    *expiry_ms = (int64_t)timegm(&tm) * 1000;
    return CB_OK;
}

static int imds_get(const char *host, int port, const char *method, const char *path, const char *hdrs, cb_buf *out) {
    cb_https_response r;
    memset(&r, 0, sizeof r);
    // The instance metadata service, for credentials: not the store's call, though plain HTTP.
    int rc = cb_http_plain_request(CB_HTTPS_OTHER, host, port, method, path, hdrs, NULL, 0, 3000, &r);
    if (rc == CB_OK && r.status == 200) cb_buf_append(out, r.body.p, r.body.len);
    else if (rc == CB_OK) rc = CB_E_IO;
    cb_https_response_free(&r);
    return rc;
}

static int imds_fetch(const char *host, int port, cb_aws_creds *out, int64_t *expiry) {
    cb_buf tok, role, doc;
    cb_buf_init(&tok);
    cb_buf_init(&role);
    cb_buf_init(&doc);
    int rc = imds_get(host, port, "PUT", "/latest/api/token", "X-aws-ec2-metadata-token-ttl-seconds: 300\r\n", &tok);
    char h[600], path[200];
    if (rc == CB_OK) {
        snprintf(h, sizeof h, "X-aws-ec2-metadata-token: %s\r\n", cb_buf_cstr(&tok));
        rc = imds_get(host, port, "GET", "/latest/meta-data/iam/security-credentials/", h, &role);
    }
    if (rc == CB_OK) {
        char name[128];
        size_t n = strcspn(cb_buf_cstr(&role), "\r\n");
        if (n == 0 || n >= sizeof name) rc = CB_E_NOT_FOUND;
        else {
            memcpy(name, cb_buf_cstr(&role), n);
            name[n] = 0;
            snprintf(path, sizeof path, "/latest/meta-data/iam/security-credentials/%s", name);
            rc = imds_get(host, port, "GET", path, h, &doc);
        }
    }
    if (rc == CB_OK) rc = cb_imds_parse(cb_buf_cstr(&doc), out, expiry);
    cb_buf_free(&tok);
    cb_buf_free(&role);
    cb_buf_free(&doc);
    return rc;
}

static pthread_mutex_t g_cred_mu = PTHREAD_MUTEX_INITIALIZER;
static cb_aws_creds g_cred;
static int64_t g_cred_expiry;

int cb_aws_creds_get(cb_aws_creds *out) {
    if (cb_aws_creds_from_env(out) == CB_OK) return CB_OK;
    pthread_mutex_lock(&g_cred_mu);
    int rc = CB_OK;
    if (g_cred_expiry - cb_now_ms() < 5 * 60 * 1000) {
        cb_aws_creds c;
        memset(&c, 0, sizeof c);
        int64_t exp = 0;
        const char *ep = getenv("CBASE_IMDS_ENDPOINT");
        if (ep && *ep) {
            char host[128];
            const char *colon = strrchr(ep, ':');
            size_t hl = colon ? (size_t)(colon - ep) : strlen(ep);
            if (hl >= sizeof host) hl = sizeof host - 1;
            memcpy(host, ep, hl);
            host[hl] = 0;
            int64_t port = 80;
            if (colon && !num_parse_i64(colon + 1, strlen(colon + 1), 1, 65535, &port)) rc = CB_E_INVALID;
            else rc = imds_fetch(host, (int)port, &c, &exp);
        } else {
            rc = imds_fetch("fd00:ec2::254", 80, &c, &exp);
            if (rc != CB_OK) rc = imds_fetch("169.254.169.254", 80, &c, &exp);
        }
        if (rc == CB_OK) {
            const char *r = getenv("AWS_REGION");
            snprintf(c.region, sizeof c.region, "%s", r && *r ? r : "us-east-1");
            g_cred = c;
            g_cred_expiry = exp;
        } else {
            CB_ERROR(MOD, "no credentials: the environment has none and the instance role could not be read (%s)", cb_strerror(rc));
        }
    }
    if (rc == CB_OK) *out = g_cred;
    pthread_mutex_unlock(&g_cred_mu);
    return rc;
}

// ---- the bucket ----------------------------------------------------------------------------

typedef struct {
    char bucket[64], region[32];
} s3_ctx;

// WHERE THE BUCKET IS: <bucket>.s3.dualstack.<region>.amazonaws.com over TLS, or, when
// AWS_ENDPOINT_URL_S3 is set, that endpoint path-style (<endpoint>/<bucket>/<key>): a local stand-in
// for S3 (tools/cbase.d/test/fake_aws.c). An http:// endpoint is refused for any amazonaws.com host,
// as for DynamoDB (aws_call.c cb_aws_url_parse).
typedef struct {
    char host[160];       // as the Host header spells it: with ":port" when not the scheme's own
    char name[160];       // the host alone
    int  port;
    bool tls;
    char base[160];       // the endpoint's own path, "" for none
    bool path_style;
} s3_place;

static int s3_place_of(const s3_ctx *c, s3_place *p) {
    memset(p, 0, sizeof *p);
    const char *ep = getenv("AWS_ENDPOINT_URL_S3");
    if (ep && *ep) {
        int rc = cb_aws_url_parse(ep, p->name, sizeof p->name, &p->port, &p->tls, p->base, sizeof p->base);
        if (rc != CB_OK) return rc;
        p->path_style = true;
        // The Host header: the name, with ":port" when it is not the scheme's own. A name that leaves no room
        // for its port is refused rather than cut (a cut Host would sign and reach another host).
        int hn = p->port == (p->tls ? 443 : 80) ? snprintf(p->host, sizeof p->host, "%s", p->name)
                                                : snprintf(p->host, sizeof p->host, "%s:%d", p->name, p->port);
        if (hn < 0 || hn >= (int)sizeof p->host) return CB_E_INVALID;
        return CB_OK;
    }
    p->tls = true;
    p->port = 443;
    int n = snprintf(p->name, sizeof p->name, "%s.s3.dualstack.%s.amazonaws.com", c->bucket, c->region);
    if (n < 0 || n >= (int)sizeof p->name) return CB_E_INVALID;
    if (snprintf(p->host, sizeof p->host, "%s", p->name) >= (int)sizeof p->host) return CB_E_INVALID;
    return CB_OK;
}

static int s3_call(s3_ctx *c, const char *method, const char *key, const void *body, size_t n, cb_https_response *res) {
    cb_aws_creds creds;
    int rc = cb_aws_creds_get(&creds);
    if (rc != CB_OK) return CB_E_IO;
    snprintf(creds.region, sizeof creds.region, "%s", c->region);
    s3_place pl;
    if ((rc = s3_place_of(c, &pl)) != CB_OK) return rc;
    char path[480], sum[65], h1[100];
    int pn = pl.path_style ? snprintf(path, sizeof path, "%s/%s/%s", pl.base, c->bucket, key)
                           : snprintf(path, sizeof path, "/%s", key);
    if (pn < 0 || pn >= (int)sizeof path) return CB_E_INVALID;   // a key too long for the path: refused, never cut
    uint8_t d[SHA256_DIGEST_LEN];
    sha256(body, n, d);
    hex_encode(d, sizeof d, sum, sizeof sum);
    snprintf(h1, sizeof h1, "x-amz-content-sha256:%s", sum);
    const char *hdrs[] = { h1 };
    cb_sigv4_req r = { .method = method, .host = pl.host, .path = path, .query = "", .service = "s3", .headers = hdrs,
                       .n_headers = 1, .payload = body, .payload_len = n, .now_ms = cb_now_ms() };
    cb_buf h;
    cb_buf_init(&h);
    cb_buf_puts(&h, h1);
    cb_buf_puts(&h, "\r\n");
    rc = cb_sigv4_sign(&creds, &r, &h);
    if (rc == CB_OK) {
        if (pl.tls) rc = cb_https_request(pl.name, pl.port, method, path, cb_buf_cstr(&h), body, n, 120000, res);
        else rc = cb_http_plain_request(CB_HTTPS_OTHER, pl.name, pl.port, method, path, cb_buf_cstr(&h), body, n, 120000, res);
    }
    cb_buf_free(&h);
    return rc;
}

static int s3_put(void *ud, const char *key, const char *file) {
    FILE *f = fopen(file, "rb");
    if (!f) return CB_E_IO;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    uint8_t *buf = malloc(n > 0 ? (size_t)n : 1);
    int rc = buf && n > 0 && fread(buf, 1, (size_t)n, f) == (size_t)n ? CB_OK : CB_E_IO;
    fclose(f);
    cb_https_response res;
    memset(&res, 0, sizeof res);
    if (rc == CB_OK) rc = s3_call(ud, "PUT", key, buf, (size_t)n, &res);
    if (rc == CB_OK && res.status != 200) {
        CB_ERROR(MOD, "PUT %s: status %d", key, res.status);
        rc = CB_E_IO;
    }
    cb_https_response_free(&res);
    free(buf);
    return rc;
}

static int s3_get(void *ud, const char *key, const char *file) {
    cb_https_response res;
    memset(&res, 0, sizeof res);
    int rc = s3_call(ud, "GET", key, "", 0, &res);
    if (rc == CB_OK && (res.status == 404 || res.status == 403 /* no ListBucket: a missing key reads as 403 */)) rc = res.status == 404 ? CB_E_NOT_FOUND : CB_E_IO;
    else if (rc == CB_OK && res.status != 200) {
        CB_ERROR(MOD, "GET %s: status %d", key, res.status);
        rc = CB_E_IO;
    }
    if (rc == CB_OK) {
        FILE *f = fopen(file, "wb");
        if (!f || fwrite(res.body.p, 1, res.body.len, f) != res.body.len) rc = CB_E_IO;
        if (f && fclose(f) != 0) rc = CB_E_IO;
    }
    cb_https_response_free(&res);
    return rc;
}

static int s3_del(void *ud, const char *key) {
    cb_https_response res;
    memset(&res, 0, sizeof res);
    int rc = s3_call(ud, "DELETE", key, "", 0, &res);
    if (rc == CB_OK && res.status != 204 && res.status != 200 && res.status != 404) {
        CB_ERROR(MOD, "DELETE %s: status %d", key, res.status);
        rc = CB_E_IO;
    }
    cb_https_response_free(&res);
    return rc;
}

// A form for one upload (util/blob.h post_form). The credentials are the process's own, and the form
// lives at most 5 minutes, which cb_aws_creds_get's instance-role refresh leaves them.
static int s3_post_form(void *ud, const char *key, const char *content_type, uint64_t max_bytes, int64_t now_ms,
                        int expires_s, cb_media_form *out) {
    s3_ctx *c = ud;
    cb_aws_creds creds;
    if (cb_aws_creds_get(&creds) != CB_OK) return CB_E_IO;
    snprintf(creds.region, sizeof creds.region, "%s", c->region);
    char url[320];
    const char *ep = getenv("AWS_ENDPOINT_URL_S3");
    if (ep && *ep) {
        char host[160], base[160];
        int port;
        bool tls;
        int rc = cb_aws_url_parse(ep, host, sizeof host, &port, &tls, base, sizeof base);
        if (rc != CB_OK) return rc;
        size_t el = strlen(ep);
        while (el > 0 && ep[el - 1] == '/') el--;
        if (snprintf(url, sizeof url, "%.*s/%s", (int)el, ep, c->bucket) >= (int)sizeof url) return CB_E_INVALID;
    } else if (snprintf(url, sizeof url, "https://%s.s3.%s.amazonaws.com/", c->bucket, c->region) >= (int)sizeof url) {
        return CB_E_INVALID;
    }
    return cb_s3_post_form_make(&creds, url, c->bucket, key, content_type, max_bytes, now_ms, expires_s, out);
}

static void s3_close(void *ud) { free(ud); }

int cb_blob_open_s3(const char *bucket, const char *region, cb_blob *out) {
    s3_ctx *c = calloc(1, sizeof *c);
    if (!c) return CB_E_NOMEM;
    if (snprintf(c->bucket, sizeof c->bucket, "%s", bucket) >= (int)sizeof c->bucket ||
        snprintf(c->region, sizeof c->region, "%s", region) >= (int)sizeof c->region) {
        free(c);
        return CB_E_INVALID;
    }
    *out = (cb_blob){ .put = s3_put, .get = s3_get, .ud = c, .del = s3_del, .post_form = s3_post_form,
                      .close = s3_close };
    return CB_OK;
}

const cb_blob_backend cb_blob_backend_s3 = { "s3", cb_blob_open_s3 };
