// jwt.h - access tokens: HS256 JWTs in GoTrue's claim shape.
//
// The claims are GoTrue's, as the accounts' first tokens were (no client reads
// them any more: the session states its expiry, netwire.h NW_SESSION):
//   {"aud":"authenticated","exp":..,"iat":..,"iss":"<public url>/auth/v1",
//    "sub":"<user id>","email":"..","role":"authenticated","session_id":"..",
//    "user_metadata":{"username":".."},"app_metadata":{"provider":"email","providers":["email"]},
//    "is_anonymous":false}
// Signed HMAC-SHA256 with CBASE_JWT_SECRET (libcrypto HMAC). The header is
// {"alg":"HS256","typ":"JWT"}; verification accepts only alg HS256. The JSON
// of both is written and read in json_surface.c.
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

// Writes the signed token and a NUL. Its length, or CB_E_FULL.
int cb_jwt_mint(const char *secret, const cb_jwt_claims *c, const char *issuer, char *out, size_t cap);
// Checks the signature (constant time), alg HS256, aud "authenticated", and
// exp against now_s with 30 s of skew. CB_OK and *out, or CB_E_AUTH.
int cb_jwt_verify(const char *secret, const char *token, int64_t now_s, cb_jwt_claims *out);

#endif
