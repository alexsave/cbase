/* string.h for the client's wasm module (cbc_wasm.c), a final program built -nostdlib
 * -ffreestanding: the program supplies the C library its objects require, and this is all of it
 * that they reach. cbase defines no C library (its sources only include the standard headers,
 * and tools/libc_gate.sh holds its objects to that); libc.c beside this directory is the module's
 * own, linked only here. A source that calls anything else from string.h does not compile here,
 * so the module learns of it at build time. */
#ifndef CBC_WASM_STRING_H
#define CBC_WASM_STRING_H

typedef __SIZE_TYPE__ size_t;

void  *memcpy(void *dst, const void *src, size_t n);
void  *memmove(void *dst, const void *src, size_t n);
void  *memset(void *dst, int c, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);

#endif
