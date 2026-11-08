// The words both libclang generators are written in.
//
// tools/structgen and tools/datagen are two programs against ONE library asking
// it two different questions - structgen asks for SHAPE, datagen asks for
// CONTENTS - and neither can answer the other's. What is here is what asking
// anything at all has in common: how a generator refuses, how it builds a line
// of output, and how it copies a string. Nothing that belongs to one tool's
// question belongs in here.
#ifndef SGC_H
#define SGC_H
#include <stddef.h>

// A refusal, with the program's own name in front of it. That name is SGC_TOOL,
// which the build defines as the output file's basename (tools/llvm.mk), so a
// program cannot introduce itself as something other than what it is called.
void die(const char *fmt, ...);

// ---- growable string -----------------------------------------------------
typedef struct { char *s; size_t n, cap; } Buf;
void bprintf(Buf *b, const char *fmt, ...);

// Not strdup: under -std=c11 glibc does not declare it, and an implicit int
// return truncates the pointer on a 64-bit Linux host (5eb22685, tools/llvm.mk).
char *xstrdup(const char *s);

// An output path, made absolute while we are still standing where the caller
// started us: both tools chdir to their --cwd, and after that a relative output
// path would mean a different place than the caller meant.
const char *absolute(const char *path);

#endif
