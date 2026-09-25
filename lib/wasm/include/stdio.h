/* Freestanding stdio.h for a wasm32 kernel: snprintf alone, for building
 * sentences. Defined in ../libc.c, and a SMALL one - %s, %d and %% only, which
 * is all its users write. Like the real one it returns the length it wanted
 * and always terminates.
 *
 * A build that needs a different stdio (a research build that declares
 * fprintf, or a snprintf with other semantics of its own) keeps its own
 * stdio.h and puts that directory first on its -isystem path. */
#ifndef WASM_LIBC_STDIO_H
#define WASM_LIBC_STDIO_H

#ifndef WASM_LIBC_SIZE_T
#define WASM_LIBC_SIZE_T
typedef __SIZE_TYPE__ size_t;
#endif

int snprintf(char *out, size_t cap, const char *fmt, ...);

#endif
