// lib_dynamo_local.h - fresh DynamoDB tables for the tests that run against DynamoDB Local
// (`make check-dynamo`, CBASE_STORE=dynamo).
//
// A table is created from providers/aws/ops/cbase.yaml through providers/aws/ops/dynamo_table.py
// (the table the stack creates), and only at the endpoint AWS_ENDPOINT_URL_DYNAMODB names, and only
// when that is an http:// one (a local stand-in): a test can never create a table in real AWS. A
// product's own test library builds on this one (seeding what its rows start with).
#ifndef CB_LIB_DYNAMO_LOCAL_H
#define CB_LIB_DYNAMO_LOCAL_H

#include "store/store.h"

#include <stdbool.h>
#include <stddef.h>

// CBASE_STORE=dynamo.
bool tdl_enabled(void);

// A table name of this process's own, `prefix` first: unique within the process (a counter) and
// across the processes of one run (the process id).
void tdl_table_name(const char *prefix, char *out, size_t cap);

// Creates DynamoDB table `name` as providers/aws/ops/cbase.yaml defines it, at the local endpoint.
// 0, or -1 (said on stderr).
int tdl_create_table(const char *name);

// A fresh store: SQLite at `sqlite_path` without CBASE_STORE=dynamo, else a new DynamoDB Local
// table (its name in tdl_table()). NULL on failure (said on stderr).
cb_store *tdl_open_fresh(const char *sqlite_path);

// The DynamoDB table the last tdl_open_fresh made ("" under SQLite), for a test's PartiQL.
const char *tdl_table(void);

#endif
