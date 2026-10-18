// bcrypt.h - password hashes GoTrue wrote, read and written here.
//
// Supabase stored bcrypt ($2a$, cost 10) in auth.users.encrypted_password; the
// migration copies the hashes unchanged, so every existing player keeps their
// password. To embed: a public-domain bcrypt (the OpenBSD-derived one, e.g.
// Solar Designer's crypt_blowfish), with the published test vectors in
// test/unit_bcrypt.c. Verification compares in constant time.
#ifndef CB_BCRYPT_H
#define CB_BCRYPT_H

#include <stdbool.h>

#define CB_BCRYPT_HASH_LEN 60
#define CB_BCRYPT_COST     10   // GoTrue's default; about 60 ms here
// The most any hash may cost, written or read (security finding L6): a stored
// hash names its own cost, and one planted at 31 would spend a Lambda timeout
// on every sign-in to its name. 14 is 16x GoTrue's, under a second here.
#define CB_BCRYPT_COST_MAX 14

// Hashes `password` (bcrypt reads at most 72 bytes of it) at `cost` (4..CB_BCRYPT_COST_MAX)
// with a fresh random salt, as "$2b$<cost>$<salt><hash>" and a NUL.
// CB_OK, CB_E_INVALID for a cost out of range.
int cb_bcrypt_hash(const char *password, int cost, char out[CB_BCRYPT_HASH_LEN + 1]);
// 1 when `password` matches `hash`, 0 when not, CB_E_INVALID for a hash that
// is not $2a$/$2b$/$2y$ bcrypt at cost 4..CB_BCRYPT_COST_MAX (refused before any work).
int cb_bcrypt_verify(const char *password, const char *hash);
// The hash is one cb_bcrypt_verify can read (the import checks every row).
bool cb_bcrypt_parse(const char *hash);

#endif
