// main.c - cbase-bench: latency and CPU numbers for c_base (bench.h).
//
// The question it answers (DESIGN.md "Binary alternatives and the latency
// bench"): how much of a request is the JSON tax, and would the fully binary
// options (Lambda Web Adapter, S3 conditional writes, polling) be faster end
// to end. The same program measures `cbase serve` here and the deployed stack.
#include "bench.h"

#include "util/log.h"

#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif

void bench_date(char *out, size_t cap) {
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    strftime(out, cap, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

void bench_host(char *out, size_t cap) {
    snprintf(out, cap, "unknown");
#ifdef __APPLE__
    size_t n = cap;
    if (sysctlbyname("machdep.cpu.brand_string", out, &n, NULL, 0) != 0) snprintf(out, cap, "unknown");
#else
    FILE *f = fopen("/proc/cpuinfo", "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *c = strchr(line, ':');
        if (c && strncmp(line, "model name", 10) == 0) {
            c++;
            while (*c == ' ') c++;
            c[strcspn(c, "\n")] = 0;
            snprintf(out, cap, "%s", c);
            break;
        }
    }
    fclose(f);
#endif
}

void bench_note_load(char *note, size_t cap) {
    double l[1];
    size_t n = strlen(note);
    if (getloadavg(l, 1) == 1 && n < cap) snprintf(note + n, cap - n, "%sload1=%.0f", n ? " " : "", l[0]);
}

static bool is_dir(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static const char *default_dir(void) {
    if (is_dir("bench/results")) return "bench/results";
    if (is_dir("c_base/bench/results")) return "c_base/bench/results";
    return ".";
}

int bench_save(const bs_run *r, const bench_opts *o) {
    cb_buf text;
    cb_buf_init(&text);
    bs_run_write(r, &text);
    fputs(cb_buf_cstr(&text), stdout);
    cb_buf md;
    cb_buf_init(&md);
    bs_report(r, 1, &md);
    printf("\n%s", cb_buf_cstr(&md));
    cb_buf_free(&md);
    // The stamp without colons, so the name is a plain file name everywhere.
    char stamp[32];
    size_t k = 0;
    for (const char *c = r->date; *c && k + 1 < sizeof stamp; c++)
        if (*c != ':' && *c != '-') stamp[k++] = *c;
    stamp[k] = 0;
    char path[1024];
    snprintf(path, sizeof path, "%s/%s-%s.txt", o->out_dir ? o->out_dir : default_dir(), r->kind, stamp);
    FILE *f = fopen(path, "w");
    int rc = -1;
    if (f) {
        rc = fwrite(text.p, 1, text.len, f) == text.len ? 0 : -1;
        if (fclose(f) != 0) rc = -1;
    }
    if (rc == 0) printf("\nsaved %s\n", path);
    else fprintf(stderr, "bench: cannot write %s: %s\n", path, strerror(errno));
    cb_buf_free(&text);
    return rc;
}

static bool read_file(const char *path, cb_buf *out) {
    FILE *f = fopen(path, "r");
    if (!f) return false;
    char b[4096];
    size_t n;
    while ((n = fread(b, 1, sizeof b, f)) > 0) cb_buf_append(out, b, n);
    fclose(f);
    return !out->oom;
}

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static int report(int argc, char **argv) {
    enum { MAX_RUNS = 256 };
    char *paths[MAX_RUNS];
    int np = 0;
    if (argc > 0) {
        for (int i = 0; i < argc && np < MAX_RUNS; i++) paths[np++] = strdup(argv[i]);
    } else {
        const char *dir = default_dir();
        DIR *d = opendir(dir);
        if (!d) { fprintf(stderr, "bench: no %s\n", dir); return 1; }
        struct dirent *e;
        while ((e = readdir(d)) && np < MAX_RUNS) {
            size_t l = strlen(e->d_name);
            if (l < 5 || strcmp(e->d_name + l - 4, ".txt") != 0) continue;
            char p[1024];
            snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
            paths[np++] = strdup(p);
        }
        closedir(d);
        qsort(paths, (size_t)np, sizeof *paths, cmp_str);
    }
    bs_run *runs = calloc((size_t)(np ? np : 1), sizeof *runs);
    int nr = 0, bad = 0;
    for (int i = 0; i < np; i++) {
        cb_buf t;
        cb_buf_init(&t);
        if (read_file(paths[i], &t) && bs_run_parse((const char *)t.p, t.len, &runs[nr])) nr++;
        else { fprintf(stderr, "bench: %s is not a run file\n", paths[i]); bad++; }
        cb_buf_free(&t);
        free(paths[i]);
    }
    cb_buf md;
    cb_buf_init(&md);
    bs_report(runs, nr, &md);
    fputs(cb_buf_cstr(&md), stdout);
    cb_buf_free(&md);
    free(runs);
    return bad ? 1 : 0;
}

static void usage(void) {
    fprintf(stderr,
            "usage:\n"
            "  cbase-bench roundtrip --url <base> [--n 500] [--concurrency 1|4] [--gap MS] [--note TEXT] [--out DIR]\n"
            "  cbase-bench coldstart --url <base> [--idle S] [--repeat K] [--path P] [--force-cmd CMD] [--note TEXT] [--out DIR]\n"
            "  cbase-bench cpu [--note TEXT] [--out DIR]\n"
            "  cbase-bench direct --url <base> [--n 200] [--table cbase] [--region R] [--edge URL] [--burst K] [--note TEXT] [--out DIR]\n"
            "  cbase-bench subscribe --url <base> [--n 30] [--as 1] [--note TEXT] [--out DIR]\n"
            "  cbase-bench page --url <base> [--n 5] [--batch 0|1] [--pushes 20] [--gap MS] [--note TEXT] [--out DIR]\n"
            "  cbase-bench spaced --url <base> [--path P] [--n 30] [--gap MS] [--games K] [--note TEXT] [--out DIR]\n"
            "  cbase-bench report [FILE...]\n");
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    cb_log_init(CB_LOG_WARN, 0);
    if (argc < 2) { usage(); return 2; }
    const char *cmd = argv[1];
    if (strcmp(cmd, "report") == 0) return report(argc - 2, argv + 2);
    bench_opts o = { .n = 500, .concurrency = 1, .idle_s = 0, .repeat = 5, .path = "/health", .note = "" };
    for (int i = 2; i < argc; i += 2) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!v) { usage(); return 2; }
        if (strcmp(a, "--url") == 0) o.url = v;
        else if (strcmp(a, "--out") == 0) o.out_dir = v;
        else if (strcmp(a, "--note") == 0) o.note = v;
        else if (strcmp(a, "--path") == 0) o.path = v;
        else if (strcmp(a, "--force-cmd") == 0) o.force_cmd = v;
        else if (strcmp(a, "--n") == 0) o.n = atoi(v);
        else if (strcmp(a, "--concurrency") == 0) o.concurrency = atoi(v);
        else if (strcmp(a, "--idle") == 0) o.idle_s = atoi(v);
        else if (strcmp(a, "--repeat") == 0) o.repeat = atoi(v);
        else if (strcmp(a, "--table") == 0) o.table = v;
        else if (strcmp(a, "--region") == 0) o.region = v;
        else if (strcmp(a, "--edge") == 0) o.edge = v;
        else if (strcmp(a, "--burst") == 0) o.burst = atoi(v);
        else if (strcmp(a, "--as") == 0) o.as = atoi(v) != 0;
        else if (strcmp(a, "--gap") == 0) o.gap_ms = atoi(v);
        else if (strcmp(a, "--games") == 0) o.games = atoi(v);
        else if (strcmp(a, "--batch") == 0) o.batch = atoi(v) != 0;
        else if (strcmp(a, "--pushes") == 0) o.pushes = atoi(v);
        else { usage(); return 2; }
    }
    if (strcmp(cmd, "cpu") == 0) return bench_cpu(&o);
    if (!o.url) { usage(); return 2; }
    if (strcmp(cmd, "roundtrip") == 0) {
        if (o.n < 1 || o.concurrency < 1 || o.concurrency > 64) { usage(); return 2; }
        return bench_roundtrip(&o);
    }
    if (strcmp(cmd, "direct") == 0) {
        if (o.n < 1) { usage(); return 2; }
        return bench_direct(&o);
    }
    if (strcmp(cmd, "subscribe") == 0) {
        if (o.n < 1) { usage(); return 2; }
        return bench_subscribe(&o);
    }
    if (strcmp(cmd, "page") == 0) {
        if (o.n < 1 || o.n > 20 || o.pushes < 0) { usage(); return 2; }
        return bench_page(&o);
    }
    if (strcmp(cmd, "spaced") == 0) {
        if (o.n < 1 || o.gap_ms < 0) { usage(); return 2; }
        return bench_spaced(&o);
    }
    if (strcmp(cmd, "coldstart") == 0) {
        if (o.repeat < 1 || o.idle_s < 0) { usage(); return 2; }
        return bench_coldstart(&o);
    }
    usage();
    return 2;
}
