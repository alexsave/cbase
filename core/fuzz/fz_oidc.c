// fz_oidc.c - an identity provider's bytes (auth/oidc.h, core_json.h's OpenID Connect readers):
// what anyone can put in an ID token, and what the provider's endpoints answer.
//
// The first byte picks the shape, the rest is the input:
//   0  a token's header JSON          cb_js_oidc_header
//   1  a token's claims JSON          cb_js_oidc_claims
//   2  a key set JSON                 cb_js_jwks_each (and every key it hands on built)
//   3  a token endpoint answer        cb_js_oidc_token_reply
//   4  a token, as text               cb_oidc_verify against a key set of the fuzzer's own key
//   5  a token's claims, signed       the claims JSON wrapped in a header and signed with that key,
//                                     so the checks after the signature are reached
// Each reader must answer within its buffers, NUL-terminated, and all-or-nothing (a refusal leaves
// its output empty); a token that verifies must hold every rule oidc.h states.
#include "fz.h"

#include "auth/oidc.h"
#include "cbase/auth_wire.h"
#include "core_json.h"
#include "../../../shared/c/b64/b64.h"
#include "../../../shared/c/hex/hex.h"

#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/sha.h>

#define ISSUER "https://idp.example"
#define CLIENT "com.example.app"
#define NONCE  "fuzz-raw-nonce-of-enough-bytes!!"
#define NOW_MS 1790000000000LL

static EVP_PKEY *g_key;
static cb_oidc *g_o;

static bool nul_in(const char *s, size_t cap) { return memchr(s, 0, cap) != NULL; }

static bool count_key(const char *kid, const char *n, const char *e, void *ud) {
    FZ_CHECK(strlen(kid) <= CB_JWKS_KID_MAX && strlen(n) <= CB_JWKS_N_MAX && strlen(e) < 16);
    (*(int *)ud)++;
    return true;
}

static void setup(void) {
    if (g_o) return;
    g_key = EVP_RSA_gen(2048);
    cb_config c;
    memset(&c, 0, sizeof c);
    strcpy(c.jwt_secret, "fuzz-secret-fuzz-secret-fuzz-secret!");
    strcpy(c.apple_issuer, ISSUER);
    strcpy(c.apple_client_ids, CLIENT ",com.example.web");
    char err[128];
    if (!g_key || cb_oidc_new(&c, &g_o, err, sizeof err) != CB_OK) abort();
    BIGNUM *bn = NULL, *be = NULL;
    uint8_t nb[512], eb[8];
    char n64[700], e64[16], jwks[1024];
    if (!EVP_PKEY_get_bn_param(g_key, OSSL_PKEY_PARAM_RSA_N, &bn) || !EVP_PKEY_get_bn_param(g_key, OSSL_PKEY_PARAM_RSA_E, &be)) abort();
    int nn = BN_bn2bin(bn, nb), en = BN_bn2bin(be, eb);
    BN_free(bn);
    BN_free(be);
    b64url_encode(nb, (size_t)nn, n64, sizeof n64);
    b64url_encode(eb, (size_t)en, e64, sizeof e64);
    int jn = snprintf(jwks, sizeof jwks, "{\"keys\":[{\"kty\":\"RSA\",\"kid\":\"fz\",\"n\":\"%s\",\"e\":\"%s\"}]}", n64, e64);
    if (cb_oidc_keys_load(g_o, CBW_IDP_APPLE, jwks, (size_t)jn, NOW_MS) != 1) abort();
}

// The rules a verified token holds (oidc.h), checked again from its claims.
static void holds(const char *tok, size_t n, const cb_oidc_identity *id) {
    FZ_CHECK(id->sub[0] && nul_in(id->sub, sizeof id->sub) && nul_in(id->email, sizeof id->email));
    FZ_CHECK(strcmp(id->client_id, CLIENT) == 0 || strcmp(id->client_id, "com.example.web") == 0);
    FZ_CHECK(id->exp * 1000 + CB_OIDC_SKEW_S * 1000 > NOW_MS);
    unsigned char d[32];
    char hex[65];
    SHA256((const unsigned char *)tok, n, d);
    hex_encode(d, 32, hex, sizeof hex);
    FZ_CHECK(strcmp(hex, id->key) == 0);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 1 || size > 12000) return 0;
    setup();
    int mode = data[0] % 6;
    const char *in = (const char *)data + 1;
    size_t n = size - 1;
    char *copy = malloc(n + 1);   // its own allocation, so a read past the input is ASan's
    if (!copy) abort();
    memcpy(copy, in, n);
    copy[n] = 0;
    if (mode == 0) {
        char alg[16], kid[CB_JWKS_KID_MAX + 1];
        if (cb_js_oidc_header(copy, n, alg, sizeof alg, kid, sizeof kid)) FZ_CHECK(nul_in(alg, sizeof alg) && nul_in(kid, sizeof kid));
    } else if (mode == 1) {
        cb_oidc_claims c;
        if (cb_js_oidc_claims(copy, n, &c)) {
            FZ_CHECK(c.sub[0] && c.n_aud >= 1 && c.n_aud <= CB_OIDC_AUD_MAX);
            FZ_CHECK(nul_in(c.iss, sizeof c.iss) && nul_in(c.nonce, sizeof c.nonce) && nul_in(c.email, sizeof c.email));
        } else {
            static const cb_oidc_claims zero;
            FZ_CHECK(memcmp(&c, &zero, sizeof c) == 0);
        }
    } else if (mode == 2) {
        int got = 0;
        int k = cb_js_jwks_each(copy, n, count_key, &got);
        FZ_CHECK(k == -1 ? got == 0 : k == got);
        cb_oidc *o = NULL;
        cb_config c;
        memset(&c, 0, sizeof c);
        strcpy(c.jwt_secret, "fuzz-secret-fuzz-secret-fuzz-secret!");
        strcpy(c.apple_issuer, ISSUER);
        strcpy(c.apple_client_ids, CLIENT);
        char err[64];
        if (cb_oidc_new(&c, &o, err, sizeof err) == CB_OK) {
            int held = cb_oidc_keys_load(o, CBW_IDP_APPLE, copy, n, NOW_MS);
            FZ_CHECK(held <= CB_OIDC_JWKS_MAX && held <= (k < 0 ? 0 : k));
            cb_oidc_free(o);
        }
    } else if (mode == 3) {
        char rt[CB_OIDC_REFRESH_MAX + 1], idt[2048], er[64];
        if (cb_js_oidc_token_reply(copy, n, rt, sizeof rt, idt, sizeof idt, er, sizeof er))
            FZ_CHECK(nul_in(rt, sizeof rt) && nul_in(idt, sizeof idt) && nul_in(er, sizeof er));
    } else {
        char *tok = copy;
        size_t tn = n;
        char *made = NULL;
        if (mode == 5) {
            // header.claims signed with the fuzzer's key: the claims are the input.
            static const char HEAD[] = "{\"alg\":\"RS256\",\"kid\":\"fz\"}";
            size_t cap = 64 + 2 * n + 700;
            made = malloc(cap);
            if (!made) abort();
            int a = b64url_encode((const uint8_t *)HEAD, sizeof HEAD - 1, made, cap);
            made[a++] = '.';
            int b = b64url_encode((const uint8_t *)copy, n, made + a, cap - (size_t)a);
            size_t signed_len = (size_t)a + (size_t)b;
            unsigned char sig[256];
            size_t sl = sizeof sig;
            EVP_MD_CTX *md = EVP_MD_CTX_new();
            if (!md || EVP_DigestSignInit(md, NULL, EVP_sha256(), NULL, g_key) != 1 ||
                EVP_DigestSign(md, sig, &sl, (const unsigned char *)made, signed_len) != 1)
                abort();
            EVP_MD_CTX_free(md);
            made[signed_len] = '.';
            int c = b64url_encode(sig, sl, made + signed_len + 1, cap - signed_len - 1);
            tok = made;
            tn = signed_len + 1 + (size_t)c;
        }
        cb_oidc_identity id;
        int rc = cb_oidc_verify(g_o, CBW_IDP_APPLE, tok, tn, NONCE, strlen(NONCE), NOW_MS, &id);
        FZ_CHECK(rc == CB_OK || rc == CB_E_AUTH || rc == CB_E_NOMEM);
        if (rc == CB_OK) holds(tok, tn, &id);
        else FZ_CHECK(id.sub[0] == 0 && id.key[0] == 0);
        free(made);
    }
    free(copy);
    return 0;
}
