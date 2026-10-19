// lib_fake_runtime.c - Lambda's Runtime API, faked (lib_fake_runtime.h).
#include "lib_fake_runtime.h"

#include "ctx.h"
#include "net/loop.h"
#include "net/router.h"
#include "net/server.h"
#include "util/err.h"
#include "util/pool.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../../../../shared/c/clock.h"

struct fr_runtime {
    cb_ctx          ctx;
    cb_config       cfg;
    pthread_t       th;
    pthread_mutex_t mu;
    pthread_cond_t  cv;
    char            arn[160];
    char           *events[FR_MAX];
    int             n_queued, n_served, n_nexts, n_init_errors;
    bool            done;
    char            path[FR_MAX][256];
    cb_buf          body[FR_MAX];
    int             order[FR_MAX], n_posted;
    char            arrivals[FR_MAX][128];
    int             n_arrivals;
};

// The handlers reach the runtime through the context they are given.
static fr_runtime *of(cb_ctx *c) { return (fr_runtime *)((char *)c - offsetof(fr_runtime, ctx)); }

static void arrive_locked(fr_runtime *fr, const char *what) {
    if (fr->n_arrivals < FR_MAX) snprintf(fr->arrivals[fr->n_arrivals++], sizeof fr->arrivals[0], "%s", what);
}

static void deadline_in(struct timespec *t, int ms) {
    host_deadline_after_ms(t, ms);
}

static void h_next(cb_ctx *c, const cb_http_req *q, cb_http_res *s) {
    fr_runtime *fr = of(c);
    pthread_mutex_lock(&fr->mu);
    fr->n_nexts++;
    struct timespec until;
    deadline_in(&until, 20000);
    while (fr->n_served == fr->n_queued && !fr->done)
        if (pthread_cond_timedwait(&fr->cv, &fr->mu, &until) != 0) break;
    if (fr->n_served == fr->n_queued) {
        pthread_mutex_unlock(&fr->mu);
        cb_res_text(s, 500, "no more\n");
        return;
    }
    int i = fr->n_served++;
    const char *ev = fr->events[i];
    pthread_mutex_unlock(&fr->mu);
    char id[16];
    snprintf(id, sizeof id, "req-%d", i + 1);
    cb_res_header(s, "Lambda-Runtime-Aws-Request-Id", id);
    cb_res_header(s, "Lambda-Runtime-Deadline-Ms", "9999999999999");
    cb_res_header(s, "Lambda-Runtime-Invoked-Function-Arn", fr->arn);
    cb_res_send(s, 200, "application/json", ev, strlen(ev));
}

// /2018-06-01/runtime/invocation/req-<n>/{response,error}
static void h_post(cb_ctx *c, const cb_http_req *q, cb_http_res *s) {
    fr_runtime *fr = of(c);
    const char *r = strstr(q->path, "/req-");
    int i = r ? atoi(r + 5) - 1 : -1;
    if (i < 0 || i >= FR_MAX) { cb_res_text(s, 400, "unknown request id\n"); return; }
    pthread_mutex_lock(&fr->mu);
    snprintf(fr->path[i], sizeof fr->path[i], "%s", q->path);
    cb_buf_reset(&fr->body[i]);
    cb_buf_append(&fr->body[i], q->body, q->body_len);
    if (fr->n_posted < FR_MAX) fr->order[fr->n_posted++] = i;
    char what[64];
    snprintf(what, sizeof what, "%s req-%d", strstr(q->path, "/error") ? "error" : "response", i + 1);
    arrive_locked(fr, what);
    pthread_cond_broadcast(&fr->cv);
    pthread_mutex_unlock(&fr->mu);
    cb_res_text(s, 202, "");
}

static void h_init_error(cb_ctx *c, const cb_http_req *q, cb_http_res *s) {
    fr_runtime *fr = of(c);
    pthread_mutex_lock(&fr->mu);
    fr->n_init_errors++;
    pthread_mutex_unlock(&fr->mu);
    fprintf(stderr, "init/error: %.*s\n", (int)q->body_len, (const char *)q->body);
    cb_res_text(s, 202, "");
}

static void *run(void *a) {
    cb_loop_run(((fr_runtime *)a)->ctx.loop);
    return NULL;
}

fr_runtime *fr_start(const char *function_arn, int *port) {
    fr_runtime *fr = calloc(1, sizeof *fr);
    if (!fr) return NULL;
    pthread_mutex_init(&fr->mu, NULL);
    pthread_cond_init(&fr->cv, NULL);
    snprintf(fr->arn, sizeof fr->arn, "%s", function_arn);
    for (int i = 0; i < FR_MAX; i++) cb_buf_init(&fr->body[i]);
    strcpy(fr->cfg.allowed_origins, "*");
    fr->ctx.cfg = &fr->cfg;
    fr->ctx.loop = cb_loop_new();
    fr->ctx.req_pool = cb_pool_new(4, "rt");
    fr->ctx.router = cb_router_new();
    cb_router_add(fr->ctx.router, "GET", "/2018-06-01/runtime/invocation/next", 0, h_next);
    cb_router_add(fr->ctx.router, "POST", "/2018-06-01/runtime/invocation/*", 0, h_post);
    cb_router_add(fr->ctx.router, "POST", "/2018-06-01/runtime/init/error", 0, h_init_error);
    fr->ctx.server = cb_server_new(&fr->ctx, NULL);
    if (!fr->ctx.server || cb_server_listen(fr->ctx.server, "127.0.0.1", 0, port) != CB_OK) {
        fr_stop(fr);
        return NULL;
    }
    pthread_create(&fr->th, NULL, run, fr);
    return fr;
}

void fr_queue(fr_runtime *fr, const char *event) {
    pthread_mutex_lock(&fr->mu);
    if (fr->n_queued < FR_MAX) fr->events[fr->n_queued++] = strdup(event);
    pthread_cond_broadcast(&fr->cv);
    pthread_mutex_unlock(&fr->mu);
}

void fr_done(fr_runtime *fr) {
    pthread_mutex_lock(&fr->mu);
    fr->done = true;
    pthread_cond_broadcast(&fr->cv);
    pthread_mutex_unlock(&fr->mu);
}

bool fr_wait_posted(fr_runtime *fr, int n, int timeout_ms) {
    pthread_mutex_lock(&fr->mu);
    struct timespec until;
    deadline_in(&until, timeout_ms);
    while (fr->n_posted < n)
        if (pthread_cond_timedwait(&fr->cv, &fr->mu, &until) != 0) break;
    bool ok = fr->n_posted >= n;
    pthread_mutex_unlock(&fr->mu);
    return ok;
}

void fr_note(fr_runtime *fr, const char *what) {
    pthread_mutex_lock(&fr->mu);
    arrive_locked(fr, what);
    pthread_cond_broadcast(&fr->cv);
    pthread_mutex_unlock(&fr->mu);
}

const char *fr_posted_path(fr_runtime *fr, int i) { return i >= 0 && i < FR_MAX ? fr->path[i] : ""; }
cb_buf *fr_posted_body(fr_runtime *fr, int i) { return i >= 0 && i < FR_MAX ? &fr->body[i] : &fr->body[0]; }

static int locked_int(fr_runtime *fr, const int *v) {
    pthread_mutex_lock(&fr->mu);
    int n = *v;
    pthread_mutex_unlock(&fr->mu);
    return n;
}

int fr_n_posted(fr_runtime *fr) { return locked_int(fr, &fr->n_posted); }
int fr_posted_order(fr_runtime *fr, int k) { return k >= 0 && k < fr_n_posted(fr) ? fr->order[k] : -1; }
int fr_n_arrivals(fr_runtime *fr) { return locked_int(fr, &fr->n_arrivals); }
const char *fr_arrival(fr_runtime *fr, int k) { return k >= 0 && k < fr_n_arrivals(fr) ? fr->arrivals[k] : ""; }
int fr_nexts(fr_runtime *fr) { return locked_int(fr, &fr->n_nexts); }
int fr_init_errors(fr_runtime *fr) { return locked_int(fr, &fr->n_init_errors); }

static void stop_loop(cb_loop *l, void *ud) { cb_loop_stop(l); }

void fr_stop(fr_runtime *fr) {
    if (!fr) return;
    fr_done(fr);
    if (fr->th) {
        cb_loop_post(fr->ctx.loop, stop_loop, NULL);
        pthread_join(fr->th, NULL);
    }
    // Down in reverse, as main_serve.c does.
    cb_pool_free(fr->ctx.req_pool);
    cb_server_free(fr->ctx.server);
    cb_router_free(fr->ctx.router);
    cb_loop_free(fr->ctx.loop);
    for (int i = 0; i < FR_MAX; i++) {
        cb_buf_free(&fr->body[i]);
        free(fr->events[i]);
    }
    pthread_mutex_destroy(&fr->mu);
    pthread_cond_destroy(&fr->cv);
    free(fr);
}
