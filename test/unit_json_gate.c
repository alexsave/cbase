// unit_json_gate.c - the JSON containment gate (DESIGN.md "Where JSON is
// tolerated"): every JSON shape c_base speaks lives in src/json_surface.c, on
// top of the one low-level writer and reader, src/util/json.{c,h}. This test reads
// every C source under src/, bench/ and migrate/ and fails, naming the file
// and line, when any other file
//
//   * includes util/json.h (by any spelling ending in /json.h or json.h),
//   * names the writer or reader: an identifier cb_jw, cb_jw_*, cb_json_*,
//     CB_JW_* or CB_JSON_* (cb_jwt_* is the JWT module, not JSON), or
//   * holds a string literal with a JSON key in it: a quoted word followed by
//     a colon ("key": in the literal's text, \"key\": in the source), or
//   * holds a string literal that is XML: an "<?xml" prolog or an "xmlns"
//     namespace. CloudFront's API is XML (json_surface.h "CloudFront"); the
//     same containment holds for it, and the unfurl route's HTML (which has
//     neither) is not XML to this gate.
//
// Comments are skipped, so a comment may describe a shape; adjacent literals
// are joined first, as the compiler joins them, so a key cannot hide across
// two of them. It runs from c_base/ (make test's directory).
#include "t.h"

#include <ctype.h>
#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

// The files allowed to speak JSON: the surface and the mechanism under it.
static const char *const ALLOWED[] = { "src/json_surface.c", "src/util/json.c", "src/util/json.h" };
static const char *const ROOTS[] = { "src", "bench", "migrate" };

static int  g_files, g_violations;
static bool g_quiet;   // planted text: counted, not printed

static bool allowed(const char *path) {
    for (size_t i = 0; i < sizeof ALLOWED / sizeof ALLOWED[0]; i++)
        if (strcmp(path, ALLOWED[i]) == 0) return true;
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

static void scan_text(const char *path, const char *s, size_t n) {
    int line = 1;
    bool bol = true;   // only blanks since the line began (a # here is a directive)
    lit l = { 0 };
    size_t i = 0;
    while (i < n) {
        char c = s[i];
        if (c == '\n') { line++; bol = true; i++; continue; }
        if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') { i++; continue; }
        // Comments: skipped, and they do not end a run of adjacent literals.
        if (c == '/' && i + 1 < n && s[i + 1] == '/') {
            while (i < n && s[i] != '\n') i++;
            continue;
        }
        if (c == '/' && i + 1 < n && s[i + 1] == '*') {
            i += 2;
            while (i + 1 < n && !(s[i] == '*' && s[i + 1] == '/')) { if (s[i] == '\n') line++; i++; }
            i += 2;
            continue;
        }
        if (c == '#' && bol) {
            // A directive: check an include, then skip the line (with its continuations).
            size_t e = i;
            while (e < n && s[e] != '\n') { if (s[e] == '\\' && e + 1 < n && s[e + 1] == '\n') { e++; line++; } e++; }
            const char *d = s + i + 1;
            while (d < s + e && (*d == ' ' || *d == '\t')) d++;
            if (s + e - d >= 7 && memcmp(d, "include", 7) == 0) {
                const char *q = memchr(d, '"', (size_t)(s + e - d));
                const char *a = memchr(d, '<', (size_t)(s + e - d));
                const char *o = q ? q : a;
                if (o) {
                    const char *close = memchr(o + 1, q ? '"' : '>', (size_t)(s + e - o - 1));
                    if (close && close - (o + 1) >= 6 && memcmp(close - 6, "json.h", 6) == 0 &&
                        (close - (o + 1) == 6 || close[-7] == '/'))
                        violation(path, line, "includes util/json.h", o, (size_t)(close - o + 1));
                }
            }
            i = e;
            continue;
        }
        bol = false;
        if (c == '"') {
            // A string literal, unescaped into l (\" and \\ only: enough to see quotes).
            if (!l.n) l.line = line;
            i++;
            while (i < n && s[i] != '"') {
                if (s[i] == '\\' && i + 1 < n) {
                    char x = s[i + 1];
                    if (x == '\n') { line++; i += 2; continue; }
                    lit_put(&l, x == '"' || x == '\\' ? x : '\\');
                    if (x != '"' && x != '\\') lit_put(&l, x);
                    i += 2;
                    continue;
                }
                if (s[i] == '\n') line++;
                lit_put(&l, s[i++]);
            }
            i++;
            continue;
        }
        // Anything else ends a run of adjacent literals.
        lit_flush(&l, path);
        if (c == '\'') {
            i++;
            while (i < n && s[i] != '\'') i += s[i] == '\\' ? 2 : 1;
            i++;
            continue;
        }
        if (isalpha((unsigned char)c) || c == '_') {
            size_t b = i;
            while (i < n && ident_char(s[i])) i++;
            if (json_ident(s + b, i - b)) violation(path, line, "names the JSON writer or reader", s + b, i - b);
            continue;
        }
        i++;
    }
    lit_flush(&l, path);
    free(l.p);
}

static void scan_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { violation(path, 0, "cannot be read", "", 0); return; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = malloc((size_t)n + 1);
    if (!s || fread(s, 1, (size_t)n, f) != (size_t)n) abort();
    fclose(f);
    g_files++;
    scan_text(path, s, (size_t)n);
    free(s);
}

static void scan_dir(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char p[1024];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(p, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) { scan_dir(p); continue; }
        size_t L = strlen(p);
        bool c_source = L > 2 && p[L - 2] == '.' && (p[L - 1] == 'c' || p[L - 1] == 'h');
        if (c_source && !allowed(p)) scan_file(p);
    }
    closedir(d);
}

// The gate itself: the tree as it is.
static void only_json_surface_speaks_json(void) {
    struct stat st;
    T_CHECK(stat(ALLOWED[0], &st) == 0);   // run from c_base/, or nothing is scanned
    for (size_t i = 0; i < sizeof ROOTS / sizeof ROOTS[0]; i++) scan_dir(ROOTS[i]);
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
    T_EQ_INT(planted("if (cb_json_find(a, n, \"x\", 0) == CB_JSON_OBJ) {}\n"), 2);
    // Not JSON: a ternary of literals, a comment, the JWT module, a char.
    T_EQ_INT(planted("const char *m = ok ? \"a\":\"b\";\n"), 0);
    T_EQ_INT(planted("// {\"key\": 1} described in a comment\n/* \"k\": */\n"), 0);
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
