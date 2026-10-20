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
#include <pthread.h>
#include <stdlib.h>
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

// A per-thread cache of tokens already verified. A signed token is a pure function of its bytes
// and the secret, so a token whose exact text verified under the thread's secret skips the HMAC
// (OpenSSL's one-shot, with its fetches and allocations) and the canonical re-encode on its next
// visit; its claims are read again from its bytes (a base64 decode and the length checks), and
// exp is checked against `now_s` on every call, so an expired token never survives in here.
// Revocation is not this file's: cb_auth_session_live runs on every request, hit or miss.
//
// Associative, and large enough for every client a worker serves: VCACHE_SETS sets of
// VCACHE_WAYS (512 tokens), the oldest way replaced. 32 direct-mapped entries missed under load (every client
// of a busy worker collided), which is when the HMAC costs most. On the heap, made by the first
// verify on a thread and freed with it, so the threads that never verify carry none of it. A
// token longer than VCACHE_TOKEN_MAX (a very long email) is verified every time, as is any token
// under a secret longer than VCACHE_SECRET_MAX.
#define VCACHE_SETS 128
#define VCACHE_WAYS 4
#define VCACHE_TOKEN_MAX 256
#define VCACHE_SECRET_MAX 128
typedef struct {
    uint16_t tlen;                       // 0: empty
    char     token[VCACHE_TOKEN_MAX];
} vcache_ent;
typedef struct {
    uint32_t   slen;
    char       secret[VCACHE_SECRET_MAX];   // the secret every entry verified under
    uint8_t    older[VCACHE_SETS];          // per set: the way to replace next
    vcache_ent e[VCACHE_SETS][VCACHE_WAYS];
} vcache;
static _Thread_local vcache *t_vcache;
static pthread_key_t  g_vcache_key;
static pthread_once_t g_vcache_once = PTHREAD_ONCE_INIT;
static bool           g_vcache_key_ok;
static void vcache_at_exit(void *p) { free(p); }
static void vcache_key_make(void) { g_vcache_key_ok = pthread_key_create(&g_vcache_key, vcache_at_exit) == 0; }

// The thread's cache for `secret` (emptied when the secret is another), or NULL.
static vcache *vcache_for(const char *secret, size_t sl) {
    vcache *v = t_vcache;
    if (!v) {
        if (!(v = calloc(1, sizeof *v))) return NULL;
        pthread_once(&g_vcache_once, vcache_key_make);
        if (g_vcache_key_ok) pthread_setspecific(g_vcache_key, v);
        t_vcache = v;
    }
    if (v->slen != sl || memcmp(v->secret, secret, sl) != 0) {
        memset(v, 0, sizeof *v);
        v->slen = (uint32_t)sl;
        memcpy(v->secret, secret, sl);
    }
    return v;
}

static uint32_t vcache_set(const char *token, size_t tn) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < tn; i++) h = (h ^ (uint8_t)token[i]) * 16777619u;
    return h % VCACHE_SETS;
}

// The way holding exactly `token` in its set, or -1.
static int vcache_find(const vcache *v, uint32_t set, const char *token, size_t tn) {
    for (int w = 0; w < VCACHE_WAYS; w++) {
        const vcache_ent *e = &v->e[set][w];
        if (e->tlen == tn && memcmp(e->token, token, tn) == 0) return w;
    }
    return -1;
}

static _Thread_local uint64_t t_macs;
uint64_t cb_jwt_macs(void) { return t_macs; }

void cb_jwt_vcache_drain(void) {
    if (!t_vcache) return;
    if (g_vcache_key_ok) pthread_setspecific(g_vcache_key, NULL);
    free(t_vcache);
    t_vcache = NULL;
}

static int times_ok(const cb_jwt_claims *c, int64_t now_s) {
    // The skew moves our clock, never the token's number: exp is any int64 a
    // signed body says, and adding to it can overflow.
    return now_s - CB_JWT_SKEW_S > c->exp ? CB_E_AUTH : CB_OK;
}

// The claims of an authenticated payload (raw[0..n), the MAC already checked), every length
// checked against the bytes.
static int claims_of(const uint8_t *raw, size_t n, cb_jwt_claims *out) {
    if (n < FIXED_LEN || raw[0] != JWT_VERSION) return CB_E_AUTH;
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
    *out = c;
    return CB_OK;
}

int cb_jwt_verify(const char *secret, const char *token, int64_t now_s, cb_jwt_claims *out) {
    if (!secret || !secret[0] || !token) return CB_E_AUTH;
    size_t tn = strlen(token);
    if (tn >= CB_JWT_MAX) return CB_E_AUTH;
    if (tn == 0) return CB_E_AUTH;
    size_t sl = strlen(secret);
    vcache *v = tn <= VCACHE_TOKEN_MAX && sl <= VCACHE_SECRET_MAX ? vcache_for(secret, sl) : NULL;
    const uint32_t set = v ? vcache_set(token, tn) : 0;
    const bool hit = v && vcache_find(v, set, token, tn) >= 0;
    uint8_t raw[FIXED_LEN + 255 + 255 + MAC_LEN + 8];
    int k = b64_decode(token, tn, raw, sizeof raw);
    if (k < FIXED_LEN + MAC_LEN) return CB_E_AUTH;
    size_t n = (size_t)k - MAC_LEN;
    if (!hit) {
        // The canonical spelling only: the text re-encodes to itself.
        char again[CB_JWT_MAX];
        if (b64url_encode(raw, (size_t)k, again, sizeof again) != (int)tn || memcmp(again, token, tn) != 0) return CB_E_AUTH;
        uint8_t want[MAC_LEN];
        t_macs++;
        if (!mac_of(secret, raw, n, want)) return CB_E_NOMEM;
        if (CRYPTO_memcmp(want, raw + n, MAC_LEN) != 0) return CB_E_AUTH;
    }
    // Authenticated: now read it.
    cb_jwt_claims c;
    if (claims_of(raw, n, &c) != CB_OK || times_ok(&c, now_s) != CB_OK) return CB_E_AUTH;
    *out = c;
    if (v && !hit) {   // a token whose claims read is cached: the older way of its set goes
        const int w = v->older[set];
        v->e[set][w].tlen = (uint16_t)tn;
        memcpy(v->e[set][w].token, token, tn);
        v->older[set] = (uint8_t)((w + 1) % VCACHE_WAYS);
    }
    return CB_OK;
}
