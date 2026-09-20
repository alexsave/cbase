// The layout hash: the one number a module and the wasm it reads are checked
// against. sg_hash.c says exactly what goes into it.
#ifndef SG_HASH_H
#define SG_HASH_H
#include <stdio.h>

// One line of layout fact into the hash. The emitters call it for what only
// they know the order of (the constants); everything else is walked below.
void hput(const char *fmt, ...);
// Every --root walked, and the hash that comes out. Called once, AFTER the
// constants have gone in: the hash is of the lines in the order they are put.
unsigned sg_layout_hash(void);
// The module holding only `export const LAYOUT_HASH = 0x...;` - no accessor, so
// a host that only checks the hash imports no reader of the raw struct.
void sg_hash_write_ts(const char *path, unsigned hash);

#endif
