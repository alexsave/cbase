// oidc.c - an identity provider's side of a sign-in (oidc.h).
#include "auth/oidc.h"

#include "cbase/auth_wire.h"
#include "core_json.h"
#include "net/https.h"
#include "util/err.h"
#include "util/log.h"
#include "util/str.h"

#include "../../../../shared/c/b64/b64.h"
#include "../../../../shared/c/hex/hex.h"
#include "../../../../shared/c/num_parse.h"

#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/param_build.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef struct {
    char      kid[CB_JWKS_KID_MAX + 1];
    EVP_PKEY *pk;
} jwk;

typedef struct {
    int       id;                       // CBW_IDP_*
    const char *name;                   // for the logs
    bool      on;
    char      issuer[256];              // "https://host[:port]"
    char      host[256];
    int       port;
    char      clients[CB_OIDC_CLIENTS_MAX][CB_OIDC_STR_MAX + 1];
    int       n_clients;
    char      team_id[16], key_id[16];
    char      redirect_uri[512];
    EVP_PKEY *signer;                   // the developer's P-256 key; NULL: no exchange, no revocation
    // The provider's keys (oidc.h): under mu, which a fetch holds (one fetch at a time).
    pthread_mutex_t mu;
    jwk       keys[CB_OIDC_JWKS_MAX];
    int       n_keys;
    int64_t   fetched_ms;               // the last fetch that answered a key set
    int64_t   tried_ms;                 // the last fetch, answered or not
    int       fetches;
} idp;

struct cb_oidc {
    idp     apple;
    uint8_t seal_key[32];
};

static idp *idp_of(cb_oidc *o, int id) { return o && id == CBW_IDP_APPLE && o->apple.on ? &o->apple : NULL; }
static const idp *idp_of_c(const cb_oidc *o, int id) { return o && id == CBW_IDP_APPLE && o->apple.on ? &o->apple : NULL; }

// ---- setup -----------------------------------------------------------------------------

// The .p8's PEM, with "\n" written for each line break where an environment cannot hold one.
static EVP_PKEY *load_p8(const char *text) {
    size_t n = strlen(text);
    char *pem = malloc(n + 1);
    if (!pem) return NULL;
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        if (text[i] == '\\' && i + 1 < n && text[i + 1] == 'n') { pem[k++] = '\n'; i++; }
        else pem[k++] = text[i];
    }
    pem[k] = 0;
    BIO *b = BIO_new_mem_buf(pem, (int)k);
    EVP_PKEY *pk = b ? PEM_read_bio_PrivateKey(b, NULL, NULL, NULL) : NULL;
    BIO_free(b);
    OPENSSL_cleanse(pem, k);
    free(pem);
    // P-256 only: what ES256 is.
    char group[32] = "";
    size_t gl = 0;
    // (A key that is not EC has no group: the one check refuses it too.)
    if (pk && (!EVP_PKEY_get_utf8_string_param(pk, OSSL_PKEY_PARAM_GROUP_NAME, group, sizeof group, &gl) ||
               strcmp(group, "prime256v1") != 0)) {
        EVP_PKEY_free(pk);
        pk = NULL;
    }
    return pk;
}

static int idp_setup(idp *p, int id, const char *name, const char *issuer, const char *clients, const char *team,
                     const char *key_id, const char *key_pem, const char *redirect, char *err, size_t cap) {
    memset(p, 0, sizeof *p);
    p->id = id;
    p->name = name;
    pthread_mutex_init(&p->mu, NULL);
    for (const char *c = clients; *c;) {
        const char *e = strchr(c, ',');
        size_t len = e ? (size_t)(e - c) : strlen(c);
        if (len > 0 && len <= CB_OIDC_STR_MAX && p->n_clients < CB_OIDC_CLIENTS_MAX) {
            memcpy(p->clients[p->n_clients], c, len);
            p->clients[p->n_clients++][len] = 0;
        } else if (len > 0) {
            snprintf(err, cap, "%s: a client id over %d bytes, or more than %d of them", name, CB_OIDC_STR_MAX, CB_OIDC_CLIENTS_MAX);
            return CB_E_INVALID;
        }
        c = e ? e + 1 : c + len;
    }
    if (p->n_clients == 0) return CB_OK;   // off
    cb_strlcpy(p->issuer, issuer, sizeof p->issuer);
    const char *h = issuer + 8;   // config.c checked "https://" and no path
    const char *colon = strrchr(h, ':');
    size_t hl = colon ? (size_t)(colon - h) : strlen(h);
    if (hl == 0 || hl >= sizeof p->host) { snprintf(err, cap, "%s: the issuer's host", name); return CB_E_INVALID; }
    memcpy(p->host, h, hl);
    p->host[hl] = 0;
    int64_t port = 443;
    if (colon && !num_parse_i64_cstr(colon + 1, 1, 65535, &port)) {
        snprintf(err, cap, "%s: the issuer's port", name);
        return CB_E_INVALID;
    }
    p->port = (int)port;
    cb_strlcpy(p->team_id, team, sizeof p->team_id);
    cb_strlcpy(p->key_id, key_id, sizeof p->key_id);
    cb_strlcpy(p->redirect_uri, redirect, sizeof p->redirect_uri);
    if (key_pem && key_pem[0] && !(p->signer = load_p8(key_pem))) {
        snprintf(err, cap, "%s: the private key does not load as a P-256 key in PEM (the .p8 file's text)", name);
        return CB_E_INVALID;
    }
    p->on = true;
    return CB_OK;
}

static void idp_clear(idp *p) {
    for (int i = 0; i < p->n_keys; i++) EVP_PKEY_free(p->keys[i].pk);
    p->n_keys = 0;
    EVP_PKEY_free(p->signer);
    p->signer = NULL;
    pthread_mutex_destroy(&p->mu);
}

int cb_oidc_new(const cb_config *cfg, cb_oidc **out, char *err, size_t cap) {
    *out = NULL;
    cb_oidc *o = calloc(1, sizeof *o);
    if (!o) return CB_E_NOMEM;
    int rc = idp_setup(&o->apple, CBW_IDP_APPLE, "apple", cfg->apple_issuer[0] ? cfg->apple_issuer : "https://appleid.apple.com",
                       cfg->apple_client_ids, cfg->apple_team_id, cfg->apple_key_id, cfg->apple_private_key,
                       cfg->apple_redirect_uri, err, cap);
    if (rc != CB_OK) {
        idp_clear(&o->apple);
        free(o);
        return rc;
    }
    // The sealing key, derived from the JWT secret (oidc.h THE STORED REFRESH TOKEN).
    static const char LABEL[] = "cbase oidc refresh-token seal v1";
    unsigned int ml = 0;
    if (!HMAC(EVP_sha256(), cfg->jwt_secret, (int)strlen(cfg->jwt_secret), (const unsigned char *)LABEL, sizeof LABEL - 1,
              o->seal_key, &ml) ||
        ml != sizeof o->seal_key) {
        idp_clear(&o->apple);
        free(o);
        return CB_E_NOMEM;
    }
    *out = o;
    return CB_OK;
}

void cb_oidc_free(cb_oidc *o) {
    if (!o) return;
    idp_clear(&o->apple);
    OPENSSL_cleanse(o->seal_key, sizeof o->seal_key);
    free(o);
}

bool cb_oidc_on(const cb_oidc *o, int id) { return idp_of_c(o, id) != NULL; }
bool cb_oidc_can_exchange(const cb_oidc *o, int id) {
    const idp *p = idp_of_c(o, id);
    return p && p->signer;
}

int cb_oidc_key_fetches(const cb_oidc *o, int id) {
    const idp *p = idp_of_c(o, id);
    return p ? p->fetches : 0;
}

// ---- the provider's keys ---------------------------------------------------------------

// A base64url field decoded strictly: the one canonical spelling of its bytes.
static int b64url_strict(const char *in, size_t n, uint8_t *out, size_t cap) {
    int k = b64_decode(in, n, out, cap);
    if (k < 0) return -1;
    char again[CB_OIDC_TOKEN_MAX + 4];
    int m = b64url_encode(out, (size_t)k, again, sizeof again);
    if (m < 0 || (size_t)m != n || memcmp(again, in, n) != 0) return -1;
    return k;
}

static EVP_PKEY *rsa_of(const char *n_b64, const char *e_b64) {
    uint8_t nb[CB_JWKS_N_MAX], eb[16];
    int nn = b64url_strict(n_b64, strlen(n_b64), nb, sizeof nb);
    int en = b64url_strict(e_b64, strlen(e_b64), eb, sizeof eb);
    if (nn < 1 || en < 1) return NULL;   // the size rule is the key's bits, below
    BIGNUM *n = BN_bin2bn(nb, nn, NULL), *e = BN_bin2bn(eb, en, NULL);
    OSSL_PARAM_BLD *bld = OSSL_PARAM_BLD_new();
    OSSL_PARAM *params = NULL;
    EVP_PKEY_CTX *ctx = NULL;
    EVP_PKEY *pk = NULL;
    if (n && e && bld && BN_is_odd(e) && !BN_is_one(e) && OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_RSA_N, n) &&
        OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_RSA_E, e) && (params = OSSL_PARAM_BLD_to_param(bld)) &&
        (ctx = EVP_PKEY_CTX_new_from_name(NULL, "RSA", NULL)) && EVP_PKEY_fromdata_init(ctx) > 0) {
        if (EVP_PKEY_fromdata(ctx, &pk, EVP_PKEY_PUBLIC_KEY, params) <= 0) pk = NULL;
    }
    EVP_PKEY_CTX_free(ctx);
    OSSL_PARAM_free(params);
    OSSL_PARAM_BLD_free(bld);
    BN_free(n);
    BN_free(e);
    if (pk && EVP_PKEY_get_bits(pk) < 2048) {
        EVP_PKEY_free(pk);
        pk = NULL;
    }
    return pk;
}

typedef struct {
    jwk keys[CB_OIDC_JWKS_MAX];
    int n;
} key_set;

static bool add_key(const char *kid, const char *n_b64, const char *e_b64, void *ud) {
    key_set *ks = ud;
    for (int i = 0; i < ks->n; i++)
        if (strcmp(ks->keys[i].kid, kid) == 0) return true;   // the first of a kid only
    EVP_PKEY *pk = rsa_of(n_b64, e_b64);
    if (!pk) return true;   // a key we cannot use is skipped, not the set
    cb_strlcpy(ks->keys[ks->n].kid, kid, sizeof ks->keys[0].kid);
    ks->keys[ks->n++].pk = pk;
    return ks->n < CB_OIDC_JWKS_MAX;
}

// A key set's usable keys in place of the held ones, under p->mu: how many (the held ones kept when
// none is usable: 0), or -1 for a document that is not a key set.
static int install_keys(idp *p, const char *json, size_t n, int64_t now_ms) {
    key_set ks;
    memset(&ks, 0, sizeof ks);
    int got = cb_js_jwks_each(json, n, add_key, &ks);
    if (got < 0 || ks.n == 0) {
        for (int i = 0; i < ks.n; i++) EVP_PKEY_free(ks.keys[i].pk);
        return got < 0 ? -1 : 0;
    }
    for (int i = 0; i < p->n_keys; i++) EVP_PKEY_free(p->keys[i].pk);
    memcpy(p->keys, ks.keys, sizeof ks.keys);
    p->n_keys = ks.n;
    p->fetched_ms = now_ms;
    return ks.n;
}

// Fetches the key set under p->mu: CB_OK (the keys replaced), or CB_E_IO (the old keys kept).
static int fetch_keys(idp *p, int64_t now_ms) {
    p->tried_ms = now_ms;
    p->fetches++;
    cb_https_response r;
    int rc = cb_https_request(p->host, p->port, "GET", "/auth/keys", "Accept: application/json\r\n", NULL, 0,
                              CB_OIDC_TIMEOUT_MS, &r);
    if (rc != CB_OK || r.status != 200) {
        CB_WARN("oidc", "%s: the key set did not come (%s, status %d)", p->name, cb_strerror(rc), rc == CB_OK ? r.status : 0);
        if (rc == CB_OK) cb_https_response_free(&r);
        return CB_E_IO;
    }
    int held = install_keys(p, (const char *)r.body.p, r.body.len, now_ms);
    cb_https_response_free(&r);
    if (held <= 0) {
        CB_WARN("oidc", "%s: the key set holds no usable key", p->name);
        return CB_E_IO;
    }
    return CB_OK;
}

int cb_oidc_keys_load(cb_oidc *o, int id, const char *json, size_t n, int64_t now_ms) {
    idp *p = idp_of(o, id);
    if (!p) return -1;
    pthread_mutex_lock(&p->mu);
    p->tried_ms = now_ms;
    int held = install_keys(p, json, n, now_ms);
    pthread_mutex_unlock(&p->mu);
    return held;
}

// The key named kid, up-referenced for the caller (free it), or NULL with *rc: CB_E_AUTH for a kid
// the provider does not publish, CB_E_IO when the keys could not be had (none held, or the fetch
// that looked for a kid not held failed).
static EVP_PKEY *key_for(idp *p, const char *kid, int64_t now_ms, int *rc) {
    pthread_mutex_lock(&p->mu);
    // None held or held too long: fetch (a failed refresh keeps what is held). With none held and a
    // fetch failed under a second ago, not again yet: a provider that is down is not asked once per
    // sign-in, each waiting out the timeout.
    if (p->n_keys == 0 || now_ms - p->fetched_ms >= CB_OIDC_KEYS_TTL_MS) {
        if (!(p->n_keys == 0 && p->tried_ms && now_ms - p->tried_ms < CB_OIDC_RETRY_MS)) fetch_keys(p, now_ms);
    }
    EVP_PKEY *pk = NULL;
    bool lost = false;
    for (int i = 0; i < p->n_keys; i++)
        if (strcmp(p->keys[i].kid, kid) == 0) pk = p->keys[i].pk;
    // A kid not held: a rotation, fetched at once, but at most once every CB_OIDC_REFETCH_MS.
    if (!pk && p->n_keys > 0 && now_ms - p->tried_ms >= CB_OIDC_REFETCH_MS) {
        if (fetch_keys(p, now_ms) != CB_OK) lost = true;
        for (int i = 0; i < p->n_keys; i++)
            if (strcmp(p->keys[i].kid, kid) == 0) pk = p->keys[i].pk;
    }
    if (pk) EVP_PKEY_up_ref(pk);
    *rc = pk ? CB_OK : p->n_keys == 0 || lost ? CB_E_IO : CB_E_AUTH;
    pthread_mutex_unlock(&p->mu);
    return pk;
}

// ---- verification ----------------------------------------------------------------------

static bool client_known(const idp *p, const cb_oidc_claims *c, char *out) {
    for (int a = 0; a < c->n_aud; a++)
        for (int i = 0; i < p->n_clients; i++)
            if (strcmp(c->aud[a], p->clients[i]) == 0) {
                cb_strlcpy(out, p->clients[i], CB_OIDC_STR_MAX + 1);
                return true;
            }
    return false;
}

static void sha256_hex(const void *p, size_t n, char out[65]) {
    unsigned char d[SHA256_DIGEST_LENGTH];
    SHA256(p, n, d);
    hex_encode(d, sizeof d, out, 65);
}

// The token checked (oidc.h); nonce NULL skips the nonce (the code exchange's own token).
static int verify(idp *p, const char *token, size_t n, const char *nonce, size_t nonce_len, int64_t now_ms,
                  cb_oidc_identity *out) {
    memset(out, 0, sizeof *out);
    // The bound before any work. A third dot, a NUL or any other byte outside the alphabet is the
    // strict decode's refusal below (the one owner of a part's shape).
    if (!token || n == 0 || n > CB_OIDC_TOKEN_MAX) return CB_E_AUTH;
    const char *d1 = memchr(token, '.', n);
    const char *d2 = d1 ? memchr(d1 + 1, '.', (size_t)(token + n - d1 - 1)) : NULL;
    if (!d1 || !d2) return CB_E_AUTH;
    size_t hn = (size_t)(d1 - token), pn = (size_t)(d2 - d1 - 1), sn = (size_t)(token + n - d2 - 1);
    if (hn == 0 || pn == 0 || sn == 0) return CB_E_AUTH;
    uint8_t *buf = malloc(n + 3);   // the largest part decoded, and a NUL
    if (!buf) return CB_E_NOMEM;
    int rc = CB_E_AUTH;
    EVP_PKEY *pk = NULL;
    EVP_MD_CTX *md = NULL;
    char alg[16], kid[CB_JWKS_KID_MAX + 1];
    int k = b64url_strict(token, hn, buf, n);
    if (k <= 0 || !cb_js_oidc_header((const char *)buf, (size_t)k, alg, sizeof alg, kid, sizeof kid) || strcmp(alg, "RS256") != 0)
        goto out;
    cb_oidc_claims c;
    k = b64url_strict(d1 + 1, pn, buf, n);
    if (k <= 0 || !cb_js_oidc_claims((const char *)buf, (size_t)k, &c)) goto out;
    // The claims before the signature's cost: a token that cannot pass them is refused for one read.
    int64_t now_s = now_ms / 1000;
    if (strcmp(c.iss, p->issuer) != 0 || !client_known(p, &c, out->client_id)) goto out;
    if (c.exp + CB_OIDC_SKEW_S <= now_s || c.iat - CB_OIDC_SKEW_S > now_s) goto out;
    if (nonce) {
        char want[65];
        if (nonce_len < CB_OIDC_NONCE_MIN || nonce_len > CB_OIDC_NONCE_MAX) goto out;
        sha256_hex(nonce, nonce_len, want);
        if (strcmp(c.nonce, want) != 0) goto out;
    }
    k = b64url_strict(d2 + 1, sn, buf, n);
    if (k <= 0) goto out;
    int krc;
    if (!(pk = key_for(p, kid, now_ms, &krc))) { rc = krc; goto out; }
    // A signature of another length than the modulus is OpenSSL's refusal (RSASSA-PKCS1-v1_5).
    if (!(md = EVP_MD_CTX_new())) { rc = CB_E_NOMEM; goto out; }
    if (EVP_DigestVerifyInit(md, NULL, EVP_sha256(), NULL, pk) != 1) { rc = CB_E_NOMEM; goto out; }
    if (EVP_DigestVerify(md, buf, (size_t)k, (const unsigned char *)token, (size_t)(d2 - token)) != 1) goto out;
    cb_strlcpy(out->sub, c.sub, sizeof out->sub);
    cb_strlcpy(out->email, c.email, sizeof out->email);
    out->email_verified = c.email_verified;
    out->private_email = c.private_email;
    out->exp = c.exp;
    sha256_hex(token, n, out->key);
    rc = CB_OK;
out:
    EVP_MD_CTX_free(md);
    EVP_PKEY_free(pk);
    free(buf);
    if (rc != CB_OK) memset(out, 0, sizeof *out);
    return rc;
}

int cb_oidc_verify(cb_oidc *o, int id, const char *token, size_t n, const char *nonce, size_t nonce_len, int64_t now_ms,
                   cb_oidc_identity *out) {
    memset(out, 0, sizeof *out);
    idp *p = idp_of(o, id);
    if (!p) return CB_E_NOT_FOUND;
    if (!nonce) return CB_E_AUTH;   // a sign-in always has one
    return verify(p, token, n, nonce, nonce_len, now_ms, out);
}

// ---- the client secret -----------------------------------------------------------------

static bool app(cb_buf *b, const void *p, size_t n) {
    cb_buf_append(b, p, n);
    return !b->oom;
}

static int b64url_buf(const uint8_t *p, size_t n, cb_buf *out) {
    size_t at = out->len, need = b64url_len(n) + 1;
    if (!cb_buf_reserve(out, need)) return CB_E_NOMEM;
    int k = b64url_encode(p, n, (char *)out->p + at, need);
    if (k < 0) return CB_E_NOMEM;
    out->len = at + (size_t)k;
    return CB_OK;
}

int cb_oidc_client_secret(cb_oidc *o, int id, const char *client_id, int64_t now_s, char *out, size_t cap) {
    idp *p = idp_of(o, id);
    if (!p || !p->signer) return CB_E_STATE;
    cb_buf head, claims, jwt;
    cb_buf_init(&head);
    cb_buf_init(&claims);
    cb_buf_init(&jwt);
    int rc = cb_js_es256_header(&head, p->key_id);
    if (rc == CB_OK) rc = cb_js_client_secret_claims(&claims, p->team_id, now_s, now_s + CB_OIDC_SECRET_S, p->issuer, client_id);
    if (rc == CB_OK) rc = b64url_buf(head.p, head.len, &jwt);
    if (rc == CB_OK) rc = app(&jwt, ".", 1) ? CB_OK : CB_E_NOMEM;
    if (rc == CB_OK) rc = b64url_buf(claims.p, claims.len, &jwt);
    // ES256: ECDSA P-256 over SHA-256, the signature as r || s (32 bytes each), not DER.
    unsigned char der[80], raw[64];
    size_t dl = sizeof der;
    EVP_MD_CTX *md = rc == CB_OK ? EVP_MD_CTX_new() : NULL;
    if (rc == CB_OK && (!md || EVP_DigestSignInit(md, NULL, EVP_sha256(), NULL, p->signer) != 1 ||
                        EVP_DigestSign(md, der, &dl, jwt.p, jwt.len) != 1))
        rc = CB_E_NOMEM;
    EVP_MD_CTX_free(md);
    if (rc == CB_OK) {
        const unsigned char *q = der;
        ECDSA_SIG *sig = d2i_ECDSA_SIG(NULL, &q, (long)dl);
        const BIGNUM *r = NULL, *s = NULL;
        if (sig) ECDSA_SIG_get0(sig, &r, &s);
        if (!sig || BN_bn2binpad(r, raw, 32) != 32 || BN_bn2binpad(s, raw + 32, 32) != 32) rc = CB_E_NOMEM;
        ECDSA_SIG_free(sig);
    }
    if (rc == CB_OK) rc = app(&jwt, ".", 1) ? CB_OK : CB_E_NOMEM;
    if (rc == CB_OK) rc = b64url_buf(raw, sizeof raw, &jwt);
    if (rc == CB_OK && jwt.len + 1 > cap) rc = CB_E_FULL;
    int len = (int)jwt.len;
    if (rc == CB_OK) {
        memcpy(out, jwt.p, jwt.len);
        out[jwt.len] = 0;
    }
    cb_buf_free(&head);
    cb_buf_free(&claims);
    cb_buf_free(&jwt);
    return rc == CB_OK ? len : rc;
}

// ---- the token and revocation endpoints ------------------------------------------------

// Appends key=value, the value percent-encoded (RFC 3986 unreserved kept).
static bool form(cb_buf *b, const char *key, const char *v, size_t n) {
    if (b->len && !app(b, "&", 1)) return false;
    if (!app(b, key, strlen(key)) || !app(b, "=", 1)) return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)v[i];
        bool plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' ||
                     c == '_' || c == '~';
        char e[3] = { '%', hex_digit_upper(c >> 4), hex_digit_upper(c) };
        if (!(plain ? app(b, (const char *)&c, 1) : app(b, e, 3))) return false;
    }
    return true;
}

static const char FORM_HEADERS[] = "Content-Type: application/x-www-form-urlencoded\r\nAccept: application/json\r\n";

static bool client_configured(const idp *p, const char *client_id) {
    for (int i = 0; i < p->n_clients; i++)
        if (strcmp(p->clients[i], client_id) == 0) return true;
    return false;
}

int cb_oidc_exchange(cb_oidc *o, int id, const char *client_id, const char *sub, const char *code, size_t code_len,
                     int64_t now_ms, char *refresh, size_t cap) {
    refresh[0] = 0;
    idp *p = idp_of(o, id);
    if (!p || !p->signer) return CB_E_STATE;
    if (!client_id || !client_configured(p, client_id) || !code || code_len == 0 || code_len > CB_OIDC_CODE_MAX ||
        memchr(code, 0, code_len))
        return CB_E_AUTH;
    char secret[1024];
    int sl = cb_oidc_client_secret(o, id, client_id, now_ms / 1000, secret, sizeof secret);
    if (sl < 0) return sl == CB_E_STATE ? CB_E_STATE : CB_E_NOMEM;
    cb_buf body;
    cb_buf_init(&body);
    bool ok = form(&body, "client_id", client_id, strlen(client_id)) && form(&body, "client_secret", secret, (size_t)sl) &&
              form(&body, "code", code, code_len) && form(&body, "grant_type", "authorization_code", 18) &&
              (!p->redirect_uri[0] || form(&body, "redirect_uri", p->redirect_uri, strlen(p->redirect_uri)));
    OPENSSL_cleanse(secret, sizeof secret);
    if (!ok) { cb_buf_free(&body); return CB_E_NOMEM; }
    cb_https_response r;
    int rc = cb_https_request(p->host, p->port, "POST", "/auth/token", FORM_HEADERS, body.p, body.len, CB_OIDC_TIMEOUT_MS, &r);
    OPENSSL_cleanse(body.p, body.len);
    cb_buf_free(&body);
    if (rc != CB_OK) return CB_E_IO;
    char tok[CB_OIDC_REFRESH_MAX + 1], err[64];
    char *idt = malloc(CB_OIDC_TOKEN_MAX + 1);
    if (!idt) { cb_https_response_free(&r); return CB_E_NOMEM; }
    bool read = cb_js_oidc_token_reply((const char *)r.body.p, r.body.len, tok, sizeof tok, idt, CB_OIDC_TOKEN_MAX + 1, err, sizeof err);
    int status = r.status;
    cb_https_response_free(&r);
    if (status == 400 || status == 401) {
        CB_WARN("oidc", "%s: the code was refused (%d %s)", p->name, status, read ? err : "?");
        rc = CB_E_AUTH;
    } else if (status != 200 || !read || !tok[0] || !idt[0]) {
        CB_WARN("oidc", "%s: the token endpoint answered %d%s", p->name, status, read ? "" : ", unreadable");
        rc = CB_E_IO;
    } else {
        // The grant must be the signed-in subject's, for the same client.
        cb_oidc_identity who;
        rc = verify(p, idt, strlen(idt), NULL, 0, now_ms, &who);
        if (rc == CB_OK && (strcmp(who.sub, sub ? sub : "") != 0 || strcmp(who.client_id, client_id) != 0)) rc = CB_E_AUTH;
        if (rc == CB_OK) {
            if (strlen(tok) + 1 > cap) rc = CB_E_FULL;
            else cb_strlcpy(refresh, tok, cap);
        } else if (rc == CB_E_AUTH) {
            CB_WARN("oidc", "%s: the exchange answered a token for another subject or client", p->name);
        }
    }
    OPENSSL_cleanse(tok, sizeof tok);
    free(idt);
    return rc;
}

int cb_oidc_revoke(cb_oidc *o, int id, const char *client_id, const char *refresh_token, int64_t now_ms) {
    idp *p = idp_of(o, id);
    if (!p || !p->signer) return CB_E_STATE;
    if (!client_id || !refresh_token || !refresh_token[0]) return CB_E_AUTH;
    char secret[1024];
    int sl = cb_oidc_client_secret(o, id, client_id, now_ms / 1000, secret, sizeof secret);
    if (sl < 0) return sl == CB_E_STATE ? CB_E_STATE : CB_E_NOMEM;
    cb_buf body;
    cb_buf_init(&body);
    bool ok = form(&body, "client_id", client_id, strlen(client_id)) && form(&body, "client_secret", secret, (size_t)sl) &&
              form(&body, "token", refresh_token, strlen(refresh_token)) &&
              form(&body, "token_type_hint", "refresh_token", 13);
    OPENSSL_cleanse(secret, sizeof secret);
    if (!ok) { cb_buf_free(&body); return CB_E_NOMEM; }
    cb_https_response r;
    int rc = cb_https_request(p->host, p->port, "POST", "/auth/revoke", FORM_HEADERS, body.p, body.len, CB_OIDC_TIMEOUT_MS, &r);
    OPENSSL_cleanse(body.p, body.len);
    cb_buf_free(&body);
    if (rc != CB_OK) return CB_E_IO;
    int status = r.status;
    cb_https_response_free(&r);
    if (status == 200) return CB_OK;
    CB_WARN("oidc", "%s: the revocation answered %d", p->name, status);
    return status == 400 || status == 401 ? CB_E_AUTH : CB_E_IO;
}

// ---- sealing ---------------------------------------------------------------------------

int cb_oidc_seal(const cb_oidc *o, const char *plain, char *out, size_t cap) {
    size_t n = plain ? strlen(plain) : 0;
    if (!o || n > CB_OIDC_REFRESH_MAX || cap < 2 * (12 + n + 16) + 1) return -1;
    uint8_t buf[12 + CB_OIDC_REFRESH_MAX + 16];
    if (RAND_bytes(buf, 12) != 1) return -1;
    EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new();
    int l1 = 0, l2 = 0;
    bool ok = c && EVP_EncryptInit_ex(c, EVP_aes_256_gcm(), NULL, o->seal_key, buf) == 1 &&
              EVP_EncryptUpdate(c, buf + 12, &l1, (const unsigned char *)plain, (int)n) == 1 &&
              EVP_EncryptFinal_ex(c, buf + 12 + l1, &l2) == 1 &&
              EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_GET_TAG, 16, buf + 12 + n) == 1;
    EVP_CIPHER_CTX_free(c);
    if (!ok) return -1;
    int k = hex_encode(buf, 12 + n + 16, out, cap);
    OPENSSL_cleanse(buf, sizeof buf);
    return k;
}

int cb_oidc_open(const cb_oidc *o, const char *sealed, char *out, size_t cap) {
    size_t sl = sealed ? strlen(sealed) : 0;
    if (!o || sl < 2 * (12 + 16) || sl % 2 || sl > 2 * (12 + CB_OIDC_REFRESH_MAX + 16)) return -1;
    uint8_t buf[12 + CB_OIDC_REFRESH_MAX + 16];
    int k = hex_decode(sealed, sl, buf, sizeof buf);
    if (k < 28) return -1;
    size_t n = (size_t)k - 28;
    if (cap < n + 1) return -1;
    EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new();
    int l1 = 0, l2 = 0;
    bool ok = c && EVP_DecryptInit_ex(c, EVP_aes_256_gcm(), NULL, o->seal_key, buf) == 1 &&
              EVP_DecryptUpdate(c, (unsigned char *)out, &l1, buf + 12, (int)n) == 1 &&
              EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_SET_TAG, 16, buf + 12 + n) == 1 &&
              EVP_DecryptFinal_ex(c, (unsigned char *)out + l1, &l2) == 1;
    EVP_CIPHER_CTX_free(c);
    OPENSSL_cleanse(buf, sizeof buf);
    if (!ok) {
        OPENSSL_cleanse(out, cap);
        out[0] = 0;
        return -1;
    }
    out[n] = 0;
    return (int)n;
}
