// unit_time_gate.c - the time-zone gate: a zone lookup has one owner, lib/tz.
//
// The C library's local-time functions read the process's zone (the TZ variable, /etc/localtime, the
// tzdata files on the machine), so the same call answers differently on a laptop, a Lambda and a
// VM, and a stray one turns a stored local time into the machine's. A zone is looked up by name in
// lib/tz/tz.h (tz_find, tz_utc_ms, tz_local), from the app's own list of zones, on the time the caller
// passes in. So this test reads every C source of the tree (core, providers, client, lib, bench, fuzz,
// tools; the tests and fuzz targets too) and the product's source roots (CBASE_APP_SRC_ROOTS,
// lib_tree.h), and fails, naming file and line, when one names localtime, localtime_r, gmtime,
// mktime or tzset. (gmtime_r and timegm read no zone: the signing code and its tests keep them.)
// Comments, character literals and #include lines are skipped (the scanner is lib_cscan.h's); this file,
// which plants the banned names in its own tests, is not read. It runs from any directory (lib_tree.h).
#include "t.h"
#include "lib_cscan.h"
#include "lib_tree.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static const char *const BANNED[] = { "localtime", "localtime_r", "gmtime", "mktime", "tzset" };
// This file, whose literals are the banned names it plants, is the one file not read.
static const char SELF[] = "core/test/unit_time_gate.c";
// A file that holds the zone code, so a gate that read nothing of it fails.
static const char TZ_HOME[] = "lib/tz/tz.c";

static int  g_files, g_violations;
static bool g_quiet;   // planted text: counted, not printed

static void on_tok(const cscan_tok *t, void *ctx) {
    const char *path = ctx;
    if (t->kind != CSCAN_IDENT) return;
    for (size_t i = 0; i < sizeof BANNED / sizeof BANNED[0]; i++) {
        if (strlen(BANNED[i]) != t->n || memcmp(BANNED[i], t->s, t->n) != 0) continue;
        g_violations++;
        if (!g_quiet)
            fprintf(stderr, "time gate: %s:%d: %.*s (a zone is looked up with lib/tz/tz.h, on a time the caller passes in)\n",
                    path, t->line, (int)t->n, t->s);
    }
}

static void scan_text(const char *path, const char *s, size_t n) { cscan_lex(s, n, on_tok, (void *)path); }

static void on_file(const char *name, const char *text, size_t n, void *ctx) {
    (void)ctx;
    g_files++;
    scan_text(name, text, n);
}
static void on_unreadable(const char *name, void *ctx) {
    (void)ctx;
    g_violations++;
    fprintf(stderr, "time gate: %s: cannot be read\n", name);
}
static bool is_self(const char *name, void *ctx) {
    (void)ctx;
    return strcmp(name, SELF) == 0;
}

// Build output and installed packages are not the tree's.
static const char *const SKIP_DIRS[] = { "build", "node_modules", NULL };
static const cscan_walker WALK = { .skip_dirs = SKIP_DIRS, .skip = is_self, .file = on_file, .unreadable = on_unreadable };

static void scan_named(const char *dir, const char *name) { cscan_walk(&WALK, dir, name); }

static void scan_dir(const char *rel) {
    char p[TT_PATH_MAX];
    scan_named(tt_path(p, sizeof p, rel), rel);
}

// The gate itself: the tree as it is.
static void no_zone_lookup_goes_through_libc(void) {
    struct stat st;
    char p[TT_PATH_MAX];
    T_CHECK(stat(tt_path(p, sizeof p, TZ_HOME), &st) == 0);   // CBASE_ROOT is cbase, or nothing is scanned
    T_CHECK(stat(tt_path(p, sizeof p, SELF), &st) == 0);      // the exemption names a real file
    static const char *const DIRS[] = { "core", "providers", "client", "lib", "bench", "fuzz", "tools" };
    for (size_t i = 0; i < sizeof DIRS / sizeof DIRS[0]; i++) scan_dir(DIRS[i]);
    char roots[TT_ROOTS_MAX][TT_PATH_MAX];
    int n = tt_env_list("CBASE_APP_SRC_ROOTS", roots, TT_ROOTS_MAX);
    for (int i = 0; i < n; i++) scan_named(roots[i], roots[i]);
    T_CHECK(g_files >= 200);   // a gate that read nothing proves nothing
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

static void the_scanner_sees_each_name(void) {
    T_EQ_INT(planted("struct tm *t = localtime(&now);\n"), 1);
    T_EQ_INT(planted("localtime_r(&now, &tm); gmtime(&now); time_t m = mktime(&tm); tzset();\n"), 4);
    T_EQ_INT(planted("struct tm *(*f)(const time_t *) = localtime;\n"), 1);
    // Not a zone lookup: comments, includes, literals, other names, longer names.
    T_EQ_INT(planted("// localtime() described in a comment\n/* mktime, tzset */\n"), 0);
    T_EQ_INT(planted("#include <time.h>\nconst char *m = \"localtime and mktime\"; char q = 'm';\n"), 0);
    T_EQ_INT(planted("gmtime_r(&t, &tm); time_t u = timegm(&tm); tz_local(z, ms, &d, &m, a, &o); int my_mktime = 0; int localtimes = 1;\n"), 0);
}

int main(void) {
    T_RUN(the_scanner_sees_each_name);
    T_RUN(no_zone_lookup_goes_through_libc);
    return T_DONE();
}
