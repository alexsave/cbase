// unit_bcrypt.c - bcrypt (auth/bcrypt.h): published vectors, GoTrue's $2a$,
// the hash and parse round trip, and the cost budget.
#include "t.h"

#include "auth/bcrypt.h"
#include "util/clock.h"
#include "util/err.h"

// crypt_blowfish's and OpenBSD's published vectors (wrapper.c / regress).
static const struct { const char *hash, *pw; } VECTORS[] = {
    { "$2a$05$CCCCCCCCCCCCCCCCCCCCC.E5YPO9kmyuRGyh0XouQYb4YMJKvyOeW", "U*U" },
    { "$2a$05$CCCCCCCCCCCCCCCCCCCCC.VGOzA784oUp/Z0DY336zx7pLYAy0lwK", "U*U*" },
    { "$2a$05$XXXXXXXXXXXXXXXXXXXXXOAcXxm9kjPGEMsLznoKqmqw7tc8WCx4a", "U*U*U" },
    { "$2a$05$abcdefghijklmnopqrstuu5s2v8.iXieOjg/.AySBTTZIIVFJeBui",
      "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789chars after 72 are ignored" },
    { "$2a$05$CCCCCCCCCCCCCCCCCCCCC.7uG0VCzI2bS7j6ymqJi9CdcdxiRTWNy", "" },
    { "$2b$05$CCCCCCCCCCCCCCCCCCCCC.E5YPO9kmyuRGyh0XouQYb4YMJKvyOeW", "U*U" },
    { "$2y$05$/OK.fbVrR/bpIqNJ5ianF.CE5elHaaO4EbggVDjb8P19RukzXSM3e", "\xff\xff\xa3" },
    { "$2y$05$/OK.fbVrR/bpIqNJ5ianF.Sa7shbm4.OzKpvFnX1pQLmQW96oUlCq", "\xa3" },
    // An independent build at GoTrue's cost: macOS htpasswd -bnBC 10 u hunter22.
    { "$2y$10$0AEdGnKT1Ux6/ZYuqgDxCu9QZ.V89ahXzgfCaVQ3GxRyFEUgqD33q", "hunter22" },
    // The same hash under GoTrue's prefix: Go's bcrypt writes the correct
    // algorithm as $2a$, so for any password the three prefixes agree.
    { "$2a$10$0AEdGnKT1Ux6/ZYuqgDxCu9QZ.V89ahXzgfCaVQ3GxRyFEUgqD33q", "hunter22" },
    { "$2b$10$0AEdGnKT1Ux6/ZYuqgDxCu9QZ.V89ahXzgfCaVQ3GxRyFEUgqD33q", "hunter22" },
};

static void published_vectors(void) {
    for (size_t i = 0; i < sizeof VECTORS / sizeof VECTORS[0]; i++) {
        T_CHECK(cb_bcrypt_parse(VECTORS[i].hash));
        T_EQ_INT(cb_bcrypt_verify(VECTORS[i].pw, VECTORS[i].hash), 1);
        T_EQ_INT(cb_bcrypt_verify("wrong", VECTORS[i].hash), 0);
    }
}

// A 0xFF password under $2a$: Go (and so GoTrue) runs the correct algorithm,
// which is $2y$'s answer, not crypt_blowfish's anti-collision $2a$ variant
// ("$2a$05$/OK.fbVrR/bpIqNJ5ianF.nqd1wy.pTMdcvrRWxyiGL2eMz.2a85.").
static void two_a_is_gos_algorithm(void) {
    T_EQ_INT(cb_bcrypt_verify("\xff\xff\xa3", "$2a$05$/OK.fbVrR/bpIqNJ5ianF.CE5elHaaO4EbggVDjb8P19RukzXSM3e"), 1);
    T_EQ_INT(cb_bcrypt_verify("\xff\xff\xa3", "$2a$05$/OK.fbVrR/bpIqNJ5ianF.nqd1wy.pTMdcvrRWxyiGL2eMz.2a85."), 0);
}

static void refuses_what_is_not_bcrypt(void) {
    const char *bad[] = {
        "",
        "plain",
        "$2x$05$/OK.fbVrR/bpIqNJ5ianF.CE5elHaaO4EbggVDjb8P19RukzXSM3e",   // the sign-extension bug
        "$2a$03$CCCCCCCCCCCCCCCCCCCCC.E5YPO9kmyuRGyh0XouQYb4YMJKvyOeW",   // cost under 4
        "$2a$32$CCCCCCCCCCCCCCCCCCCCC.E5YPO9kmyuRGyh0XouQYb4YMJKvyOeW",   // cost over 31
        "$2a$05$CCCCCCCCCCCCCCCCCCCCC.E5YPO9kmyuRGyh0XouQYb4YMJKvyOe",    // short
        "$2a$05$CCCCCCCCCCCCCCCCCCCCC.E5YPO9kmyuRGyh0XouQYb4YMJKvyOeWW",  // long
        "$2a$05$CCCCCCCCCCCCCCCCCCCCC.E5YPO9kmyuRGyh0XouQYb4YMJKvyOe!",   // outside the alphabet
        "$1$05$CCCCCCCCCCCCCCCCCCCCCC.E5YPO9kmyuRGyh0XouQYb4YMJKvyOeW",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        T_CHECK(!cb_bcrypt_parse(bad[i]));
        T_EQ_INT(cb_bcrypt_verify("U*U", bad[i]), CB_E_INVALID);
    }
    T_CHECK(!cb_bcrypt_parse(NULL));
}

static void hash_round_trip(void) {
    char h1[CB_BCRYPT_HASH_LEN + 1], h2[CB_BCRYPT_HASH_LEN + 1];
    T_EQ_INT(cb_bcrypt_hash("correct horse", 5, h1), CB_OK);
    T_EQ_INT(cb_bcrypt_hash("correct horse", 5, h2), CB_OK);
    T_EQ_INT(strlen(h1), CB_BCRYPT_HASH_LEN);
    T_CHECK(strncmp(h1, "$2b$05$", 7) == 0);
    T_CHECK(cb_bcrypt_parse(h1));
    T_CHECK(strcmp(h1, h2) != 0);   // a fresh salt each time
    T_EQ_INT(cb_bcrypt_verify("correct horse", h1), 1);
    T_EQ_INT(cb_bcrypt_verify("correct horse", h2), 1);
    T_EQ_INT(cb_bcrypt_verify("correct horsf", h1), 0);
    T_EQ_INT(cb_bcrypt_hash("x", 3, h1), CB_E_INVALID);
    T_EQ_INT(h1[0], 0);
    T_EQ_INT(cb_bcrypt_hash("x", 32, h1), CB_E_INVALID);
}

// GoTrue's cost on this machine: one hash and one verify each well under 100 ms.
static void cost_ten_budget(void) {
    char h[CB_BCRYPT_HASH_LEN + 1];
    int64_t t0 = cb_mono_ms();
    T_EQ_INT(cb_bcrypt_hash("budget", CB_BCRYPT_COST, h), CB_OK);
    int64_t t1 = cb_mono_ms();
    T_EQ_INT(cb_bcrypt_verify("budget", h), 1);
    int64_t t2 = cb_mono_ms();
    T_CHECK(strncmp(h, "$2b$10$", 7) == 0);
    printf("     bcrypt cost %d: hash %lld ms, verify %lld ms\n", CB_BCRYPT_COST, (long long)(t1 - t0), (long long)(t2 - t1));
#if defined(__SANITIZE_ADDRESS__)
#define CB_SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define CB_SANITIZED 1
#endif
#endif
#ifndef CB_SANITIZED
    T_CHECK(t2 - t1 < 100);   // the sanitized build is several times slower; the budget is the shipped one
#endif
}

int main(void) {
    T_RUN(published_vectors);
    T_RUN(two_a_is_gos_algorithm);
    T_RUN(refuses_what_is_not_bcrypt);
    T_RUN(hash_round_trip);
    T_RUN(cost_ten_budget);
    return T_DONE();
}
