// conformance/store.h - THE STORE CONFORMANCE SUITE (docs/PLATFORM.md 8.3.2,
// 8.6 stage 5, 8.7 decisions 1 and 2): the item layer's contract (store/items.h,
// store.h cb_store_ops item_*), as one library every provider's lane runs
// against its own backend.
//
// It asserts only the weaker side of each row of 8.3.2's table, so a backend
// that passes here is one a product may be written against:
//   - an index may lag the table: a read of gsi1 or gsi2 is polled briefly
//     until it shows what was written (CS_INDEX_WAIT_MS);
//   - an expired item may still be stored: a read gets it or does not, and
//     the layer above filters it (rows_auth.c), never the backend;
//   - a conditional write expected to go through may fail spuriously under
//     contention, so it is sent again a few times; one expected to be
//     refused must be refused;
//   - the limits (key length, item size, attribute names, transaction size
//     and count) are the tightest of every provider, and past one each
//     backend refuses the same way, CB_E_INVALID, with nothing written.
//
// The suite writes only under a partition prefix of its own (one per run),
// so it can share a store that holds other items (a product's seed rows).
//
//   static cb_store *open_mine(void *ud) { ... a fresh store ... }
//   cs_backend b = { "sqlite", open_mine, NULL };
//   int checks, failed = cs_store_run(&b, &checks);
//
// The cases print their own FAIL lines (file:line, as t.h does); the runner
// asserts failed == 0 and checks > 0 in its own tally.
#ifndef CB_CONF_STORE_H
#define CB_CONF_STORE_H

#include "store/store.h"

#define CS_INDEX_WAIT_MS 5000

typedef struct {
    const char *name;               // for the log: "sqlite", "dynamo"
    cb_store *(*open)(void *ud);    // a store to run on (NULL: the run fails)
    void       *ud;
} cs_backend;

// Every case, over one store b opens (closed at the end). The number of
// failed checks; *checks (may be NULL) gets the number that ran.
int cs_store_run(const cs_backend *b, int *checks);

#endif
