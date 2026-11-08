/* libc.c - the C library the client's wasm module requires and cannot get from an instruction:
 * memcmp and strlen (include/string.h says why memcpy, memmove and memset need none). The module
 * is a final program, so this is its own; nothing else links it, and tools/libc_gate.sh, which
 * holds cbase's objects to defining no C library, leaves it out for that reason. */
#include <string.h>

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
