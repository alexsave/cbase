#include "pool.h"

#include "err.h"
#include "str.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct task {
    cb_task_fn fn;
    void *ud;
    struct task *next;
} task;

struct cb_pool {
    pthread_mutex_t lock;
    pthread_cond_t  cv;
    task *head, *tail;
    size_t pending;
    bool stopping;
    int n;
    pthread_t *threads;
    char name[16];
};

typedef struct { cb_pool *p; int i; } start_arg;

static void *worker(void *arg) {
    start_arg a = *(start_arg *)arg;
    free(arg);
    cb_pool *p = a.p;
#if defined(__APPLE__)
    char nm[32];
    snprintf(nm, sizeof nm, "%s-%d", p->name, a.i);
    pthread_setname_np(nm);
#elif defined(__linux__)
    char nm[16];
    snprintf(nm, sizeof nm, "%.10s-%d", p->name, a.i);
    pthread_setname_np(pthread_self(), nm);
#endif
    for (;;) {
        pthread_mutex_lock(&p->lock);
        while (!p->head && !p->stopping) pthread_cond_wait(&p->cv, &p->lock);
        task *t = p->head;
        if (!t) { pthread_mutex_unlock(&p->lock); return NULL; }   // stopping and drained
        p->head = t->next;
        if (!p->head) p->tail = NULL;
        p->pending--;
        pthread_mutex_unlock(&p->lock);
        t->fn(t->ud);
        free(t);
    }
}

cb_pool *cb_pool_new(int threads, const char *name) {
    if (threads < 1) threads = 1;
    cb_pool *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    pthread_mutex_init(&p->lock, NULL);
    pthread_cond_init(&p->cv, NULL);
    cb_strlcpy(p->name, name ? name : "pool", sizeof p->name);
    p->threads = calloc((size_t)threads, sizeof *p->threads);
    if (!p->threads) { free(p); return NULL; }
    for (int i = 0; i < threads; i++) {
        // Filled BEFORE pthread_create: the worker may run before it returns.
        start_arg *a = malloc(sizeof *a);
        if (a) { a->p = p; a->i = i; }
        if (!a || pthread_create(&p->threads[i], NULL, worker, a) != 0) {
            free(a);
            cb_pool_free(p);   // joins the ones that started (p->n of them)
            return NULL;
        }
        p->n++;
    }
    return p;
}

int cb_pool_submit(cb_pool *p, cb_task_fn fn, void *ud) {
    task *t = malloc(sizeof *t);
    if (!t) return CB_E_NOMEM;
    t->fn = fn; t->ud = ud; t->next = NULL;
    pthread_mutex_lock(&p->lock);
    if (p->stopping) { pthread_mutex_unlock(&p->lock); free(t); return CB_E_STATE; }
    if (p->tail) p->tail->next = t; else p->head = t;
    p->tail = t;
    p->pending++;
    pthread_cond_signal(&p->cv);
    pthread_mutex_unlock(&p->lock);
    return CB_OK;
}

size_t cb_pool_pending(cb_pool *p) {
    pthread_mutex_lock(&p->lock);
    size_t n = p->pending;
    pthread_mutex_unlock(&p->lock);
    return n;
}

void cb_pool_free(cb_pool *p) {
    if (!p) return;
    pthread_mutex_lock(&p->lock);
    p->stopping = true;
    pthread_cond_broadcast(&p->cv);
    pthread_mutex_unlock(&p->lock);
    for (int i = 0; i < p->n; i++) pthread_join(p->threads[i], NULL);
    pthread_mutex_destroy(&p->lock);
    pthread_cond_destroy(&p->cv);
    free(p->threads);
    free(p);
}
