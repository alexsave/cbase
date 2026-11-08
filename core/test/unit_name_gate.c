// unit_name_gate.c - the name gate (docs/PLATFORM.md 8.4): each side of the tree
// names nothing on the far side of its line.
//
//   * core (core/src, core/include) and the client (client/src, include, wasm, ts) hit no pattern of any list: no product,
//     no provider, no foolish domain word;
//   * a provider (providers/<p>/src) hits no product and no domain word;
//   * the product (its source roots, CBASE_APP_SRC_ROOTS) hits no provider word.
//
// The lists are tools/name_gate/words.txt (its header says the pattern
// syntax). Every line of every file under those directories is read,
// comments and strings included: a name in a comment is the first step of
// the drift, not an exception to it.
//
// THE ALLOWLISTS hold today's hits, one line per file and pattern: cbase's
// own, tools/name_gate/allow.txt, for its files (provider words only: a line
// allowing a [products] or [domain] pattern is refused), and the product's
// (CBASE_APP_NAME_ALLOW, cbase.mk's APP_NAME_ALLOW) for the product's:
//
//     <count> <path> <pattern, as words.txt writes it>
//
// A cbase path is from cbase's root ("core/src/x.c"), a product path as the
// product spells its roots (lib_tree.h).
//
// The count is the number of lines of that file the pattern hits, and it must
// be exact: a hit beyond it fails (a new name), and so does a count above
// today's (the file got cleaner: lower the count, or delete the line), so the
// list can only shrink. CBASE_NAME_GATE_WRITE=<file> writes the list as the
// tree is today, sorted, for a change that removes names. The gate goes strict
// when the allowlists are empty. It runs from any directory (lib_tree.h).
#include "t.h"
#include "lib_tree.h"

#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define WORDS "tools/name_gate/words.txt"
#define ALLOW "tools/name_gate/allow.txt"

enum { L_PRODUCTS = 1, L_PROVIDERS = 2, L_DOMAIN = 4 };

typedef struct {
    char text[128];   // as words.txt writes it: /source/flags
    char src[128];    // the source between the slashes
    bool icase;
    int  list;
} pattern;

typedef struct {
    char path[256];   // from the repository root
    int  pat;
    int  hits, allowed;
    int  first_line;
    char first[128];
} tally;

static pattern g_pat[128];
static int     g_npat;
static tally  *g_tally;
static int     g_ntally, g_cap;
static int     g_files;

// ---- the matcher: the subset of RegExp words.txt uses -------------------------

static bool word_char(char c) {   // "_" is a break here (the header of words.txt)
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}
static char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c; }

// One atom at re: its length in the pattern, and whether it takes the character c.
static int atom_len(const char *re) { return re[0] == '\\' && re[1] ? 2 : 1; }
static bool atom_takes(const char *re, char c, bool icase) {
    if (re[0] == '\\' && re[1] == 'w') return word_char(c);
    char a = re[0] == '\\' ? re[1] : re[0];
    return icase ? lower(a) == lower(c) : a == c;
}

// Does re match at s[i] (s is n bytes)?
static bool match_here(const char *re, const char *s, size_t n, size_t i, bool icase) {
    if (!*re) return true;
    if (re[0] == '\\' && re[1] == 'b') {
        bool before = i > 0 && word_char(s[i - 1]), after = i < n && word_char(s[i]);
        return before != after && match_here(re + 2, s, n, i, icase);
    }
    int k = atom_len(re);
    char q = re[k];
    if (q == '?') {
        if (i < n && atom_takes(re, s[i], icase) && match_here(re + k + 1, s, n, i + 1, icase)) return true;
        return match_here(re + k + 1, s, n, i, icase);
    }
    if (q == '*') {
        size_t j = i;
        while (j < n && atom_takes(re, s[j], icase)) j++;
        for (;;) {
            if (match_here(re + k + 1, s, n, j, icase)) return true;
            if (j == i) return false;
            j--;
        }
    }
    return i < n && atom_takes(re, s[i], icase) && match_here(re + k, s, n, i + 1, icase);
}

static bool matches(const pattern *p, const char *s, size_t n) {
    for (size_t i = 0; i <= n; i++)
        if (match_here(p->src, s, n, i, p->icase)) return true;
    return false;
}

// ---- the files --------------------------------------------------------------

static char *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long k = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = malloc((size_t)k + 1);
    if (!s || fread(s, 1, (size_t)k, f) != (size_t)k) abort();
    fclose(f);
    s[k] = 0;
    *n = (size_t)k;
    return s;
}

// words.txt into g_pat. Returns false on a line it cannot read.
static bool load_words(void) {
    size_t n;
    char words[TT_PATH_MAX];
    char *s = slurp(tt_path(words, sizeof words, WORDS), &n);
    if (!s) return false;
    int list = 0;
    bool ok = true;
    for (char *line = strtok(s, "\n"); line; line = strtok(NULL, "\n")) {
        if (!*line || *line == '#') continue;
        if (strcmp(line, "[products]") == 0) { list = L_PRODUCTS; continue; }
        if (strcmp(line, "[providers]") == 0) { list = L_PROVIDERS; continue; }
        if (strcmp(line, "[domain]") == 0) { list = L_DOMAIN; continue; }
        char *end = strrchr(line, '/');
        size_t len = strlen(line);
        if (!list || line[0] != '/' || end == line || len >= sizeof g_pat[0].text || g_npat == 128 ||
            !(end[1] == 0 || (end[1] == 'i' && end[2] == 0))) {
            fprintf(stderr, "name gate: words.txt: cannot read: %s\n", line);
            ok = false;
            continue;
        }
        pattern *p = &g_pat[g_npat++];
        snprintf(p->text, sizeof p->text, "%s", line);
        memcpy(p->src, line + 1, (size_t)(end - line - 1));
        p->src[end - line - 1] = 0;
        p->icase = end[1] == 'i';
        p->list = list;
    }
    free(s);
    return ok;
}

static tally *tally_of(const char *path, int pat) {
    for (int i = 0; i < g_ntally; i++)
        if (g_tally[i].pat == pat && strcmp(g_tally[i].path, path) == 0) return &g_tally[i];
    if (g_ntally == g_cap) {
        g_cap = g_cap ? g_cap * 2 : 256;
        g_tally = realloc(g_tally, (size_t)g_cap * sizeof *g_tally);
        if (!g_tally) abort();
    }
    tally *t = &g_tally[g_ntally++];
    memset(t, 0, sizeof *t);
    snprintf(t->path, sizeof t->path, "%s", path);
    t->pat = pat;
    return t;
}

// Count, per pattern of the forbidden lists, the lines of s it hits. With a
// path, into the tallies; returns the total either way.
static int scan_text(const char *path, int forbidden, const char *s, size_t n) {
    int total = 0, line = 1;
    size_t b = 0;
    while (b <= n) {
        size_t e = b;
        while (e < n && s[e] != '\n') e++;
        for (int p = 0; p < g_npat; p++) {
            if (!(g_pat[p].list & forbidden) || !matches(&g_pat[p], s + b, e - b)) continue;
            total++;
            if (!path) continue;
            tally *t = tally_of(path, p);
            if (!t->hits++) {
                t->first_line = line;
                size_t k = e - b < sizeof t->first - 1 ? e - b : sizeof t->first - 1;
                memcpy(t->first, s + b, k);
                t->first[k] = 0;
            }
        }
        b = e + 1;
        line++;
    }
    return total;
}

// Every file under dir (opened as `dir`), each named `name`/<the rest of its path>.
static void scan_named(const char *dir, const char *name, int forbidden) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char p[TT_PATH_MAX + 256], q[TT_PATH_MAX + 256];
        if (snprintf(p, sizeof p, "%s/%s", dir, e->d_name) >= (int)sizeof p) continue;   // no path is that long
        if (snprintf(q, sizeof q, "%s/%s", name, e->d_name) >= (int)sizeof q) continue;
        struct stat st;
        if (stat(p, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) { scan_named(p, q, forbidden); continue; }
        size_t n;
        char *s = slurp(p, &n);
        if (!s) continue;
        g_files++;
        scan_text(q, forbidden, s, n);
        free(s);
    }
    closedir(d);
}

// A cbase directory, from cbase's root.
static void scan_dir(const char *rel, int forbidden) {
    char p[TT_PATH_MAX];
    scan_named(tt_path(p, sizeof p, rel), rel, forbidden);
}

// The sides and what each may not say.
static void scan_tree(void) {
    scan_dir("core/src", L_PRODUCTS | L_PROVIDERS | L_DOMAIN);
    scan_dir("core/include", L_PRODUCTS | L_PROVIDERS | L_DOMAIN);
    // The client (client/) is the other half of the platform, held to core's rule.
    scan_dir("client/include", L_PRODUCTS | L_PROVIDERS | L_DOMAIN);
    scan_dir("client/src", L_PRODUCTS | L_PROVIDERS | L_DOMAIN);
    scan_dir("client/wasm", L_PRODUCTS | L_PROVIDERS | L_DOMAIN);
    scan_dir("client/ts", L_PRODUCTS | L_PROVIDERS | L_DOMAIN);
    char prov[TT_PATH_MAX];
    DIR *d = opendir(tt_path(prov, sizeof prov, "providers"));
    struct dirent *e;
    while (d && (e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char p[300];
        if (snprintf(p, sizeof p, "providers/%s/src", e->d_name) >= (int)sizeof p) continue;
        scan_dir(p, L_PRODUCTS | L_DOMAIN);
    }
    if (d) closedir(d);
    // The product's sources, as the product names them.
    char roots[TT_ROOTS_MAX][TT_PATH_MAX];
    int n = tt_env_list("CBASE_APP_SRC_ROOTS", roots, TT_ROOTS_MAX);
    for (int i = 0; i < n; i++) scan_named(roots[i], roots[i], L_PROVIDERS);
}

// An allowlist into the tallies' allowed counts. Returns the number of lines it
// could not read (a pattern not in words.txt is one), and 1 for no such file.
static int load_allow(const char *path, int refused_lists) {
    size_t n;
    char *s = slurp(path, &n);
    if (!s) {
        fprintf(stderr, "name gate: cannot read the allowlist %s\n", path);
        return 1;
    }
    int bad = 0;
    for (char *line = strtok(s, "\n"); line; line = strtok(NULL, "\n")) {
        if (!*line || *line == '#') continue;
        char *sp1 = strchr(line, ' '), *sp2 = sp1 ? strchr(sp1 + 1, ' ') : NULL;
        int count = atoi(line);   // a test reading its own fixture, not shipped text
        int pat = -1;
        if (sp2)
            for (int p = 0; p < g_npat; p++)
                if (strcmp(g_pat[p].text, sp2 + 1) == 0) pat = p;
        if (count <= 0 || pat < 0 || (size_t)(sp2 - sp1 - 1) >= sizeof g_tally[0].path) {
            fprintf(stderr, "name gate: allow.txt: cannot read: %s\n", line);
            bad++;
            continue;
        }
        if (g_pat[pat].list & refused_lists) {
            fprintf(stderr, "name gate: %s may not allow %s: no file of its side may name it\n", path, g_pat[pat].text);
            bad++;
            continue;
        }
        *sp2 = 0;
        tally *t = tally_of(sp1 + 1, pat);
        if (t->allowed) { fprintf(stderr, "name gate: allow.txt: twice: %s %s\n", sp1 + 1, g_pat[pat].text); bad++; }
        t->allowed = count;
    }
    free(s);
    return bad;
}

// A path under a provider whose directory is not in this tree: a product that vendors core and
// providers/local alone has no providers/aws, so the allowlist's lines for it are not stale, they
// are about files this tree does not carry. Where the provider is present, a line for a file it no
// longer has is stale as ever.
static bool provider_absent(const char *path) {
    static const char pre[] = "providers/";
    if (strncmp(path, pre, sizeof pre - 1) != 0) return false;
    const char *slash = strchr(path + sizeof pre - 1, '/');
    char dir[TT_PATH_MAX], p[TT_PATH_MAX];
    if (!slash || (size_t)(slash - path) >= sizeof dir) return false;
    memcpy(dir, path, (size_t)(slash - path));
    dir[slash - path] = 0;
    struct stat st;
    return stat(tt_path(p, sizeof p, dir), &st) != 0;
}

static int cmp_tally(const void *a, const void *b) {
    const tally *x = a, *y = b;
    int c = strcmp(x->path, y->path);
    return c ? c : x->pat - y->pat;
}

static void write_allow(const char *out) {
    qsort(g_tally, (size_t)g_ntally, sizeof *g_tally, cmp_tally);
    FILE *f = fopen(out, "w");
    if (!f) return;
    fprintf(f, "# allow.txt - the name gate's shrinking allowlist (core/test/unit_name_gate.c says the format).\n"
               "# <count> <path> <pattern>: the lines of that file the pattern hits today. Only ever lowered.\n");
    for (int i = 0; i < g_ntally; i++)
        if (g_tally[i].hits) fprintf(f, "%d %s %s\n", g_tally[i].hits, g_tally[i].path, g_pat[g_tally[i].pat].text);
    fclose(f);
}

// ---- the tests --------------------------------------------------------------

static int planted(int forbidden, const char *text) { return scan_text(NULL, forbidden, text, strlen(text)); }

// The matcher and the side rules against planted text, so a gate that stopped
// seeing would fail here too.
static void the_matcher_sees_each_shape(void) {
    T_CHECK(g_npat >= 40);
    // \b at both ends, with "_" a break: an identifier's word is a word.
    T_EQ_INT(planted(L_DOMAIN, "cb_rows_bots_replace(s);"), 1);
    T_EQ_INT(planted(L_DOMAIN, "both robots"), 0);
    T_EQ_INT(planted(L_DOMAIN, "the game"), 1);
    T_EQ_INT(planted(L_DOMAIN, "endgame, gamely"), 0);
    // ? and case: /pick ?'?em ?up/i over three spellings, /\bFoolish\w*/ only in its case.
    T_EQ_INT(planted(L_PRODUCTS, "Pick 'Em Up\npick em up\npickemup\npick up"), 4);   // pickemup: /pickemup/i too
    T_EQ_INT(planted(L_PRODUCTS, "FoolishKit"), 2);         // the any-case pattern and the capitalized one
    T_EQ_INT(planted(L_PRODUCTS, "foolishkit"), 1);         // the any-case pattern alone
    // An escaped punctuation character is itself: /cards\.foolish/i, /pub\/sub/i.
    T_EQ_INT(planted(L_PRODUCTS, "cards.foolish.msg"), 2);  // and the any-case pattern
    T_EQ_INT(planted(L_PROVIDERS, "a pub/sub topic\na pub-sub topic"), 1);
    // A pattern counts one per line, however often the line says it.
    T_EQ_INT(planted(L_PROVIDERS, "aws_call(aws, AWS)"), 1);
    T_EQ_INT(planted(L_PROVIDERS, "draws laws"), 0);
    // The side rules: a provider word is no product's word, and so on.
    T_EQ_INT(planted(L_PRODUCTS | L_DOMAIN, "store_dynamo.c over sqlite"), 0);
    T_EQ_INT(planted(L_PROVIDERS, "store_dynamo.c over sqlite"), 2);
    T_EQ_INT(planted(L_PROVIDERS, "the game's seat"), 0);
}

// The gate itself: the tree as it is, against the allowlist.
static void each_side_names_only_its_own(void) {
    struct stat st;
    char core[TT_PATH_MAX];
    T_CHECK(stat(tt_path(core, sizeof core, "core/src"), &st) == 0);   // CBASE_ROOT is cbase, or nothing is scanned
    scan_tree();
    T_CHECK(g_files >= 100);                // a gate that read nothing proves nothing
    const char *out = getenv("CBASE_NAME_GATE_WRITE");
    if (out && *out) { write_allow(out); printf("name gate: wrote %s\n", out); }
    char allow[TT_PATH_MAX];
    // cbase's own allowlist is strict for the product and domain lists: the platform names no
    // product and no product's nouns, comments included, so no line may allow one.
    T_EQ_INT(load_allow(tt_path(allow, sizeof allow, ALLOW), L_PRODUCTS | L_DOMAIN), 0);
    const char *app_allow = getenv("CBASE_APP_NAME_ALLOW");
    if (app_allow && *app_allow) T_EQ_INT(load_allow(app_allow, 0), 0);
    int new_hits = 0, stale = 0;
    for (int i = 0; i < g_ntally; i++) {
        tally *t = &g_tally[i];
        if (t->hits > t->allowed) {
            new_hits++;
            fprintf(stderr, "name gate: %s: %d line(s) hit %s, %d allowed; the first is %d: %s\n", t->path, t->hits,
                    g_pat[t->pat].text, t->allowed, t->first_line, t->first);
        } else if (t->hits < t->allowed && !provider_absent(t->path)) {
            stale++;
            fprintf(stderr, "name gate: %s hits %s on %d line(s) now, and allow.txt still allows %d: lower it%s\n",
                    t->path, g_pat[t->pat].text, t->hits, t->allowed, t->hits ? "" : " (delete the line)");
        }
    }
    T_EQ_INT(new_hits, 0);
    T_EQ_INT(stale, 0);
}

int main(void) {
    T_CHECK(load_words());
    T_RUN(the_matcher_sees_each_shape);
    T_RUN(each_side_names_only_its_own);
    free(g_tally);
    return T_DONE();
}
