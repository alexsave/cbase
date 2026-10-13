// jwt.c - HS256 access tokens in GoTrue's claim shape (jwt.h).
//
// Minting writes the header and claims with the JSON writer, base64url
// (unpadded) each, and signs "<header>.<payload>" with HMAC-SHA256.
// Verification is strict where strictness costs nothing: each part must be in
// the URL-safe alphabet with no padding, the signature is recomputed and
// compared as text in constant time (so no second spelling of the same bytes
// verifies), the header must say alg HS256 and nothing else, and the claims
// must carry aud and role "authenticated", a UUID sub and session_id, and an
// exp. exp and nbf allow CB_JWT_SKEW_S of clock skew, as auth.ts did.
#include "jwt.h"

#include "util/b64.h"
#include "util/err.h"
#include "util/json.h"
#include "util/str.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <string.h>

#define CB_JWT_SKEW_S 30
#define SIG_B64_LEN   43   // base64url of 32 bytes, unpadded

static const char HEADER_JSON[] = "{\"alg\":\"HS256\",\"typ\":\"JWT\"}";

// base64url(HMAC-SHA256(secret, msg[0..n))) into out (SIG_B64_LEN + 1).
static bool sign(const char *secret, const char *msg, size_t n, char out[SIG_B64_LEN + 1]) {
    unsigned char mac[EVP_MAX_MD_SIZE];
    unsigned int mac_len = 0;
    if (!HMAC(EVP_sha256(), secret, (int)strlen(secret), (const unsigned char *)msg, n, mac, &mac_len) || mac_len != 32)
        return false;
    bool ok = cb_b64url_encode(mac, 32, out, SIG_B64_LEN + 1) == SIG_B64_LEN;
    OPENSSL_cleanse(mac, sizeof mac);
    return ok;
}

int cb_jwt_mint(const char *secret, const cb_jwt_claims *c, const char *issuer, char *out, size_t cap) {
    if (!secret || !secret[0]) return CB_E_INVALID;
    cb_buf claims;
    cb_buf_init(&claims);
    cb_jw w;
    cb_jw_init(&w, &claims);
    cb_jw_obj(&w);
    cb_jw_kstr(&w, "aud", "authenticated");
    cb_jw_kint(&w, "exp", c->exp);
    cb_jw_kint(&w, "iat", c->iat);
    if (issuer && issuer[0]) cb_jw_kstr(&w, "iss", issuer);
    cb_jw_kstr(&w, "sub", c->sub);
    cb_jw_kstr(&w, "email", c->email);
    cb_jw_kstr(&w, "role", "authenticated");
    cb_jw_kstr(&w, "session_id", c->session_id);
    cb_jw_key(&w, "user_metadata");
    cb_jw_obj(&w);
    cb_jw_kstr(&w, "username", c->username);
    cb_jw_end(&w);
    cb_jw_key(&w, "app_metadata");
    cb_jw_obj(&w);
    cb_jw_kstr(&w, "provider", "email");
    cb_jw_key(&w, "providers");
    cb_jw_arr(&w);
    cb_jw_str(&w, "email");
    cb_jw_end(&w);
    cb_jw_end(&w);
    cb_jw_kbool(&w, "is_anonymous", false);
    cb_jw_end(&w);
    if (!cb_jw_ok(&w)) { cb_buf_free(&claims); return CB_E_NOMEM; }

    size_t hn = cb_b64url_len(sizeof HEADER_JSON - 1), pn = cb_b64url_len(claims.len);
    size_t total = hn + 1 + pn + 1 + SIG_B64_LEN;
    if (total + 1 > cap) { cb_buf_free(&claims); return CB_E_FULL; }
    cb_b64url_encode((const uint8_t *)HEADER_JSON, sizeof HEADER_JSON - 1, out, cap);
    out[hn] = '.';
    cb_b64url_encode(claims.p, claims.len, out + hn + 1, cap - hn - 1);
    out[hn + 1 + pn] = '.';
    cb_buf_free(&claims);
    if (!sign(secret, out, hn + 1 + pn, out + hn + 1 + pn + 1)) return CB_E_STATE;
    return (int)total;
}

// Only the URL-safe alphabet, no padding, not empty.
static bool b64url_part(const char *p, size_t n) {
    if (n == 0) return false;
    for (size_t i = 0; i < n; i++) {
        char c = p[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_'))
            return false;
    }
    return true;
}

// Decodes one part into a NUL-terminated buffer that must hold valid JSON.
static bool decode_json(const char *p, size_t n, char *out, size_t cap, size_t *len) {
    if (!b64url_part(p, n)) return false;
    int k = cb_b64_decode(p, n, (uint8_t *)out, cap - 1);
    if (k <= 0) return false;
    out[k] = 0;
    *len = (size_t)k;
    return cb_json_valid(out, *len);
}

static bool str_is(const char *json, size_t n, const char *path, const char *want) {
    char v[64];
    return cb_json_get_str(json, n, path, v, sizeof v) && strcmp(v, want) == 0;
}

int cb_jwt_verify(const char *secret, const char *token, int64_t now_s, cb_jwt_claims *out) {
    if (!secret || !secret[0] || !token) return CB_E_AUTH;
    size_t tn = strlen(token);
    if (tn >= CB_JWT_MAX) return CB_E_AUTH;
    const char *d1 = memchr(token, '.', tn);
    if (!d1) return CB_E_AUTH;
    const char *d2 = memchr(d1 + 1, '.', tn - (size_t)(d1 + 1 - token));
    if (!d2) return CB_E_AUTH;
    const char *sig = d2 + 1;
    size_t sn = tn - (size_t)(sig - token);
    if (sn != SIG_B64_LEN || memchr(sig, '.', sn)) return CB_E_AUTH;

    // The signature first: nothing unauthenticated is parsed beyond the dots.
    char want[SIG_B64_LEN + 1];
    if (!sign(secret, token, (size_t)(d2 - token), want)) return CB_E_AUTH;
    if (CRYPTO_memcmp(want, sig, SIG_B64_LEN) != 0) return CB_E_AUTH;

    char head[256], body[CB_JWT_MAX];
    size_t hl, bl;
    if (!decode_json(token, (size_t)(d1 - token), head, sizeof head, &hl)) return CB_E_AUTH;
    if (cb_json_find(head, hl, "", NULL) != CB_JSON_OBJ || !str_is(head, hl, "alg", "HS256")) return CB_E_AUTH;
    if (!decode_json(d1 + 1, (size_t)(d2 - d1 - 1), body, sizeof body, &bl)) return CB_E_AUTH;
    if (cb_json_find(body, bl, "", NULL) != CB_JSON_OBJ) return CB_E_AUTH;

    cb_jwt_claims c;
    memset(&c, 0, sizeof c);
    if (!str_is(body, bl, "aud", "authenticated") || !str_is(body, bl, "role", "authenticated")) return CB_E_AUTH;
    if (!cb_json_get_i64(body, bl, "exp", &c.exp)) return CB_E_AUTH;
    if (now_s > c.exp + CB_JWT_SKEW_S) return CB_E_AUTH;
    cb_json_type nbt = cb_json_find(body, bl, "nbf", NULL);
    if (nbt != CB_JSON_MISSING) {
        int64_t nbf;
        if (!cb_json_get_i64(body, bl, "nbf", &nbf) || now_s + CB_JWT_SKEW_S < nbf) return CB_E_AUTH;
    }
    if (!cb_json_get_i64(body, bl, "iat", &c.iat)) c.iat = 0;
    if (!cb_json_get_str(body, bl, "sub", c.sub, sizeof c.sub) || !cb_uuid_valid(c.sub, strlen(c.sub))) return CB_E_AUTH;
    if (!cb_json_get_str(body, bl, "session_id", c.session_id, sizeof c.session_id) ||
        !cb_uuid_valid(c.session_id, strlen(c.session_id)))
        return CB_E_AUTH;
    if (!cb_json_get_str(body, bl, "email", c.email, sizeof c.email)) c.email[0] = 0;
    if (!cb_json_get_str(body, bl, "user_metadata.username", c.username, sizeof c.username)) c.username[0] = 0;
    *out = c;
    return CB_OK;
}
