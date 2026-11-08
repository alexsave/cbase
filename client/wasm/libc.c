/* libc.c - the C library of the client's wasm module, a final program: what the client and lib/'s
 * digests require (include/string.h declares it). Nothing else links this file, and
 * tools/libc_gate.sh, which holds cbase's objects to defining no C library, is not handed it.
 *
 * memcpy, memmove and memset are out-of-line functions over the compiler's builtins, which
 * -mbulk-memory compiles to one memory.copy or memory.fill each (struct copies and zeroing lower
 * to those instructions directly). Mapping the calls themselves to the builtins in string.h was
 * measured and refused: every call site becomes an inline instruction with its operands, which
 * cost cbase_client.wasm 306 B raw under clang 23 and 347 B under clang 21, where it went past
 * the 10240 B budget, to save 37 B of gzip under 23 and lose 2 B under 21. */
#include <string.h>

void *memcpy(void *dst, const void *src, size_t n)
{
    __builtin_memcpy(dst, src, n);
    return dst;
}

/* Overlap allowed, either direction. */
void *memmove(void *dst, const void *src, size_t n)
{
    __builtin_memmove(dst, src, n);
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    __builtin_memset(dst, c, n);
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = a, *y = b;
    for (; n; n--, x++, y++)
        if (*x != *y) return *x - *y;
    return 0;
}

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n]) n++;
    return n;
}
