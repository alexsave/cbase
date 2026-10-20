// jwt.h - access tokens: a fixed binary layout (jwt.c owns it), HMAC-SHA256,
// base64url. Not JSON and not a JWT any more: no client reads the token (the
// session states its expiry, netwire.h NW_SESSION). Signed with
// CBASE_JWT_SECRET (libcrypto HMAC).
#ifndef CB_JWT_H
#define CB_JWT_H

#include "store/store.h"

#include <stddef.h>
#include <stdint.h>

#define CB_JWT_MAX 2048

typedef struct {
    cb_uuid sub;                              // the user id
    char    email[CB_EMAIL_MAX + 1];
    char    username[CB_USERNAME_MAX + 1];    // user_metadata.username
    cb_uuid session_id;
    int64_t iat, exp;                         // seconds since the epoch
} cb_jwt_claims;

// Writes the signed token and a NUL. Its length, CB_E_FULL, or CB_E_INVALID for
// no secret, an id that is not a lowercase UUID or a string over 255 bytes.
int cb_jwt_mint(const char *secret, const cb_jwt_claims *c, char *out, size_t cap);
// Checks the signature (constant time), the version, the exact length, and exp
// against now_s with 30 s of skew. CB_OK and *out, CB_E_AUTH, or CB_E_NOMEM
// when the HMAC itself could not be computed: the server's failure, not the
// token's, which a caller must not answer as a bad token (that signs a user out).
int cb_jwt_verify(const char *secret, const char *token, int64_t now_s, cb_jwt_claims *out);

// The calling thread's verified-token cache (jwt.c): freed (a leak-counting test), and the HMACs
// this thread's verifications computed (a test of the cache's hits).
void     cb_jwt_vcache_drain(void);
uint64_t cb_jwt_macs(void);

#endif
