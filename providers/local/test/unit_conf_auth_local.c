// unit_conf_auth_local.c - the auth rows' conformance suite (core/test/conformance/auth.h) on the
// local provider's backend, SQLite, in a fresh file. The DynamoDB run is
// providers/aws/test/unit_conf_auth_dynamo.c (`make check-dynamo`).
#include "t.h"

#include "conformance/auth.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static char g_db[512];

static cb_store *open_sqlite(void *ud) {
    (void)ud;
    cb_store *s = NULL;
    return cb_store_open_sqlite(g_db, &s) == CB_OK ? s : NULL;
}

static void sqlite_conforms(void) {
    const char *tmp = getenv("TMPDIR");
    snprintf(g_db, sizeof g_db, "%s/cbase-conf-auth-%d.sqlite", tmp && *tmp ? tmp : "/tmp", (int)getpid());
    unlink(g_db);
    cs_backend b = { "sqlite", open_sqlite, NULL };
    int checks = 0;
    T_EQ_INT(ca_auth_run(&b, &checks), 0);
    T_CHECK(checks > 200);
    char side[600];
    unlink(g_db);
    snprintf(side, sizeof side, "%s-wal", g_db);
    unlink(side);
    snprintf(side, sizeof side, "%s-shm", g_db);
    unlink(side);
}

int main(void) {
    T_RUN(sqlite_conforms);
    return T_DONE();
}
