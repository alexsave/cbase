// jwt.c - access tokens (jwt.h): a fixed binary layout, HMAC-SHA256, base64url.
//
// payload  = version u8 (1) | sub 16 bytes | session_id 16 bytes | iat i64 BE |
//            exp i64 BE | email_len u8 | email | username_len u8 | username
// token    = base64url(payload || HMAC-SHA256(secret, payload)), unpadded.
// This file owns the layout; no client reads it (the session states its expiry,
// netwire.h NW_SESSION), so there is no JSON and no header to negotiate.
// Verification is strict: the text must be the canonical spelling of its bytes
// (no second spelling of a valid token verifies), the MAC is checked in
// constant time before anything else is read, the version must be 1, the
// lengths must account for every byte, and exp allows CB_JWT_SKEW_S of clock
// skew. Tokens of the earlier GoTrue-JWT format do not verify: one re-sign-in.
#include "jwt.h"

#include "../../../../shared/c/b64/b64.h"
#include "util/err.h"
#include "util/str.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <string.h>

#define CB_JWT_SKEW_S 30
#define JWT_VERSION   1
#define MAC_LEN       32
#define FIXED_LEN     (1 + 16 + 16 + 8 + 8 + 1 + 1)   // everything but the two strings and the MAC

static bool mac_of(const char *secret, const uint8_t *msg, size_t n, uint8_t out[MAC_LEN]) {
    unsigned char mac[EVP_MAX_MD_SIZE];
    unsigned int mac_len = 0;
    if (!HMAC(EVP_sha256(), secret, (int)strlen(secret), msg, n, mac, &mac_len) || mac_len != MAC_LEN) return false;
    memcpy(out, mac, MAC_LEN);
    OPENSSL_cleanse(mac, sizeof mac);
    return true;
}

static void put_i64(uint8_t *p, int64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)((uint64_t)v >> (56 - 8 * i)); }
static int64_t get_i64(const uint8_t *p) { uint64_t v = 0; for (int i = 0; i < 8; i++) v = v << 8 | p[i]; return (int64_t)v; }

int cb_jwt_mint(const char *secret, const cb_jwt_claims *c, char *out, size_t cap) {
    if (!secret || !secret[0]) return CB_E_INVALID;
    size_t el = strlen(c->email), ul = strlen(c->username);
    if (el > 255 || ul > 255) return CB_E_INVALID;
    uint8_t raw[FIXED_LEN + 255 + 255 + MAC_LEN];
    size_t n = 0;
    raw[n++] = JWT_VERSION;
    if (!cb_uuid_pack(c->sub, raw + n)) return CB_E_INVALID;
    n += 16;
    if (!cb_uuid_pack(c->session_id, raw + n)) return CB_E_INVALID;
    n += 16;
    put_i64(raw + n, c->iat); n += 8;
    put_i64(raw + n, c->exp); n += 8;
    raw[n++] = (uint8_t)el; memcpy(raw + n, c->email, el); n += el;
    raw[n++] = (uint8_t)ul; memcpy(raw + n, c->username, ul); n += ul;
    if (!mac_of(secret, raw, n, raw + n)) return CB_E_STATE;
    n += MAC_LEN;
    size_t total = b64url_len(n);
    if (total + 1 > cap) return CB_E_FULL;
    b64url_encode(raw, n, out, cap);
    return (int)total;
}

// A per-thread cache of tokens already verified. A signed token is a pure
// function of its bytes and the secret, so a hit (the exact token text and the
// exact secret) skips the HMAC, both base64 decodes and the claims JSON parse;
// exp and nbf are still checked against `now_s` on every call, so an expired
// token never survives in here. A client sends the same token on every request
// for up to an hour, which is why this is most of the per-request auth cost.
#define VCACHE_N 32
#define VCACHE_TOKEN_MAX 768
#define VCACHE_SECRET_MAX 128
typedef struct {
    uint32_t       tlen, slen;                  // 0: empty
    char           token[VCACHE_TOKEN_MAX], secret[VCACHE_SECRET_MAX];
    cb_jwt_claims  c;
} vcache_ent;
static _Thread_local vcache_ent t_vcache[VCACHE_N];

static vcache_ent *vcache_slot(const char *token, size_t tn) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < tn; i++) h = (h ^ (uint8_t)token[i]) * 16777619u;
    return &t_vcache[h % VCACHE_N];
}

static int times_ok(const cb_jwt_claims *c, int64_t now_s) {
    // The skew moves our clock, never the token's number: exp is any int64 a
    // signed body says, and adding to it can overflow.
    return now_s - CB_JWT_SKEW_S > c->exp ? CB_E_AUTH : CB_OK;
}

int cb_jwt_verify(const char *secret, const char *token, int64_t now_s, cb_jwt_claims *out) {
    if (!secret || !secret[0] || !token) return CB_E_AUTH;
    size_t tn = strlen(token);
    if (tn >= CB_JWT_MAX) return CB_E_AUTH;
    size_t sl = strlen(secret);
    bool cacheable = tn < VCACHE_TOKEN_MAX && sl < VCACHE_SECRET_MAX;
    vcache_ent *ce = cacheable ? vcache_slot(token, tn) : NULL;
    if (ce && ce->tlen == tn && ce->slen == sl && memcmp(ce->token, token, tn) == 0 &&
        memcmp(ce->secret, secret, sl) == 0) {
        int rc = times_ok(&ce->c, now_s);
        if (rc == CB_OK) *out = ce->c;
        return rc;
    }
    uint8_t raw[FIXED_LEN + 255 + 255 + MAC_LEN + 8];
    if (tn == 0) return CB_E_AUTH;
    int k = b64_decode(token, tn, raw, sizeof raw);
    if (k < FIXED_LEN + MAC_LEN) return CB_E_AUTH;
    // The canonical spelling only: the text re-encodes to itself.
    char again[CB_JWT_MAX];
    if (b64url_encode(raw, (size_t)k, again, sizeof again) != (int)tn || memcmp(again, token, tn) != 0) return CB_E_AUTH;
    size_t n = (size_t)k - MAC_LEN;
    uint8_t want[MAC_LEN];
    if (!mac_of(secret, raw, n, want)) return CB_E_NOMEM;
    if (CRYPTO_memcmp(want, raw + n, MAC_LEN) != 0) return CB_E_AUTH;

    // Authenticated: now read it, every length checked against the bytes.
    if (raw[0] != JWT_VERSION) return CB_E_AUTH;
    cb_jwt_claims c;
    memset(&c, 0, sizeof c);
    size_t o = 1;
    cb_uuid_unpack(raw + o, c.sub); o += 16;
    cb_uuid_unpack(raw + o, c.session_id); o += 16;
    c.iat = get_i64(raw + o); o += 8;
    c.exp = get_i64(raw + o); o += 8;
    size_t el = raw[o++];
    if (o + el + 1 > n || el > CB_EMAIL_MAX) return CB_E_AUTH;
    memcpy(c.email, raw + o, el); o += el;
    size_t ul = raw[o++];
    if (o + ul != n || ul > CB_USERNAME_MAX) return CB_E_AUTH;
    memcpy(c.username, raw + o, ul);
    if (times_ok(&c, now_s) != CB_OK) return CB_E_AUTH;
    *out = c;
    if (ce) {
        ce->tlen = (uint32_t)tn;
        ce->slen = (uint32_t)sl;
        memcpy(ce->token, token, tn);
        memcpy(ce->secret, secret, sl);
        ce->c = c;
    }
    return CB_OK;
}
