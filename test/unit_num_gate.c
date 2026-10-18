// unit_num_gate.c - the number-parsing gate (shared/c/num_parse.h): every decimal
// integer c_base reads from text goes through num_parse_i64. This test reads
// every C source under core/, providers/ and ../foolish/server/ and fails, naming the file and line, when one
// names a C library integer parser:
//
//   * strtol, strtoll, strtoul, strtoull, strtoimax, strtoumax: a leading
//     space and '+' taken, a '-' that the unsigned ones wrap around, and the
//     end pointer easily ignored,
//   * atoi, atol, atoll: the same with no way to see an error at all,
//   * any scanf (sscanf, fscanf, ...): %d and friends have strtol's rules.
//
// The floating-point parsers (strtod, strtof, strtold) are allowed in one
// file, providers/aws/src/aws_json.c, which reads the one fractional number c_base
// takes (DynamoDB's CapacityUnits); anywhere else they are a violation too.
//
// Comments, string literals and character literals are skipped, so a comment
// may name strtol. It runs from c_base/ (make test's directory).
#include "t.h"

#include <ctype.h>
#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
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

static bool ident_char(char c) { return isalnum((unsigned char)c) || c == '_'; }

static void scan_text(const char *path, const char *s, size_t n) {
    bool float_ok = strcmp(path, FLOAT_HOME) == 0;
    int line = 1;
    size_t i = 0;
    while (i < n) {
        char c = s[i];
        if (c == '\n') { line++; i++; continue; }
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
        if (c == '"' || c == '\'') {
            i++;
            while (i < n && s[i] != c) {
                if (s[i] == '\n') line++;
                i += s[i] == '\\' ? 2 : 1;
            }
            i++;
            continue;
        }
        if (isalpha((unsigned char)c) || c == '_') {
            size_t b = i;
            while (i < n && ident_char(s[i])) i++;
            if (in(INTEGER, sizeof INTEGER / sizeof INTEGER[0], s + b, i - b) ||
                (!float_ok && in(FLOATING, sizeof FLOATING / sizeof FLOATING[0], s + b, i - b)))
                violation(path, line, s + b, i - b);
            continue;
        }
        if (isdigit((unsigned char)c)) {   // a number's suffix is not an identifier
            while (i < n && ident_char(s[i])) i++;
            continue;
        }
        i++;
    }
}

static void scan_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { violation(path, 0, "(cannot be read)", 16); return; }
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
        if (L > 2 && p[L - 2] == '.' && (p[L - 1] == 'c' || p[L - 1] == 'h')) scan_file(p);
    }
    closedir(d);
}

// The gate itself: the tree as it is.
static void only_num_parse_i64_reads_numbers(void) {
    struct stat st;
    T_CHECK(stat("core/src/config.c", &st) == 0);   // run from c_base/, or nothing is scanned
    T_CHECK(stat(FLOAT_HOME, &st) == 0);              // the float exemption names a real file
    scan_dir("core");
    scan_dir("providers");
    scan_dir("../foolish/server");
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
