// lib_tree.c - see lib_tree.h.
#include "lib_tree.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *tt_root(void) {
    const char *r = getenv("CBASE_ROOT");
    return r && *r ? r : ".";
}

const char *tt_path(char *out, size_t cap, const char *rel) {
    const char *r = tt_root();
    if (strcmp(r, ".") == 0) snprintf(out, cap, "%s", rel);
    else snprintf(out, cap, "%s/%s", r, rel);
    return out;
}

int tt_env_list(const char *name, char roots[][TT_PATH_MAX], int max) {
    const char *s = getenv(name);
    int n = 0;
    while (s && *s && n < max) {
        while (*s == ' ' || *s == '\t') s++;
        size_t k = strcspn(s, " \t");
        if (k == 0) break;
        if (k < TT_PATH_MAX) {
            memcpy(roots[n], s, k);
            roots[n][k] = 0;
            n++;
        }
        s += k;
    }
    return n;
}
