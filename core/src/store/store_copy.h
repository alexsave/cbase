// store_copy.h - copying items from one store into another, and checking the copy.
//
// The body of cbase-export (a product's export tool: a remote table into the file
// `cbase serve` opens), kept here so a test can run it without a table. It works on the item layer
// (items.h), so it is the same for any product's rows and loses nothing: an item is its pk and sk and
// every attribute it holds, the index keys (gsi1pk .. gsi2sk) and the TTL pair among them.
#ifndef CB_STORE_COPY_H
#define CB_STORE_COPY_H

#include "store/store.h"

// Writes `it` (an item as the source answered it, its pk and sk among the attributes) at `key` in dst,
// through the same item_write a running store uses, so the stored bytes are the ones the store would
// write for that item. CB_OK or the store's error.
int cb_copy_put(cb_store *dst, cb_ikey key, const cb_item *it);

// Does dst hold the item at `key` exactly as `src`: the same attributes, each of the same kind and value?
bool cb_copy_same(cb_store *dst, cb_ikey key, const cb_item *src);

// The pk's family: the text before its first '#' (the whole pk when it has none), into out.
void cb_copy_family(const char *pk, char *out, size_t cap);

// Does the index entry of the item at `key` in dst answer a query of its index key? For gsi1 and gsi2: the
// item's gsiNpk and gsiNsk, queried (the pk, the sort key as a prefix), must list the item itself.
// *indexed counts the index entries checked; the result is how many of them did not list their item (0:
// every index key round-trips), or -1 when dst could not be read.
int cb_copy_index_roundtrip(cb_store *dst, cb_ikey key, int *indexed);

#endif
