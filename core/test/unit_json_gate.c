// unit_json_gate.c - the JSON containment gate (DESIGN.md "Where JSON is
// tolerated"): every JSON shape cbase speaks lives in core/src/core_json.c or
// providers/aws/src/aws_json.c (a product's, in its one file: CBASE_APP_JSON_FILES), on
// top of the one low-level writer and reader, core/src/util/json.{c,h}. This test reads
// every C source under core/, providers/ and bench/, and the product's trees
// (CBASE_APP_JSON_ROOTS, lib_tree.h), but the tests and the fuzz targets (each
// side's test/ and fuzz/, which build JSON to feed it) and fails, naming the file
// and line, when any other file
//
//   * includes util/json.h (by any spelling ending in /json.h or json.h),
//   * names the writer or reader: an identifier cb_jw, cb_jw_*, cb_json_*,
//     CB_JW_* or CB_JSON_* (cb_jwt_* is the JWT module, not JSON), or
//   * holds a string literal with a JSON key in it: a quoted word followed by
//     a colon ("key": in the literal's text, \"key\": in the source), or
//   * holds a string literal that is XML: an "<?xml" prolog or an "xmlns"
//     namespace. CloudFront's API is XML (aws_json.h "CloudFront"); the
//     same containment holds for it, and the unfurl route's HTML (which has
//     neither) is not XML to this gate.
//
// Comments are skipped (the scanner is lib_cscan.h's), so a comment may describe
// a shape; adjacent literals are joined first, as the compiler joins them, so a
// key cannot hide across two of them, and a #define is read as code, so a key
// cannot hide in a macro. It runs from any directory (lib_tree.h).
#include "t.h"
#include "lib_cscan.h"
#include "lib_tree.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

// The files allowed to speak JSON: the surface and the mechanism under it.
static const char *const ALLOWED[] = { "core/src/core_json.c", "providers/aws/src/aws_json.c", "core/src/util/json.c", "core/src/util/json.h" };
static const char *const ROOTS[] = { "core", "providers", "bench" };

static int  g_files, g_violations;
static bool g_quiet;   // planted text: counted, not printed

// The product's own JSON file, as the product names it (CBASE_APP_JSON_FILES, cbase.mk's
// APP_JSON_FILES): one per product, the same containment on its side.
static char g_app_files[TT_ROOTS_MAX][TT_PATH_MAX];
static int  g_n_app_files = -1;

static bool allowed(const char *path) {
    for (size_t i = 0; i < sizeof ALLOWED / sizeof ALLOWED[0]; i++)
        if (strcmp(path, ALLOWED[i]) == 0) return true;
    if (g_n_app_files < 0) g_n_app_files = tt_env_list("CBASE_APP_JSON_FILES", g_app_files, TT_ROOTS_MAX);
    for (int i = 0; i < g_n_app_files; i++)
        if (strcmp(path, g_app_files[i]) == 0) return true;
    return false;
}

static void violation(const char *path, int line, const char *what, const char *text, size_t n) {
    g_violations++;
    if (!g_quiet) fprintf(stderr, "json gate: %s:%d: %s: %.*s\n", path, line, what, (int)(n > 120 ? 120 : n), text);
}

static bool ident_char(char c) { return isalnum((unsigned char)c) || c == '_'; }

static bool starts(const char *s, size_t n, const char *prefix) {
    size_t k = strlen(prefix);
    return n >= k && memcmp(s, prefix, k) == 0;
}

// The writer's and reader's names (util/json.h).
static bool json_ident(const char *s, size_t n) {
    if (n == 5 && memcmp(s, "cb_jw", 5) == 0) return true;
    return starts(s, n, "cb_jw_") || starts(s, n, "cb_json_") || starts(s, n, "CB_JW_") || starts(s, n, "CB_JSON_");
}

// A JSON key in a literal's text: '"', a word, '"', blanks, ':'.
static bool has_json_key(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (s[i] != '"') continue;
        size_t j = i + 1;
        while (j < n && (ident_char(s[j]) || s[j] == '-' || s[j] == '.' || s[j] == '$')) j++;
        if (j == i + 1 || j >= n || s[j] != '"') continue;
        size_t k = j + 1;
        while (k < n && (s[k] == ' ' || s[k] == '\t')) k++;
        if (k < n && s[k] == ':') return true;
    }
    return false;
}

typedef struct {
    char  *p;
    size_t n, cap;
    int    line;   // where the joined literal began
} lit;

static void lit_put(lit *l, char c) {
    if (l->n + 1 >= l->cap) {
        l->cap = l->cap ? l->cap * 2 : 256;
        l->p = realloc(l->p, l->cap);
        if (!l->p) abort();
    }
    l->p[l->n++] = c;
}

// XML in a literal's text: a prolog or a namespace declaration.
static bool has_xml(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++)
        if ((n - i >= 5 && memcmp(s + i, "<?xml", 5) == 0) || (n - i >= 5 && memcmp(s + i, "xmlns", 5) == 0)) return true;
    return false;
}

static void lit_flush(lit *l, const char *path) {
    if (l->n && has_json_key(l->p, l->n)) violation(path, l->line, "a JSON key in a string literal", l->p, l->n);
    else if (l->n && has_xml(l->p, l->n)) violation(path, l->line, "XML in a string literal", l->p, l->n);
    l->n = 0;
    l->line = 0;
}

// An #include's spec: is the header util/json.h (by any spelling ending in /json.h or json.h)?
static void check_include(const char *path, const cscan_tok *t) {
    const char *end = t->s + t->n;
    const char *q = memchr(t->s, '"', t->n), *a = memchr(t->s, '<', t->n);
    const char *o = q ? q : a;
    if (!o) return;
    const char *close = memchr(o + 1, q ? '"' : '>', (size_t)(end - o - 1));
    if (close && close - (o + 1) >= 6 && memcmp(close - 6, "json.h", 6) == 0 && (close - (o + 1) == 6 || close[-7] == '/'))
        violation(path, t->line, "includes util/json.h", o, (size_t)(close - o + 1));
}

typedef struct {
    const char *path;
    lit         l;
} scan;

static void on_tok(const cscan_tok *t, void *ctx) {
    scan *sc = ctx;
    lit *l = &sc->l;
    if (t->kind == CSCAN_STRING) {
        // Adjacent literals join, as the compiler joins them (comments and blanks between them
        // are no token); unescaped into l, \" and \\ only: enough to see quotes.
        if (!l->n) l->line = t->line;
        for (size_t i = 0; i < t->n; i++) {
            if (t->s[i] == '\\' && i + 1 < t->n) {
                char x = t->s[++i];
                if (x == '\n') continue;
                lit_put(l, x == '"' || x == '\\' ? x : '\\');
                if (x != '"' && x != '\\') lit_put(l, x);
                continue;
            }
            lit_put(l, t->s[i]);
        }
        return;
    }
    // Anything else ends a run of adjacent literals.
    lit_flush(l, sc->path);
    if (t->kind == CSCAN_INCLUDE) check_include(sc->path, t);
    else if (t->kind == CSCAN_IDENT && json_ident(t->s, t->n))
        violation(sc->path, t->line, "names the JSON writer or reader", t->s, t->n);
}

static void scan_text(const char *path, const char *s, size_t n) {
    scan sc = { .path = path };
    cscan_lex(s, n, on_tok, &sc);
    lit_flush(&sc.l, path);
    free(sc.l.p);
}

static void on_file(const char *name, const char *text, size_t n, void *ctx) {
    (void)ctx;
    g_files++;
    scan_text(name, text, n);
}
static void on_unreadable(const char *name, void *ctx) {
    (void)ctx;
    violation(name, 0, "cannot be read", "", 0);
}
static bool is_allowed(const char *name, void *ctx) {
    (void)ctx;
    return allowed(name);
}

// The tests and the fuzz targets build JSON to feed it.
static const char *const SKIP_DIRS[] = { "test", "fuzz", NULL };
static const cscan_walker WALK = { .skip_dirs = SKIP_DIRS, .skip = is_allowed, .file = on_file, .unreadable = on_unreadable };

// Every C source under dir (opened as `dir`), each named `name`/<the rest of its path>.
static void scan_named(const char *dir, const char *name) { cscan_walk(&WALK, dir, name); }

// The gate itself: the tree as it is.
static void only_json_surface_speaks_json(void) {
    struct stat st;
    char p[TT_PATH_MAX];
    T_CHECK(stat(tt_path(p, sizeof p, ALLOWED[0]), &st) == 0);   // CBASE_ROOT is cbase, or nothing is scanned
    for (size_t i = 0; i < sizeof ROOTS / sizeof ROOTS[0]; i++) scan_named(tt_path(p, sizeof p, ROOTS[i]), ROOTS[i]);
    char roots[TT_ROOTS_MAX][TT_PATH_MAX];
    int n = tt_env_list("CBASE_APP_JSON_ROOTS", roots, TT_ROOTS_MAX);
    for (int i = 0; i < n; i++) scan_named(roots[i], roots[i]);
    T_CHECK(g_files >= 50);   // a gate that read nothing proves nothing
    T_EQ_INT(g_violations, 0);
}

// The scanner against planted text, so a gate that stopped seeing would fail here too.
static int planted(const char *text) {
    int before = g_violations;
    g_quiet = true;
    scan_text("(planted)", text, strlen(text));
    g_quiet = false;
    int found = g_violations - before;
    g_violations = before;
    return found;
}

static void the_scanner_sees_each_kind(void) {
    T_EQ_INT(planted("static const char *k = \"\\\"foo\\\":1\";\n"), 1);
    T_EQ_INT(planted("x = \"{\\\"a\\\" \" \": 1}\";\n"), 1);   // joined across literals
    T_EQ_INT(planted("#include \"util/json.h\"\n"), 1);
    T_EQ_INT(planted("#  include \"json.h\"\n"), 1);
    T_EQ_INT(planted("cb_jw w; cb_jw_obj(&w);\n"), 2);
    T_EQ_INT(planted("#define K \"{\\\"k\\\": 1}\"\n#define W(x) cb_jw_obj(x)\n"), 2);   // a macro hides nothing
    T_EQ_INT(planted("if (cb_json_find(a, n, \"x\", 0) == CB_JSON_OBJ) {}\n"), 2);
    // Not JSON: a ternary of literals, a comment, the JWT module, a char.
    T_EQ_INT(planted("const char *m = ok ? \"a\":\"b\";\n"), 0);
    T_EQ_INT(planted("// {\"key\": 1} described in a comment\n/* \"k\": */\n"), 0);
    T_EQ_INT(planted("// k = \"\\\"a\\\":1\"; cb_jw_obj(&w);\n/* k = \"\\\"b\\\":2\"; */\n"), 0);   // code in a comment
    T_EQ_INT(planted("cb_jwt_verify(s, t, 0, &c); char q = '\"';\n"), 0);
    T_EQ_INT(planted("#include \"json_surface.h\"\n"), 0);
    // XML: a prolog, a namespace (also across literals); HTML is not XML here.
    T_EQ_INT(planted("const char *x = \"<?xml version=\\\"1.0\\\"?>\";\n"), 1);
    T_EQ_INT(planted("const char *x = \"<Batch xml\" \"ns=\\\"http://a/\\\">\";\n"), 1);
    T_EQ_INT(planted("const char *h = \"<title>%s</title><meta property=og:title>\";\n"), 0);
}

int main(void) {
    T_RUN(the_scanner_sees_each_kind);
    T_RUN(only_json_surface_speaks_json);
    return T_DONE();
}
