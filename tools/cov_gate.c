// cov_gate.c - c_base's coverage gate and per-function report (docs/COVERAGE.md).
//
//   build/cov_gate <coverage.lcov> <floor.txt> [--report]
//
// Reads the LCOV tracefile gcovr writes from the coverage build (`make
// coverage`) and holds it against the floor file:
//
//   line   <percent>          the whole measured scope's line coverage may not fall below this
//   branch <percent>          nor its branch coverage
//   zero <file> <function> <reason...>
//                             a function allowed to run zero times in the suites;
//                             every OTHER function with a body that is never
//                             entered fails the gate
//
// So coverage can only go up: raise the floors when it climbs, and a new
// function nothing calls fails CI until a test calls it (or the floor file
// says, in words, why nothing can).
//
// A function's lines are the executable lines from its first line up to the
// next function's first line in the same file (LCOV names only where a
// function starts). Lines are DA records, branches BRDA records whose count is
// not "-" (a branch never reached is counted as not taken).
//
// --report prints, for every file, its line and branch coverage, then every
// function under 100% line coverage with its own numbers, worst first within
// the file. Exit status: 0 the gate holds, 1 it does not, 2 bad input.
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { int line; long hits; char name[128]; int lt, lh, bt, bh; } fn_t;
typedef struct { char path[512]; int lt, lh, bt, bh; fn_t *fn; int nfn, capfn;
                 int *dline; long *dhit; int nd, capd;          // DA
                 int *bline; bool *btaken; int nb, capb; } file_t;  // BRDA

static file_t *g_files;
static int g_nfiles, g_capfiles;

#define GROW(p, n, cap) do { if ((n) == (cap)) { (cap) = (cap) ? 2 * (cap) : 64; \
    void *q_ = realloc((p), (size_t)(cap) * sizeof *(p)); if (!q_) { perror("realloc"); exit(2); } (p) = q_; } } while (0)

static int by_line(const void *a, const void *b) { return ((const fn_t *)a)->line - ((const fn_t *)b)->line; }

// Path relative to c_base ("core/src/...", "providers/aws/src/..."), or from
// "foolish/" on for the kernel and the product.
static const char *short_path(const char *p) {
    const char *f = strstr(p, "/foolish/c/src/");
    if (f) return f + 1;
    f = strstr(p, "/foolish/server/src/");
    if (f) return f + 1;
    if (!strncmp(p, "../foolish/", 11)) return p + 3;
    const char *s = strstr(p, "/c_base/");
    if (s) return s + 8;
    return p;
}

static int read_lcov(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); return -1; }
    char line[4096];
    file_t *cur = NULL;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "SF:", 3)) {
            GROW(g_files, g_nfiles, g_capfiles);
            cur = &g_files[g_nfiles++];
            memset(cur, 0, sizeof *cur);
            snprintf(cur->path, sizeof cur->path, "%.*s", (int)sizeof cur->path - 1, short_path(line + 3));
        } else if (!cur) {
            continue;
        } else if (!strncmp(line, "FN:", 3)) {
            char *comma = strchr(line + 3, ',');
            if (!comma) continue;
            // gcovr writes FN:<start>,<name>; lcov 2 may write FN:<start>,<end>,<name>.
            char *name = strrchr(line, ',') + 1;
            GROW(cur->fn, cur->nfn, cur->capfn);
            fn_t *x = &cur->fn[cur->nfn++];
            memset(x, 0, sizeof *x);
            x->line = atoi(line + 3);
            snprintf(x->name, sizeof x->name, "%s", name);
        } else if (!strncmp(line, "FNDA:", 5)) {
            char *comma = strchr(line + 5, ',');
            if (!comma) continue;
            for (int i = 0; i < cur->nfn; i++)
                if (!strcmp(cur->fn[i].name, comma + 1)) cur->fn[i].hits += atol(line + 5);
        } else if (!strncmp(line, "DA:", 3)) {
            char *comma = strchr(line + 3, ',');
            if (!comma) continue;
            GROW(cur->dline, cur->nd, cur->capd);
            cur->dhit = realloc(cur->dhit, (size_t)cur->capd * sizeof *cur->dhit);
            if (!cur->dhit) { perror("realloc"); exit(2); }
            cur->dline[cur->nd] = atoi(line + 3);
            cur->dhit[cur->nd++] = atol(comma + 1);
        } else if (!strncmp(line, "BRDA:", 5)) {
            // BRDA:<line>,<block>,<branch>,<taken or ->
            char *last = strrchr(line, ',');
            if (!last) continue;
            GROW(cur->bline, cur->nb, cur->capb);
            cur->btaken = realloc(cur->btaken, (size_t)cur->capb * sizeof *cur->btaken);
            if (!cur->btaken) { perror("realloc"); exit(2); }
            cur->bline[cur->nb] = atoi(line + 5);
            cur->btaken[cur->nb++] = last[1] != '-' && atol(last + 1) > 0;
        }
    }
    fclose(f);
    return 0;
}

// Attributes each DA and BRDA record to the function whose range holds it.
static void tally(file_t *fl) {
    qsort(fl->fn, (size_t)fl->nfn, sizeof *fl->fn, by_line);
    for (int i = 0; i < fl->nd; i++) {
        fl->lt++;
        fl->lh += fl->dhit[i] > 0;
        for (int k = fl->nfn - 1; k >= 0; k--)
            if (fl->fn[k].line <= fl->dline[i]) { fl->fn[k].lt++; fl->fn[k].lh += fl->dhit[i] > 0; break; }
    }
    for (int i = 0; i < fl->nb; i++) {
        fl->bt++;
        fl->bh += fl->btaken[i];
        for (int k = fl->nfn - 1; k >= 0; k--)
            if (fl->fn[k].line <= fl->bline[i]) { fl->fn[k].bt++; fl->fn[k].bh += fl->btaken[i]; break; }
    }
}

static double pct(int h, int t) { return t ? 100.0 * h / t : 100.0; }

typedef struct { char file[512]; char fn[128]; bool used; } zero_t;

static int fn_cmp(const void *a, const void *b) {
    const fn_t *x = a, *y = b;
    double dx = pct(x->lh, x->lt), dy = pct(y->lh, y->lt);
    return dx < dy ? -1 : dx > dy ? 1 : x->line - y->line;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: cov_gate <coverage.lcov> <floor.txt> [--report]\n"); return 2; }
    bool report = argc > 3 && !strcmp(argv[3], "--report");
    if (read_lcov(argv[1]) != 0) return 2;

    double floor_line = -1, floor_branch = -1;
    zero_t *zero = NULL;
    int nzero = 0, capzero = 0;
    FILE *ff = fopen(argv[2], "r");
    if (!ff) { perror(argv[2]); return 2; }
    char line[1024];
    for (int ln = 1; fgets(line, sizeof line, ff); ln++) {
        char *p = line;
        while (isspace((unsigned char)*p)) p++;
        if (!*p || *p == '#') continue;
        char a[64], b[512], c[128];
        if (sscanf(p, "line %lf", &floor_line) == 1) continue;
        if (sscanf(p, "branch %lf", &floor_branch) == 1) continue;
        if (sscanf(p, "%63s %511s %127s", a, b, c) == 3 && !strcmp(a, "zero")) {
            GROW(zero, nzero, capzero);
            snprintf(zero[nzero].file, sizeof zero[nzero].file, "%s", b);
            snprintf(zero[nzero].fn, sizeof zero[nzero].fn, "%s", c);
            zero[nzero++].used = false;
            continue;
        }
        fprintf(stderr, "%s:%d: not a floor line: %s", argv[2], ln, line);
        return 2;
    }
    fclose(ff);
    if (floor_line < 0 || floor_branch < 0) { fprintf(stderr, "%s: needs a line and a branch floor\n", argv[2]); return 2; }
    if (g_nfiles == 0) { fprintf(stderr, "%s: no files: nothing was measured\n", argv[1]); return 2; }

    int lt = 0, lh = 0, bt = 0, bh = 0, nfn = 0, nzero_fn = 0, nunder = 0;
    bool ok = true;
    for (int i = 0; i < g_nfiles; i++) {
        file_t *fl = &g_files[i];
        tally(fl);
        lt += fl->lt; lh += fl->lh; bt += fl->bt; bh += fl->bh;
        if (report) printf("%-40s lines %6.2f%% (%d/%d)  branches %6.2f%% (%d/%d)\n", fl->path,
                           pct(fl->lh, fl->lt), fl->lh, fl->lt, pct(fl->bh, fl->bt), fl->bh, fl->bt);
        fn_t *sorted = malloc((size_t)(fl->nfn ? fl->nfn : 1) * sizeof *sorted);
        if (!sorted) return 2;
        memcpy(sorted, fl->fn, (size_t)fl->nfn * sizeof *sorted);
        qsort(sorted, (size_t)fl->nfn, sizeof *sorted, fn_cmp);
        for (int k = 0; k < fl->nfn; k++) {
            fn_t *x = &sorted[k];
            nfn++;
            if (x->lt == 0) continue;   // no executable line of its own (folded away)
            if (x->lh < x->lt) nunder++;
            if (report && x->lh < x->lt)
                printf("    %-36s :%-5d lines %6.2f%% (%d/%d)  branches %6.2f%% (%d/%d)%s\n", x->name, x->line,
                       pct(x->lh, x->lt), x->lh, x->lt, pct(x->bh, x->bt), x->bh, x->bt, x->hits ? "" : "  NEVER CALLED");
            if (x->hits || x->lh) continue;
            nzero_fn++;
            bool allowed = false;
            for (int z = 0; z < nzero; z++)
                if (!strcmp(zero[z].file, fl->path) && !strcmp(zero[z].fn, x->name)) { allowed = zero[z].used = true; }
            if (!allowed) {
                fprintf(stderr, "cov_gate: %s %s (line %d, %d lines) is never called by any test\n", fl->path, x->name, x->line, x->lt);
                ok = false;
            }
        }
        free(sorted);
    }
    for (int z = 0; z < nzero; z++)
        if (!zero[z].used)
            fprintf(stderr, "cov_gate: note: %s %s is listed as allowed at zero but is covered (or gone): delete its line\n",
                    zero[z].file, zero[z].fn);

    printf("cov_gate: %d files, %d functions (%d never called, %d under 100%% of lines)\n", g_nfiles, nfn, nzero_fn, nunder);
    printf("cov_gate: lines %.2f%% (%d/%d), floor %.2f%%\n", pct(lh, lt), lh, lt, floor_line);
    printf("cov_gate: branches %.2f%% (%d/%d), floor %.2f%%\n", pct(bh, bt), bh, bt, floor_branch);
    // Floors compare at two decimals, the way they are written.
    if ((long)(pct(lh, lt) * 100) < (long)(floor_line * 100 + 0.5)) { fprintf(stderr, "cov_gate: line coverage fell below its floor\n"); ok = false; }
    if ((long)(pct(bh, bt) * 100) < (long)(floor_branch * 100 + 0.5)) { fprintf(stderr, "cov_gate: branch coverage fell below its floor\n"); ok = false; }
    printf("cov_gate: %s\n", ok ? "holds" : "FAILED");
    return ok ? 0 : 1;
}
