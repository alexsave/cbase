// lib_store_backend.h - the store a test runs against: SQLite, or DynamoDB
// Local when CBASE_STORE=dynamo (`make check-dynamo`).
//
// Under dynamo every fresh store is its own new table, created from
// ops/cbase.yaml through ops/dynamo_table.py (the table the stack creates) and
// seeded with the schema's bots as SQLite seeds them, so a test sees the same
// starting state on both backends. Tables are made only at the endpoint
// AWS_ENDPOINT_URL_DYNAMODB names, and only when it is an http:// one (a
// local stand-in): a test can never create a table in real AWS.
#ifndef CB_TEST_STORE_BACKEND_H
#define CB_TEST_STORE_BACKEND_H

#include "store/store.h"

#include <stdbool.h>

// CBASE_STORE=dynamo.
bool tsb_dynamo(void);

// A fresh store: SQLite at `sqlite_path`, or a new DynamoDB Local table. NULL
// on failure (said on stderr).
cb_store *tsb_open_fresh(const char *sqlite_path);

// The DynamoDB table the last tsb_open_fresh made ("" under SQLite), for a
// test's PartiQL.
const char *tsb_table(void);

// Creates DynamoDB table `name` as ops/cbase.yaml defines it, at the local
// endpoint. 0, or -1.
int tsb_create_table(const char *name);

#endif
