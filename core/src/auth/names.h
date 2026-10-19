// names.h - a username's canonical form (security finding M4: look-alike
// names on the leaderboard, in rosters and in chat).
//
// An account is found by the email its name derives (v1_http.h
// cb_v1_email_of: SHA-256 of the name's bytes), so two names that read the
// same must be the same bytes, and a name no one can see whole must not be one.
// The clients have always upper-cased the name before sending it (JavaScript's
// toUpperCase, Swift's uppercased()); the server never checked, so a request
// that skipped the client could sign up "alice" beside "ALICE".
//
// Two steps:
//   cb_name_case   the case step, for every request that names an account
//                  (sign-up and sign-in): letters upper-cased in Latin, Latin-1,
//                  Latin Extended-A, Greek and Cyrillic, exactly as the clients'
//                  toUpperCase does for them, so a name a client sent is left as
//                  it is (and every existing account still signs in), and one a
//                  client did not send becomes the one it would have.
//   cb_name_check  the rest, for a NEW name (sign-up): valid UTF-8; no control,
//                  invisible, formatting, bidirectional, private-use or
//                  variation characters (a variation selector only right after
//                  an emoji); U+0020 the only space, never at either end or
//                  twice in a row; no '%' (a bot's prefix); no Latin, Greek and
//                  Cyrillic letters mixed in one name (the homoglyph spoof: "A"
//                  and Cyrillic "А"); 1 to CB_NAME_MAX_CHARS characters.
// Names made before this rule (the 23 lower-case ASCII names imported from the
// old backend among them) are left as they are: the rule is for new ones.
#ifndef CB_NAMES_H
#define CB_NAMES_H

#include <stddef.h>

#define CB_NAME_MAX_CHARS 32

// `in` (n bytes) case-folded upward into out, NUL-terminated: the length, or
// -1 when it is not valid UTF-8 or does not fit (cap - 1 bytes).
int cb_name_case(const char *in, size_t n, char *out, size_t cap);

// The key two names compare by (the store's unique username_key): the name's
// cb_name_case form, or its bytes as they are when it has none (not UTF-8). A
// name is displayed as it was stored; only the key is canonical, so a name made
// before the rule (lower case) keeps its look and still blocks "ALICE" beside
// "alice". The length, or -1 when it does not fit (cap - 1 bytes).
int cb_name_key(const char *name, size_t n, char *out, size_t cap);

// CB_OK, or CB_AUTH_E_BAD_USERNAME (auth.h). `name` is as cb_name_case left it.
int cb_name_check(const char *name, size_t n);

#endif
