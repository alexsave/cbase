// unit_conf_store_local.c - the store conformance suite (core/test/conformance/store.h)
// on the local provider's backend, SQLite: the reference (docs/PLATFORM.md
// 8.7 decision 1), so every case must pass here first.
#include "t.h"

#include "conformance/store.h"
#include "lib_proc.h"

#include <stdio.h>
#include <stdlib.h>

static char g_dir[256];

static cb_store *open_sqlite(void *ud) {
    char path[320];
    snprintf(path, sizeof path, "%s/conf.sqlite", g_dir);
    cb_store *s = NULL;
    return cb_store_open_sqlite(path, &s) == CB_OK ? s : NULL;
}

static void sqlite_conforms(void) {
    cs_backend b = { "sqlite", open_sqlite, NULL };
    int checks = 0;
    T_EQ_INT(cs_store_run(&b, &checks), 0);
    T_CHECK(checks > 100);
}

int main(void) {
    if (tp_tempdir(g_dir, sizeof g_dir) != 0) return 1;
    T_RUN(sqlite_conforms);
    char cmd[300];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", g_dir);
    if (system(cmd) != 0) return 1;
    return T_DONE();
}
