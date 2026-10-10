/* Freestanding stdlib.h for a wasm32 kernel built -nostdlib -ffreestanding.
 *
 * atoi and atol are defined in ../libc.c. malloc, calloc and free are defined
 * in ../bump_alloc.c, which is a separate file because it is a choice: free
 * does nothing, so it suits a kernel that allocates a few long-lived buffers
 * and nothing else. A product that wants that links the file.
 *
 * A build that needs more of stdlib (getenv, abort) keeps its own stdlib.h,
 * puts that directory first on its -isystem path, and reaches this one with
 * #include_next. */
#ifndef WASM_LIBC_STDLIB_H
#define WASM_LIBC_STDLIB_H

#ifndef WASM_LIBC_SIZE_T
#define WASM_LIBC_SIZE_T
typedef __SIZE_TYPE__ size_t;
#endif

void *malloc(size_t n);
void *calloc(size_t n, size_t sz);
void  free(void *p);
int   atoi(const char *s);
long  atol(const char *s);

#endif
