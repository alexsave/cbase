// jwt.c - (jwt.h). STUB: the auth builder fills this in (libcrypto HMAC, util/b64.h, util/json.h).
#include "jwt.h"

int cb_jwt_mint(const char *secret, const cb_jwt_claims *c, const char *issuer, char *out, size_t cap) { return CB_E_NOTIMPL; }
int cb_jwt_verify(const char *secret, const char *token, int64_t now_s, cb_jwt_claims *out) { return CB_E_NOTIMPL; }
