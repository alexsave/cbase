// unit_conf_store_dynamo.c - the store conformance suite (core/test/conformance/store.h)
// on the aws provider's backend, DynamoDB, against DynamoDB Local in its own
// fresh table (`make check-dynamo`, CBASE_STORE=dynamo). Without DynamoDB
// Local it skips, saying so: the suite's SQLite run is
// providers/local/test/unit_conf_store_local.c.
#include "t.h"

#include "conformance/store.h"
#include "lib_dynamo_local.h"

#include <stdio.h>
#include <stdlib.h>

static cb_store *open_dynamo(void *ud) {
    char name[64];
    tdl_table_name("conf_", name, sizeof name);
    cb_store *s = NULL;
    if (tdl_create_table(name) != 0 || cb_store_open_dynamo(name, &s) != CB_OK) return NULL;
    return s;
}

static void dynamo_conforms(void) {
    cs_backend b = { "dynamo", open_dynamo, NULL };
    int checks = 0;
    T_EQ_INT(cs_store_run(&b, &checks), 0);
    T_CHECK(checks > 100);
}

int main(void) {
    if (!tdl_enabled()) {
        printf("skip unit_conf_store_dynamo.c: no DynamoDB Local (CBASE_STORE=dynamo, make check-dynamo)\n");
        return 0;
    }
    T_RUN(dynamo_conforms);
    return T_DONE();
}
