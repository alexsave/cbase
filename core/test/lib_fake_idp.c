// lib_fake_idp.c - the fake identity provider (lib_fake_idp.h).
#include "lib_fake_idp.h"

#include "util/json.h"
#include "util/str.h"

#include "../../../shared/c/b64/b64.h"
#include "../../../shared/c/hex/hex.h"

#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/ec.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/sha.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

static int64_t now_of(const fake_idp *f) { return f->clock_ms ? *f->clock_ms / 1000 : (int64_t)time(NULL); }

static void b64u(const uint8_t *p, size_t n, char *out, size_t cap) {
    if (b64url_encode(p, n, out, cap) < 0) out[0] = 0;
}

// A key's modulus or exponent as base64url.
static void bn_b64u(EVP_PKEY *pk, const char *which, char *out, size_t cap) {
    BIGNUM *bn = NULL;
    out[0] = 0;
    if (!EVP_PKEY_get_bn_param(pk, which, &bn)) return;
    uint8_t buf[1100];
    int n = BN_bn2bin(bn, buf);
    BN_free(bn);
    if (n > 0) b64u(buf, (size_t)n, out, cap);
}

const char *fi_jwk(fake_idp *f, int key, const char *kid) {
    static _Thread_local char out[2048];
    char n[1500], e[16];
    EVP_PKEY *pk = key < 0 ? f->weak : f->rsa[key];
    bn_b64u(pk, OSSL_PKEY_PARAM_RSA_N, n, sizeof n);
    bn_b64u(pk, OSSL_PKEY_PARAM_RSA_E, e, sizeof e);
    snprintf(out, sizeof out, "{\"kty\":\"RSA\",\"kid\":\"%s\",\"use\":\"sig\",\"alg\":\"RS256\",\"n\":\"%s\",\"e\":\"%s\"}", kid, n, e);
    return out;
}

static void jwks(fake_idp *f, cb_buf *b) {
    cb_buf_puts(b, "{\"keys\":[");
    bool first = true;
    char n[1500], e[16];
    for (int i = 0; i <= FI_KEYS; i++) {
        EVP_PKEY *pk = i < FI_KEYS ? (f->published[i] ? f->rsa[i] : NULL) : (f->publish_weak ? f->weak : NULL);
        if (!pk) continue;
        bn_b64u(pk, OSSL_PKEY_PARAM_RSA_N, n, sizeof n);
        bn_b64u(pk, OSSL_PKEY_PARAM_RSA_E, e, sizeof e);
        cb_buf_printf(b, "%s{\"kty\":\"RSA\",\"kid\":\"%s\",\"use\":\"sig\",\"alg\":\"RS256\",\"n\":\"%s\",\"e\":\"%s\"}",
                      first ? "" : ",", i < FI_KEYS ? f->kid[i] : "weak", n, e);
        first = false;
    }
    cb_buf_puts(b, "]}");
}

static void answer(cb_buf *resp, int status, const char *body) {
    cb_buf_printf(resp, "HTTP/1.1 %d X\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n\r\n%s", status,
                  strlen(body), body);
}

// The form field `key`, percent-decoded, into out. false when absent.
static bool form_get(const char *body, const char *key, char *out, size_t cap) {
    size_t kl = strlen(key);
    for (const char *p = body; p && *p;) {
        const char *amp = strchr(p, '&');
        size_t len = amp ? (size_t)(amp - p) : strlen(p);
        if (len > kl && p[kl] == '=' && strncmp(p, key, kl) == 0) {
            size_t o = 0;
            for (size_t i = kl + 1; i < len && o + 1 < cap; i++) {
                if (p[i] == '%' && i + 2 < len) {
                    out[o++] = (char)(hex_nibble((unsigned char)p[i + 1]) * 16 + hex_nibble((unsigned char)p[i + 2]));
                    i += 2;
                } else {
                    out[o++] = p[i] == '+' ? ' ' : p[i];
                }
            }
            out[o] = 0;
            return true;
        }
        p = amp ? amp + 1 : NULL;
    }
    out[0] = 0;
    return false;
}

bool fi_secret_ok(fake_idp *f, const char *secret, const char *client_id) {
    const char *d1 = strchr(secret, '.'), *d2 = d1 ? strchr(d1 + 1, '.') : NULL;
    if (!d1 || !d2) return false;
    uint8_t head[256], claims[512], sig[96];
    int hn = b64_decode(secret, (size_t)(d1 - secret), head, sizeof head - 1);
    int cn = b64_decode(d1 + 1, (size_t)(d2 - d1 - 1), claims, sizeof claims - 1);
    int sn = b64_decode(d2 + 1, strlen(d2 + 1), sig, sizeof sig);
    if (hn <= 0 || cn <= 0 || sn != 64) return false;
    char alg[16], kid[32], iss[32], aud[128], sub[256];
    int64_t iat = 0, exp = 0;
    if (!cb_json_get_str((char *)head, (size_t)hn, "alg", alg, sizeof alg) || strcmp(alg, "ES256") != 0) return false;
    if (!cb_json_get_str((char *)head, (size_t)hn, "kid", kid, sizeof kid) || strcmp(kid, f->key_id) != 0) return false;
    if (!cb_json_get_str((char *)claims, (size_t)cn, "iss", iss, sizeof iss) || strcmp(iss, f->team_id) != 0) return false;
    if (!cb_json_get_str((char *)claims, (size_t)cn, "aud", aud, sizeof aud) || strcmp(aud, f->issuer) != 0) return false;
    if (!cb_json_get_str((char *)claims, (size_t)cn, "sub", sub, sizeof sub) || strcmp(sub, client_id) != 0) return false;
    if (!cb_json_get_i64((char *)claims, (size_t)cn, "iat", &iat) || !cb_json_get_i64((char *)claims, (size_t)cn, "exp", &exp))
        return false;
    int64_t now = now_of(f);
    if (iat > now + 60 || exp <= now || exp - iat > 15777000) return false;   // Apple: at most six months
    // r || s back into DER, and verified with the developer key's public half.
    ECDSA_SIG *es = ECDSA_SIG_new();
    BIGNUM *r = BN_bin2bn(sig, 32, NULL), *s = BN_bin2bn(sig + 32, 32, NULL);
    unsigned char *der = NULL;
    int dl = -1;
    if (es && r && s && ECDSA_SIG_set0(es, r, s)) {
        r = s = NULL;
        dl = i2d_ECDSA_SIG(es, &der);
    }
    BN_free(r);
    BN_free(s);
    ECDSA_SIG_free(es);
    bool ok = false;
    EVP_MD_CTX *md = EVP_MD_CTX_new();
    if (dl > 0 && md && EVP_DigestVerifyInit(md, NULL, EVP_sha256(), NULL, f->p8) == 1)
        ok = EVP_DigestVerify(md, der, (size_t)dl, (const unsigned char *)secret, (size_t)(d2 - secret)) == 1;
    EVP_MD_CTX_free(md);
    OPENSSL_free(der);
    return ok;
}

static tls_action handle(const cb_buf *req, cb_buf *resp, void *ud) {
    fake_idp *f = ud;
    const char *r = (const char *)req->p;
    const char *body = strstr(r, "\r\n\r\n");
    body = body ? body + 4 : "";
    if (strncmp(r, "GET /auth/keys ", 15) == 0) {
        f->key_fetches++;
        if (f->keys_status) { answer(resp, f->keys_status, "{}"); return TLS_KEEP; }
        cb_buf b;
        cb_buf_init(&b);
        jwks(f, &b);
        answer(resp, 200, cb_buf_cstr(&b));
        cb_buf_free(&b);
        return TLS_KEEP;
    }
    bool token = strncmp(r, "POST /auth/token ", 17) == 0, revoke = strncmp(r, "POST /auth/revoke ", 18) == 0;
    if (!token && !revoke) { answer(resp, 404, "{}"); return TLS_KEEP; }
    cb_strlcpy(f->last_form, body, sizeof f->last_form);
    char client[256], secret[1024];
    form_get(body, "client_id", client, sizeof client);
    form_get(body, "client_secret", secret, sizeof secret);
    if (!fi_secret_ok(f, secret, client)) {
        f->bad_secrets++;
        answer(resp, 400, "{\"error\":\"invalid_client\"}");
        return TLS_KEEP;
    }
    if (revoke) {
        f->revoke_calls++;
        char tok[128];
        form_get(body, "token", tok, sizeof tok);
        if (f->n_revoked < 8) {
            cb_strlcpy(f->revoked[f->n_revoked], tok, sizeof f->revoked[0]);
            cb_strlcpy(f->revoked_client[f->n_revoked++], client, sizeof f->revoked_client[0]);
        }
        answer(resp, f->revoke_status ? f->revoke_status : 200, "");
        return TLS_KEEP;
    }
    f->token_calls++;
    char code[256], grant[64];
    form_get(body, "code", code, sizeof code);
    form_get(body, "grant_type", grant, sizeof grant);
    if (strcmp(grant, "authorization_code") != 0 || strcmp(code, "code-bad") == 0) {
        answer(resp, 400, "{\"error\":\"invalid_grant\"}");
        return TLS_KEEP;
    }
    if (strcmp(code, "code-500") == 0) { answer(resp, 500, "{}"); return TLS_KEEP; }
    if (strncmp(code, "code-", 5) != 0) { answer(resp, 400, "{\"error\":\"invalid_grant\"}"); return TLS_KEEP; }
    char sub[256];
    bool other = strncmp(code, "code-other-", 11) == 0;
    snprintf(sub, sizeof sub, "%s%s", code + (other ? 11 : 5), other ? "-other" : "");
    int64_t now = now_of(f);
    fi_claims c = { .iss = f->issuer, .sub = sub, .aud = client, .iat = now, .exp = now + 600 };
    const char *idt = fi_mint(f, 0, NULL, NULL, &c);
    cb_buf b;
    cb_buf_init(&b);
    cb_buf_printf(&b, "{\"access_token\":\"at\",\"token_type\":\"Bearer\",\"expires_in\":3600,\"refresh_token\":\"rt-%s-%d\",\"id_token\":\"%s\"}",
                  sub, f->token_calls, idt ? idt : "");
    answer(resp, 200, cb_buf_cstr(&b));
    cb_buf_free(&b);
    return TLS_KEEP;
}

int fi_start(fake_idp *f) {
    memset(f, 0, sizeof *f);
    for (int i = 0; i < FI_KEYS; i++) {
        if (!(f->rsa[i] = EVP_RSA_gen(2048))) return -1;
        snprintf(f->kid[i], sizeof f->kid[i], "k%d", i);
    }
    f->published[0] = true;
    if (!(f->weak = EVP_RSA_gen(1024)) || !(f->p8 = EVP_EC_gen("P-256"))) return -1;
    cb_strlcpy(f->team_id, "TEAM123456", sizeof f->team_id);
    cb_strlcpy(f->key_id, "KEY1234567", sizeof f->key_id);
    if (tls_srv_start_concurrent(&f->srv, handle, f) != 0) return -1;
    snprintf(f->issuer, sizeof f->issuer, "https://localhost:%d", f->srv.port);
    return 0;
}

void fi_stop(fake_idp *f) {
    tls_srv_stop(&f->srv);
    for (int i = 0; i < FI_KEYS; i++) EVP_PKEY_free(f->rsa[i]);
    EVP_PKEY_free(f->weak);
    EVP_PKEY_free(f->p8);
}

void fi_publish(fake_idp *f, int key, bool on) {
    pthread_mutex_lock(&f->srv.fn_lock);
    f->published[key] = on;
    pthread_mutex_unlock(&f->srv.fn_lock);
}

const char *fi_p8_pem(fake_idp *f, bool escaped) {
    static _Thread_local char out[2048];
    BIO *b = BIO_new(BIO_s_mem());
    out[0] = 0;
    if (b && PEM_write_bio_PrivateKey(b, f->p8, NULL, NULL, 0, NULL, NULL) == 1) {
        char *p = NULL;
        long n = BIO_get_mem_data(b, &p);
        size_t o = 0;
        for (long i = 0; i < n && o + 3 < sizeof out; i++) {
            if (p[i] == '\n' && escaped) { out[o++] = '\\'; out[o++] = 'n'; }
            else out[o++] = p[i];
        }
        out[o] = 0;
    }
    BIO_free(b);
    return out;
}

const char *fi_mint_raw(fake_idp *f, int key, const char *header_json, const char *payload_json) {
    static _Thread_local char out[32768];
    char h[1024], p[16384];
    b64u((const uint8_t *)header_json, strlen(header_json), h, sizeof h);
    b64u((const uint8_t *)payload_json, strlen(payload_json), p, sizeof p);
    int n = snprintf(out, sizeof out, "%s.%s", h, p);
    EVP_PKEY *pk = key < 0 ? f->weak : f->rsa[key];
    unsigned char sig[512];
    size_t sl = sizeof sig;
    EVP_MD_CTX *md = EVP_MD_CTX_new();
    bool ok = md && EVP_DigestSignInit(md, NULL, EVP_sha256(), NULL, pk) == 1 &&
              EVP_DigestSign(md, sig, &sl, (const unsigned char *)out, (size_t)n) == 1;
    EVP_MD_CTX_free(md);
    if (!ok) return NULL;
    out[n++] = '.';
    b64u(sig, sl, out + n, sizeof out - (size_t)n);
    return out;
}

static void sha_hex(const char *s, char out[65]) {
    unsigned char d[32];
    SHA256((const unsigned char *)s, strlen(s), d);
    hex_encode(d, 32, out, 65);
}

const char *fi_mint(fake_idp *f, int key, const char *alg, const char *kid, const fi_claims *c) {
    char header[128], payload[12288];
    snprintf(header, sizeof header, "{\"kid\":\"%s\",\"alg\":\"%s\"}", kid ? kid : key >= 0 ? f->kid[key] : "weak",
             alg ? alg : "RS256");
    cb_buf b;
    cb_buf_init(&b);
    cb_buf_putc(&b, '{');
    bool first = true;
#define FI_STR(name, v) do { if (v) { cb_buf_printf(&b, "%s\"" name "\":\"%s\"", first ? "" : ",", v); first = false; } } while (0)
    FI_STR("iss", c->iss);
    if (c->aud && strchr(c->aud, ',')) {
        cb_buf_printf(&b, "%s\"aud\":[", first ? "" : ",");
        first = false;
        const char *a = c->aud;
        for (bool one = true; *a; one = false) {
            const char *e = strchr(a, ',');
            size_t len = e ? (size_t)(e - a) : strlen(a);
            cb_buf_printf(&b, "%s\"%.*s\"", one ? "" : ",", (int)len, a);
            a = e ? e + 1 : a + len;
        }
        cb_buf_putc(&b, ']');
    } else {
        FI_STR("aud", c->aud);
    }
    FI_STR("sub", c->sub);
    if (!c->no_iat) { cb_buf_printf(&b, "%s\"iat\":%lld", first ? "" : ",", (long long)c->iat); first = false; }
    if (!c->no_exp) { cb_buf_printf(&b, "%s\"exp\":%lld", first ? "" : ",", (long long)c->exp); first = false; }
    char nh[65];
    if (c->nonce_raw) {
        sha_hex(c->nonce_raw, nh);
        FI_STR("nonce", nh);
    } else {
        FI_STR("nonce", c->nonce_hashed);
    }
    FI_STR("email", c->email);
    if (c->email_verified) { cb_buf_printf(&b, ",\"email_verified\":%s", c->email_verified); }
    if (c->is_private_email) { cb_buf_printf(&b, ",\"is_private_email\":%s", c->is_private_email); }
    if (c->extra) cb_buf_puts(&b, c->extra);
#undef FI_STR
    cb_buf_putc(&b, '}');
    cb_strlcpy(payload, cb_buf_cstr(&b), sizeof payload);
    cb_buf_free(&b);
    return fi_mint_raw(f, key, header, payload);
}
