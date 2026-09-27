/* libc_test.c - the freestanding C library (libc.c), run natively.
 *
 *   cc -std=c11 -Wall -Wextra -Werror libc_test.c -o libc_test && ./libc_test
 *
 * libc.c defines memcpy, snprintf and the rest under their real names, which
 * the host C library owns here, so this file includes it with every name
 * renamed to wlibc_*. The host's own functions stay reachable for the
 * harness and serve as the reference: every snprintf this shim supports
 * must match the host's, byte for byte and in its return value, at every
 * capacity.
 *
 * A truncated conversion used to loop forever (PUT skipped its argument
 * once the buffer was full, so *s++ never advanced). alarm() turns a
 * regression into a red run instead of a wedged `make run`. Exits 1 on any
 * failure. */
#include <signal.h>
#include <stdint.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../test/check.h"

#undef memcpy
#undef memset
#undef memcmp
#undef strlen
#undef strcmp
#undef strncmp
#undef snprintf
#define memcpy   wlibc_memcpy
#define memset   wlibc_memset
#define memcmp   wlibc_memcmp
#define strlen   wlibc_strlen
#define strcmp   wlibc_strcmp
#define strncmp  wlibc_strncmp
#define snprintf wlibc_snprintf
void  *wlibc_memcpy(void *dst, const void *src, size_t n);
void  *wlibc_memset(void *dst, int c, size_t n);
int    wlibc_memcmp(const void *a, const void *b, size_t n);
size_t wlibc_strlen(const char *s);
int    wlibc_strcmp(const char *a, const char *b);
int    wlibc_strncmp(const char *a, const char *b, size_t n);
int    wlibc_snprintf(char *out, size_t cap, const char *fmt, ...);
#include "libc.c"
#undef memcpy
#undef memset
#undef memcmp
#undef strlen
#undef strcmp
#undef strncmp
#undef snprintf

static uint32_t rng = 0x9e3779b9u;
static uint32_t next(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

static int sign(int v) { return (v > 0) - (v < 0); }

#define BUF 48
/* One call through the shim and the host at every capacity 0..BUF, into
 * buffers pre-filled with a guard byte: same return, same bytes, and nothing
 * written at or past cap. */
#define SAME(label, ...) do {                                                   \
        for (size_t cap = 0; cap <= BUF; cap++) {                               \
            char a[BUF + 8], b[BUF + 8];                                        \
            memset(a, 0x5a, sizeof a); memset(b, 0x5a, sizeof b);               \
            int ra = wlibc_snprintf(a, cap, __VA_ARGS__);                       \
            int rb = snprintf(b, cap, __VA_ARGS__);                             \
            CHECK(ra == rb, "%s cap %zu: returned %d, host %d", label, cap, ra, rb); \
            CHECK(memcmp(a, b, sizeof a) == 0, "%s cap %zu: bytes differ (\"%s\" vs \"%s\")", \
                  label, cap, cap ? a : "", cap ? b : "");                      \
            CHECK((unsigned char)a[cap] == 0x5a, "%s cap %zu: wrote past cap", label, cap); \
        }                                                                       \
    } while (0)

/* A call that must trap: run it in a child and expect a signal. */
static int traps(const char *fmt)
{
    fflush(stdout); fflush(stderr);
    pid_t p = fork();
    if (p == 0) {
        char b[16];
        wlibc_snprintf(b, sizeof b, fmt, 7, 8);
        _exit(0);
    }
    int st = 0;
    waitpid(p, &st, 0);
    return WIFSIGNALED(st) && WTERMSIG(st) != SIGALRM;
}

static void hung(int sig)
{
    (void)sig;
    static const char msg[] = "FAIL libc_test: hung past the alarm - a truncated conversion loops\n";
    ssize_t w = write(2, msg, sizeof msg - 1);
    (void)w;
    _exit(1);
}

int main(void)
{
    signal(SIGALRM, hung);
    alarm(10);

    TEST("snprintf: truncation");
    SAME("fits %s", "%s", "hi");
    SAME("truncated %s", "%s", "hello");
    SAME("truncated %d", "%d", 123456);
    SAME("INT_MIN then %s", "%d|%s", -2147483647 - 1, "xy");
    SAME("INT_MAX", "[%d]", 2147483647);
    SAME("literal text", "abcdef");
    SAME("percent", "100%% of %d", 7);
    SAME("empty", "%s", "");
    SAME("long %s", "<%s>", "a nickname much longer than any buffer a caller would give it");
    {
        char b[4] = "zzz";
        CHECK(wlibc_snprintf(b, 0, "%s", "hello") == 5 && b[0] == 'z',
              "cap 0 returns the wanted length and writes nothing");
        CHECK(wlibc_snprintf(NULL, 0, "%d%s", -12, "abc") == 6, "NULL out with cap 0");
        CHECK(wlibc_snprintf(b, sizeof b, "%s", (const char *)NULL) == 0 && b[0] == 0,
              "a NULL %%s prints nothing");
        CHECK(wlibc_snprintf(b, sizeof b, "ab%") == 2 && !strcmp(b, "ab"),
              "a lone trailing %% ends the format");
    }

    TEST("snprintf: random against the host");
    static const char *const fmts[] = { "%s", "%d", "%d %s", "%s:%d:%s", "x%%y%d", "%d%d%d", "[%s]" };
    for (int i = 0; i < 3000; i++) {
        char s1[40], s2[40];
        int l1 = (int)(next() % 39), l2 = (int)(next() % 39);
        for (int k = 0; k < l1; k++) s1[k] = (char)(' ' + next() % 95);
        for (int k = 0; k < l2; k++) s2[k] = (char)(' ' + next() % 95);
        s1[l1] = s2[l2] = 0;
        int d1 = (int)next(), d2 = (int)(next() % 2001) - 1000;
        const char *f = fmts[next() % (sizeof fmts / sizeof fmts[0])];
        char label[32];
        snprintf(label, sizeof label, "random %d", i);
        if (!strcmp(f, "%s"))            SAME(label, "%s", s1);
        else if (!strcmp(f, "%d"))       SAME(label, "%d", d1);
        else if (!strcmp(f, "%d %s"))    SAME(label, "%d %s", d2, s1);
        else if (!strcmp(f, "%s:%d:%s")) SAME(label, "%s:%d:%s", s1, d1, s2);
        else if (!strcmp(f, "x%%y%d"))   SAME(label, "x%%y%d", d2);
        else if (!strcmp(f, "%d%d%d"))   SAME(label, "%d%d%d", d1, d2, d1);
        else                             SAME(label, "[%s]", s2);
    }

    TEST("snprintf: an unsupported conversion traps");
    CHECK(traps("%u|%d"), "%%u did not trap");
    CHECK(traps("%c%s"), "%%c did not trap");
    CHECK(traps("%5d"), "a width did not trap");

    TEST("memory and string functions against the host");
    for (int i = 0; i < 2000; i++) {
        unsigned char x[64], y[64], z[64];
        size_t n = next() % 64;
        for (size_t k = 0; k < 64; k++) x[k] = (unsigned char)next();
        memcpy(y, x, sizeof y);
        if (n && (next() & 1)) y[next() % n] ^= (unsigned char)(1 + next() % 255);
        CHECK(sign(wlibc_memcmp(x, y, n)) == sign(memcmp(x, y, n)), "memcmp %d", i);
        memset(z, 0, sizeof z);
        CHECK(wlibc_memcpy(z, x, n) == z && !memcmp(z, x, n) && (n == 64 || z[n] == 0), "memcpy %d", i);
        int c = (int)next();
        CHECK(wlibc_memset(z, c, n) == z, "memset returns dst %d", i);
        for (size_t k = 0; k < n; k++) CHECK(z[k] == (unsigned char)c, "memset truncates c to a byte %d", i);

        char s[24], t[24];
        size_t ls = next() % 23, lt = next() % 23;
        for (size_t k = 0; k < ls; k++) s[k] = (char)(1 + next() % 3 + (next() & 1) * 125);
        for (size_t k = 0; k < lt; k++) t[k] = (char)(1 + next() % 3 + (next() & 1) * 125);
        s[ls] = t[lt] = 0;
        size_t m = next() % 26;
        CHECK(wlibc_strlen(s) == strlen(s), "strlen %d", i);
        CHECK(sign(wlibc_strcmp(s, t)) == sign(strcmp(s, t)), "strcmp %d", i);
        CHECK(sign(wlibc_strncmp(s, t, m)) == sign(strncmp(s, t, m)), "strncmp %d", i);
    }
    return report("libc_test");
}
