// The primitives both generators are written with. One copy, because the one
// thing worse than the same eight lines twice is the same FIX applied to one of
// them (xstrdup below is a repeat offender).
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "sgc.h"

void die(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fputs(SGC_TOOL ": ", stderr); vfprintf(stderr, fmt, ap); fputc('\n', stderr);
    va_end(ap); exit(1);
}

// ---- growable string -----------------------------------------------------
void bprintf(Buf *b, const char *fmt, ...) {
    for (;;) {
        va_list ap; va_start(ap, fmt);
        size_t room = b->cap - b->n;
        int k = vsnprintf(b->s ? b->s + b->n : NULL, room, fmt, ap);
        va_end(ap);
        if (k >= 0 && (size_t)k < room) { b->n += (size_t)k; return; }
        b->cap = b->cap * 2 + (size_t)k + 256;
        if (!(b->s = realloc(b->s, b->cap))) die("out of memory");
    }
}
// Not strdup: under -std=c11 glibc does not declare it, and an implicit int
// return truncates the pointer on a 64-bit Linux host.
char *xstrdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (!d) die("out of memory");
    return memcpy(d, s, n);
}

const char *absolute(const char *path) {
    if (!path || path[0] == '/') return path;
    char dir[4096];
    if (!getcwd(dir, sizeof dir)) die("cannot read the working directory");
    char *abs = malloc(strlen(dir) + strlen(path) + 2);
    if (!abs) die("out of memory");
    return strcat(strcat(strcpy(abs, dir), "/"), path);
}
