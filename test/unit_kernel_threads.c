// unit_kernel_threads.c - a kernel solve on a thread that then exits leaves
// nothing behind.
//
// serve drives bots on a pool's worker threads, and those threads end when the
// pool stops at the SIGTERM drain. The solvers keep their scratch behind a
// _Thread_local pointer (cordite_sim.c's 1 MB transposition table,
// astrolite_strategy.c's child states and move lists); when that scratch was a
// plain calloc, every worker that had run an endgame solve leaked it as the
// thread ended, and the ASan smoke failed at the drain with a LeakSanitizer
// report of octogen's table. thread_heap.h frees each such block when its thread
// exits.
//
// The check is LeakSanitizer's, asked for on the spot after the threads are
// joined, so it only says something in the ASan build (make test-asan, which
// is the CI lane that caught it). Elsewhere the calls run and nothing is checked.
#include "t.h"

#include "cordite_sim.h"
#include "game.h"
#include "thread_heap.h"

#include <pthread.h>
#include <string.h>

#if defined(__SANITIZE_ADDRESS__)
#define CB_LSAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define CB_LSAN 1
#endif
#endif

#ifdef CB_LSAN
#include <sanitizer/lsan_interface.h>
#endif

// 0 when there is nothing to check (not an ASan build, or LeakSanitizer is off).
static int leaks_now(void) {
#ifdef CB_LSAN
    return __lsan_do_recoverable_leak_check();
#else
    return 0;
#endif
}

static void *solve_once(void *arg) {
    SimState *s = arg;
    int aborted = 0;
    cd_sim_solve(s, 0, -1000, 1000, 1, &aborted);   // a budget of one node: the table is all it needs
    return NULL;
}

static void a_solver_thread_frees_its_table(void) {
    game_set_seed(4242);
    Game g;
    memset(&g, 0, sizeof g);
    game_seat_and_deal(&g, NULL, 2);
    static SimState s;
    cd_sim_from_game(&s, &g);

    for (int i = 0; i < 3; i++) {
        pthread_t t;
        T_EQ_INT(pthread_create(&t, NULL, solve_once, &s), 0);
        T_EQ_INT(pthread_join(t, NULL), 0);
    }
    T_EQ_INT(leaks_now(), 0);
}

// Several blocks on one thread (astrolite takes two): all of them go.
static void *take_blocks(void *arg) {
    for (int i = 0; i < 4; i++) {
        unsigned char *b = thread_heap_calloc(1000 + (size_t)i, 8);
        if (!b) return NULL;
        b[0] = 1;
        *(void **)arg = b;   // the block is handed out and outlives nothing but the thread
    }
    return arg;
}

static void every_block_of_an_exited_thread_is_freed(void) {
    for (int i = 0; i < 3; i++) {
        void *last = NULL;
        pthread_t t;
        void *ret = NULL;
        T_EQ_INT(pthread_create(&t, NULL, take_blocks, &last), 0);
        T_EQ_INT(pthread_join(t, &ret), 0);
        T_CHECK(ret == &last && last != NULL);
        last = NULL;   // the freed block is no longer pointed at either way
    }
    T_EQ_INT(leaks_now(), 0);
}

static void a_block_is_zeroed_and_aligned(void) {
    long double *p = thread_heap_calloc(4, sizeof *p);
    T_CHECK(p != NULL);
    if (!p) return;
    T_CHECK(((uintptr_t)p % _Alignof(max_align_t)) == 0);
    unsigned char *c = (unsigned char *)p;
    int zero = 1;
    for (size_t i = 0; i < 4 * sizeof *p; i++) zero &= c[i] == 0;
    T_CHECK(zero);
    T_CHECK(thread_heap_calloc(SIZE_MAX / 2, 4) == NULL);   // the size overflows: refused, not wrapped
}

int main(void) {
    T_RUN(a_solver_thread_frees_its_table);
    T_RUN(every_block_of_an_exited_thread_is_freed);
    T_RUN(a_block_is_zeroed_and_aligned);
    return T_DONE();
}
