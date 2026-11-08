// unit_random_gate.c - the randomness gate: every random draw has one owner.
//
//   * Secret bytes (a token, a salt, a nonce, an id) come from cb_random_bytes,
//     core/src/util/random.c, the one caller of OpenSSL's RAND_bytes (and the file a
//     fuzz build replaces, fuzz/fuzz.mk, to make every id deterministic).
//   * A seeded stream (a backoff's jitter, a test's draws) is lib/rng.h's splitmix64,
//     over a seed the caller owns, so one line fixes it to a constant.
//   * The client core takes its seed from its host (cbase/client.h); the POSIX host,
//     client/hosts/posix/cbc_posix.c, draws it with getentropy.
//
// So this test reads every C source of the tree (core, providers, client, lib, bench,
// fuzz, tools; the tests and fuzz targets too) and the product's source roots
// (CBASE_APP_SRC_ROOTS, lib_tree.h), and fails, naming file and line, when one names a
// random source outside its home: the C library's (rand, random, srand, the *rand48
// family, arc4random*), the system's (getentropy, getrandom, SecRandomCopyBytes),
// OpenSSL's (RAND_bytes and its kin), or a device ("/dev/urandom", "/dev/random", in a
// string literal). Comments, character literals and #include lines are skipped (the scanner is
// lib_cscan.h's), so a comment may say "random" and random.c may include <openssl/rand.h>; this file, which plants
// the banned names in its own tests, is not read.
// It runs from any directory (lib_tree.h).
#include "t.h"
#include "lib_cscan.h"
#include "lib_tree.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

// One banned name and the one file allowed to name it (NULL: none).
typedef struct {
    const char *name;
    const char *home;
} source_rule;

static const char RANDOM_HOME[] = "core/src/util/random.c";
static const char HOST_HOME[] = "client/hosts/posix/cbc_posix.c";
// This file, whose literals are the banned names it plants, is the one file not read.
static const char SELF[] = "core/test/unit_random_gate.c";

static const source_rule RULES[] = {
    { "RAND_bytes", RANDOM_HOME },  { "RAND_priv_bytes", NULL }, { "RAND_pseudo_bytes", NULL },
    { "RAND_bytes_ex", NULL },      { "RAND_priv_bytes_ex", NULL },
    { "rand", NULL },               { "rand_r", NULL },          { "srand", NULL },
    { "random", NULL },             { "srandom", NULL },         { "initstate", NULL },
    { "drand48", NULL },            { "erand48", NULL },         { "lrand48", NULL },
    { "nrand48", NULL },            { "mrand48", NULL },         { "jrand48", NULL },
    { "srand48", NULL },            { "seed48", NULL },
    { "arc4random", NULL },         { "arc4random_buf", NULL },  { "arc4random_uniform", NULL },
    { "getentropy", HOST_HOME },    { "getrandom", NULL },       { "SecRandomCopyBytes", NULL },
};
// Devices, matched inside string literals.
static const char *const DEVICES[] = { "/dev/urandom", "/dev/random" };

static int  g_files, g_violations;
static bool g_quiet;   // planted text: counted, not printed

static void violation(const char *path, int line, const char *s, size_t n) {
    g_violations++;
    if (!g_quiet)
        fprintf(stderr, "random gate: %s:%d: %.*s (secret bytes: cb_random_bytes, util/uuid.h; a seeded stream: lib/rng.h)\n",
                path, line, (int)n, s);
}

static bool ident_char(char c) { return isalnum((unsigned char)c) || c == '_'; }

static void check_ident(const char *path, int line, const char *s, size_t n) {
    for (size_t r = 0; r < sizeof RULES / sizeof RULES[0]; r++) {
        if (strlen(RULES[r].name) != n || memcmp(RULES[r].name, s, n) != 0) continue;
        if (!RULES[r].home || strcmp(RULES[r].home, path) != 0) violation(path, line, s, n);
        return;
    }
}

static void check_literal(const char *path, int line, const char *s, size_t n) {
    for (size_t d = 0; d < sizeof DEVICES / sizeof DEVICES[0]; d++) {
        size_t k = strlen(DEVICES[d]);
        for (size_t i = 0; i + k <= n; i++)
            if (memcmp(s + i, DEVICES[d], k) == 0 && (i + k == n || !ident_char(s[i + k]))) {
                violation(path, line, DEVICES[d], k);
                return;
            }
    }
}

static void on_tok(const cscan_tok *t, void *ctx) {
    const char *path = ctx;
    if (t->kind == CSCAN_IDENT) check_ident(path, t->line, t->s, t->n);
    else if (t->kind == CSCAN_STRING) check_literal(path, t->line, t->s, t->n);
}

static void scan_text(const char *path, const char *s, size_t n) { cscan_lex(s, n, on_tok, (void *)path); }

static void on_file(const char *name, const char *text, size_t n, void *ctx) {
    (void)ctx;
    g_files++;
    scan_text(name, text, n);
}
static void on_unreadable(const char *name, void *ctx) {
    (void)ctx;
    violation(name, 0, "(cannot be read)", 16);
}
static bool is_self(const char *name, void *ctx) {
    (void)ctx;
    return strcmp(name, SELF) == 0;
}

// Build output and installed packages are not the tree's.
static const char *const SKIP_DIRS[] = { "build", "node_modules", NULL };
static const cscan_walker WALK = { .skip_dirs = SKIP_DIRS, .skip = is_self, .file = on_file, .unreadable = on_unreadable };

static void scan_named(const char *dir, const char *name) { cscan_walk(&WALK, dir, name); }

static void scan_dir(const char *rel) {
    char p[TT_PATH_MAX];
    scan_named(tt_path(p, sizeof p, rel), rel);
}

// The gate itself: the tree as it is.
static void every_draw_has_one_owner(void) {
    struct stat st;
    char p[TT_PATH_MAX];
    T_CHECK(stat(tt_path(p, sizeof p, RANDOM_HOME), &st) == 0);   // CBASE_ROOT is cbase, or nothing is scanned
    T_CHECK(stat(tt_path(p, sizeof p, HOST_HOME), &st) == 0);
    T_CHECK(stat(tt_path(p, sizeof p, SELF), &st) == 0);   // the exemption names a real file
    static const char *const DIRS[] = { "core", "providers", "client", "lib", "bench", "fuzz", "tools" };
    for (size_t i = 0; i < sizeof DIRS / sizeof DIRS[0]; i++) scan_dir(DIRS[i]);
    char roots[TT_ROOTS_MAX][TT_PATH_MAX];
    int n = tt_env_list("CBASE_APP_SRC_ROOTS", roots, TT_ROOTS_MAX);
    for (int i = 0; i < n; i++) scan_named(roots[i], roots[i]);
    T_CHECK(g_files >= 200);   // a gate that read nothing proves nothing
    T_EQ_INT(g_violations, 0);
}

// The scanner against planted text, so a gate that stopped seeing would fail here too.
static int planted(const char *path, const char *text) {
    int before = g_violations;
    g_quiet = true;
    scan_text(path, text, strlen(text));
    g_quiet = false;
    int found = g_violations - before;
    g_violations = before;
    return found;
}

static void the_scanner_sees_each_kind(void) {
    T_EQ_INT(planted("(planted)", "int x = rand() % 6;\n"), 1);
    T_EQ_INT(planted("(planted)", "srand(time(NULL)); long y = random();\n"), 2);
    T_EQ_INT(planted("(planted)", "uint32_t z = arc4random_uniform(10); arc4random_buf(b, 8);\n"), 2);
    T_EQ_INT(planted("(planted)", "if (RAND_bytes(buf, 12) != 1) return -1;\n"), 1);
    T_EQ_INT(planted("(planted)", "getentropy(&s, 8); getrandom(&s, 8, 0); double d = drand48();\n"), 3);
    T_EQ_INT(planted("(planted)", "int fd = open(\"/dev/urandom\", O_RDONLY);\n"), 1);
    T_EQ_INT(planted("(planted)", "FILE *f = fopen(\"/dev/random\", \"rb\");\n"), 1);
    T_EQ_INT(planted("(planted)", "int (*f)(void) = rand;\n"), 1);
    // Each home may name its own source, and only its own.
    T_EQ_INT(planted(RANDOM_HOME, "if (RAND_bytes(p, k) != 1) abort();\n"), 0);
    T_EQ_INT(planted(RANDOM_HOME, "int x = rand();\n"), 1);
    T_EQ_INT(planted(HOST_HOME, "getentropy(&k.seed, sizeof k.seed);\n"), 0);
    T_EQ_INT(planted(HOST_HOME, "RAND_bytes(b, 8);\n"), 1);
    // Not a draw: comments, includes, other literals, longer names, a number's suffix.
    T_EQ_INT(planted("(planted)", "// rand() described in a comment\n/* random() */\n"), 0);
    T_EQ_INT(planted("(planted)", "#include <openssl/rand.h>\n  #  include <sys/random.h>\n"), 0);
    T_EQ_INT(planted("(planted)", "const char *m = \"rand and random\"; char q = 'r';\n"), 0);
    T_EQ_INT(planted("(planted)", "rng_splitmix64(&s); cb_random_bytes(b, 8); int randomness = xRandomness; long l = 10l;\n"), 0);
    T_EQ_INT(planted("(planted)", "const char *d = \"/dev/urandomish\";\n"), 0);
}

int main(void) {
    T_RUN(the_scanner_sees_each_kind);
    T_RUN(every_draw_has_one_owner);
    return T_DONE();
}
