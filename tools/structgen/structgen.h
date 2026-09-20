// The few things every part of structgen shares: its caps, and how it names a
// generated member.
//
// WHAT THE TOOL IS, and every flag it takes, is the comment at the top of
// structgen.c. The parts, each about one thing:
//
//   sg_args.c    the request - argv, checked, and the banner it prints
//   sg_clang.c   the traversal - the only part that talks to libclang
//   sg_model.c   the model the traversal builds, and the requests over it
//   sg_ts.c      the TypeScript emitter (accessors and snapshots over wasm32)
//   sg_swift.c   the Swift emitter (value snapshots over the linked struct)
//   sg_kotlin.c  the Kotlin emitter (value snapshots over a JNI ByteBuffer)
//   sg_hash.c    the layout hash, and the module that carries it
//   structgen.c  main: parse, traverse, emit, write
//
// die, Buf/bprintf, xstrdup, absolute and the probe translation unit are in
// tools/sgcommon, shared with tools/datagen: the two tools ask libclang
// different questions, but they ask them the same way.
#ifndef SG_STRUCTGEN_H
#define SG_STRUCTGEN_H
#include <stddef.h>
#include "sgc.h"

// The cap on every repeatable request (--header, --root, --count, ...).
#define MAXN 64

#define GROW(arr, n, cap) do { if ((n) == (cap) && !((arr) = realloc((arr), sizeof *(arr) * ((cap) = (cap) * 2 + 16)))) die("out of memory"); } while (0)

// snake_case -> camelCase, into a static buffer.
const char *camel(const char *s);
// generated names are joined with '_' and could collide
void check_unique_names(const Buf *ts, const char *decl, const char *stop);

// Which string helpers a generated module uses.
#define STR_ACCESSORS 1   // cstrGet and cstrSet (char[N] accessors)
#define STR_CSTR_GET  2   // cstrGet (a snapshot's NUL-terminated char[N])
#define STR_UTF8_GET  4   // utf8Get (a snapshot's counted char[N])
#define STR_UTF8_SET  8   // utf8Set (a writer's counted char[N])

#endif
