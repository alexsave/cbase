// unit_serve_knobs.c - what an app sets for serve (cbase/app.h "serve's defaults"), seen through a
// real `cbase serve` (cb_process_main): the request pool it asks for is the pool its handlers get.
//
// This program runs ITSELF as the server (lib_proc.h: a scripted child is the test's own binary):
// each serve-* mode serves one small app through the serve ingress.
#include "t.h"
#include "lib_client.h"
#include "lib_proc.h"

#include "cbase/app.h"
#include "cbase/provider.h"
#include "ctx.h"
#include "ingress_process.h"
#include "net/http.h"
#include "num_parse.h"
#include "util/err.h"
#include "util/log.h"

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifdef CBASE_LINKS_LOCAL

// ---- the servers' apps --------------------------------------------------------------------------

// THE POOL. GET /t/meet: a handler that holds its pool thread until POOL_N of them hold theirs at
// once (a barrier), as amgi's handlers hold theirs on a remote call. With POOL_N workers every one
// arrives and all answer "met"; with fewer, the first waits out its deadline, answers "alone", and
// every later one answers "alone" at once (so a short pool fails in one deadline, not POOL_N).
#define POOL_N 12
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static int g_here, g_failed;

static void h_meet(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    pthread_mutex_lock(&g_mu);
    g_here++;
    pthread_cond_broadcast(&g_cv);
    struct timespec end;
    clock_gettime(CLOCK_REALTIME, &end);
    end.tv_sec += 15;   // only a failing pool ever waits it out
    while (g_here < POOL_N && !g_failed)
        if (pthread_cond_timedwait(&g_cv, &g_mu, &end) != 0) g_failed = 1;
    bool met = g_here >= POOL_N;
    if (!met) g_failed = 1;
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mu);
    cb_res_text(res, 200, met ? "met" : "alone");
}

static const cb_route POOL_ROUTES[] = {
    { .method = "GET", .pattern = "/t/meet", .flags = CB_ROUTE_POOL, .fn = h_meet },
};
static const cb_route_list POOL_LIST = { POOL_ROUTES, 1 };
static const cb_route_list *const POOL_LISTS[] = { &POOL_LIST };
static const cb_app POOL_APP = { .name = "knobs-pool", .routes = POOL_LISTS, .n_route_lists = 1, .storeless = true,
                                 .req_workers = POOL_N };

// ---- the tests ----------------------------------------------------------------------------------

static const char *g_self;

#define DEV_ENV "CBASE_DEV=1", "CBASE_LISTEN=127.0.0.1:0", "CBASE_STORE="

// Starts this binary in `mode`; its port from the log's listening line. 0, or -1.
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
    char      body[16];
} meet_call;

static void *meet_run(void *ud) {
    meet_call *m = ud;
    tc_response r;
    int fd = tc_connect(m->port);
    m->status = fd >= 0 && tc_request(fd, "GET", "/t/meet", NULL, NULL, 0, 30000, &r) == 0 ? r.status : -1;
    if (m->status > 0) {
        snprintf(m->body, sizeof m->body, "%s", cb_buf_cstr(&r.body));
        tc_response_free(&r);
    }
    if (fd >= 0) close(fd);
    return NULL;
}

// The app's req_workers is the pool serve runs: POOL_N handlers hold their threads at once.
static void the_apps_pool_is_the_pool(void) {
    char *env[] = { DEV_ENV, NULL };
    tp_child c;
    int port = 0;
    T_EQ_INT(serve(&c, "serve-pool", env, &port), 0);
    meet_call m[POOL_N];
    for (int i = 0; i < POOL_N; i++) {
        m[i] = (meet_call){ .port = port };
        T_EQ_INT(pthread_create(&m[i].th, NULL, meet_run, &m[i]), 0);
    }
    int met = 0;
    for (int i = 0; i < POOL_N; i++) {
        pthread_join(m[i].th, NULL);
        T_EQ_INT(m[i].status, 200);
        met += strcmp(m[i].body, "met") == 0;
    }
    T_EQ_INT(met, POOL_N);
    T_EQ_INT(tp_stop(&c, SIGTERM, 30000), 0);
}

int main(int argc, char **argv) {
    g_self = argv[0];
    if (argc == 2 && strncmp(argv[1], "serve-", 6) == 0) {
        const cb_app *app = strcmp(argv[1], "serve-pool") == 0 ? &POOL_APP : NULL;
        if (!app) return 2;
        char *sargv[] = { argv[0], "serve", NULL };
        return cb_process_main(&cb_provider_local, app, 2, sargv);
    }
    T_RUN(the_apps_pool_is_the_pool);
    return T_DONE();
}

#else

int main(void) {
    T_CHECK(1);   // the local provider is not linked: there is no serve to run an app under
    return T_DONE();
}

#endif
