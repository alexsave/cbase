// unit_slow_pool.c - the slow pool (net/router.h CB_ROUTE_SLOW, cbase/app.h slow_workers,
// slow_queue, slow_full) through a real `cbase serve` (cb_process_main): slow handlers held on a
// full slow pool never hold up a fast route or a file; a slow request past the pool and its queue
// is answered at once (cbase's 503, or the app's own refusal); and a drain waits for the slow
// handlers in flight before serve_close.
//
// No timing: every slow handler is HELD until the test lets it go (a loop route, or a file once
// the listener is shut), and the test waits on what the server says (log lines, a status route),
// never on a sleep. Every wait has a deadline far past a passing run, which a failing one meets.
//
// This program runs ITSELF as the server (lib_proc.h): each serve-* mode serves one small app.
#include "t.h"
#include "lib_client.h"
#include "lib_proc.h"

#include "cbase/app.h"
#include "cbase/provider.h"
#include "ctx.h"
#include "ingress_process.h"
#include "net/files.h"
#include "net/http.h"
#include "num_parse.h"
#include "util/err.h"
#include "util/log.h"
#include "util/pool.h"

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifdef CBASE_LINKS_LOCAL

// ---- the servers' apps --------------------------------------------------------------------------

#define SLOW_N 12   // as many remote calls as an app lets wait at once

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static int g_held, g_released, g_answered;

// GET /t/gem: a slow handler (a remote model call), held until POST /t/release or the file SP_GO
// names exists; 20 s at most, so a failing run ends.
static void h_gem(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    const char *go = getenv("SP_GO");
    pthread_mutex_lock(&g_mu);
    const int held = ++g_held;
    pthread_mutex_unlock(&g_mu);
    CB_INFO("t", "gem held %d", held);
    pthread_mutex_lock(&g_mu);
    bool let_go = false;
    for (int i = 0; i < 2000 && !(let_go = g_released || (go && access(go, F_OK) == 0)); i++) {
        struct timespec t;
        clock_gettime(CLOCK_REALTIME, &t);
        t.tv_nsec += 10 * 1000 * 1000;
        if (t.tv_nsec >= 1000000000) { t.tv_sec++; t.tv_nsec -= 1000000000; }
        pthread_cond_timedwait(&g_cv, &g_mu, &t);
    }
    g_held--;
    if (let_go) g_answered++;
    pthread_mutex_unlock(&g_mu);
    cb_res_text(res, 200, let_go ? "released" : "never let go");
}

// POST /t/release: lets every slow handler go. On the loop, so it answers whatever the pools hold.
static void h_release(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    pthread_mutex_lock(&g_mu);
    g_released = 1;
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mu);
    cb_res_text(res, 200, "ok");
}

// GET /t/status: a fast route, as a client's status poll: the slow handlers held and queued.
static void h_status(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    pthread_mutex_lock(&g_mu);
    const int held = g_held;
    pthread_mutex_unlock(&g_mu);
    char b[64];
    snprintf(b, sizeof b, "held=%d queued=%zu", held, ctx->slow_pool ? cb_pool_pending(ctx->slow_pool) : (size_t)0);
    cb_res_text(res, 200, b);
}

// GET /t/file: a static file (net/files.h), as a local app's shell and media.
static void h_file(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    cb_send_file(ctx, req, res, getenv("SP_FILE"), "text/plain", "no-store");
}

// The app's own refusal, in its own wire format.
static void h_full(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    cb_res_text(res, 429, "notStarted");
    cb_res_header(res, "Retry-After", "30");
}

static void sp_close(cb_ctx *ctx) {
    pthread_mutex_lock(&g_mu);
    const int n = g_answered;
    pthread_mutex_unlock(&g_mu);
    CB_INFO("t", "serve_close: %d slow answered", n);
}

static const cb_route ROUTES[] = {
    { .method = "GET", .pattern = "/t/gem", .flags = CB_ROUTE_SLOW, .fn = h_gem },
    { .method = "POST", .pattern = "/t/release", .flags = CB_ROUTE_LOOP, .fn = h_release },
    { .method = "GET", .pattern = "/t/status", .flags = CB_ROUTE_POOL, .fn = h_status },
    { .method = "GET", .pattern = "/t/file", .flags = CB_ROUTE_POOL, .fn = h_file },
};
static const cb_route_list LIST = { ROUTES, 4 };
static const cb_route_list *const LISTS[] = { &LIST };
// The request pool is cbase's one worker: a slow handler on it would hold every fast route.
static const cb_app SLOW_APP = { .name = "slow", .routes = LISTS, .n_route_lists = 1, .storeless = true,
                                 .slow_workers = SLOW_N, .serve_close = sp_close };
static const cb_app HOOK_APP = { .name = "slow-hook", .routes = LISTS, .n_route_lists = 1, .storeless = true,
                                 .slow_workers = SLOW_N, .slow_full = h_full, .serve_close = sp_close };

// ---- the tests ----------------------------------------------------------------------------------

static const char *g_self;

#define DEV_ENV "CBASE_DEV=1", "CBASE_LISTEN=127.0.0.1:0", "CBASE_STORE="

static int serve(tp_child *c, const char *mode, char **env, int *port) {
    char *argv[] = { (char *)g_self, (char *)mode, NULL };
    if (tp_start(c, argv, env) != 0) return -1;
    char line[512] = "";
    if (tp_wait_line(c, "listening on", 20000, line, sizeof line) != 0) return -1;
    const char *colon = strrchr(line, ':');
    int64_t p = 0;
    if (!colon || !num_parse_i64(colon + 1, strcspn(colon + 1, " \r\n"), 1, 65535, &p)) return -1;
    *port = (int)p;
    return 0;
}

typedef struct {
    int       port;
    pthread_t th;
    int       status;
    char      body[32];
} call;

static void *gem_run(void *ud) {
    call *m = ud;
    tc_response r;
    int fd = tc_connect(m->port);
    m->status = fd >= 0 && tc_request(fd, "GET", "/t/gem", NULL, NULL, 0, 30000, &r) == 0 ? r.status : -1;
    if (m->status > 0) {
        snprintf(m->body, sizeof m->body, "%s", cb_buf_cstr(&r.body));
        tc_response_free(&r);
    }
    if (fd >= 0) close(fd);
    return NULL;
}

// One request with a short deadline (a fast route answers in milliseconds; 3 s is only ever met
// by one queued behind a slow handler): its status, the body into body, a header into hv.
static int quick(int port, const char *method, const char *path, char *body, size_t cap, const char *hname, char *hv, size_t hcap) {
    tc_response r;
    int fd = tc_connect(port);
    int st = fd >= 0 && tc_request(fd, method, path, NULL, NULL, 0, 3000, &r) == 0 ? r.status : -1;
    if (st > 0) {
        if (body) snprintf(body, cap, "%s", cb_buf_cstr(&r.body));
        if (hname && !tc_header(&r, hname, hv, hcap)) hv[0] = 0;
        tc_response_free(&r);
    }
    if (fd >= 0) close(fd);
    return st;
}

static void start_gems(call *m, int n, int port) {
    for (int i = 0; i < n; i++) {
        m[i] = (call){ .port = port };
        T_EQ_INT(pthread_create(&m[i].th, NULL, gem_run, &m[i]), 0);
    }
}

static int join_released(call *m, int n) {
    int ok = 0;
    for (int i = 0; i < n; i++) {
        pthread_join(m[i].th, NULL);
        ok += m[i].status == 200 && strcmp(m[i].body, "released") == 0;
    }
    return ok;
}

// SLOW_N slow handlers held at once on a slow pool of SLOW_N, and meanwhile the fast route and a
// file, on cbase's one request worker, answer at once.
static void fast_routes_answer_while_the_slow_pool_is_held(void) {
    char dir[256], file[300], env_file[320];
    T_EQ_INT(tp_tempdir(dir, sizeof dir), 0);
    snprintf(file, sizeof file, "%s/shell.txt", dir);
    FILE *f = fopen(file, "w");
    T_CHECK(f != NULL);
    if (f) { fputs("the app's shell\n", f); fclose(f); }
    snprintf(env_file, sizeof env_file, "SP_FILE=%s", file);
    char *env[] = { DEV_ENV, env_file, NULL };
    tp_child c;
    int port = 0;
    T_EQ_INT(serve(&c, "serve-slow", env, &port), 0);
    call m[SLOW_N];
    start_gems(m, SLOW_N, port);
    char line[512] = "";
    T_EQ_INT(tp_wait_line(&c, "gem held 12", 20000, line, sizeof line), 0);
    char body[64] = "";
    T_EQ_INT(quick(port, "GET", "/t/status", body, sizeof body, NULL, NULL, 0), 200);
    T_EQ_STR(body, "held=12 queued=0");
    T_EQ_INT(quick(port, "GET", "/t/file", body, sizeof body, NULL, NULL, 0), 200);
    T_EQ_STR(body, "the app's shell\n");
    T_EQ_INT(quick(port, "GET", "/t/status", body, sizeof body, NULL, NULL, 0), 200);   // and again
    T_EQ_STR(body, "held=12 queued=0");
    T_EQ_INT(quick(port, "POST", "/t/release", NULL, 0, NULL, NULL, 0), 200);
    T_EQ_INT(join_released(m, SLOW_N), SLOW_N);
    T_EQ_INT(tp_stop(&c, SIGTERM, 30000), 0);
    unlink(file);
    rmdir(dir);
}

// The slow pool's two workers held and its one queued place taken (CBASE_SLOW_WORKERS and
// CBASE_SLOW_QUEUE over the app's 12 and 0): the next slow request is answered at once, by
// cbase's 503 or by the app's slow_full, and a fast route still answers.
static void full(const char *mode, int want_status, const char *want_body, const char *want_retry) {
    char *env[] = { DEV_ENV, "CBASE_SLOW_WORKERS=2", "CBASE_SLOW_QUEUE=1", NULL };
    tp_child c;
    int port = 0;
    T_EQ_INT(serve(&c, mode, env, &port), 0);
    char line[512] = "";
    call m[3];
    start_gems(m, 3, port);
    T_EQ_INT(tp_wait_line(&c, "gem held 2", 20000, line, sizeof line), 0);
    char body[64] = "";
    // The third is in the queue once the status says so (the server's own count, not a sleep).
    // 20 s at most in all, a failing status call included.
    for (time_t end = time(NULL) + 20; strcmp(body, "held=2 queued=1") != 0 && time(NULL) < end;) {
        if (quick(port, "GET", "/t/status", body, sizeof body, NULL, NULL, 0) != 200) break;
        if (strcmp(body, "held=2 queued=1") != 0) usleep(50000);
    }
    T_EQ_STR(body, "held=2 queued=1");
    char retry[16] = "";
    T_EQ_INT(quick(port, "GET", "/t/gem", body, sizeof body, "Retry-After", retry, sizeof retry), want_status);
    T_EQ_STR(body, want_body);
    T_EQ_STR(retry, want_retry);
    T_EQ_INT(quick(port, "GET", "/t/status", body, sizeof body, NULL, NULL, 0), 200);   // nothing changed
    T_EQ_STR(body, "held=2 queued=1");
    T_EQ_INT(quick(port, "POST", "/t/release", NULL, 0, NULL, NULL, 0), 200);
    T_EQ_INT(join_released(m, 3), 3);   // the queued one ran once a worker was free
    T_EQ_INT(tp_stop(&c, SIGTERM, 30000), 0);
}

static void a_full_slow_pool_answers_503_at_once(void) { full("serve-slow", 503, "server busy", "5"); }
static void a_full_slow_pool_answers_the_apps_refusal(void) { full("serve-hook", 429, "notStarted", "30"); }

// SIGTERM with slow handlers in flight: the drain waits for them (let go once the listener is
// shut, by a file), every one answers, and only then serve_close runs.
static void the_drain_waits_for_the_slow_handlers(void) {
    char dir[256], go[300], env_go[320];
    T_EQ_INT(tp_tempdir(dir, sizeof dir), 0);
    snprintf(go, sizeof go, "%s/go", dir);
    snprintf(env_go, sizeof env_go, "SP_GO=%s", go);
    char *env[] = { DEV_ENV, env_go, NULL };
    tp_child c;
    int port = 0;
    T_EQ_INT(serve(&c, "serve-slow", env, &port), 0);
    call m[3];
    start_gems(m, 3, port);
    char line[512] = "";
    T_EQ_INT(tp_wait_line(&c, "gem held 3", 20000, line, sizeof line), 0);
    T_EQ_INT(kill(c.pid, SIGTERM), 0);
    T_EQ_INT(tp_wait_line(&c, "SIGTERM from pid", 20000, line, sizeof line), 0);
    FILE *f = fopen(go, "w");
    T_CHECK(f != NULL);
    if (f) fclose(f);
    T_EQ_INT(join_released(m, 3), 3);
    T_EQ_INT(tp_wait_line(&c, "serve_close: 3 slow answered", 20000, line, sizeof line), 0);
    T_EQ_INT(tp_stop(&c, 0, 30000), 0);
    unlink(go);
    rmdir(dir);
}

int main(int argc, char **argv) {
    g_self = argv[0];
    if (argc == 2 && strncmp(argv[1], "serve-", 6) == 0) {
        const cb_app *app = strcmp(argv[1], "serve-slow") == 0   ? &SLOW_APP
                            : strcmp(argv[1], "serve-hook") == 0 ? &HOOK_APP
                                                                 : NULL;
        if (!app) return 2;
        char *sargv[] = { argv[0], "serve", NULL };
        return cb_process_main(&cb_provider_local, app, 2, sargv);
    }
    T_RUN(fast_routes_answer_while_the_slow_pool_is_held);
    T_RUN(a_full_slow_pool_answers_503_at_once);
    T_RUN(a_full_slow_pool_answers_the_apps_refusal);
    T_RUN(the_drain_waits_for_the_slow_handlers);
    return T_DONE();
}

#else

int main(void) {
    T_CHECK(1);   // the local provider is not linked: there is no serve to run an app under
    return T_DONE();
}

#endif
