// lib_cscan.c - see lib_cscan.h.
#include "lib_cscan.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static bool ident_char(char c) { return isalnum((unsigned char)c) || c == '_'; }

static void emit(cscan_tok_fn fn, void *ctx, cscan_kind k, const char *s, size_t n, int line) {
    cscan_tok t = { k, s, n, line };
    fn(&t, ctx);
}

void cscan_lex(const char *s, size_t n, cscan_tok_fn fn, void *ctx) {
    int line = 1;
    bool bol = true;   // only blanks since the line began (a # here is a directive)
    size_t i = 0;
    while (i < n) {
        char c = s[i];
        if (c == '\n') { line++; bol = true; i++; continue; }
        if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') { i++; continue; }
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
            size_t b = i + 1;
            while (b < n && (s[b] == ' ' || s[b] == '\t')) b++;
            if (n - b >= 7 && memcmp(s + b, "include", 7) == 0 && (n - b == 7 || !ident_char(s[b + 7]))) {
                // The header spec, to the end of the line and its continuations.
                int at = line;
                size_t e = b + 7;
                while (e < n && s[e] != '\n') {
                    if (s[e] == '\\' && e + 1 < n && s[e + 1] == '\n') { e++; line++; }
                    e++;
                }
                emit(fn, ctx, CSCAN_INCLUDE, s + b + 7, e - (b + 7), at);
                i = e;
                continue;
            }
        }
        bol = false;
        if (c == '"' || c == '\'') {
            size_t b = ++i;
            int at = line;
            while (i < n && s[i] != c) {
                if (s[i] == '\\' && i + 1 < n) {
                    if (s[i + 1] == '\n') line++;
                    i += 2;
                    continue;
                }
                if (s[i] == '\n') line++;
                i++;
            }
            emit(fn, ctx, c == '"' ? CSCAN_STRING : CSCAN_CHAR, s + b, (i < n ? i : n) - b, at);
            i++;
            continue;
        }
        if (isalpha((unsigned char)c) || c == '_') {
            size_t b = i;
            while (i < n && ident_char(s[i])) i++;
            emit(fn, ctx, CSCAN_IDENT, s + b, i - b, line);
            continue;
        }
        if (isdigit((unsigned char)c)) {
            size_t b = i;
            while (i < n && ident_char(s[i])) i++;
            emit(fn, ctx, CSCAN_NUMBER, s + b, i - b, line);
            continue;
        }
        emit(fn, ctx, CSCAN_PUNCT, s + i, 1, line);
        i++;
    }
}

char *cscan_slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long k = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = k >= 0 ? malloc((size_t)k + 1) : NULL;
    if (!s || fread(s, 1, (size_t)k, f) != (size_t)k) abort();
    fclose(f);
    s[k] = 0;
    *n = (size_t)k;
    return s;
}

static bool skipped_dir(const cscan_walker *w, const char *d) {
    for (const char *const *p = w->skip_dirs; p && *p; p++)
        if (strcmp(*p, d) == 0) return true;
    return false;
}

int cscan_walk(const cscan_walker *w, const char *dir, const char *name) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    int files = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char p[1024], q[1024];
        if (snprintf(p, sizeof p, "%s/%s", dir, e->d_name) >= (int)sizeof p) continue;   // no path is that long
        if (snprintf(q, sizeof q, "%s/%s", name, e->d_name) >= (int)sizeof q) continue;
        struct stat st;
        if (stat(p, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (!skipped_dir(w, e->d_name)) files += cscan_walk(w, p, q);
            continue;
        }
        size_t L = strlen(q);
        bool c_source = L > 2 && q[L - 2] == '.' && (q[L - 1] == 'c' || q[L - 1] == 'h');
        if (!(w->all_files || c_source) || (w->skip && w->skip(q, w->ctx))) continue;
        size_t n;
        char *s = cscan_slurp(p, &n);
        if (!s) {
            if (w->unreadable) w->unreadable(q, w->ctx);
            continue;
        }
        files++;
        w->file(q, s, n, w->ctx);
        free(s);
    }
    closedir(d);
    return files;
}
