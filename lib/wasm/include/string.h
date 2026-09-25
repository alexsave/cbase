/* Freestanding string.h for a wasm32 kernel built -nostdlib -ffreestanding.
 *
 * Declares what a kernel calls, and what clang lowers struct copies and
 * zeroing to (memcpy, memset). Defined in ../libc.c, which a product links;
 * the linker keeps only the ones something reaches. */
#ifndef WASM_LIBC_STRING_H
#define WASM_LIBC_STRING_H

#ifndef WASM_LIBC_SIZE_T
#define WASM_LIBC_SIZE_T
typedef __SIZE_TYPE__ size_t;
#endif

void  *memcpy(void *dst, const void *src, size_t n);
void  *memset(void *dst, int c, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);

#endif
