/* The kernel test harness: TEST names the test in hand, CHECK counts an
 * assertion and names the test and the line of a failure, and report()
 * prints the tally and hands main its exit status, 1 on any failure, so
 * `make run` goes red. Header-only and test-only: nothing here ships.
 *
 * A product's own test header includes this one first, by relative path,
 * and adds what pokes its own game struct (action builders, a hand-built
 * table, seed makers). Tests may poke those structs directly to build a
 * position; a kernel's structs are plain data on purpose. */
#ifndef SHARED_TEST_CHECK_H
#define SHARED_TEST_CHECK_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int         g_checks, g_fails;
static const char *g_test = "";

#define TEST(name) (g_test = (name))

/* A failure report is capped PER TEST, not per binary, so a test that goes
 * red after a noisier one is still named in the output (a mutation check
 * reads it by name). The first CHECK_NAMED_CAP distinct test names that
 * fail each print their first 5 failures; a name past the cap still counts
 * in g_fails but prints nothing. */
#ifndef CHECK_NAMED_CAP
#define CHECK_NAMED_CAP 128
#endif

/* How many failures of one test print before the rest only count. The
 * default is the named-test rule above; a suite that never calls TEST() (all
 * its checks are one nameless test) raises it, or its sixth failure would go
 * unreported. */
#ifndef CHECK_PRINT_CAP
#define CHECK_PRINT_CAP 5
#endif

static const char *g_named[CHECK_NAMED_CAP];
static int         g_named_fails[CHECK_NAMED_CAP];
static inline int first_fails_of(const char *test)
{
    int i = 0;
    while (i < CHECK_NAMED_CAP && g_named[i] && strcmp(g_named[i], test)) i++;
    if (i == CHECK_NAMED_CAP) return 0;
    g_named[i] = test;
    return ++g_named_fails[i] <= CHECK_PRINT_CAP;
}

#define CHECK(cond, ...) do {                                                   \
        g_checks++;                                                             \
        if (!(cond)) {                                                          \
            g_fails++;                                                          \
            if (first_fails_of(g_test)) {                                       \
                fprintf(stderr, "FAIL %s:%d [%s] %s: ", __FILE__, __LINE__,     \
                        g_test, #cond);                                         \
                fprintf(stderr, __VA_ARGS__);                                   \
                fprintf(stderr, "\n");                                          \
            }                                                                   \
        }                                                                       \
    } while (0)

/* The plain spelling most suites grew: OK(cond, "what it checked"). */
#define OK(c, what) CHECK(c, "%s", what)

/* report() fails a binary that ran no check at all when CHECK_ZERO_IS_FAIL is
 * defined (c_base's t.h rule: a suite that silently stopped registering its
 * tests must not pass). Off by default: some suites check inside a loop that
 * may legitimately be empty. */
static inline int report(const char *what)
{
#ifdef CHECK_ZERO_IS_FAIL
    if (g_checks == 0) { printf("%s: no assertions ran\n", what); return 1; }
#endif
    printf("%s: %d assertions, %d failed\n", what, g_checks, g_fails);
    return g_fails ? 1 : 0;
}

#endif
