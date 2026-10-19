// provider_local.c - `cbase serve`'s provider (cbase/provider.h): SQLite by
// default (CBASE_STORE=dynamo for the DynamoDB Local lane), the in-process
// MQTT hub at /mqtt, jobs on the job pool, no edge to purge.
#include "cbase/provider.h"

#include "config.h"
#include "core.h"
#include "jobs.h"
#include "realtime/rt_local.h"
#include "store/store.h"
#include "util/err.h"
#include "util/log.h"
#include "net/loop.h"
#include "util/pool.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- the job pool --------------------------------------------------------------------

#define JOB_ID_MAX (CB_JOB_NAME_MAX + 1 + CB_JOB_KEY_MAX)   // "<job>:<key>"

typedef struct {
    cb_ctx *ctx;
    char    job[CB_JOB_NAME_MAX + 1];
    char    key[CB_JOB_KEY_MAX + 1];
    char    id[JOB_ID_MAX + 1];
} job_task;

// The jobs queued and not yet started. A start for one of them is dropped:
// the queued run reads its state when it starts, so it sees whatever the
// later start would have. Without this every bump during a long bot drive
// queued one more drive (thousands in a game), each of which later took the
// lease, read the row, found nothing and let go. A job leaves the set when it
// STARTS, so a start while one runs still queues one. Full (more jobs waiting
// than slots): starts queue as they always did.
#define PENDING_MAX 1024
static pthread_mutex_t g_pending_mu = PTHREAD_MUTEX_INITIALIZER;
static char g_pending[PENDING_MAX][JOB_ID_MAX + 1];
static int g_pending_n;

// true when `id` was not pending and now is (or the set is full).
static bool pending_mark(const char *id) {
    pthread_mutex_lock(&g_pending_mu);
    bool add = true;
    for (int i = 0; i < g_pending_n && add; i++) add = strcmp(g_pending[i], id) != 0;
    if (add && g_pending_n < PENDING_MAX) snprintf(g_pending[g_pending_n++], sizeof g_pending[0], "%s", id);
    pthread_mutex_unlock(&g_pending_mu);
    return add;
}

static void pending_unmark(const char *id) {
    pthread_mutex_lock(&g_pending_mu);
    for (int i = 0; i < g_pending_n; i++)
        if (strcmp(g_pending[i], id) == 0) {
            memcpy(g_pending[i], g_pending[--g_pending_n], sizeof g_pending[0]);
            break;
        }
    pthread_mutex_unlock(&g_pending_mu);
}

// The job arrives as every provider's does, through cb_core_job, which owns
// what "nothing to do" (CB_E_NOT_FOUND) means.
static void job_run(void *ud) {
    job_task *j = ud;
    pending_unmark(j->id);
    int rc = cb_core_job(j->ctx, j->job, j->key);
    if (rc < 0) CB_WARN("jobs", "%s: %s", j->id, cb_strerror(rc));
    free(j);
}

static int local_job_start(cb_ctx *ctx, const char *job, const char *key) {
    if (!ctx->job_pool) { CB_WARN("jobs", "%s:%s: no job pool", job, key); return CB_E_STATE; }
    job_task *j = calloc(1, sizeof *j);
    if (!j) return CB_E_NOMEM;
    j->ctx = ctx;
    snprintf(j->job, sizeof j->job, "%s", job);
    snprintf(j->key, sizeof j->key, "%s", key);
    snprintf(j->id, sizeof j->id, "%s:%s", job, key);
    if (!pending_mark(j->id)) { free(j); return CB_OK; }   // one is already waiting
    if (cb_pool_submit(ctx->job_pool, job_run, j) != CB_OK) {
        CB_WARN("jobs", "%s: the job pool is stopping", j->id);
        pending_unmark(j->id);
        free(j);
        return CB_E_STATE;
    }
    return CB_OK;
}

// ---- a start after a delay: a loop timer, no thread held ---------------------------
// Any thread may ask; the timer is armed on the loop thread (cb_loop_post) and
// fires there into local_job_start, which queues the job as any start does (and
// drops it when one is already queued). The wait costs one heap entry.

typedef struct {
    cb_ctx *ctx;
    int64_t delay_ms;
    char    job[CB_JOB_NAME_MAX + 1];
    char    key[CB_JOB_KEY_MAX + 1];
} later_task;

static void later_fire(cb_loop *l, void *ud) {
    (void)l;
    later_task *t = ud;
    int rc = local_job_start(t->ctx, t->job, t->key);
    if (rc != CB_OK) CB_WARN("jobs", "%s:%s: delayed start failed (%s)", t->job, t->key, cb_strerror(rc));
    free(t);
}

static void later_arm(cb_loop *l, void *ud) {
    later_task *t = ud;
    if (!cb_loop_timer(l, t->delay_ms, later_fire, t)) {
        CB_WARN("jobs", "%s:%s: no timer for a delayed start", t->job, t->key);
        free(t);
    }
}

static int local_job_start_after(cb_ctx *ctx, const char *job, const char *key, int64_t delay_ms) {
    if (!ctx->loop) return CB_E_STATE;
    later_task *t = calloc(1, sizeof *t);
    if (!t) return CB_E_NOMEM;
    t->ctx = ctx;
    t->delay_ms = delay_ms;
    snprintf(t->job, sizeof t->job, "%s", job);
    snprintf(t->key, sizeof t->key, "%s", key);
    int rc = cb_loop_post(ctx->loop, later_arm, t);
    if (rc != CB_OK) free(t);
    return rc;
}

const cb_provider cb_provider_local = {
    .name = "local",
    .mode = "serve",
    .flags = CB_PROV_HELD_SOCKETS | CB_PROV_TEST_CLOCK,
    .store = "sqlite",
    .rt_open = cb_rt_local_new,
    .routes = cb_rt_local_routes,
    .job_start = local_job_start,
    .job_start_after = local_job_start_after,
};
