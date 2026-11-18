// s3_post.c - see s3_post.h.
#include "aws/s3_post.h"

#include "aws_json.h"
#include "b64/b64.h"
#include "hex/hex.h"
#include "util/err.h"
#include "util/str.h"

#include <openssl/crypto.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int cb_s3_post_sign(const cb_aws_creds *c, const char *date8, const char *policy_b64, char sig_hex[65]) {
    uint8_t key[32], sig[32];
    if (!date8 || strlen(date8) != 8) return CB_E_INVALID;
    bool ok = cb_sigv4_signing_key(c->secret_key, date8, c->region, "s3", key) &&
              cb_sigv4_hmac(key, sizeof key, policy_b64, strlen(policy_b64), sig);
    OPENSSL_cleanse(key, sizeof key);
    if (!ok) return CB_E_NOMEM;
    hex_encode(sig, sizeof sig, sig_hex, 65);
    return CB_OK;
}

static bool add(cb_media_form *f, const char *name, const char *value) {
    if (f->n >= CB_MEDIA_FORM_FIELDS) return false;
    char *v = strdup(value);
    if (!v) return false;
    snprintf(f->name[f->n], sizeof f->name[0], "%s", name);
    f->value[f->n++] = v;
    return true;
}

int cb_s3_post_form_make(const cb_aws_creds *c, const char *url, const char *bucket, const char *key,
                         const char *content_type, uint64_t max_bytes, int64_t now_ms, int expires_s,
                         cb_media_form *out) {
    if (!url || !*url || !bucket || !*bucket || !key || !*key || !content_type || !*content_type) return CB_E_INVALID;
    if (expires_s < 1 || expires_s > CB_S3_POST_MAX_EXPIRES_S) return CB_E_INVALID;
    if (max_bytes == 0 || max_bytes > CB_S3_POST_MAX_BYTES) return CB_E_INVALID;
    if (strlen(url) >= sizeof out->url) return CB_E_INVALID;

    time_t now = (time_t)(now_ms / 1000);
    struct tm tm;
    if (!gmtime_r(&now, &tm)) return CB_E_INVALID;
    char amz_date[20], credential[300];
    if (strftime(amz_date, sizeof amz_date, "%Y%m%dT%H%M%SZ", &tm) != 16) return CB_E_INVALID;
    if (snprintf(credential, sizeof credential, "%s/%.8s/%s/s3/aws4_request", c->access_key, amz_date, c->region) >=
        (int)sizeof credential)
        return CB_E_INVALID;

    s3p_policy p = { .bucket = bucket, .key = key, .content_type = content_type, .max_bytes = max_bytes,
                     .credential = credential, .amz_date = amz_date, .security_token = c->session_token,
                     .expires_unix_s = (int64_t)now + expires_s };
    cb_buf json;
    cb_buf_init(&json);
    if (!s3p_policy_json(&json, &p)) {
        cb_buf_free(&json);
        return CB_E_NOMEM;
    }
    size_t cap = b64_len(json.len) + 1;
    char *b64 = malloc(cap);
    if (!b64 || b64_encode((const uint8_t *)json.p, json.len, b64, cap) < 0) {
        free(b64);
        cb_buf_free(&json);
        return CB_E_NOMEM;
    }
    cb_buf_free(&json);

    char sig[65], date8[9];
    memcpy(date8, amz_date, 8);
    date8[8] = 0;
    int rc = cb_s3_post_sign(c, date8, b64, sig);
    cb_media_form f;
    memset(&f, 0, sizeof f);
    if (rc == CB_OK &&
        !(add(&f, "key", key) && add(&f, "Content-Type", content_type) && add(&f, "x-amz-algorithm", "AWS4-HMAC-SHA256") &&
          add(&f, "x-amz-credential", credential) && add(&f, "x-amz-date", amz_date) &&
          (c->session_token[0] ? add(&f, "x-amz-security-token", c->session_token) : true) && add(&f, "policy", b64) &&
          add(&f, "x-amz-signature", sig)))
        rc = CB_E_NOMEM;
    free(b64);
    if (rc != CB_OK) {
        cb_media_form_free(&f);
        return rc;
    }
    snprintf(f.url, sizeof f.url, "%s", url);
    *out = f;
    return CB_OK;
}
