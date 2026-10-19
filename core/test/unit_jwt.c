// unit_jwt.c - access tokens (auth/jwt.h): the round trip, the skew window, and
// every refusal of the binary layout (tampered byte, wrong key, expired,
// truncated, wrong version, wrong lengths, a second spelling).
#include "t.h"

#include "auth/jwt.h"
#include "../../../shared/c/b64/b64.h"
#include "util/err.h"

#include <openssl/evp.h>
#include <openssl/hmac.h>

static const char SECRET[] = "unit-test-secret-of-some-length";
static const char UID[] = "0f8fad5b-d9cb-469f-a165-70867728950e";
static const char SID[] = "7c9e6679-7425-40de-944b-e07fc1f90ae7";
#define NOW 1800000000LL

static cb_jwt_claims sample(void) {
    cb_jwt_claims c;
    memset(&c, 0, sizeof c);
    memcpy(c.sub, UID, sizeof c.sub);
    memcpy(c.session_id, SID, sizeof c.session_id);
    strcpy(c.email, "abcdef0123456789@foolish.cards");
    strcpy(c.username, "ANNA \"Q\" \xc3\x85");
    c.iat = NOW;
    c.exp = NOW + 3600;
    return c;
}

// A token over raw payload bytes, MAC'd independently of jwt.c (so a refusal is
// the verifier's, not the minter's).
static void forge(const char *secret, const uint8_t *payload, size_t n, char *out, size_t cap) {
    uint8_t raw[1024];
    memcpy(raw, payload, n);
    unsigned int ml = 0;
    HMAC(EVP_sha256(), secret, (int)strlen(secret), raw, n, raw + n, &ml);
    b64url_encode(raw, n + ml, out, cap);
}

// The payload of a token minted by jwt.c, MAC cut off.
static size_t payload_of(const char *tok, uint8_t *raw, size_t cap) {
    int k = b64_decode(tok, strlen(tok), raw, cap);
    return k > 32 ? (size_t)k - 32 : 0;
}

static void round_trip(void) {
    cb_jwt_claims c = sample(), got;
    char tok[CB_JWT_MAX];
    int n = cb_jwt_mint(SECRET, &c, tok, sizeof tok);
    T_CHECK(n > 0);
    T_EQ_INT(strlen(tok), n);
    T_CHECK(n < 200);   // no JSON, no header: the layout is small
    T_EQ_INT(cb_jwt_verify(SECRET, tok, NOW + 10, &got), CB_OK);
    T_EQ_STR(got.sub, UID);
    T_EQ_STR(got.session_id, SID);
    T_EQ_STR(got.email, c.email);
    T_EQ_STR(got.username, c.username);
    T_EQ_INT(got.iat, NOW);
    T_EQ_INT(got.exp, NOW + 3600);
    T_EQ_INT(cb_jwt_mint(SECRET, &c, tok, 16), CB_E_FULL);
    T_EQ_INT(cb_jwt_mint("", &c, tok, sizeof tok), CB_E_INVALID);
    cb_jwt_claims bad = c;
    strcpy(bad.sub, "u-anna");
    T_EQ_INT(cb_jwt_mint(SECRET, &bad, tok, sizeof tok), CB_E_INVALID);
    bad = c;
    strcpy(bad.session_id, "0F8FAD5B-D9CB-469F-A165-70867728950E");   // not canonical lowercase
    T_EQ_INT(cb_jwt_mint(SECRET, &bad, tok, sizeof tok), CB_E_INVALID);
    // Empty strings round trip.
    bad = c;
    bad.email[0] = 0;
    bad.username[0] = 0;
    T_EQ_INT(cb_jwt_mint(SECRET, &bad, tok, sizeof tok) > 0, 1);
    T_EQ_INT(cb_jwt_verify(SECRET, tok, NOW, &got), CB_OK);
    T_EQ_STR(got.email, "");
}

// The per-thread verified-token cache answers a repeat exactly as a fresh
// verification would: same claims, the clock still checked, another secret
// still refused (a hit must not skip any of that).
static void repeat_verify_matches_fresh(void) {
    cb_jwt_claims c = sample(), a, b;
    char tok[CB_JWT_MAX];
    cb_jwt_mint(SECRET, &c, tok, sizeof tok);
    T_EQ_INT(cb_jwt_verify(SECRET, tok, NOW, &a), CB_OK);
    T_EQ_INT(cb_jwt_verify(SECRET, tok, NOW, &b), CB_OK);
    T_CHECK(memcmp(&a, &b, sizeof a) == 0);
    char other[64];
    snprintf(other, sizeof other, "%s", SECRET);
    other[strlen(other) - 1] ^= 1;                                  // same length, one bit off
    T_EQ_INT(cb_jwt_verify(other, tok, NOW, &b), CB_E_AUTH);
    T_EQ_INT(cb_jwt_verify(SECRET, tok, NOW + 3600 + 31, &b), CB_E_AUTH);
    T_EQ_INT(cb_jwt_verify(SECRET, tok, NOW, &b), CB_OK);
}

static void expiry_and_skew(void) {
    cb_jwt_claims c = sample(), got;
    char tok[CB_JWT_MAX];
    cb_jwt_mint(SECRET, &c, tok, sizeof tok);
    T_EQ_INT(cb_jwt_verify(SECRET, tok, NOW + 3600 + 30, &got), CB_OK);     // inside the skew
    T_EQ_INT(cb_jwt_verify(SECRET, tok, NOW + 3600 + 31, &got), CB_E_AUTH); // expired
    // The largest exp is far in the future, not a signed overflow (exp + the
    // skew was one under UBSan: core/fuzz/fz_jwt.c found it), the smallest long past.
    c.exp = INT64_MAX;
    cb_jwt_mint(SECRET, &c, tok, sizeof tok);
    T_EQ_INT(cb_jwt_verify(SECRET, tok, NOW, &got), CB_OK);
    c.exp = INT64_MIN;
    cb_jwt_mint(SECRET, &c, tok, sizeof tok);
    T_EQ_INT(cb_jwt_verify(SECRET, tok, NOW, &got), CB_E_AUTH);
}

static void refusals(void) {
    cb_jwt_claims c = sample(), got;
    char tok[CB_JWT_MAX], t2[CB_JWT_MAX + 8];
    cb_jwt_mint(SECRET, &c, tok, sizeof tok);
    T_EQ_INT(cb_jwt_verify(SECRET, tok, NOW, &got), CB_OK);
    T_EQ_INT(cb_jwt_verify("another secret", tok, NOW, &got), CB_E_AUTH);
    T_EQ_INT(cb_jwt_verify("", tok, NOW, &got), CB_E_AUTH);

    // Any single flipped bit anywhere in the token (payload or MAC) is refused.
    uint8_t raw[1024];
    int k = b64_decode(tok, strlen(tok), raw, sizeof raw);
    for (int i = 0; i < k; i++) {
        raw[i] ^= 0x40;
        b64url_encode(raw, (size_t)k, t2, sizeof t2);
        T_EQ_INT(cb_jwt_verify(SECRET, t2, NOW, &got), CB_E_AUTH);
        raw[i] ^= 0x40;
    }
    // Truncated at every length, and extended by a byte.
    for (size_t n = 0; n < strlen(tok); n++) {
        snprintf(t2, sizeof t2, "%.*s", (int)n, tok);
        T_EQ_INT(cb_jwt_verify(SECRET, t2, NOW, &got), CB_E_AUTH);
    }
    snprintf(t2, sizeof t2, "%sA", tok);
    T_EQ_INT(cb_jwt_verify(SECRET, t2, NOW, &got), CB_E_AUTH);
    T_EQ_INT(cb_jwt_verify(SECRET, NULL, NOW, &got), CB_E_AUTH);
    T_EQ_INT(cb_jwt_verify(SECRET, "", NOW, &got), CB_E_AUTH);

    // Correctly MAC'd payloads that are wrong: version, a length that leaves
    // bytes over or runs past the end, a missing string.
    uint8_t pl[512];
    size_t pn = payload_of(tok, pl, sizeof pl);
    T_CHECK(pn > 50);
    forge(SECRET, pl, pn, t2, sizeof t2);
    T_EQ_INT(cb_jwt_verify(SECRET, t2, NOW, &got), CB_OK);                   // the forger agrees with the minter
    pl[0] = 2;
    forge(SECRET, pl, pn, t2, sizeof t2);
    T_EQ_INT(cb_jwt_verify(SECRET, t2, NOW, &got), CB_E_AUTH);               // version
    pl[0] = 1;
    forge(SECRET, pl, pn + 1, t2, sizeof t2);                                // one byte too many (the zero left in pl)
    T_EQ_INT(cb_jwt_verify(SECRET, t2, NOW, &got), CB_E_AUTH);
    forge(SECRET, pl, pn - 1, t2, sizeof t2);                                // one byte short
    T_EQ_INT(cb_jwt_verify(SECRET, t2, NOW, &got), CB_E_AUTH);
    pl[1 + 16 + 16 + 8 + 8] = 255;                                           // email longer than the token
    forge(SECRET, pl, pn, t2, sizeof t2);
    T_EQ_INT(cb_jwt_verify(SECRET, t2, NOW, &got), CB_E_AUTH);
    forge(SECRET, pl, 20, t2, sizeof t2);                                    // shorter than the fixed part
    T_EQ_INT(cb_jwt_verify(SECRET, t2, NOW, &got), CB_E_AUTH);

    // A second spelling of the same bytes: padding, the other alphabet, a
    // non-zero trailing bit in the last character.
    snprintf(t2, sizeof t2, "%s=", tok);
    T_EQ_INT(cb_jwt_verify(SECRET, t2, NOW, &got), CB_E_AUTH);
    strcpy(t2, tok);
    for (char *p = t2; *p; p++) { if (*p == '-') *p = '+'; else if (*p == '_') *p = '/'; }
    if (strcmp(t2, tok) != 0) T_EQ_INT(cb_jwt_verify(SECRET, t2, NOW, &got), CB_E_AUTH);
    strcpy(t2, tok);
    char *last = t2 + strlen(t2) - 1;
    *last = (char)(*last ^ 1);
    T_EQ_INT(cb_jwt_verify(SECRET, t2, NOW, &got), CB_E_AUTH);

    // The old format (three dotted parts) is not a token.
    T_EQ_INT(cb_jwt_verify(SECRET, "eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiJ4In0.AAAA", NOW, &got), CB_E_AUTH);
}

int main(void) {
    T_RUN(round_trip);
    T_RUN(repeat_verify_matches_fresh);
    T_RUN(expiry_and_skew);
    T_RUN(refusals);
    return T_DONE();
}
