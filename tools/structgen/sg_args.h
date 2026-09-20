// The request: what the caller asked this run to do (structgen.c documents
// every flag). Parsed once into these globals, which every other part reads and
// none of them writes.
#ifndef SG_ARGS_H
#define SG_ARGS_H
#include <stdio.h>

extern const char *headers[], *roots[], *prefixes[], *snaps[], *writers[], *build, *out_ts, *out_swift, *out_kotlin, *out_hash_ts, *cwd;
// --kotlin-package: what the Kotlin module declares. Required with --kotlin and
// never defaulted, because a wrong package is a wrong import at every call site.
extern const char *kotlin_package;
extern const char *target;
extern int nheaders, nroots, nprefixes, nsnaps, nwriters, print_hash, snapshot_only;
extern const char *build_name_end;   // build .. build_name_end is NAME in --build NAME=FLAGS
extern const char *build_flags;      // ...and this is FLAGS, the flags clang parses under

void sg_args(int argc, char **argv);
// The header both generated modules start with.
void emit_banner(FILE *fp);

#endif
