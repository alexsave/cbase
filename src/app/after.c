// after.c - what an invocation does once its answer is out (after.h).
#include "after.h"

#include "bots.h"
#include "edge.h"
#include "realtime/realtime.h"
#include "table_ops.h"
#include "util/log.h"
#include "util/str.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// One queued step: a fan-out (with its own copies of every byte) or a kick.
typedef struct after_job {
    struct after_job *next;
    bool              kick;
    char              game_id[CB_GAME_ID_MAX + 1];
    cb_fanout         f;   // kick false: game_id points at the field above, the bytes are owned
} after_job;

static _Thread_local bool       t_open;
static _Thread_local after_job *t_head, *t_tail;

void cb_after_begin(void) { t_open = true; }
bool cb_after_open(void) { return t_open; }

static void job_free(after_job *j) {
    if (!j) return;
    for (int i = 0; i < CB_MAX_SEATS; i++) {
        free((void *)j->f.seats[i].envelope);
        free((void *)j->f.seats[i].cast);
    }
    free((void *)j->f.spectator_cast);
    free(j);
}

static bool dup_into(const uint8_t *p, size_t n, const uint8_t **out) {
    *out = NULL;
    if (!p) return true;
    uint8_t *c = malloc(n ? n : 1);
    if (!c) return false;
    if (n) memcpy(c, p, n);
    *out = c;
    return true;
}

static void push(after_job *j) {
    if (t_tail) t_tail->next = j;
    else t_head = j;
    t_tail = j;
}

bool cb_after_fanout(const cb_fanout *f) {
    if (!t_open) return false;
    after_job *j = calloc(1, sizeof *j);
    bool ok = j != NULL;
    if (ok) {
        j->f = *f;
        cb_strlcpy(j->game_id, f->game_id, sizeof j->game_id);
        j->f.game_id = j->game_id;
        // status is a static label (cb_table_status_label): kept as a pointer.
        for (int i = 0; i < CB_MAX_SEATS; i++) j->f.seats[i].envelope = j->f.seats[i].cast = NULL;
        j->f.spectator_cast = NULL;
        for (int i = 0; ok && i < f->n_seats; i++)
            ok = dup_into(f->seats[i].envelope, f->seats[i].envelope_len, &j->f.seats[i].envelope) &&
                 dup_into(f->seats[i].cast, f->seats[i].cast_len, &j->f.seats[i].cast);
        ok = ok && dup_into(f->spectator_cast, f->spectator_cast_len, &j->f.spectator_cast);
    }
    if (!ok) {
        // Published at once by the caller: late is better than lost.
        CB_WARN("after", "game %s v%lld: no memory to queue its pushes, sending them before the answer", f->game_id,
                (long long)f->version);
        job_free(j);
        return false;
    }
    push(j);
    return true;
}

void cb_after_kick(cb_ctx *ctx, const char *game_id) {
    if (!ctx->kick_bots) return;
    after_job *j = t_open ? calloc(1, sizeof *j) : NULL;
    if (!j) {
        cb_bots_kick(ctx, game_id);
        return;
    }
    j->kick = true;
    cb_strlcpy(j->game_id, game_id, sizeof j->game_id);
    push(j);
}

void cb_after_run(cb_ctx *ctx) {
    t_open = false;   // what the queued work itself commits or kicks acts at once
    after_job *j = t_head;
    t_head = t_tail = NULL;
    while (j) {
        after_job *next = j->next;
        if (j->kick) {
            cb_bots_kick(ctx, j->game_id);   // after the commit, so its lease check sees any drive
        } else {
            pthread_mutex_t *order = cb_table_order_lock(j->game_id);
            cb_rt_failures_take();
            cb_fanout_publish(ctx, &j->f);
            int failed = cb_rt_failures_take();
            pthread_mutex_unlock(order);
            if (failed)
                CB_WARN("after", "game %s v%lld: %d push(es) failed after the answer was sent", j->game_id,
                        (long long)j->f.version, failed);
        }
        job_free(j);
        j = next;
    }
    cb_edge_flush(ctx->cfg);
}
