// after.c - what an invocation does once its answer is out (after.h).
#include "after.h"

#include "edge.h"
#include "util/log.h"

#include <stdlib.h>

// One deferred step, in call order.
typedef struct after_job {
    struct after_job *next;
    cb_after_fn       run;
    void            (*drop)(void *arg);
    void             *arg;
} after_job;

static _Thread_local bool       t_open;
static _Thread_local after_job *t_head, *t_tail;

void cb_after_begin(void) { t_open = true; }
bool cb_after_open(void) { return t_open; }

bool cb_after_defer(cb_after_fn run, void (*drop)(void *arg), void *arg) {
    after_job *j = t_open ? calloc(1, sizeof *j) : NULL;
    if (!j) return false;
    j->run = run;
    j->drop = drop;
    j->arg = arg;
    if (t_tail) t_tail->next = j;
    else t_head = j;
    t_tail = j;
    return true;
}

void cb_after_run(cb_ctx *ctx) {
    t_open = false;   // what the queued work itself commits or kicks acts at once
    after_job *j = t_head;
    t_head = t_tail = NULL;
    while (j) {
        after_job *next = j->next;
        j->run(ctx, j->arg);
        if (j->drop) j->drop(j->arg);
        free(j);
        j = next;
    }
    cb_edge_flush(ctx->cfg);
}
