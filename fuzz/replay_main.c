// replay_main.c - a main for a fuzz target without libFuzzer: runs
// LLVMFuzzerTestOneInput once on every file named, or on every file in every
// directory named (the checked-in corpus, a crash reproducer). Built under the
// ordinary ASan + UBSan variant, so `make fuzz-replay` holds every corpus
// entry to the sanitizers on any compiler (fz.h).
#include "fz.h"

#include <dirent.h>
#include <sys/stat.h>

__attribute__((weak)) int LLVMFuzzerInitialize(int *argc, char ***argv) { return 0; }

static int run_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "replay: cannot open %s\n", path); return 1; }
    uint8_t *buf = NULL;
    size_t n = 0, cap = 0;
    for (;;) {
        if (n == cap) {
            cap = cap ? 2 * cap : 4096;
            buf = realloc(buf, cap);
            if (!buf) abort();
        }
        size_t k = fread(buf + n, 1, cap - n, f);
        if (k == 0) break;
        n += k;
    }
    fclose(f);
    // An exact-size copy, as libFuzzer hands it, so a read past the end is caught.
    uint8_t *exact = malloc(n ? n : 1);
    if (!exact) abort();
    memcpy(exact, buf, n);
    free(buf);
    LLVMFuzzerTestOneInput(exact, n);
    free(exact);
    return 0;
}

static int by_name(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

int main(int argc, char **argv) {
    LLVMFuzzerInitialize(&argc, &argv);
    int files = 0, bad = 0;
    for (int i = 1; i < argc; i++) {
        struct stat st;
        if (stat(argv[i], &st) != 0) { fprintf(stderr, "replay: no %s\n", argv[i]); return 1; }
        if (!S_ISDIR(st.st_mode)) { bad += run_file(argv[i]); files++; continue; }
        DIR *d = opendir(argv[i]);
        if (!d) { fprintf(stderr, "replay: cannot open %s\n", argv[i]); return 1; }
        char **names = NULL;
        int n = 0, cap = 0;
        for (struct dirent *e; (e = readdir(d)) != NULL;) {
            if (e->d_name[0] == '.') continue;
            if (n == cap) { cap = cap ? 2 * cap : 64; names = realloc(names, (size_t)cap * sizeof *names); if (!names) abort(); }
            size_t len = strlen(argv[i]) + strlen(e->d_name) + 2;
            names[n] = malloc(len);
            if (!names[n]) abort();
            snprintf(names[n++], len, "%s/%s", argv[i], e->d_name);
        }
        closedir(d);
        qsort(names, (size_t)n, sizeof *names, by_name);   // the same order on every machine
        for (int k = 0; k < n; k++) { bad += run_file(names[k]); files++; free(names[k]); }
        free(names);
    }
    printf("replay: %d inputs, %s\n", files, bad ? "UNREADABLE FILES" : "ok");
    return bad ? 1 : 0;
}
