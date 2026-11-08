// unit_num_gate.c - the number-parsing gate (shared/c/num_parse.h): every decimal
// integer cbase reads from text goes through num_parse_i64. This test reads
// every C source under core/src, each provider's src/ and the product's source
// roots (CBASE_APP_SRC_ROOTS, lib_tree.h) and fails, naming the file and line, when one
// names a C library integer parser:
//
//   * strtol, strtoll, strtoul, strtoull, strtoimax, strtoumax: a leading
//     space and '+' taken, a '-' that the unsigned ones wrap around, and the
//     end pointer easily ignored,
//   * atoi, atol, atoll: the same with no way to see an error at all,
//   * any scanf (sscanf, fscanf, ...): %d and friends have strtol's rules.
//
// The floating-point parsers (strtod, strtof, strtold) are allowed in one
// file, providers/aws/src/aws_json.c, which reads the one fractional number cbase
// takes (DynamoDB's CapacityUnits); anywhere else they are a violation too.
//
// Comments, string literals and character literals are skipped (the scanner is
// lib_cscan.h's), so a comment may name strtol. It runs from any directory (lib_tree.h).
#include "t.h"
#include "lib_cscan.h"
#include "lib_tree.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static const char *const INTEGER[] = { "strtol", "strtoll", "strtoul", "strtoull", "strtoimax", "strtoumax",
                                       "atoi",   "atol",    "atoll",   "sscanf",   "fscanf",    "scanf",
                                       "vsscanf", "vfscanf", "vscanf" };
static const char *const FLOATING[] = { "strtod", "strtof", "strtold" };
static const char FLOAT_HOME[] = "providers/aws/src/aws_json.c";

static int  g_files, g_violations;
static bool g_quiet;   // planted text: counted, not printed

static bool in(const char *const *names, size_t k, const char *s, size_t n) {
    for (size_t i = 0; i < k; i++)
        if (strlen(names[i]) == n && memcmp(names[i], s, n) == 0) return true;
    return false;
}

static void violation(const char *path, int line, const char *s, size_t n) {
    g_violations++;
    if (!g_quiet)
        fprintf(stderr, "num gate: %s:%d: %.*s (use num_parse_i64, shared/c/num_parse.h)\n", path, line, (int)n, s);
}

static void on_tok(const cscan_tok *t, void *ctx) {
    const char *path = ctx;
    if (t->kind != CSCAN_IDENT) return;
    if (in(INTEGER, sizeof INTEGER / sizeof INTEGER[0], t->s, t->n) ||
        (strcmp(path, FLOAT_HOME) != 0 && in(FLOATING, sizeof FLOATING / sizeof FLOATING[0], t->s, t->n)))
        violation(path, t->line, t->s, t->n);
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

static const cscan_walker WALK = { .file = on_file, .unreadable = on_unreadable };

// Every C source under dir (opened as `dir`), each named `name`/<the rest of its path>.
static void scan_named(const char *dir, const char *name) { cscan_walk(&WALK, dir, name); }

// A cbase directory, from cbase's root.
static void scan_dir(const char *rel) {
    char p[TT_PATH_MAX];
    scan_named(tt_path(p, sizeof p, rel), rel);
}

// The gate itself: the tree as it is.
static void only_num_parse_i64_reads_numbers(void) {
    struct stat st;
    char p[TT_PATH_MAX];
    T_CHECK(stat(tt_path(p, sizeof p, "core/src/config.c"), &st) == 0);   // CBASE_ROOT is cbase, or nothing is scanned
    // The float exemption names a real file wherever its provider is in the tree (a product that
    // vendors core and providers/local alone has no providers/aws, and nothing there to scan).
    if (stat(tt_path(p, sizeof p, "providers/aws"), &st) == 0) T_CHECK(stat(tt_path(p, sizeof p, FLOAT_HOME), &st) == 0);
    scan_dir("core/src");
    scan_dir("providers/local/src");
    scan_dir("providers/aws/src");
    char roots[TT_ROOTS_MAX][TT_PATH_MAX];
    int n = tt_env_list("CBASE_APP_SRC_ROOTS", roots, TT_ROOTS_MAX);
    for (int i = 0; i < n; i++) scan_named(roots[i], roots[i]);
    T_CHECK(g_files >= 50);   // a gate that read nothing proves nothing
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
    T_EQ_INT(planted("(planted)", "long x = strtol(v, &end, 10);\n"), 1);
    T_EQ_INT(planted("(planted)", "x = strtoll(a, 0, 10) + strtoull(b, 0, 10);\n"), 2);
    T_EQ_INT(planted("(planted)", "p = atoi(c); q = atol(c); r = atoll(c);\n"), 3);
    T_EQ_INT(planted("(planted)", "if (sscanf(line, \"%d\", &v) == 1) {}\n"), 1);
    T_EQ_INT(planted("(planted)", "long (*f)(const char *, char **, int) = strtol;\n"), 1);
    T_EQ_INT(planted("(planted)", "#define P(s) strtoul(s, 0, 10)\n"), 1);
    T_EQ_INT(planted("(planted)", "double d = strtod(t, NULL);\n"), 1);
    T_EQ_INT(planted(FLOAT_HOME, "double d = strtod(t, NULL);\n"), 0);
    T_EQ_INT(planted(FLOAT_HOME, "long x = strtol(t, NULL, 10);\n"), 1);
    // Not a parser: comments, literals, longer names, a number's suffix.
    T_EQ_INT(planted("(planted)", "// strtol(v) described in a comment\n/* atoi */\n"), 0);
    T_EQ_INT(planted("(planted)", "const char *m = \"strtol and atoi\"; char q = '\"'; int k = sscanf_count;\n"), 0);
    T_EQ_INT(planted("(planted)", "num_parse_i64(s, n, 0, 9, &v); my_atoi(x); long l = 10l;\n"), 0);
}

int main(void) {
    T_RUN(the_scanner_sees_each_kind);
    T_RUN(only_num_parse_i64_reads_numbers);
    return T_DONE();
}
