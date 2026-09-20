// The Kotlin emitter: the same snapshots and writers as the Swift module, over
// a java.nio.ByteBuffer rather than a raw pointer. structgen.c documents what
// --kotlin promises and why the crossing is a buffer and not an address.
#ifndef SG_KOTLIN_H
#define SG_KOTLIN_H
#include "structgen.h"

// The module body: the constants, the snapshot data classes and their readers,
// and the writers. Only built when --kotlin asks for the file.
void sg_kotlin_module(Buf *kt, int *strings);
// ...and the file around it: the banner, the package, the imports, this run's
// layout hash with the check that uses it, the refusal type and whichever
// string helpers the body used (`strings`).
void sg_kotlin_write(const char *path, const Buf *kt, int strings, unsigned hash);

#endif
