// uuid.h - random ids, and the process's one source of random bytes.
//
// Every secret c_base draws (a deal seed, a refresh token, a session id, a game
// id) comes through cb_random_bytes, which is OpenSSL's CSPRNG. A failure to
// draw is fatal: there is no safe fallback for a secret.
#ifndef CB_UUID_H
#define CB_UUID_H

#include <stdbool.h>
#include <stddef.h>

#define CB_UUID_LEN 36   // characters, without the NUL

// n bytes from the CSPRNG (random.c). Aborts the process if OpenSSL cannot supply them.
void cb_random_bytes(void *out, size_t n);
// A random (version 4, variant 1) UUID in canonical lowercase form, NUL-terminated.
void cb_uuid_v4(char out[CB_UUID_LEN + 1]);
// The canonical 8-4-4-4-12 hex form, either case; any version.
bool cb_uuid_valid(const char *s, size_t n);

#endif
