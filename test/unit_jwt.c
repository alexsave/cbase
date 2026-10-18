// unit_jwt.c - HS256 access tokens (auth/jwt.h): the round trip, a token
// another implementation signed, the skew window, and every refusal.
#include "t.h"

#include "auth/jwt.h"
#include "../../shared/c/b64/b64.h"
#include "util/err.h"
#include "util/json.h"

#include <openssl/evp.h>
#include <openssl/hmac.h>

static const char SECRET[] = "unit-test-secret-of-some-length";
static const char UID[] = "0f8fad5b-d9cb-469f-a165-70867728950e";
static const char SID[] = "7c9e6679-7425-40de-944b-e07fc1f90ae7";
#define NOW 1800000000LL

// A token over arbitrary header and claims JSON, signed with `secret`
// independently of jwt.c (so a refusal is the verifier's, not the minter's).
static void forge(const char *secret, const char *head, const char *body, char *out, size_t cap) {
    int n = b64url_encode((const uint8_t *)head, strlen(head), out, cap);
    out[n++] = '.';
    n += b64url_encode((const uint8_t *)body, strlen(body), out + n, cap - (size_t)n);
    unsigned char mac[32];
    unsigned int ml = 0;
    HMAC(EVP_sha256(), secret, (int)strlen(secret), (const unsigned char *)out, (size_t)n, mac, &ml);
    out[n++] = '.';
    b64url_encode(mac, ml, out + n, cap - (size_t)n);
}

static const char HS256[] = "{\"alg\":\"HS256\",\"typ\":\"JWT\"}";

static void claims_body(char *out, size_t cap, const char *aud, const char *role, const char *sub, long long exp,
                        const char *extra) {
    snprintf(out, cap,
             "{\"aud\":\"%s\",\"role\":\"%s\",\"sub\":\"%s\",\"session_id\":\"%s\",\"exp\":%lld,\"iat\":%lld,"
             "\"email\":\"e@x.y\",\"user_metadata\":{\"username\":\"ANNA\"}%s}",
             aud, role, sub, SID, exp, NOW, extra);
}

static cb_jwt_claims sample(void) {
    cb_jwt_claims c;
    memset(&c, 0, sizeof c);
    memcpy(c.sub, UID, sizeof c.sub);
    memcpy(c.session_id, SID, sizeof c.session_id);
    strcpy(c.email, "abcdef0123456789@foolish.cards");
    strcpy(c.username, "ANNA \"Q\"");
    c.iat = NOW;
    c.exp = NOW + 3600;
    return c;
}

static void round_trip(void) {
    cb_jwt_claims c = sample(), got;
    char tok[CB_JWT_MAX];
    int n = cb_jwt_mint(SECRET, &c, "https://api.foolish.cards/auth/v1", tok, sizeof tok);
    T_CHECK(n > 0);
    T_EQ_INT(strlen(tok), n);
    T_EQ_INT(cb_jwt_verify(SECRET, tok, NOW + 10, &got), CB_OK);
    T_EQ_STR(got.sub, UID);
    T_EQ_STR(got.session_id, SID);
    T_EQ_STR(got.email, c.email);
    T_EQ_STR(got.username, c.username);
    T_EQ_INT(got.iat, NOW);
    T_EQ_INT(got.exp, NOW + 3600);

    // The claims GoTrue's tokens carry, which the SDKs decode.
    const char *d1 = strchr(tok, '.'), *d2 = strrchr(tok, '.');
    char body[1024];
    int bn = b64_decode(d1 + 1, (size_t)(d2 - d1 - 1), (uint8_t *)body, sizeof body - 1);
    T_CHECK(bn > 0);
    body[bn > 0 ? bn : 0] = 0;
    char v[128];
    T_CHECK(cb_json_get_str(body, (size_t)bn, "aud", v, sizeof v) && strcmp(v, "authenticated") == 0);
    T_CHECK(cb_json_get_str(body, (size_t)bn, "role", v, sizeof v) && strcmp(v, "authenticated") == 0);
    T_CHECK(cb_json_get_str(body, (size_t)bn, "iss", v, sizeof v) && strcmp(v, "https://api.foolish.cards/auth/v1") == 0);
    T_CHECK(cb_json_get_str(body, (size_t)bn, "app_metadata.provider", v, sizeof v) && strcmp(v, "email") == 0);
    T_CHECK(cb_json_get_str(body, (size_t)bn, "app_metadata.providers.0", v, sizeof v) && strcmp(v, "email") == 0);
    bool anon = true;
    T_CHECK(cb_json_get_bool(body, (size_t)bn, "is_anonymous", &anon) && !anon);
    char head[128];
    int hn = b64_decode(tok, (size_t)(d1 - tok), (uint8_t *)head, sizeof head - 1);
    T_EQ_MEM(head, hn, HS256, strlen(HS256));

    // Too small a buffer.
    T_EQ_INT(cb_jwt_mint(SECRET, &c, NULL, tok, 64), CB_E_FULL);
}

// Signed by Node's crypto (HMAC-SHA256, base64url), with a kid, a UTF-8
// username and a claim this verifier does not know.
static void a_token_node_signed(void) {
    const char *tok =
        "eyJ0eXAiOiJKV1QiLCJhbGciOiJIUzI1NiIsImtpZCI6IngifQ."
        "eyJzdWIiOiIwZjhmYWQ1Yi1kOWNiLTQ2OWYtYTE2NS03MDg2NzcyODk1MGUiLCJhdWQiOiJhdXRoZW50aWNhdGVkIiwicm9sZSI6ImF1dGhlbnRp"
        "Y2F0ZWQiLCJleHAiOjQxMDI0NDQ4MDAsImlhdCI6MTcwMDAwMDAwMCwiZW1haWwiOiJhQGZvb2xpc2guY2FyZHMiLCJzZXNzaW9uX2lkIjoiN2M5"
        "ZTY2NzktNzQyNS00MGRlLTk0NGItZTA3ZmMxZjkwYWU3IiwidXNlcl9tZXRhZGF0YSI6eyJ1c2VybmFtZSI6IsOFU0EifSwiZXh0cmEiOlsxLHsi"
        "eCI6bnVsbH1dfQ."
        "A_PsZ_G0ZvTCl_SEPfIegSCLMSu8M9X7U1f6hzSUw98";
    cb_jwt_claims got;
    T_EQ_INT(cb_jwt_verify("node-secret", tok, NOW, &got), CB_OK);
    T_EQ_STR(got.sub, UID);
    T_EQ_STR(got.username, "\xc3\x85SA");
    T_EQ_STR(got.email, "a@foolish.cards");
    T_EQ_INT(got.exp, 4102444800LL);
    T_EQ_INT(cb_jwt_verify("node-secreT", tok, NOW, &got), CB_E_AUTH);
}

static void expiry_and_skew(void) {
    cb_jwt_claims c = sample(), got;
    char tok[CB_JWT_MAX];
    cb_jwt_mint(SECRET, &c, NULL, tok, sizeof tok);
    T_EQ_INT(cb_jwt_verify(SECRET, tok, NOW + 3600 + 30, &got), CB_OK);     // inside the skew
    T_EQ_INT(cb_jwt_verify(SECRET, tok, NOW + 3600 + 31, &got), CB_E_AUTH); // expired

    char body[512];
    claims_body(body, sizeof body, "authenticated", "authenticated", UID, NOW + 100, ",\"nbf\":1800000031");
    forge(SECRET, HS256, body, tok, sizeof tok);
    T_EQ_INT(cb_jwt_verify(SECRET, tok, NOW, &got), CB_E_AUTH);              // not yet valid
    T_EQ_INT(cb_jwt_verify(SECRET, tok, NOW + 1, &got), CB_OK);              // within 30 s of nbf
    claims_body(body, sizeof body, "authenticated", "authenticated", UID, NOW + 100, ",\"nbf\":\"soon\"");
    forge(SECRET, HS256, body, tok, sizeof tok);
    T_EQ_INT(cb_jwt_verify(SECRET, tok, NOW, &got), CB_E_AUTH);              // nbf not a number
    // The largest exp a signed token can say is far in the future, not a
    // signed overflow (exp + the skew was, under UBSan: fuzz/fz_jwt.c found it),
    claims_body(body, sizeof body, "authenticated", "authenticated", UID, INT64_MAX, "");
    forge(SECRET, HS256, body, tok, sizeof tok);
    T_EQ_INT(cb_jwt_verify(SECRET, tok, NOW, &got), CB_OK);
    // and the smallest one is long past.
    claims_body(body, sizeof body, "authenticated", "authenticated", UID, INT64_MIN, "");
    forge(SECRET, HS256, body, tok, sizeof tok);
    T_EQ_INT(cb_jwt_verify(SECRET, tok, NOW, &got), CB_E_AUTH);
    // No exp at all.
    snprintf(body, sizeof body, "{\"aud\":\"authenticated\",\"role\":\"authenticated\",\"sub\":\"%s\",\"session_id\":\"%s\"}", UID, SID);
    forge(SECRET, HS256, body, tok, sizeof tok);
    T_EQ_INT(cb_jwt_verify(SECRET, tok, NOW, &got), CB_E_AUTH);
}

static void refusals(void) {
    cb_jwt_claims got;
    char tok[CB_JWT_MAX], body[512];
    claims_body(body, sizeof body, "authenticated", "authenticated", UID, NOW + 100, "");
    // The baseline the forgeries below differ from verifies.
    forge(SECRET, HS256, body, tok, sizeof tok);
    T_EQ_INT(cb_jwt_verify(SECRET, tok, NOW, &got), CB_OK);
    T_EQ_INT(cb_jwt_verify("another secret", tok, NOW, &got), CB_E_AUTH);

    // Header algorithms other than HS256, even correctly HMAC'd.
    forge(SECRET, "{\"alg\":\"none\",\"typ\":\"JWT\"}", body, tok, sizeof tok);
    T_EQ_INT(cb_jwt_verify(SECRET, tok, NOW, &got), CB_E_AUTH);
    forge(SECRET, "{\"alg\":\"HS512\"}", body, tok, sizeof tok);
    T_EQ_INT(cb_jwt_verify(SECRET, tok, NOW, &got), CB_E_AUTH);
    forge(SECRET, "{\"typ\":\"JWT\"}", body, tok, sizeof tok);
    T_EQ_INT(cb_jwt_verify(SECRET, tok, NOW, &got), CB_E_AUTH);
    forge(SECRET, "[\"HS256\"]", body, tok, sizeof tok);
    T_EQ_INT(cb_jwt_verify(SECRET, tok, NOW, &got), CB_E_AUTH);
    // alg none with the signature left off, the classic.
    forge(SECRET, "{\"alg\":\"none\"}", body, tok, sizeof tok);
    *(strrchr(tok, '.') + 1) = 0;
    T_EQ_INT(cb_jwt_verify(SECRET, tok, NOW, &got), CB_E_AUTH);

    // Claims: a service or anon role, another audience, a sub that is not a UUID.
    claims_body(body, sizeof body, "authenticated", "service_role", UID, NOW + 100, "");
    forge(SECRET, HS256, body, tok, sizeof tok);
    T_EQ_INT(cb_jwt_verify(SECRET, tok, NOW, &got), CB_E_AUTH);
    claims_body(body, sizeof body, "other", "authenticated", UID, NOW + 100, "");
    forge(SECRET, HS256, body, tok, sizeof tok);
    T_EQ_INT(cb_jwt_verify(SECRET, tok, NOW, &got), CB_E_AUTH);
    claims_body(body, sizeof body, "authenticated", "authenticated", "u-anna", NOW + 100, "");
    forge(SECRET, HS256, body, tok, sizeof tok);
    T_EQ_INT(cb_jwt_verify(SECRET, tok, NOW, &got), CB_E_AUTH);
    // Not JSON, though signed.
    forge(SECRET, HS256, "{\"aud\":\"authenticated\"", tok, sizeof tok);
    T_EQ_INT(cb_jwt_verify(SECRET, tok, NOW, &got), CB_E_AUTH);

    // Malformed shapes around a good token.
    claims_body(body, sizeof body, "authenticated", "authenticated", UID, NOW + 100, "");
    forge(SECRET, HS256, body, tok, sizeof tok);
    char t2[CB_JWT_MAX + 8];   // room for the suffixes below
    T_EQ_INT(cb_jwt_verify(SECRET, "", NOW, &got), CB_E_AUTH);
    T_EQ_INT(cb_jwt_verify(SECRET, "a.b", NOW, &got), CB_E_AUTH);
    T_EQ_INT(cb_jwt_verify(SECRET, NULL, NOW, &got), CB_E_AUTH);
    snprintf(t2, sizeof t2, "%s.x", tok);                                  // a fourth part
    T_EQ_INT(cb_jwt_verify(SECRET, t2, NOW, &got), CB_E_AUTH);
    snprintf(t2, sizeof t2, "%s=", tok);                                   // padding
    T_EQ_INT(cb_jwt_verify(SECRET, t2, NOW, &got), CB_E_AUTH);
    strcpy(t2, tok);                                                       // a flipped payload byte
    char *p = strchr(t2, '.') + 5;
    *p = *p == 'A' ? 'B' : 'A';
    T_EQ_INT(cb_jwt_verify(SECRET, t2, NOW, &got), CB_E_AUTH);
    strcpy(t2, tok);                                                       // the other alphabet in the signature
    char *s = strrchr(t2, '.') + 1;
    for (; *s; s++) { if (*s == '-') *s = '+'; else if (*s == '_') *s = '/'; }
    if (strcmp(t2, tok) != 0) T_EQ_INT(cb_jwt_verify(SECRET, t2, NOW, &got), CB_E_AUTH);
    strcpy(t2, tok);                                                       // the last signature character respelled
    s = t2 + strlen(t2) - 1;
    *s = (char)(*s ^ 1);
    T_EQ_INT(cb_jwt_verify(SECRET, t2, NOW, &got), CB_E_AUTH);
    T_EQ_INT(cb_jwt_verify("", tok, NOW, &got), CB_E_AUTH);
}

int main(void) {
    T_RUN(round_trip);
    T_RUN(a_token_node_signed);
    T_RUN(expiry_and_skew);
    T_RUN(refusals);
    return T_DONE();
}
