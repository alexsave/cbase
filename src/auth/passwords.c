// passwords.c - the most common passwords, refused at sign-up and on a change
// (auth.h cb_auth_password_common).
//
// Only the ones at least CB_AUTH_PASSWORD_MIN (8) characters long matter: a
// shorter one is refused for its length. Drawn from the published top lists
// of leaked passwords (the 8-plus-character entries near the top of each),
// lowercase; the comparison ignores case, so "Password1" is refused with
// "password1". Small on purpose: it stops the first guesses of a spray, not a
// dictionary.
#include "auth.h"

#include <strings.h>

static const char *const COMMON[] = {
    "00000000", "11111111", "12121212", "12341234", "12344321", "123123123", "12345678", "123456789",
    "1234567890", "12345678910", "123456789a", "1234qwer", "123qweasd", "1q2w3e4r", "1q2w3e4r5t", "1qaz2wsx",
    "1qazxsw2", "22222222", "55555555", "66666666", "77777777", "87654321", "88888888", "987654321",
    "9876543210", "99999999", "a1b2c3d4", "aa123456", "aaaaaaaa", "abc12345", "abc123456", "abcd1234",
    "abcdefgh", "access14", "admin123", "administrator", "asdf1234", "asdfasdf", "asdfghjk", "asdfghjkl",
    "baseball", "basketball", "batman123", "charlie1", "chocolate", "computer", "corvette", "dearbook",
    "dragon123", "football", "football1", "freedom1", "iloveyou", "iloveyou1", "iloveyou2", "internet",
    "jennifer", "jordan23", "letmein1", "liverpool", "login123", "master123", "michael1", "michelle",
    "monkey123", "mustang1", "neworleans", "nicole12", "p@ssw0rd", "p@ssword", "passw0rd", "password",
    "password!", "password1", "password12", "password123", "password1234", "princess", "princess1", "q1w2e3r4",
    "q1w2e3r4t5", "qazwsxedc", "qwer1234", "qwerty12", "qwerty123", "qwerty1234", "qwertyui", "qwertyuiop",
    "samantha", "shadow12", "starwars", "summer2024", "summer2025", "summer2026", "sunshine", "sunshine1",
    "superman", "superman1", "test1234", "thomas12", "trustno1", "welcome1", "welcome123", "whatever",
    "winter2025", "winter2026", "zaq12wsx", "zxcvbnm1", "zxcvbnm123", "foolish1", "foolish123", "foolishcards",
    "durak123", "durakonline",
};

bool cb_auth_password_common(const char *pw) {
    if (!pw) return false;
    for (size_t i = 0; i < sizeof COMMON / sizeof COMMON[0]; i++)
        if (strcasecmp(pw, COMMON[i]) == 0) return true;
    return false;
}
