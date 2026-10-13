// import.c - `cbase-import <export dir>`: the Supabase export into a c_base
// store. The work is src/store/store_import.c (store_import.h); this is the
// command around it.
//
//   CBASE_STORE=sqlite CBASE_DB=./cbase.sqlite build/cbase-import migrate/out
//   CBASE_STORE=dynamo CBASE_TABLE=cbase       build/cbase-import migrate/out
//
// Exit 0 when every check passed, 1 when one failed, 2 on a usage or I/O error.
#include "store/store.h"
#include "store/store_import.h"
#include "util/err.h"
#include "util/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *env_or(const char *name, const char *dflt) {
    const char *v = getenv(name);
    return v && *v ? v : dflt;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: cbase-import <export dir>   (CBASE_STORE=sqlite|dynamo, CBASE_DB, CBASE_TABLE)\n");
        return 2;
    }
    cb_log_init(CB_LOG_INFO, 0);
    const char *kind = env_or("CBASE_STORE", "sqlite");
    cb_store *s = NULL;
    int rc = strcmp(kind, "dynamo") == 0 ? cb_store_open_dynamo(env_or("CBASE_TABLE", "cbase"), &s)
                                         : cb_store_open_sqlite(env_or("CBASE_DB", "./cbase.sqlite"), &s);
    if (rc != CB_OK) {
        fprintf(stderr, "cbase-import: cannot open the %s store: %s\n", kind, cb_strerror(rc));
        return 2;
    }
    rc = cb_import_dir(s, argv[1], stdout);
    cb_store_close(s);
    if (rc < 0) fprintf(stderr, "cbase-import: %s\n", cb_strerror(rc));
    return rc < 0 ? 2 : rc;
}
