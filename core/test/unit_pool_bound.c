// unit_pool_bound.c - a bounded pool (util/pool.h cb_pool_new_bounded) takes threads + queue tasks
// and refuses the next at once with CB_E_FULL; a finished task frees its place; an unbounded pool
// takes them all; and cb_pool_free runs what was queued. Held tasks, no sleeps: each task waits on
// a gate the test opens.
#include "t.h"

#include "util/err.h"
#include "util/pool.h"

#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static int g_open, g_started;
static atomic_int g_ran;

static void held(void *ud) {
    (void)ud;
    pthread_mutex_lock(&g_mu);
    g_started++;
    pthread_cond_broadcast(&g_cv);
    while (!g_open) pthread_cond_wait(&g_cv, &g_mu);
    pthread_mutex_unlock(&g_mu);
    atomic_fetch_add(&g_ran, 1);
}

static void gate(int open) {
    pthread_mutex_lock(&g_mu);
    g_open = open;
    if (!open) g_started = 0;
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mu);
}

static void wait_started(int n) {
    pthread_mutex_lock(&g_mu);
    while (g_started < n) pthread_cond_wait(&g_cv, &g_mu);
    pthread_mutex_unlock(&g_mu);
}

static void bounded_refuses_past_threads_plus_queue(void) {
    gate(0);
    atomic_store(&g_ran, 0);
    cb_pool *p = cb_pool_new_bounded(3, "tb", 2);
    T_CHECK(p != NULL);
    for (int i = 0; i < 5; i++) T_EQ_INT(cb_pool_submit(p, held, NULL), CB_OK);
    wait_started(3);
    T_EQ_INT((int)cb_pool_pending(p), 2);
    T_EQ_INT(cb_pool_submit(p, held, NULL), CB_E_FULL);   // 3 running, 2 waiting: full
    T_EQ_INT(cb_pool_submit(p, held, NULL), CB_E_FULL);
    gate(1);
    cb_pool_free(p);   // runs the two queued
    T_EQ_INT(atomic_load(&g_ran), 5);
}

static void no_queue_is_threads_alone(void) {
    gate(0);
    atomic_store(&g_ran, 0);
    cb_pool *p = cb_pool_new_bounded(2, "tq", 0);
    T_EQ_INT(cb_pool_submit(p, held, NULL), CB_OK);
    T_EQ_INT(cb_pool_submit(p, held, NULL), CB_OK);
    T_EQ_INT(cb_pool_submit(p, held, NULL), CB_E_FULL);
    wait_started(2);
    gate(1);
    while (!cb_pool_idle(p)) sched_yield();   // both tasks returned (the count drops after fn)
    T_EQ_INT(cb_pool_submit(p, held, NULL), CB_OK);   // the places are free again
    cb_pool_free(p);
    T_EQ_INT(atomic_load(&g_ran), 3);
}

static void unbounded_takes_them_all(void) {
    gate(0);
    atomic_store(&g_ran, 0);
    cb_pool *p = cb_pool_new(1, "tu");
    for (int i = 0; i < 100; i++) T_EQ_INT(cb_pool_submit(p, held, NULL), CB_OK);
    gate(1);
    cb_pool_free(p);
    T_EQ_INT(atomic_load(&g_ran), 100);
}

int main(void) {
    T_RUN(bounded_refuses_past_threads_plus_queue);
    T_RUN(no_queue_is_threads_alone);
    T_RUN(unbounded_takes_them_all);
    return T_DONE();
}
