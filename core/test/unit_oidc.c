// unit_oidc.c - Sign in with Apple, end to end against a fake Apple (lib_fake_idp.h): the token
// verifier (auth/oidc.h) and every one of its refusals, the key set's cache and rotation, the client
// secret, the code exchange and the revocation, the grant's seal; then the account rules (auth.h
// cb_auth_oidc) through the route (POST /v1/auth/oidc) on a real store: a new identity asks for a
// name, makes an account with no password, signs in again, is used once, links, refuses another
// account's, and is revoked at Apple when its account is deleted.
#include "t.h"
#include "cbase/app.h"
#include "cbase/provider.h"

#include "auth/auth.h"
#include "auth/auth_http.h"
#include "auth/oidc.h"
#include "cbase/auth_wire.h"
#include "handler.h"
#include "lib_fake_idp.h"
#include "net/https.h"
#include "net/router.h"
#include "rate_limits.h"
#include "store/store.h"
#include "util/clock.h"
#include "util/err.h"
#include "util/log.h"
#include "util/str.h"

#include "../../../shared/c/b64/b64.h"
#include "../../../shared/c/le_bytes.h"

#include <openssl/pem.h>
#include <openssl/sha.h>
#include <stdlib.h>
#include <unistd.h>

#define IOS "com.example.app"
#define WEB "com.example.web"
#define NONCE "a-raw-nonce-of-thirty-two-bytes!"
#define SECRET "unit-oidc-secret-with-enough-bytes-in-it"

static fake_idp g_f;
static int64_t g_now;   // ms
static int64_t test_now(void) { return g_now; }
static int64_t now_s(void) { return g_now / 1000; }

static void config(cb_config *c, bool with_key) {
    memset(c, 0, sizeof *c);
    strcpy(c->jwt_secret, SECRET);
    strcpy(c->allowed_origins, "*");
    strcpy(c->public_url, "https://api.example/");
    c->jwt_ttl_s = 3600;
    cb_strlcpy(c->apple_issuer, g_f.issuer, sizeof c->apple_issuer);
    strcpy(c->apple_client_ids, IOS "," WEB);
    if (with_key) {
        strcpy(c->apple_team_id, g_f.team_id);
        strcpy(c->apple_key_id, g_f.key_id);
        cb_strlcpy(c->apple_private_key, fi_p8_pem(&g_f, true), sizeof c->apple_private_key);
    }
}

static cb_oidc *new_oidc(bool with_key) {
    cb_config c;
    config(&c, with_key);
    cb_oidc *o = NULL;
    char err[256];
    T_EQ_INT(cb_oidc_new(&c, &o, err, sizeof err), CB_OK);
    return o;
}

// A good token's claims for subject sub, now.
static fi_claims good(const char *sub) {
    return (fi_claims){ .iss = g_f.issuer, .sub = sub, .aud = IOS, .nonce_raw = NONCE, .iat = now_s(), .exp = now_s() + 600,
                        .email = "x7k2@privaterelay.appleid.com", .email_verified = "\"true\"", .is_private_email = "true" };
}

static int verify(cb_oidc *o, const char *tok, cb_oidc_identity *id) {
    return cb_oidc_verify(o, CBW_IDP_APPLE, tok, tok ? strlen(tok) : 0, NONCE, strlen(NONCE), g_now, id);
}

// ---- the verifier ---------------------------------------------------------------------------

static void a_good_token(void) {
    cb_oidc *o = new_oidc(false);
    T_CHECK(cb_oidc_on(o, CBW_IDP_APPLE));
    T_CHECK(!cb_oidc_can_exchange(o, CBW_IDP_APPLE));
    T_CHECK(!cb_oidc_on(o, 9));
    fi_claims c = good("001234.subject.5678");
    const char *tok = fi_mint(&g_f, 0, NULL, NULL, &c);
    cb_oidc_identity id;
    T_EQ_INT(verify(o, tok, &id), CB_OK);
    T_EQ_STR(id.sub, "001234.subject.5678");
    T_EQ_STR(id.client_id, IOS);
    T_EQ_STR(id.email, "x7k2@privaterelay.appleid.com");
    T_CHECK(id.email_verified && id.private_email);
    T_EQ_INT(id.exp, now_s() + 600);
    // The replay key is the token's hash.
    unsigned char d[32];
    SHA256((const unsigned char *)tok, strlen(tok), d);
    char hex[65];
    for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", d[i]);
    T_EQ_STR(id.key, hex);
    // The web's client, an aud array naming it, booleans as booleans, no email at all.
    c.aud = "someone.else," WEB;
    c.email = NULL;
    c.email_verified = "false";
    c.is_private_email = NULL;
    T_EQ_INT(verify(o, fi_mint(&g_f, 0, NULL, NULL, &c), &id), CB_OK);
    T_EQ_STR(id.client_id, WEB);
    T_EQ_STR(id.email, "");
    T_CHECK(!id.email_verified && !id.private_email);
    // Within the skew on both ends.
    c = good("s");
    c.exp = now_s() - CB_OIDC_SKEW_S + 1;
    c.iat = now_s() + CB_OIDC_SKEW_S;
    T_EQ_INT(verify(o, fi_mint(&g_f, 0, NULL, NULL, &c), &id), CB_OK);
    // A provider that is off.
    T_EQ_INT(cb_oidc_verify(o, 9, "a.b.c", 5, NONCE, strlen(NONCE), g_now, &id), CB_E_NOT_FOUND);
    T_EQ_INT(cb_oidc_verify(o, CBW_IDP_APPLE, tok, strlen(tok), NULL, 0, g_now, &id), CB_E_AUTH);
    cb_oidc_free(o);
}

static void every_refusal(void) {
    cb_oidc *o = new_oidc(false);
    cb_oidc_identity id;
    fi_claims c;
#define REFUSED(expr) do { const char *t_ = (expr); T_CHECK(t_ != NULL); T_EQ_INT(verify(o, t_, &id), CB_E_AUTH); T_EQ_STR(id.sub, ""); } while (0)
    c = good("s"); c.iss = "https://appleid.apple.com"; REFUSED(fi_mint(&g_f, 0, NULL, NULL, &c));
    c = good("s"); c.iss = NULL; REFUSED(fi_mint(&g_f, 0, NULL, NULL, &c));
    c = good("s"); c.aud = "com.example.other"; REFUSED(fi_mint(&g_f, 0, NULL, NULL, &c));
    c = good("s"); c.aud = "a,b"; REFUSED(fi_mint(&g_f, 0, NULL, NULL, &c));
    c = good("s"); c.aud = NULL; REFUSED(fi_mint(&g_f, 0, NULL, NULL, &c));
    c = good("s"); c.exp = now_s() - CB_OIDC_SKEW_S; REFUSED(fi_mint(&g_f, 0, NULL, NULL, &c));
    c = good("s"); c.iat = now_s() + CB_OIDC_SKEW_S + 1; REFUSED(fi_mint(&g_f, 0, NULL, NULL, &c));
    c = good("s"); c.no_exp = true; REFUSED(fi_mint(&g_f, 0, NULL, NULL, &c));
    c = good("s"); c.no_iat = true; REFUSED(fi_mint(&g_f, 0, NULL, NULL, &c));
    c = good("s"); c.nonce_raw = "another-raw-nonce-of-32-bytes!!!"; REFUSED(fi_mint(&g_f, 0, NULL, NULL, &c));
    c = good("s"); c.nonce_raw = NULL; REFUSED(fi_mint(&g_f, 0, NULL, NULL, &c));
    c = good("s"); c.nonce_raw = NULL; c.nonce_hashed = NONCE; REFUSED(fi_mint(&g_f, 0, NULL, NULL, &c));   // the raw one is not the claim
    c = good("s"); c.sub = ""; REFUSED(fi_mint(&g_f, 0, NULL, NULL, &c));
    c = good("s"); c.sub = NULL; REFUSED(fi_mint(&g_f, 0, NULL, NULL, &c));
    c = good("s"); c.extra = ",\"exp\""; REFUSED(fi_mint(&g_f, 0, NULL, NULL, &c));   // not JSON as a whole
    // The header: only RS256, a kid that is published, by the key that kid names.
    c = good("s");
    REFUSED(fi_mint(&g_f, 0, "HS256", NULL, &c));
    REFUSED(fi_mint(&g_f, 0, "none", NULL, &c));
    REFUSED(fi_mint(&g_f, 0, "RS512", NULL, &c));
    REFUSED(fi_mint(&g_f, 1, NULL, "k0", &c));      // signed by another key than k0
    REFUSED(fi_mint_raw(&g_f, 0, "{\"alg\":\"RS256\"}", "{}"));
    REFUSED(fi_mint_raw(&g_f, 0, "[1]", "{}"));
    REFUSED(fi_mint_raw(&g_f, 0, "{\"alg\":\"RS256\",\"kid\":\"k0\"}", "{\"iss\":1}"));
    REFUSED(fi_mint_raw(&g_f, 0, "{\"alg\":\"RS256\",\"kid\":\"k0\"}", "not json"));
    // The shape: three parts, canonical base64url, a whole signature.
    char t[32768];
    cb_strlcpy(t, fi_mint(&g_f, 0, NULL, NULL, &c), sizeof t);
    T_EQ_INT(verify(o, t, &id), CB_OK);
    size_t n = strlen(t);
    char *sig = strrchr(t, '.') + 1;
    char keep = sig[5];
    sig[5] = sig[5] == 'A' ? 'B' : 'A';              // one signature character
    REFUSED(t);
    sig[5] = keep;
    // The last character of a 256-byte signature carries 4 bits and 2 spare ones: setting a spare
    // bit spells the same bytes another way, which only the canonical check refuses.
    static const char ALPHA[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    char last = t[n - 1];
    int v = (int)(strchr(ALPHA, last) - ALPHA);
    T_EQ_INT(v & 3, 0);
    t[n - 1] = ALPHA[v | 1];
    REFUSED(t);
    t[n - 1] = last;
    // The other alphabet: '+' and '/' decode as '-' and '_' do, to the same bytes, and only the
    // canonical check refuses the second spelling.
    char *alt = strpbrk(sig, "-_");
    T_CHECK(alt != NULL);
    if (alt) {
        char was = *alt;
        *alt = was == '-' ? '+' : '/';
        REFUSED(t);
        *alt = was;
    }
    T_EQ_INT(verify(o, t, &id), CB_OK);
    char four[33000];
    snprintf(four, sizeof four, "%s.x", t);
    REFUSED(four);
    snprintf(four, sizeof four, "%.*s", (int)(sig - 1 - t), t);   // two parts
    REFUSED(four);
    snprintf(four, sizeof four, "%.*s.", (int)(sig - 1 - t), t);  // an empty signature
    REFUSED(four);
    snprintf(four, sizeof four, "%s=", t);          // padding
    REFUSED(four);
    REFUSED("");
    REFUSED("..");
    T_EQ_INT(cb_oidc_verify(o, CBW_IDP_APPLE, t, n + 1, NONCE, strlen(NONCE), g_now, &id), CB_E_AUTH);   // a NUL inside
    char *big = malloc(CB_OIDC_TOKEN_MAX + 2);
    memset(big, 'A', CB_OIDC_TOKEN_MAX + 1);
    big[CB_OIDC_TOKEN_MAX + 1] = 0;
    REFUSED(big);
    free(big);
    // The raw nonce's bounds, with tokens whose nonce claim is that raw nonce's hash.
    {
        char shortn[CB_OIDC_NONCE_MIN];
        memset(shortn, 's', sizeof shortn - 1);
        shortn[sizeof shortn - 1] = 0;
        fi_claims sc = good("s");
        sc.nonce_raw = shortn;
        const char *st = fi_mint(&g_f, 0, NULL, NULL, &sc);
        T_EQ_INT(cb_oidc_verify(o, CBW_IDP_APPLE, st, strlen(st), shortn, strlen(shortn), g_now, &id), CB_E_AUTH);
        char longr[CB_OIDC_NONCE_MAX + 2];
        memset(longr, 'l', sizeof longr - 1);
        longr[sizeof longr - 1] = 0;
        sc.nonce_raw = longr;
        st = fi_mint(&g_f, 0, NULL, NULL, &sc);
        T_EQ_INT(cb_oidc_verify(o, CBW_IDP_APPLE, st, strlen(st), longr, strlen(longr), g_now, &id), CB_E_AUTH);
        longr[CB_OIDC_NONCE_MAX] = 0;   // at the bound it is good
        sc.nonce_raw = longr;
        st = fi_mint(&g_f, 0, NULL, NULL, &sc);
        T_EQ_INT(cb_oidc_verify(o, CBW_IDP_APPLE, st, strlen(st), longr, strlen(longr), g_now, &id), CB_OK);
    }
    // A token past CB_OIDC_TOKEN_MAX, well formed and signed, is refused for its size alone.
    {
        static char pad[CB_OIDC_TOKEN_MAX];
        fi_claims bc = good("s");
        snprintf(pad, sizeof pad, ",\"pad\":\"%0*d\"", CB_OIDC_TOKEN_MAX * 3 / 4 - 400, 0);
        bc.extra = pad;
        const char *bt = fi_mint(&g_f, 0, NULL, NULL, &bc);
        T_CHECK(bt && strlen(bt) > CB_OIDC_TOKEN_MAX);
        T_EQ_INT(verify(o, bt, &id), CB_E_AUTH);
        snprintf(pad, sizeof pad, ",\"pad\":\"%0*d\"", CB_OIDC_TOKEN_MAX * 3 / 4 - 1200, 0);
        bt = fi_mint(&g_f, 0, NULL, NULL, &bc);
        T_CHECK(bt && strlen(bt) <= CB_OIDC_TOKEN_MAX);
        T_EQ_INT(verify(o, bt, &id), CB_OK);
    }
    // The raw nonce's bounds.
    T_EQ_INT(cb_oidc_verify(o, CBW_IDP_APPLE, t, n, NONCE, CB_OIDC_NONCE_MIN - 1, g_now, &id), CB_E_AUTH);
    char longn[CB_OIDC_NONCE_MAX + 2];
    memset(longn, 'n', sizeof longn);
    c.nonce_raw = NULL;
    T_EQ_INT(cb_oidc_verify(o, CBW_IDP_APPLE, t, n, longn, CB_OIDC_NONCE_MAX + 1, g_now, &id), CB_E_AUTH);
#undef REFUSED
    cb_oidc_free(o);
}

// The key set: fetched once, kept; a kid it does not hold fetched again at once, but not more than
// once a minute; a weak key never used; a provider that is down.
static void keys_rotate_and_refetch(void) {
    fi_publish(&g_f, 0, true);
    fi_publish(&g_f, 1, false);
    cb_oidc *o = new_oidc(false);
    cb_oidc_identity id;
    fi_claims c = good("s");
    int f0 = g_f.key_fetches;
    T_EQ_INT(verify(o, fi_mint(&g_f, 0, NULL, NULL, &c), &id), CB_OK);
    T_EQ_INT(verify(o, fi_mint(&g_f, 0, NULL, NULL, &c), &id), CB_OK);
    T_EQ_INT(g_f.key_fetches - f0, 1);
    T_EQ_INT(cb_oidc_key_fetches(o, CBW_IDP_APPLE), 1);
    // Apple rotates: k1 appears. The first token of it fetches, at once.
    fi_publish(&g_f, 1, true);
    g_now += CB_OIDC_REFETCH_MS;
    c = good("s");
    T_EQ_INT(verify(o, fi_mint(&g_f, 1, NULL, NULL, &c), &id), CB_OK);
    T_EQ_INT(g_f.key_fetches - f0, 2);
    // Made-up kids: one fetch, then none for a minute.
    T_EQ_INT(verify(o, fi_mint(&g_f, 2, NULL, NULL, &c), &id), CB_E_AUTH);   // within the minute of the last
    T_EQ_INT(g_f.key_fetches - f0, 2);
    g_now += CB_OIDC_REFETCH_MS;
    c = good("s");
    for (int i = 0; i < 5; i++) T_EQ_INT(verify(o, fi_mint(&g_f, 2, NULL, NULL, &c), &id), CB_E_AUTH);
    T_EQ_INT(g_f.key_fetches - f0, 3);
    // k2 published now: still refused within the minute, taken after it.
    fi_publish(&g_f, 2, true);
    T_EQ_INT(verify(o, fi_mint(&g_f, 2, NULL, NULL, &c), &id), CB_E_AUTH);
    g_now += CB_OIDC_REFETCH_MS;
    c = good("s");
    T_EQ_INT(verify(o, fi_mint(&g_f, 2, NULL, NULL, &c), &id), CB_OK);
    // A key no longer published still verifies until the next fetch (a day, or another unknown kid).
    fi_publish(&g_f, 2, false);
    T_EQ_INT(verify(o, fi_mint(&g_f, 2, NULL, NULL, &c), &id), CB_OK);
    g_now += CB_OIDC_KEYS_TTL_MS;
    c = good("s");
    T_EQ_INT(verify(o, fi_mint(&g_f, 2, NULL, NULL, &c), &id), CB_E_AUTH);   // the day's fetch dropped it
    T_EQ_INT(verify(o, fi_mint(&g_f, 0, NULL, NULL, &c), &id), CB_OK);
    // A 1024-bit key is skipped: its kid is as good as unknown.
    g_f.publish_weak = true;
    g_now += CB_OIDC_REFETCH_MS;
    c = good("s");
    T_EQ_INT(verify(o, fi_mint(&g_f, -1, NULL, NULL, &c), &id), CB_E_AUTH);
    g_f.publish_weak = false;
    // The provider down: the keys held still verify; a fetch the TTL wants fails and keeps them.
    g_f.keys_status = 503;
    g_now += CB_OIDC_KEYS_TTL_MS;
    c = good("s");
    T_EQ_INT(verify(o, fi_mint(&g_f, 0, NULL, NULL, &c), &id), CB_OK);
    // With none held it is the server's moment (CB_E_IO), and a second within a second asks nothing.
    cb_oidc *fresh = new_oidc(false);
    int f1 = g_f.key_fetches;
    T_EQ_INT(verify(fresh, fi_mint(&g_f, 0, NULL, NULL, &c), &id), CB_E_IO);
    T_EQ_INT(verify(fresh, fi_mint(&g_f, 0, NULL, NULL, &c), &id), CB_E_IO);
    T_EQ_INT(g_f.key_fetches - f1, 1);
    g_now += CB_OIDC_RETRY_MS;
    g_f.keys_status = 0;
    c = good("s");
    T_EQ_INT(verify(fresh, fi_mint(&g_f, 0, NULL, NULL, &c), &id), CB_OK);
    // A kid not held while the provider is down: the moment's, not the token's.
    g_f.keys_status = 500;
    g_now += CB_OIDC_REFETCH_MS;
    c = good("s");
    T_EQ_INT(verify(fresh, fi_mint(&g_f, 3, NULL, NULL, &c), &id), CB_E_IO);
    g_f.keys_status = 0;
    cb_oidc_free(fresh);
    cb_oidc_free(o);
    for (int i = 1; i < FI_KEYS; i++) fi_publish(&g_f, i, false);
}

// A key set read as a fetch reads it (cb_oidc_keys_load): unusable keys skipped, not the set; a kid
// twice is its first key; nothing usable keeps what is held.
static void key_sets(void) {
    cb_oidc *o = new_oidc(false);
    cb_oidc_identity id;
    char set[8192], k0[2048], k1[2048];
    cb_strlcpy(k0, fi_jwk(&g_f, 0, "k0"), sizeof k0);
    cb_strlcpy(k1, fi_jwk(&g_f, 1, "k0"), sizeof k1);   // another key under the same kid
    // An unusable key first (1024 bits; then one of kty EC; then a bad modulus), then the good one.
    snprintf(set, sizeof set, "{\"keys\":[%s,{\"kty\":\"EC\",\"kid\":\"e\",\"crv\":\"P-256\"},"
             "{\"kty\":\"RSA\",\"kid\":\"z\",\"n\":\"AA\",\"e\":\"AQAB\"},%s,%s]}", fi_jwk(&g_f, -1, "weak"), k0, k1);
    T_EQ_INT(cb_oidc_keys_load(o, CBW_IDP_APPLE, set, strlen(set), g_now), 1);
    fi_claims c = good("s");
    T_EQ_INT(verify(o, fi_mint(&g_f, 0, NULL, "k0", &c), &id), CB_OK);    // the first k0
    T_EQ_INT(verify(o, fi_mint(&g_f, 1, NULL, "k0", &c), &id), CB_E_AUTH);
    T_EQ_INT(verify(o, fi_mint(&g_f, -1, NULL, "weak", &c), &id), CB_E_AUTH);
    // Nothing usable: 0, and the held key still verifies; not a key set: -1.
    snprintf(set, sizeof set, "{\"keys\":[%s]}", fi_jwk(&g_f, -1, "weak"));
    T_EQ_INT(cb_oidc_keys_load(o, CBW_IDP_APPLE, set, strlen(set), g_now), 0);
    T_EQ_INT(verify(o, fi_mint(&g_f, 0, NULL, "k0", &c), &id), CB_OK);
    T_EQ_INT(cb_oidc_keys_load(o, CBW_IDP_APPLE, "{\"keys\":1}", 10, g_now), -1);
    T_EQ_INT(cb_oidc_keys_load(o, CBW_IDP_APPLE, "[", 1, g_now), -1);
    T_EQ_INT(cb_oidc_keys_load(o, 9, set, strlen(set), g_now), -1);
    // More than it holds: the first CB_OIDC_JWKS_MAX.
    cb_buf b;
    cb_buf_init(&b);
    cb_buf_puts(&b, "{\"keys\":[");
    for (int i = 0; i < CB_OIDC_JWKS_MAX + 2; i++) {
        char kid[8];
        snprintf(kid, sizeof kid, "m%d", i);
        cb_buf_printf(&b, "%s%s", i ? "," : "", fi_jwk(&g_f, i % 2, kid));
    }
    cb_buf_puts(&b, "]}");
    T_EQ_INT(cb_oidc_keys_load(o, CBW_IDP_APPLE, (const char *)b.p, b.len, g_now), CB_OIDC_JWKS_MAX);
    T_EQ_INT(verify(o, fi_mint(&g_f, 1, NULL, "m15", &c), &id), CB_OK);
    T_EQ_INT(verify(o, fi_mint(&g_f, 0, NULL, "m16", &c), &id), CB_E_AUTH);
    cb_buf_free(&b);
    cb_oidc_free(o);
}

// The configuration's own refusals.
static void configuration(void) {
    cb_config c;
    cb_oidc *o = NULL;
    char err[256];
    config(&c, false);
    char big[CB_OIDC_STR_MAX + 2];
    memset(big, 'c', sizeof big - 1);
    big[sizeof big - 1] = 0;
    snprintf(c.apple_client_ids, sizeof c.apple_client_ids, IOS ",%s", big);
    T_EQ_INT(cb_oidc_new(&c, &o, err, sizeof err), CB_E_INVALID);
    big[CB_OIDC_STR_MAX] = 0;                       // at the bound it is one
    snprintf(c.apple_client_ids, sizeof c.apple_client_ids, IOS ",%s", big);
    T_EQ_INT(cb_oidc_new(&c, &o, err, sizeof err), CB_OK);
    cb_oidc_free(o);
    strcpy(c.apple_client_ids, "a,b,c,d,e,f,g,h,i");   // nine: one past CB_OIDC_CLIENTS_MAX
    T_EQ_INT(cb_oidc_new(&c, &o, err, sizeof err), CB_E_INVALID);
    strcpy(c.apple_client_ids, "a,,b,");            // empty items are no client
    T_EQ_INT(cb_oidc_new(&c, &o, err, sizeof err), CB_OK);
    T_CHECK(cb_oidc_on(o, CBW_IDP_APPLE));
    cb_oidc_free(o);
    strcpy(c.apple_client_ids, ",");
    T_EQ_INT(cb_oidc_new(&c, &o, err, sizeof err), CB_OK);
    T_CHECK(!cb_oidc_on(o, CBW_IDP_APPLE));        // no client: off
    cb_oidc_free(o);
    // A P-384 key is not ES256's.
    config(&c, true);
    EVP_PKEY *p384 = EVP_EC_gen("P-384");
    BIO *bio = BIO_new(BIO_s_mem());
    PEM_write_bio_PrivateKey(bio, p384, NULL, NULL, 0, NULL, NULL);
    char *pem = NULL;
    long pl = BIO_get_mem_data(bio, &pem);
    snprintf(c.apple_private_key, sizeof c.apple_private_key, "%.*s", (int)pl, pem);
    BIO_free(bio);
    EVP_PKEY_free(p384);
    T_EQ_INT(cb_oidc_new(&c, &o, err, sizeof err), CB_E_INVALID);
    // The issuer's port.
    config(&c, false);
    strcpy(c.apple_issuer, "https://localhost:0");
    T_EQ_INT(cb_oidc_new(&c, &o, err, sizeof err), CB_E_INVALID);
    strcpy(c.apple_issuer, "https://localhost:70000");
    T_EQ_INT(cb_oidc_new(&c, &o, err, sizeof err), CB_E_INVALID);
    strcpy(c.apple_issuer, "https://:443");
    T_EQ_INT(cb_oidc_new(&c, &o, err, sizeof err), CB_E_INVALID);
}

// ---- the client secret, the exchange, the revocation, the seal -------------------------------

static void client_secret(void) {
    cb_oidc *o = new_oidc(true);
    T_CHECK(cb_oidc_can_exchange(o, CBW_IDP_APPLE));
    char s[1024];
    int n = cb_oidc_client_secret(o, CBW_IDP_APPLE, WEB, now_s(), s, sizeof s);
    T_CHECK(n > 0 && (size_t)n == strlen(s));
    T_CHECK(fi_secret_ok(&g_f, s, WEB));
    T_CHECK(!fi_secret_ok(&g_f, s, IOS));          // its sub is the client it was made for
    g_now += CB_OIDC_SECRET_S * 1000;              // and it expires
    T_CHECK(!fi_secret_ok(&g_f, s, WEB));
    T_EQ_INT(cb_oidc_client_secret(o, CBW_IDP_APPLE, WEB, now_s(), s, 10), CB_E_FULL);
    cb_oidc *nokey = new_oidc(false);
    T_EQ_INT(cb_oidc_client_secret(nokey, CBW_IDP_APPLE, WEB, now_s(), s, sizeof s), CB_E_STATE);
    char r[CB_OIDC_REFRESH_MAX + 1];
    T_EQ_INT(cb_oidc_exchange(nokey, CBW_IDP_APPLE, IOS, "s", "code-s", 6, g_now, r, sizeof r), CB_E_STATE);
    T_EQ_INT(cb_oidc_revoke(nokey, CBW_IDP_APPLE, IOS, "rt", g_now), CB_E_STATE);
    cb_oidc_free(nokey);
    // A key that is not P-256 PEM refuses the configuration.
    cb_config c;
    config(&c, true);
    strcpy(c.apple_private_key, "-----BEGIN PRIVATE KEY-----\\nnot a key\\n-----END PRIVATE KEY-----\\n");
    cb_oidc *bad = NULL;
    char err[256];
    T_EQ_INT(cb_oidc_new(&c, &bad, err, sizeof err), CB_E_INVALID);
    T_CHECK(strstr(err, "P-256") != NULL);
    // An RSA key in PEM is not one either (ES256 is P-256).
    BIO *b = BIO_new(BIO_s_mem());
    PEM_write_bio_PrivateKey(b, g_f.rsa[0], NULL, NULL, 0, NULL, NULL);
    char *p = NULL;
    long pl = BIO_get_mem_data(b, &p);
    snprintf(c.apple_private_key, sizeof c.apple_private_key, "%.*s", (int)pl, p);
    BIO_free(b);
    T_EQ_INT(cb_oidc_new(&c, &bad, err, sizeof err), CB_E_INVALID);
    // The PEM with real line breaks loads as the escaped one does.
    config(&c, true);
    cb_strlcpy(c.apple_private_key, fi_p8_pem(&g_f, false), sizeof c.apple_private_key);
    T_EQ_INT(cb_oidc_new(&c, &bad, err, sizeof err), CB_OK);
    cb_oidc_free(bad);
    cb_oidc_free(o);
}

static void exchange_and_revoke(void) {
    cb_oidc *o = new_oidc(true);
    char r[CB_OIDC_REFRESH_MAX + 1];
    int calls = g_f.token_calls;
    T_EQ_INT(cb_oidc_exchange(o, CBW_IDP_APPLE, IOS, "sub1", "code-sub1", 9, g_now, r, sizeof r), CB_OK);
    T_CHECK(strncmp(r, "rt-sub1-", 8) == 0);
    T_EQ_INT(g_f.token_calls - calls, 1);
    T_CHECK(strstr(g_f.last_form, "client_id=com.example.app&") != NULL);
    T_CHECK(strstr(g_f.last_form, "&code=code-sub1&grant_type=authorization_code") != NULL);
    T_CHECK(strstr(g_f.last_form, "redirect_uri") == NULL);   // none configured
    T_EQ_INT(g_f.bad_secrets, 0);
    T_EQ_INT(cb_oidc_exchange(o, CBW_IDP_APPLE, IOS, "sub1", "code-bad", 8, g_now, r, sizeof r), CB_E_AUTH);
    T_EQ_STR(r, "");
    T_EQ_INT(cb_oidc_exchange(o, CBW_IDP_APPLE, IOS, "sub1", "code-500", 8, g_now, r, sizeof r), CB_E_IO);
    // The grant must be the signed-in subject's.
    T_EQ_INT(cb_oidc_exchange(o, CBW_IDP_APPLE, IOS, "sub1", "code-other-sub1", 15, g_now, r, sizeof r), CB_E_AUTH);
    T_EQ_INT(cb_oidc_exchange(o, CBW_IDP_APPLE, IOS, "sub2", "code-sub1", 9, g_now, r, sizeof r), CB_E_AUTH);
    // A client that is not ours, an empty code, a NUL in it: never sent.
    calls = g_f.token_calls;
    T_EQ_INT(cb_oidc_exchange(o, CBW_IDP_APPLE, "com.example.x", "sub1", "code-sub1", 9, g_now, r, sizeof r), CB_E_AUTH);
    // A revocation names the client the grant was made to, configured still or not: a client retired
    // from the configuration still has its grants revoked.
    T_EQ_INT(cb_oidc_revoke(o, CBW_IDP_APPLE, "com.example.retired", "rt", g_now), CB_OK);
    T_EQ_INT(cb_oidc_exchange(o, CBW_IDP_APPLE, IOS, "sub1", "", 0, g_now, r, sizeof r), CB_E_AUTH);
    T_EQ_INT(cb_oidc_exchange(o, CBW_IDP_APPLE, IOS, "sub1", "code\0x", 6, g_now, r, sizeof r), CB_E_AUTH);
    T_EQ_INT(g_f.token_calls - calls, 0);
    T_EQ_INT(cb_oidc_exchange(o, CBW_IDP_APPLE, IOS, "sub1", "code-sub1", 9, g_now, r, 5), CB_E_FULL);
    // The web client's exchange repeats the redirect URI it was configured with.
    cb_config c;
    config(&c, true);
    strcpy(c.apple_redirect_uri, "https://staging.example/auth/apple");
    cb_oidc *web = NULL;
    char err[256];
    T_EQ_INT(cb_oidc_new(&c, &web, err, sizeof err), CB_OK);
    T_EQ_INT(cb_oidc_exchange(web, CBW_IDP_APPLE, WEB, "w", "code-w", 6, g_now, r, sizeof r), CB_OK);
    T_CHECK(strstr(g_f.last_form, "&redirect_uri=https%3A%2F%2Fstaging.example%2Fauth%2Fapple") != NULL);
    cb_oidc_free(web);
    // Revocation.
    int n0 = g_f.n_revoked;
    T_EQ_INT(cb_oidc_revoke(o, CBW_IDP_APPLE, IOS, "rt-sub1-1", g_now), CB_OK);
    T_EQ_INT(g_f.n_revoked - n0, 1);
    T_EQ_STR(g_f.revoked[n0], "rt-sub1-1");
    T_CHECK(strstr(g_f.last_form, "&token_type_hint=refresh_token") != NULL);
    g_f.revoke_status = 400;
    T_EQ_INT(cb_oidc_revoke(o, CBW_IDP_APPLE, IOS, "rt", g_now), CB_E_AUTH);
    g_f.revoke_status = 503;
    T_EQ_INT(cb_oidc_revoke(o, CBW_IDP_APPLE, IOS, "rt", g_now), CB_E_IO);
    g_f.revoke_status = 0;
    T_EQ_INT(cb_oidc_revoke(o, CBW_IDP_APPLE, IOS, "", g_now), CB_E_AUTH);
    T_EQ_INT(g_f.bad_secrets, 0);
    cb_oidc_free(o);
}

static void seal(void) {
    cb_oidc *o = new_oidc(true);
    char s[CB_OIDC_SEALED_MAX], s2[CB_OIDC_SEALED_MAX], back[CB_OIDC_REFRESH_MAX + 1];
    T_CHECK(cb_oidc_seal(o, "rt-secret-grant", s, sizeof s) > 0);
    T_CHECK(cb_oidc_seal(o, "rt-secret-grant", s2, sizeof s2) > 0);
    T_CHECK(strcmp(s, s2) != 0);                    // a fresh nonce each time
    T_CHECK(strstr(s, "rt-secret") == NULL);
    T_EQ_INT(cb_oidc_open(o, s, back, sizeof back), 15);
    T_EQ_STR(back, "rt-secret-grant");
    T_EQ_INT(cb_oidc_open(o, s, back, 15), -1);     // no room for the NUL
    s[30] = s[30] == '0' ? '1' : '0';               // tampered
    T_EQ_INT(cb_oidc_open(o, s, back, sizeof back), -1);
    T_EQ_STR(back, "");
    T_EQ_INT(cb_oidc_open(o, "", back, sizeof back), -1);
    T_EQ_INT(cb_oidc_open(o, "zz", back, sizeof back), -1);
    // Another JWT secret is another key.
    cb_config c;
    config(&c, true);
    strcpy(c.jwt_secret, "another-secret-another-secret-another-secret");
    cb_oidc *other = NULL;
    char err[256];
    T_EQ_INT(cb_oidc_new(&c, &other, err, sizeof err), CB_OK);
    T_EQ_INT(cb_oidc_open(other, s2, back, sizeof back), -1);
    T_CHECK(cb_oidc_open(o, s2, back, sizeof back) > 0);
    char big[CB_OIDC_REFRESH_MAX + 2];
    memset(big, 'r', sizeof big - 1);
    big[sizeof big - 1] = 0;
    T_EQ_INT(cb_oidc_seal(o, big, s, sizeof s), -1);
    big[CB_OIDC_REFRESH_MAX] = 0;
    T_CHECK(cb_oidc_seal(o, big, s, sizeof s) > 0);
    T_EQ_INT(cb_oidc_open(o, s, back, sizeof back), CB_OIDC_REFRESH_MAX);
    T_EQ_INT(cb_oidc_seal(o, "x", s, 10), -1);
    cb_oidc_free(other);
    cb_oidc_free(o);
}

// ---- the account rules, through the route -----------------------------------------------------

static cb_config g_cfg;
static cb_ctx g_ctx;
static char g_db[512];
static const cb_app APP = { .name = "unit-oidc", .account_domain = "accounts.example" };

static void setup(bool with_key) {
    config(&g_cfg, with_key);
    memset(&g_ctx, 0, sizeof g_ctx);
    g_ctx.cfg = &g_cfg;
    g_ctx.prov = &cb_provider_local;
    g_ctx.app = &APP;
    const char *tmp = getenv("TMPDIR");
    snprintf(g_db, sizeof g_db, "%s/cbase-oidc-%d.sqlite", tmp && *tmp ? tmp : "/tmp", (int)getpid());
    unlink(g_db);
    T_EQ_INT(cb_store_open_sqlite(g_db, &g_ctx.store), CB_OK);
    T_EQ_INT(cb_auth_new(&g_ctx, &g_ctx.auth), CB_OK);
    g_ctx.router = cb_router_new();
    T_EQ_INT(cb_auth_routes(&g_ctx), CB_OK);
    cb_limits_reset();
}

static void teardown(void) {
    cb_router_free(g_ctx.router);
    cb_auth_free(g_ctx.auth);
    cb_store_close(g_ctx.store);
    unlink(g_db);
    memset(&g_ctx, 0, sizeof g_ctx);
}

typedef struct {
    int     status;
    uint8_t body[4096];
    size_t  len;
} answer;
static answer g_a;

static int send_req(const char *path, const char *bearer, const uint8_t *body, size_t n) {
    cb_http_req r;
    memset(&r, 0, sizeof r);
    strcpy(r.method, "POST");
    r.path = path;
    r.query = "";
    r.minor = 1;
    char authz[CBW_ACCESS_MAX + 16];
    if (bearer) {
        snprintf(authz, sizeof authz, "Bearer %s", bearer);
        r.headers[r.n_headers++] = (cb_http_header){ "Authorization", authz };
    }
    r.body = body;
    r.body_len = n;
    cb_http_res res;
    cb_res_init(&res);
    cb_handle(&g_ctx, &r, &res);
    memset(&g_a, 0, sizeof g_a);
    g_a.status = res.status;
    g_a.len = res.body.len < sizeof g_a.body ? res.body.len : 0;
    if (g_a.len) memcpy(g_a.body, res.body.p, g_a.len);
    cb_res_free(&res);
    return g_a.status;
}

// A CBW_OIDC body, by hand from cbase/auth_wire.h.
static int oidc(const char *bearer, uint8_t idp, uint8_t flags, const char *token, const char *nonce, const char *code,
                const char *name) {
    static uint8_t b[CBW_ID_TOKEN_MAX + 2048];
    size_t at = 0, tn = token ? strlen(token) : 0, nn = strlen(nonce), cn = code ? strlen(code) : 0,
           mn = name ? strlen(name) : 0;
    b[at++] = CBW_V;
    b[at++] = CBW_OIDC;
    b[at++] = idp;
    b[at++] = flags;
    le_put_u16(b + at, (uint16_t)tn); at += 2; memcpy(b + at, token, tn); at += tn;
    b[at++] = (uint8_t)nn; memcpy(b + at, nonce, nn); at += nn;
    le_put_u16(b + at, (uint16_t)cn); at += 2; memcpy(b + at, code, cn); at += cn;
    b[at++] = (uint8_t)mn; memcpy(b + at, name, mn); at += mn;
    return send_req("/v1/auth/oidc", bearer, b, at);
}

// A password sign-up (or sign-in) body, by hand.
static int password_route(const char *path, uint8_t kind, const char *name, const char *pw) {
    uint8_t b[256];
    size_t at = 0, nn = strlen(name), pn = strlen(pw);
    b[at++] = CBW_V;
    b[at++] = kind;
    b[at++] = (uint8_t)nn; memcpy(b + at, name, nn); at += nn;
    b[at++] = (uint8_t)pn; memcpy(b + at, pw, pn); at += pn;
    return send_req(path, NULL, b, at);
}
static int signup(const char *name) { return password_route("/v1/auth/signup", CBW_SIGNUP, name, "correct-horse-1"); }

static bool refused(int why, int code) {
    return g_a.len == CBW_REFUSAL_BYTES && g_a.body[0] == CBW_REFUSAL_FORMAT && g_a.body[1] == why &&
           (int32_t)le_get_u32(g_a.body + 2) == code;
}

typedef struct {
    char access[CBW_ACCESS_MAX + 1], user_id[CBW_ID_MAX + 1], name[CBW_NAME_MAX + 1];
} session;
static bool read_session(session *s) {
    memset(s, 0, sizeof *s);
    const uint8_t *p = g_a.body;
    if (g_a.status != 200 || g_a.len < 4 || p[1] != CBW_SESSION) return false;
    size_t an = le_get_u16(p + 2), at = 4;
    memcpy(s->access, p + at, an);
    at += an;
    at += 1 + p[at] + 6;
    size_t un = p[at++];
    memcpy(s->user_id, p + at, un);
    at += un;
    size_t nn = p[at++];
    memcpy(s->name, p + at, nn);
    return at + nn == g_a.len;
}

static const char *tok_for(const char *sub) {
    fi_claims c = good(sub);
    c.iat = now_s();
    c.exp = now_s() + 600;
    g_now += 1000;   // each token its own text (iat moves), as each Apple sign-in is
    c.iat = now_s();
    return fi_mint(&g_f, 0, NULL, NULL, &c);
}

static int identity_of(const char *sub, cb_identity_row *r) {
    return CB_STORE_CALL(g_ctx.store, identity_get, CBW_IDP_APPLE, sub, r);
}

static void new_identity_asks_a_name_then_signs_in(void) {
    setup(true);
    char tok[16384];
    cb_strlcpy(tok, tok_for("apple.sub.1"), sizeof tok);
    // No account has it, and no name came: asked, and the token is not spent.
    T_EQ_INT(oidc(NULL, CBW_IDP_APPLE, 0, tok, NONCE, "code-apple.sub.1", NULL), 400);
    T_CHECK(refused(CBW_WHY_ACCOUNT, CBW_ACCOUNT_E_NEEDS_NAME));
    // A name another account has: refused, the token still not spent.
    T_EQ_INT(signup("TAKE"), 200);
    T_EQ_INT(oidc(NULL, CBW_IDP_APPLE, 0, tok, NONCE, "code-apple.sub.1", "take"), 409);
    T_CHECK(refused(CBW_WHY_ACCOUNT, CBW_ACCOUNT_E_EXISTS));
    T_EQ_INT(oidc(NULL, CBW_IDP_APPLE, 0, tok, NONCE, "code-apple.sub.1", "%BAD"), 400);
    T_CHECK(refused(CBW_WHY_ACCOUNT, CBW_ACCOUNT_E_BAD_NAME));
    // With a free name: the account, its identity, its grant.
    int calls = g_f.token_calls;
    T_EQ_INT(oidc(NULL, CBW_IDP_APPLE, 0, tok, NONCE, "code-apple.sub.1", "Ada"), 200);
    session s;
    T_CHECK(read_session(&s));
    T_EQ_STR(s.name, "ADA");   // the case step, as sign-up's
    T_EQ_INT(g_f.token_calls - calls, 1);
    cb_identity_row r;
    T_EQ_INT(identity_of("apple.sub.1", &r), CB_OK);
    T_EQ_STR(r.user_id, s.user_id);
    T_EQ_STR(r.client_id, IOS);
    T_EQ_STR(r.email, "x7k2@privaterelay.appleid.com");   // the relay address, kept as given
    T_CHECK(r.private_email && r.email_verified);
    char grant[CB_OIDC_REFRESH_MAX + 1];
    T_CHECK(r.grant[0] && strstr(r.grant, "rt-") == NULL);   // sealed
    T_CHECK(cb_oidc_open(cb_auth_oidc_of(g_ctx.auth), r.grant, grant, sizeof grant) > 0);
    T_CHECK(strncmp(grant, "rt-apple.sub.1-", 15) == 0);
    // The same token again: used.
    T_EQ_INT(oidc(NULL, CBW_IDP_APPLE, 0, tok, NONCE, "code-apple.sub.1", "Ada"), 400);
    T_CHECK(refused(CBW_WHY_ACCOUNT, CBW_ACCOUNT_E_BAD_IDENTITY));
    // A new sign-in: the same account, no second exchange (the grant is held), no name needed.
    calls = g_f.token_calls;
    T_EQ_INT(oidc(NULL, CBW_IDP_APPLE, 0, tok_for("apple.sub.1"), NONCE, "code-apple.sub.1", NULL), 200);
    session s2;
    T_CHECK(read_session(&s2));
    T_EQ_STR(s2.user_id, s.user_id);
    T_EQ_INT(g_f.token_calls - calls, 0);
    // The sign-in is the account's last; the provider's new email is the identity's.
    cb_user_row ur;
    T_EQ_INT(cb_auth_user_row(g_ctx.auth, s.user_id, &ur), CB_OK);
    T_EQ_INT(ur.last_sign_in_at, g_now);
    fi_claims moved = good("apple.sub.1");
    moved.email = "ada@example.com";
    moved.is_private_email = "false";
    g_now += 1000;
    moved.iat = now_s();
    T_EQ_INT(oidc(NULL, CBW_IDP_APPLE, 0, fi_mint(&g_f, 0, NULL, NULL, &moved), NONCE, NULL, NULL), 200);
    T_EQ_INT(identity_of("apple.sub.1", &r), CB_OK);
    T_EQ_STR(r.email, "ada@example.com");
    T_CHECK(!r.private_email);
    // The account has no password: a password sign-in by its name refuses, whatever is sent, and
    // counts nothing (no guess can be right), so it never locks.
    for (int i = 0; i < 8; i++) {
        cb_limits_reset();
        T_EQ_INT(password_route("/v1/auth/signin", CBW_SIGNIN, "ADA", "guess-guess-1"), 400);
    }
    T_CHECK(refused(CBW_WHY_ACCOUNT, CBW_ACCOUNT_E_BAD_LOGIN));
    T_EQ_INT(password_route("/v1/auth/signin", CBW_SIGNIN, "ADA", ""), 400);
    T_CHECK(refused(CBW_WHY_ACCOUNT, CBW_ACCOUNT_E_BAD_LOGIN));
    T_EQ_INT(password_route("/v1/auth/signin", CBW_SIGNIN, "ADA", "anything"), 400);
    T_CHECK(refused(CBW_WHY_ACCOUNT, CBW_ACCOUNT_E_BAD_LOGIN));
    // The session is an ordinary one: it signs out.
    T_EQ_INT(send_req("/v1/auth/signout", s2.access, (const uint8_t *)"\x01\x04\x00", 3), 204);
    teardown();
}

static void a_failed_exchange_does_not_fail_the_sign_in(void) {
    setup(true);
    T_EQ_INT(oidc(NULL, CBW_IDP_APPLE, 0, tok_for("apple.sub.2"), NONCE, "code-500", "Bea"), 200);
    cb_identity_row r;
    T_EQ_INT(identity_of("apple.sub.2", &r), CB_OK);
    T_EQ_STR(r.grant, "");
    // The next sign-in's good code takes the grant; one with no code leaves it be.
    T_EQ_INT(oidc(NULL, CBW_IDP_APPLE, 0, tok_for("apple.sub.2"), NONCE, NULL, NULL), 200);
    T_EQ_INT(identity_of("apple.sub.2", &r), CB_OK);
    T_EQ_STR(r.grant, "");
    T_EQ_INT(oidc(NULL, CBW_IDP_APPLE, 0, tok_for("apple.sub.2"), NONCE, "code-apple.sub.2", NULL), 200);
    T_EQ_INT(identity_of("apple.sub.2", &r), CB_OK);
    T_CHECK(r.grant[0] != 0);
    // A code for another subject is refused by the exchange, and the identity keeps what it had.
    T_EQ_INT(oidc(NULL, CBW_IDP_APPLE, 0, tok_for("apple.sub.3"), NONCE, "code-other-apple.sub.3", "Cy"), 200);
    T_EQ_INT(identity_of("apple.sub.3", &r), CB_OK);
    T_EQ_STR(r.grant, "");
    teardown();
    // Without the developer key there is no exchange at all; sign-in still works.
    setup(false);
    int calls = g_f.token_calls;
    T_EQ_INT(oidc(NULL, CBW_IDP_APPLE, 0, tok_for("apple.sub.4"), NONCE, "code-apple.sub.4", "Dee"), 200);
    T_EQ_INT(g_f.token_calls - calls, 0);
    teardown();
}

static void link_rules(void) {
    setup(true);
    // Two password accounts.
    T_EQ_INT(signup("EVE"), 200);
    session eve;
    T_CHECK(read_session(&eve));
    T_EQ_INT(signup("FAY"), 200);
    session fay;
    T_CHECK(read_session(&fay));
    // A link needs the bearer.
    T_EQ_INT(oidc(NULL, CBW_IDP_APPLE, CBW_OIDC_LINK, tok_for("apple.sub.5"), NONCE, NULL, NULL), 401);
    T_CHECK(refused(CBW_WHY_AUTH, 0));
    // Eve links it; it then signs in to Eve.
    T_EQ_INT(oidc(eve.access, CBW_IDP_APPLE, CBW_OIDC_LINK, tok_for("apple.sub.5"), NONCE, "code-apple.sub.5", NULL), 204);
    T_EQ_INT(oidc(NULL, CBW_IDP_APPLE, 0, tok_for("apple.sub.5"), NONCE, NULL, NULL), 200);
    session s;
    T_CHECK(read_session(&s));
    T_EQ_STR(s.user_id, eve.user_id);
    T_EQ_STR(s.name, "EVE");
    // Fay cannot take it; Eve linking it again is fine.
    T_EQ_INT(oidc(fay.access, CBW_IDP_APPLE, CBW_OIDC_LINK, tok_for("apple.sub.5"), NONCE, NULL, NULL), 409);
    T_CHECK(refused(CBW_WHY_ACCOUNT, CBW_ACCOUNT_E_LINKED));
    T_EQ_INT(oidc(eve.access, CBW_IDP_APPLE, CBW_OIDC_LINK, tok_for("apple.sub.5"), NONCE, NULL, NULL), 204);
    // A link's token is used once too.
    char tok[16384];
    cb_strlcpy(tok, tok_for("apple.sub.6"), sizeof tok);
    T_EQ_INT(oidc(fay.access, CBW_IDP_APPLE, CBW_OIDC_LINK, tok, NONCE, NULL, NULL), 204);
    T_EQ_INT(oidc(fay.access, CBW_IDP_APPLE, CBW_OIDC_LINK, tok, NONCE, NULL, NULL), 400);
    T_CHECK(refused(CBW_WHY_ACCOUNT, CBW_ACCOUNT_E_BAD_IDENTITY));
    // An email never links: an identity whose email matches nothing signs in to no one without a name.
    fi_claims c = good("apple.sub.7");
    c.email = "eve@accounts.example";
    c.email_verified = "true";
    c.is_private_email = "false";
    T_EQ_INT(oidc(NULL, CBW_IDP_APPLE, 0, fi_mint(&g_f, 0, NULL, NULL, &c), NONCE, NULL, NULL), 400);
    T_CHECK(refused(CBW_WHY_ACCOUNT, CBW_ACCOUNT_E_NEEDS_NAME));
    teardown();
}

// The races the store settles: an identity made between this request's read and its write.
static int g_hide_gets;
static const cb_store_ops *g_real;
static int hiding_get(cb_store *st, int idp, const char *sub, cb_identity_row *out) {
    if (g_hide_gets > 0) { g_hide_gets--; memset(out, 0, sizeof *out); return CB_E_NOT_FOUND; }
    return g_real->identity_get(st, idp, sub, out);
}
static void races(void) {
    setup(true);
    T_EQ_INT(signup("KAY"), 200);
    session kay;
    T_CHECK(read_session(&kay));
    T_EQ_INT(signup("LEO"), 200);
    session leo;
    T_CHECK(read_session(&leo));
    T_EQ_INT(oidc(kay.access, CBW_IDP_APPLE, CBW_OIDC_LINK, tok_for("apple.sub.r"), NONCE, NULL, NULL), 204);
    cb_store_ops ops = *g_ctx.store->ops;
    g_real = g_ctx.store->ops;
    ops.identity_get = hiding_get;
    g_ctx.store->ops = &ops;
    // Leo's link reads no identity, then its write finds Kay's: refused as another account's.
    g_hide_gets = 1;
    T_EQ_INT(oidc(leo.access, CBW_IDP_APPLE, CBW_OIDC_LINK, tok_for("apple.sub.r"), NONCE, NULL, NULL), 409);
    T_CHECK(refused(CBW_WHY_ACCOUNT, CBW_ACCOUNT_E_LINKED));
    // Kay's own, the same way: it is hers, so it is done.
    g_hide_gets = 1;
    T_EQ_INT(oidc(kay.access, CBW_IDP_APPLE, CBW_OIDC_LINK, tok_for("apple.sub.r"), NONCE, NULL, NULL), 204);
    // A sign-in that read no identity and made one that is there: that account's session.
    g_hide_gets = 1;
    T_EQ_INT(oidc(NULL, CBW_IDP_APPLE, 0, tok_for("apple.sub.r"), NONCE, NULL, "Mae"), 200);
    session s;
    T_CHECK(read_session(&s));
    T_EQ_STR(s.user_id, kay.user_id);
    g_ctx.store->ops = g_real;
    // The API's own: an empty name is no name.
    cb_oidc_ask q = { .idp = CBW_IDP_APPLE, .nonce = NONCE, .nonce_len = strlen(NONCE), .name = "", .email = "x@accounts.example" };
    q.id_token = tok_for("apple.sub.q");
    q.id_token_len = strlen(q.id_token);
    cb_session out;
    T_EQ_INT(cb_auth_oidc(g_ctx.auth, &q, &out), CB_AUTH_E_NEEDS_NAME);
    teardown();
}

static void delete_revokes_at_apple(void) {
    setup(true);
    T_EQ_INT(oidc(NULL, CBW_IDP_APPLE, 0, tok_for("apple.sub.8"), NONCE, "code-apple.sub.8", "Gus"), 200);
    session s;
    T_CHECK(read_session(&s));
    cb_identity_row r;
    T_EQ_INT(identity_of("apple.sub.8", &r), CB_OK);
    char grant[CB_OIDC_REFRESH_MAX + 1];
    T_CHECK(cb_oidc_open(cb_auth_oidc_of(g_ctx.auth), r.grant, grant, sizeof grant) > 0);
    int n0 = g_f.n_revoked;
    int64_t fails0 = cb_auth_revoke_failures();
    T_EQ_INT(cb_auth_delete_user(g_ctx.auth, s.user_id), CB_OK);
    T_EQ_INT(g_f.n_revoked - n0, 1);
    T_EQ_STR(g_f.revoked[n0], grant);
    T_EQ_STR(g_f.revoked_client[n0], IOS);
    T_EQ_INT(cb_auth_revoke_failures() - fails0, 0);
    // The identity went with the account: Apple's sign-in now asks for a name again.
    T_EQ_INT(identity_of("apple.sub.8", &r), CB_E_NOT_FOUND);
    T_EQ_INT(oidc(NULL, CBW_IDP_APPLE, 0, tok_for("apple.sub.8"), NONCE, NULL, NULL), 400);
    T_CHECK(refused(CBW_WHY_ACCOUNT, CBW_ACCOUNT_E_NEEDS_NAME));
    // Apple down at the delete: tried twice, counted, and the account is deleted all the same.
    T_EQ_INT(oidc(NULL, CBW_IDP_APPLE, 0, tok_for("apple.sub.9"), NONCE, "code-apple.sub.9", "Hal"), 200);
    T_CHECK(read_session(&s));
    g_f.revoke_status = 503;
    int calls = g_f.revoke_calls;
    T_EQ_INT(cb_auth_delete_user(g_ctx.auth, s.user_id), CB_OK);
    T_EQ_INT(g_f.revoke_calls - calls, 2);
    T_EQ_INT(cb_auth_revoke_failures() - fails0, 1);
    // Refused (a 400) is not tried again.
    g_f.revoke_status = 400;
    T_EQ_INT(oidc(NULL, CBW_IDP_APPLE, 0, tok_for("apple.sub.10"), NONCE, "code-apple.sub.10", "Ivy"), 200);
    T_CHECK(read_session(&s));
    calls = g_f.revoke_calls;
    T_EQ_INT(cb_auth_delete_user(g_ctx.auth, s.user_id), CB_OK);
    T_EQ_INT(g_f.revoke_calls - calls, 1);
    T_EQ_INT(cb_auth_revoke_failures() - fails0, 2);
    g_f.revoke_status = 0;
    // A password account with no identity revokes nothing.
    T_EQ_INT(signup("JON"), 200);
    T_CHECK(read_session(&s));
    calls = g_f.revoke_calls;
    T_EQ_INT(cb_auth_delete_user(g_ctx.auth, s.user_id), CB_OK);
    T_EQ_INT(g_f.revoke_calls - calls, 0);
    teardown();
}

static void route_refusals(void) {
    setup(true);
    // Not a provider, or none configured.
    T_EQ_INT(oidc(NULL, 9, 0, tok_for("s"), NONCE, NULL, NULL), 400);
    T_CHECK(refused(CBW_WHY_ACCOUNT, CBW_ACCOUNT_E_NO_PROVIDER));
    // A forged token.
    T_EQ_INT(oidc(NULL, CBW_IDP_APPLE, 0, "aaa.bbb.ccc", NONCE, NULL, NULL), 400);
    T_CHECK(refused(CBW_WHY_ACCOUNT, CBW_ACCOUNT_E_BAD_IDENTITY));
    // Malformed bodies: a flag this server does not know, a NUL in a field, a byte over.
    T_EQ_INT(oidc(NULL, CBW_IDP_APPLE, 0x2, tok_for("s"), NONCE, NULL, NULL), 400);
    T_CHECK(refused(CBW_WHY_MALFORMED, 0));
    T_EQ_INT(send_req("/v1/auth/oidc", NULL, (const uint8_t *)"\x01\x07\x01\x00\x01\x00x\x01\x00\x00\x00\x00", 12), 400);
    T_CHECK(refused(CBW_WHY_MALFORMED, 0));   // the nonce holds a NUL
    T_EQ_INT(send_req("/v1/auth/oidc", NULL, (const uint8_t *)"\x01\x07\x01\x00\x00\x00\x00\x00\x00\x00\x00", 11), 400);
    T_CHECK(refused(CBW_WHY_MALFORMED, 0));   // a byte over
    T_EQ_INT(send_req("/v1/auth/oidc", NULL, (const uint8_t *)"\x01\x07", 2), 400);
    T_CHECK(refused(CBW_WHY_MALFORMED, 0));
    // Apple's keys unreachable: the moment's (503 busy), sent again.
    teardown();
    setup(true);
    g_f.keys_status = 503;
    T_EQ_INT(oidc(NULL, CBW_IDP_APPLE, 0, tok_for("s"), NONCE, NULL, NULL), 503);
    T_CHECK(refused(CBW_WHY_BUSY, 0));
    g_f.keys_status = 0;
    teardown();
    // No client configured: the provider is off.
    setup(true);
    cb_auth_free(g_ctx.auth);
    g_cfg.apple_client_ids[0] = 0;
    g_cfg.apple_team_id[0] = g_cfg.apple_key_id[0] = g_cfg.apple_private_key[0] = 0;
    T_EQ_INT(cb_auth_new(&g_ctx, &g_ctx.auth), CB_OK);
    T_EQ_INT(oidc(NULL, CBW_IDP_APPLE, 0, tok_for("s"), NONCE, NULL, NULL), 400);
    T_CHECK(refused(CBW_WHY_ACCOUNT, CBW_ACCOUNT_E_NO_PROVIDER));
    teardown();
}

int main(void) {
    cb_log_init(CB_LOG_ERROR, 0);
    g_now = cb_now_ms();
    cb_auth_now = test_now;
    if (fi_start(&g_f) != 0) {
        fprintf(stderr, "unit_oidc: the fake provider did not start\n");
        return 1;
    }
    g_f.clock_ms = &g_now;   // the fake's clock is the server's
    T_EQ_INT(cb_https_set_ca_file(g_f.srv.ca_file), CB_OK);
    T_RUN(a_good_token);
    T_RUN(every_refusal);
    T_RUN(keys_rotate_and_refetch);
    T_RUN(key_sets);
    T_RUN(configuration);
    T_RUN(client_secret);
    T_RUN(exchange_and_revoke);
    T_RUN(seal);
    T_RUN(new_identity_asks_a_name_then_signs_in);
    T_RUN(a_failed_exchange_does_not_fail_the_sign_in);
    T_RUN(link_rules);
    T_RUN(races);
    T_RUN(delete_revokes_at_apple);
    T_RUN(route_refusals);
    fi_stop(&g_f);
    return T_DONE();
}
