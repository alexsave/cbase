// The TypeScript emitter: accessors and value snapshots over wasm32 linear
// memory. structgen.c documents what each of them is.
#ifndef SG_TS_H
#define SG_TS_H
#include "structgen.h"

// How TypeScript reads (write 0) or writes (write 1) one scalar of this kind
// and size, as a printf format taking the address, with the TS type it widens
// to in *vt. NULL when there is no reader for that shape, which is also how the
// model asks whether a scalar can be snapshotted at all.
const char *scalar_access(char kind, long size, int write, const char **vt);

// The module body: the constants, the per-field accessors, the snapshot readers
// and the writers. Built on every run whether or not --ts asks for the file,
// because the layout hash takes the constants in the order this emits them.
void sg_ts_module(Buf *ts, int *strings);
// ...and the file around it: the banner, the `Mem` views and whichever string
// helpers the body used (`strings`).
void sg_ts_write(const char *path, const Buf *ts, int strings);

#endif
