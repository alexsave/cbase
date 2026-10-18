// lib_admin_fakestore.h - a tiny in-memory store for test/unit_admin.c: just
// the operations the admin console reads, with call records a test can check.
#ifndef CB_LIB_ADMIN_FAKESTORE_H
#define CB_LIB_ADMIN_FAKESTORE_H

#include "store/store.h"

#include <stdbool.h>

#define FAKE_ADMIN_USERS 7   // "users" rows u0..u6, ids "u0".."u6"

typedef struct {
    // what the last admin_query call was given
    int  query_calls;
    bool last_allow_write;
    char last_statement[512];
    // what the last admin_browse call was given
    char last_cursor[64];
    int  last_limit;
} fake_admin_record;

// A store whose ops->name is `name` ("sqlite" or "dynamo"). Free with
// cb_store_close. *rec is the store's record, valid until it is closed.
cb_store *fake_admin_store_new(const char *name, fake_admin_record **rec);

#endif
