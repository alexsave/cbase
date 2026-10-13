// bcrypt.c - (bcrypt.h). STUB: the auth builder embeds a public-domain bcrypt here.
#include "bcrypt.h"

#include "util/err.h"

int cb_bcrypt_hash(const char *password, int cost, char out[CB_BCRYPT_HASH_LEN + 1]) { out[0] = 0; return CB_E_NOTIMPL; }
int cb_bcrypt_verify(const char *password, const char *hash) { return CB_E_NOTIMPL; }
bool cb_bcrypt_parse(const char *hash) { return false; }
