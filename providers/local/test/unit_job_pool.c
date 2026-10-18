// unit_job_pool.c - `cbase serve`'s job pool (provider_local.c): one job per
// "<job>:<key>" waiting at a time, so two job names with the same key are two
// jobs, a repeat of a waiting one is dropped, and a start while one runs
// queues one more.
//
// The pool is one thread, held by a job that waits on a gate, so every start
// made meanwhile is still waiting when the test looks.
#include "t.h"

#include "cbase/app.h"
#include "cbase/provider.h"
#include "ctx.h"
#include "util/clock.h"
#include "util/err.h"
#include "util/log.h"
#include "util/pool.h"

#include <pthread.h>
#include <string.h>
#include <unistd.h>

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static bool g_open;          // the gate the holding job waits on
static int g_holding;        // hold jobs that have started
static char g_ran[512];      // "<job>:<key>" of every run, in order
static int g_runs;

static void note(const char *job, const char *key) {
    pthread_mutex_lock(&g_mu);
    size_t n = strlen(g_ran);
    snprintf(g_ran + n, sizeof g_ran - n, "%s%s:%s", n ? " " : "", job, key);
    g_runs++;
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mu);
}

static int job_hold(cb_ctx *ctx, const char *key) {
    pthread_mutex_lock(&g_mu);
    g_holding++;
    pthread_cond_broadcast(&g_cv);
    while (!g_open) pthread_cond_wait(&g_cv, &g_mu);
    pthread_mutex_unlock(&g_mu);
    note("hold", key);
    return CB_OK;
}
static int job_drive(cb_ctx *ctx, const char *key) { note("drive", key); return CB_OK; }
static int job_other(cb_ctx *ctx, const char *key) { note("other", key); return CB_OK; }

static const cb_job JOBS[] = { { "hold", job_hold }, { "drive", job_drive }, { "other", job_other } };
static const cb_app APP = { .name = "pool_test", .jobs = JOBS, .n_jobs = 3 };

// Waits until `field` reaches n (true) or two seconds pass (false).
static bool wait_for(const int *field, int n) {
    int64_t end = cb_mono_ms() + 2000;
    pthread_mutex_lock(&g_mu);
    while (*field < n && cb_mono_ms() < end) {
        pthread_mutex_unlock(&g_mu);
        usleep(2000);
        pthread_mutex_lock(&g_mu);
    }
    bool ok = *field >= n;
    pthread_mutex_unlock(&g_mu);
    return ok;
}

static void set_gate(bool open) {
    pthread_mutex_lock(&g_mu);
    g_open = open;
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mu);
}

static void reset(void) {
    pthread_mutex_lock(&g_mu);
    g_ran[0] = 0;
    g_runs = 0;
    g_holding = 0;
    g_open = false;
    pthread_mutex_unlock(&g_mu);
}

static int start(cb_ctx *ctx, const char *job, const char *key) { return cb_provider_local.job_start(ctx, job, key); }

static void one_waiting_per_job_and_key(void) {
    cb_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.app = &APP;
    ctx.prov = &cb_provider_local;
    ctx.job_pool = cb_pool_new(1, "tjob");
    T_CHECK(ctx.job_pool != NULL);
    reset();

    // The one thread is held.
    T_EQ_INT(start(&ctx, "hold", "h1"), CB_OK);
    T_CHECK(wait_for(&g_holding, 1));
    // drive:k1 waits; its repeat is dropped (and still CB_OK: it was handed on).
    T_EQ_INT(start(&ctx, "drive", "k1"), CB_OK);
    T_EQ_INT(start(&ctx, "drive", "k1"), CB_OK);
    // Another job with the same key is another job; it waits too, once.
    T_EQ_INT(start(&ctx, "other", "k1"), CB_OK);
    T_EQ_INT(start(&ctx, "other", "k1"), CB_OK);
    // The same job with another key, too.
    T_EQ_INT(start(&ctx, "drive", "k2"), CB_OK);
    T_EQ_INT(start(&ctx, "drive", "k1"), CB_OK);
    T_EQ_INT(cb_pool_pending(ctx.job_pool), 3);

    set_gate(true);
    T_CHECK(wait_for(&g_runs, 4));
    usleep(50000);   // nothing more arrives
    pthread_mutex_lock(&g_mu);
    T_EQ_STR(g_ran, "hold:h1 drive:k1 other:k1 drive:k2");
    T_EQ_INT(g_runs, 4);
    pthread_mutex_unlock(&g_mu);
    cb_pool_free(ctx.job_pool);
}

static void a_start_while_one_runs_queues_one_more(void) {
    cb_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.app = &APP;
    ctx.prov = &cb_provider_local;
    ctx.job_pool = cb_pool_new(1, "tjob");
    reset();

    // hold:h2 has started (it left the waiting set when it did), so a start of
    // it now is a new run, which a later repeat joins.
    T_EQ_INT(start(&ctx, "hold", "h2"), CB_OK);
    T_CHECK(wait_for(&g_holding, 1));
    T_EQ_INT(start(&ctx, "hold", "h2"), CB_OK);
    T_EQ_INT(start(&ctx, "hold", "h2"), CB_OK);
    T_EQ_INT(cb_pool_pending(ctx.job_pool), 1);
    set_gate(true);
    T_CHECK(wait_for(&g_runs, 2));
    usleep(50000);
    pthread_mutex_lock(&g_mu);
    T_EQ_STR(g_ran, "hold:h2 hold:h2");
    T_EQ_INT(g_holding, 2);
    pthread_mutex_unlock(&g_mu);

    // Once it ran, the same job and key starts again.
    reset();
    set_gate(true);
    T_EQ_INT(start(&ctx, "drive", "k1"), CB_OK);
    T_CHECK(wait_for(&g_runs, 1));
    pthread_mutex_lock(&g_mu);
    T_EQ_STR(g_ran, "drive:k1");
    pthread_mutex_unlock(&g_mu);
    cb_pool_free(ctx.job_pool);
}

static void no_pool_is_a_state_error(void) {
    cb_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.app = &APP;
    reset();
    T_EQ_INT(start(&ctx, "drive", "k1"), CB_E_STATE);
    // And it left nothing waiting: with a pool, the same job starts at once.
    ctx.job_pool = cb_pool_new(1, "tjob");
    T_EQ_INT(start(&ctx, "drive", "k1"), CB_OK);
    T_CHECK(wait_for(&g_runs, 1));
    cb_pool_free(ctx.job_pool);
}

int main(void) {
    cb_log_init(CB_LOG_ERROR, 0);
    T_RUN(one_waiting_per_job_and_key);
    T_RUN(a_start_while_one_runs_queues_one_more);
    T_RUN(no_pool_is_a_state_error);
    return T_DONE();
}
