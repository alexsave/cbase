// fz_jwt.c - access token verification (auth/jwt.h cb_jwt_verify), the one
// check between a bearer header and a user id.
//
// Two shapes of input, by the first byte:
//   even  the token text as is: what an attacker can send. Nothing past the
//         signature check should matter, and nothing before it may crash.
//   odd   a header and a claims JSON (split at the first 0xff), signed here
//         with the server's secret: the token a server minted, or one a
//         leaked secret could mint. This is the only way to reach the header
//         and claims decoders (json_surface.c) behind the HMAC.
// A token that verifies must mint back to one that verifies to the same claims.
#include "fz.h"

#include "auth/jwt.h"
#include "../../../shared/c/b64/b64.h"

#include <openssl/evp.h>
#include <openssl/hmac.h>

#define SECRET "fuzz-secret-fuzz-secret-fuzz-secret!"
#define NOW_S  1790000000

static size_t part(const uint8_t *p, size_t n, char *out, size_t cap) {
    int k = b64url_encode(p, n, out, cap);
    return k < 0 ? 0 : (size_t)k;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 1 || size > 6000) return 0;
    char *tok;
    if (!(data[0] & 1)) {
        tok = fz_cstr(data + 1, size - 1);
    } else {
        const uint8_t *h = data + 1, *sep = memchr(h, 0xff, size - 1);
        size_t hn = sep ? (size_t)(sep - h) : 0;
        const uint8_t *c = sep ? sep + 1 : h;
        size_t cn = (size_t)(data + size - c);
        size_t cap = 2 * size + 128;
        tok = malloc(cap);
        if (!tok) abort();
        size_t at = part(h, hn, tok, cap);
        tok[at++] = '.';
        at += part(c, cn, tok + at, cap - at);
        unsigned char mac[EVP_MAX_MD_SIZE];
        unsigned int ml = 0;
        HMAC(EVP_sha256(), SECRET, (int)strlen(SECRET), (const unsigned char *)tok, at, mac, &ml);
        tok[at++] = '.';
        at += part(mac, ml, tok + at, cap - at);
        tok[at] = 0;
    }
    cb_jwt_claims cl;
    memset(&cl, 0, sizeof cl);
    if (cb_jwt_verify(SECRET, tok, NOW_S, &cl) == 0) {
        FZ_CHECK(memchr(cl.email, 0, sizeof cl.email) && memchr(cl.username, 0, sizeof cl.username));
        FZ_CHECK(cb_uuid_valid(cl.sub, strlen(cl.sub)) && cb_uuid_valid(cl.session_id, strlen(cl.session_id)));
        char again[CB_JWT_MAX];
        if (cb_jwt_mint(SECRET, &cl, "https://fuzz", again, sizeof again) > 0) {
            cb_jwt_claims back;
            memset(&back, 0, sizeof back);
            FZ_CHECK(cb_jwt_verify(SECRET, again, NOW_S, &back) == 0);
            FZ_CHECK(strcmp(back.sub, cl.sub) == 0 && strcmp(back.session_id, cl.session_id) == 0);
            FZ_CHECK(strcmp(back.email, cl.email) == 0 && strcmp(back.username, cl.username) == 0);
            FZ_CHECK(back.exp == cl.exp && back.iat == cl.iat);
        }
        // The same token under another secret never verifies.
        FZ_CHECK(cb_jwt_verify(SECRET "x", tok, NOW_S, &cl) != 0);
    }
    free(tok);
    return 0;
}
