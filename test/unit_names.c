// unit_names.c - a username's canonical form (auth/names.h, finding M4): the
// case step the clients have always taken, and the rule for a new name, held
// against the look-alikes the security review signed up as separate accounts.
#include "t.h"

#include "auth/auth.h"
#include "auth/names.h"
#include "util/err.h"

static const char *cased(const char *in) {
    static char out[256];
    int n = cb_name_case(in, strlen(in), out, sizeof out);
    return n < 0 ? "(refused)" : out;
}

static void the_case_step(void) {
    T_EQ_STR(cased("alice"), "ALICE");
    T_EQ_STR(cased("ALICE"), "ALICE");                      // what a client sends stays as it is
    T_EQ_STR(cased("Ann_02"), "ANN_02");
    T_EQ_STR(cased("\xc3\xa9t\xc3\xa9"), "\xc3\x89T\xc3\x89");   // été -> ÉTÉ
    T_EQ_STR(cased("\xc3\xbf"), "\xc5\xb8");                  // ÿ -> Ÿ
    T_EQ_STR(cased("\xc2\xb5"), "\xce\x9c");                  // µ -> Μ, as toUpperCase
    T_EQ_STR(cased("\xc4\x81\xc4\xba"), "\xc4\x80\xc4\xb9");  // āĺ -> ĀĹ (both pairings of Extended-A)
    T_EQ_STR(cased("\xc4\xb1"), "I");                          // ı -> I
    T_EQ_STR(cased("\xce\xb1\xcf\x82"), "\xce\x91\xce\xa3");  // ας -> ΑΣ
    T_EQ_STR(cased("\xce\xac\xcf\x8e"), "\xce\x86\xce\x8f");  // άώ -> ΆΏ
    T_EQ_STR(cased("\xd0\xb8\xd0\xb2\xd0\xb0\xd0\xbd"), "\xd0\x98\xd0\x92\xd0\x90\xd0\x9d");   // иван -> ИВАН
    T_EQ_STR(cased("\xd1\x91"), "\xd0\x81");                  // ё -> Ё
    T_EQ_STR(cased("\xc3\x9f"), "\xc3\x9f");                  // ß: a client sends "SS"; left alone
    T_EQ_STR(cased("\xea\xb9\x80\xec\xb2\xa0"), "\xea\xb9\x80\xec\xb2\xa0");   // Hangul has no case
    T_EQ_STR(cased("\xf0\x9f\x98\x80"), "\xf0\x9f\x98\x80");  // an emoji
    // Not UTF-8: overlong, a surrogate, a lone continuation, a truncated sequence, past U+10FFFF.
    T_EQ_STR(cased("\xc0\xaf"), "(refused)");
    T_EQ_STR(cased("\xed\xa0\x80"), "(refused)");
    T_EQ_STR(cased("\x80"), "(refused)");
    T_EQ_STR(cased("A\xe2\x82"), "(refused)");
    T_EQ_STR(cased("\xf4\x90\x80\x80"), "(refused)");
    char small[4];
    T_EQ_INT(cb_name_case("ABCD", 4, small, sizeof small), -1);   // does not fit
}

static int check(const char *name) { return cb_name_check(name, strlen(name)); }

static void the_rule_for_a_new_name(void) {
    // The review's harness: each of these signed up beside ALICE as another account.
    T_EQ_INT(check("ALICE"), CB_OK);
    T_EQ_INT(check(" ALICE"), CB_AUTH_E_BAD_USERNAME);              // a leading space
    T_EQ_INT(check("ALICE "), CB_AUTH_E_BAD_USERNAME);
    T_EQ_INT(check("AL  ICE"), CB_AUTH_E_BAD_USERNAME);             // two spaces
    T_EQ_INT(check("ALICE\xe2\x80\x8b"), CB_AUTH_E_BAD_USERNAME);    // + U+200B zero width space
    T_EQ_INT(check("\xe2\x80\xae" "ECILA"), CB_AUTH_E_BAD_USERNAME); // U+202E right-to-left override
    T_EQ_INT(check("ALI\xe2\x81\xa6" "CE"), CB_AUTH_E_BAD_USERNAME); // U+2066 isolate
    T_EQ_INT(check("ALICE\xef\xbb\xbf"), CB_AUTH_E_BAD_USERNAME);    // BOM
    T_EQ_INT(check("ALICE\xc2\xa0"), CB_AUTH_E_BAD_USERNAME);        // a no-break space
    T_EQ_INT(check("A\xc2\xadLICE"), CB_AUTH_E_BAD_USERNAME);        // a soft hyphen
    T_EQ_INT(check("ALICE\xe3\x85\xa4"), CB_AUTH_E_BAD_USERNAME);    // a Hangul filler
    T_EQ_INT(check("\xff\xfe"), CB_AUTH_E_BAD_USERNAME);             // not UTF-8
    T_EQ_INT(check("AL\x01ICE"), CB_AUTH_E_BAD_USERNAME);
    T_EQ_INT(check("%BOT"), CB_AUTH_E_BAD_USERNAME);                 // a bot's prefix
    T_EQ_INT(check(""), CB_AUTH_E_BAD_USERNAME);
    // Homoglyphs: Latin with Cyrillic (А, U+0410) or Greek (Α, U+0391) letters.
    T_EQ_INT(check("\xd0\x90LICE"), CB_AUTH_E_BAD_USERNAME);
    T_EQ_INT(check("\xce\x91LICE"), CB_AUTH_E_BAD_USERNAME);
    T_EQ_INT(check("\xd0\x90\xce\x91"), CB_AUTH_E_BAD_USERNAME);
    // A whole-script name, any script, with digits and spaces: fine.
    T_EQ_INT(check("\xd0\x98\xd0\x92\xd0\x90\xd0\x9d 2"), CB_OK);    // ИВАН 2
    T_EQ_INT(check("\xce\x91\xce\x9b\xce\xa6\xce\x91"), CB_OK);      // ΑΛΦΑ
    T_EQ_INT(check("\xea\xb9\x80\xec\xb2\xa0\xec\x88\x98"), CB_OK);  // 김철수
    T_EQ_INT(check("\xe7\x8e\x8b\xe5\xb0\x8f\xe6\x98\x8e"), CB_OK);  // 王小明
    T_EQ_INT(check("KIM \xea\xb9\x80"), CB_OK);                      // Latin and Hangul do not pass for each other
    T_EQ_INT(check("J.R. SMITH-JONES"), CB_OK);
    // An emoji, alone or with its style selector; a selector after a letter is invisible.
    T_EQ_INT(check("\xf0\x9f\x98\x80"), CB_OK);
    T_EQ_INT(check("\xe2\x98\xba\xef\xb8\x8f"), CB_OK);              // ☺ + VS16
    T_EQ_INT(check("ALICE\xef\xb8\x8f"), CB_AUTH_E_BAD_USERNAME);
    T_EQ_INT(check("\xea\xb9\x80\xef\xb8\x8f"), CB_AUTH_E_BAD_USERNAME);
    T_EQ_INT(check("\xe2\x80\x8d"), CB_AUTH_E_BAD_USERNAME);          // a zero width joiner
    // Length: characters and bytes.
    char n32[64], n33[64];
    memset(n32, 'A', 32); n32[32] = 0;
    memset(n33, 'A', 33); n33[33] = 0;
    T_EQ_INT(check(n32), CB_OK);
    T_EQ_INT(check(n33), CB_AUTH_E_BAD_USERNAME);
    char wide[CB_USERNAME_MAX + 8];   // 22 three-byte characters: 66 bytes
    size_t at = 0;
    for (int i = 0; i < 22; i++) { memcpy(wide + at, "\xe7\x8e\x8b", 3); at += 3; }
    wide[at] = 0;
    T_EQ_INT(check(wide), CB_AUTH_E_BAD_USERNAME);
}

int main(void) {
    T_RUN(the_case_step);
    T_RUN(the_rule_for_a_new_name);
    return T_DONE();
}
