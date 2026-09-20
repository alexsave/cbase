// The Swift emitter: the same snapshots and writers as the TS module, over a
// pointer to the C struct itself. structgen.c documents what --swift promises.
#ifndef SG_SWIFT_H
#define SG_SWIFT_H
#include "structgen.h"

// The module body: the constants, the snapshot structs and their readers, and
// the writers. Only built when --swift asks for the file.
void sg_swift_module(Buf *sw, int *strings);
// ...and the file around it: the banner, this run's layout hash, the refusal
// enum and whichever string helpers the body used (`strings`).
void sg_swift_write(const char *path, const Buf *sw, int strings, unsigned hash);

#endif
