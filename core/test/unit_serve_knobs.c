// unit_serve_knobs.c - what an app sets for serve (cbase/app.h "serve's defaults"), seen through a
// real `cbase serve` (cb_process_main): the request pool it asks for is the pool its handlers get,
// and its log level quiets the log but never the startup line; and Ctrl-C (SIGINT) drains as
// SIGTERM does, then runs the app's serve_close.
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

#include <poll.h>
#include <stdatomic.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
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

// THE LOG LEVEL. An app that ships "error": the log keeps no INFO line, and the startup line on
// stdout, which is not the log's, is printed all the same.
static const cb_app QUIET_APP = { .name = "knobs-quiet", .storeless = true, .log_level = "error" };

// SIGINT. GET /t/slow: a request in flight when Ctrl-C comes, which holds its pool thread until
// the test makes the file KNOBS_GO names (so it is still running when the drain starts, by
// construction, not by a sleep). serve_close says whether that request had finished: the drain
// must let it finish before anything is closed.
static atomic_int g_slow_done;

static void h_slow(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    const char *go = getenv("KNOBS_GO");
    CB_INFO("t", "slow: started");
    bool seen = false;
    for (int i = 0; go && i < 2000 && !(seen = access(go, F_OK) == 0); i++) usleep(10000);
    atomic_store(&g_slow_done, 1);
    cb_res_text(res, 200, seen ? "done" : "no go");
}

static void slow_close(cb_ctx *ctx) {
    CB_INFO("t", "serve_close: the slow request done %d", atomic_load(&g_slow_done));
}

static const cb_route SLOW_ROUTES[] = {
    { .method = "GET", .pattern = "/t/slow", .flags = CB_ROUTE_POOL, .fn = h_slow },
};
static const cb_route_list SLOW_LIST = { SLOW_ROUTES, 1 };
static const cb_route_list *const SLOW_LISTS[] = { &SLOW_LIST };
static const cb_app SLOW_APP = { .name = "knobs-slow", .routes = SLOW_LISTS, .n_route_lists = 1, .storeless = true,
                                 .serve_close = slow_close };

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

// One line from fd (the child's stdout), within timeout_ms. 0, or -1.
static int read_line(int fd, char *out, size_t cap, int timeout_ms) {
    size_t n = 0;
    for (int waited = 0; n + 1 < cap; ) {
        struct pollfd p = { .fd = fd, .events = POLLIN };
        int r = poll(&p, 1, 100);
        if (r == 0) {
            if ((waited += 100) >= timeout_ms) return -1;
            continue;
        }
        if (r < 0 || read(fd, out + n, 1) != 1) return -1;
        if (out[n] == '\n') { out[n] = 0; return 0; }
        n++;
    }
    return -1;
}

// At "error" the startup line is on stdout, and the log on stderr holds no INFO line.
static void the_startup_line_at_every_level(void) {
    char *env[] = { DEV_ENV, NULL };
    char *argv[] = { (char *)g_self, "serve-quiet", NULL };
    tp_child c;
    T_EQ_INT(tp_start_out(&c, argv, env), 0);
    char line[256] = "";
    T_EQ_INT(read_line(c.out_fd, line, sizeof line, 20000), 0);
    T_CHECK(strncmp(line, "cbase: listening 127.0.0.1:", 27) == 0);
    close(c.out_fd);
    kill(c.pid, SIGTERM);
    T_EQ_INT(tp_wait_line(&c, " INFO ", 20000, line, sizeof line), -1);   // to the end of its stderr: none
    T_EQ_INT(tp_stop(&c, 0, 30000), 0);
}

typedef struct {
    int       port;
    pthread_t th;
    int       status;
    char      body[16];
} slow_call;

static void *slow_run(void *ud) {
    slow_call *m = ud;
    tc_response r;
    int fd = tc_connect(m->port);
    m->status = fd >= 0 && tc_request(fd, "GET", "/t/slow", NULL, NULL, 0, 30000, &r) == 0 ? r.status : -1;
    if (m->status > 0) {
        snprintf(m->body, sizeof m->body, "%s", cb_buf_cstr(&r.body));
        tc_response_free(&r);
    }
    if (fd >= 0) close(fd);
    return NULL;
}

// Ctrl-C is SIGTERM's graceful drain: the request in flight completes, then serve_close runs, and
// the process exits 0.
static void sigint_drains_then_closes(void) {
    char dir[256], go[300], env_go[320];
    T_EQ_INT(tp_tempdir(dir, sizeof dir), 0);
    snprintf(go, sizeof go, "%s/go", dir);
    snprintf(env_go, sizeof env_go, "KNOBS_GO=%s", go);
    char *env[] = { DEV_ENV, env_go, NULL };
    tp_child c;
    int port = 0;
    T_EQ_INT(serve(&c, "serve-slow", env, &port), 0);
    slow_call m = { .port = port };
    T_EQ_INT(pthread_create(&m.th, NULL, slow_run, &m), 0);
    char line[512] = "";
    T_EQ_INT(tp_wait_line(&c, "slow: started", 20000, line, sizeof line), 0);
    T_EQ_INT(kill(c.pid, SIGINT), 0);
    T_EQ_INT(tp_wait_line(&c, "SIGINT from pid", 20000, line, sizeof line), 0);
    FILE *f = fopen(go, "w");
    T_CHECK(f != NULL);
    if (f) fclose(f);
    pthread_join(m.th, NULL);
    T_EQ_INT(m.status, 200);
    T_EQ_STR(m.body, "done");
    T_EQ_INT(tp_wait_line(&c, "serve_close: the slow request done 1", 20000, line, sizeof line), 0);
    T_EQ_INT(tp_stop(&c, 0, 30000), 0);
    unlink(go);
    rmdir(dir);
}

int main(int argc, char **argv) {
    g_self = argv[0];
    if (argc == 2 && strncmp(argv[1], "serve-", 6) == 0) {
        const cb_app *app = strcmp(argv[1], "serve-pool") == 0    ? &POOL_APP
                            : strcmp(argv[1], "serve-quiet") == 0 ? &QUIET_APP
                            : strcmp(argv[1], "serve-slow") == 0  ? &SLOW_APP
                                                                  : NULL;
        if (!app) return 2;
        char *sargv[] = { argv[0], "serve", NULL };
        return cb_process_main(&cb_provider_local, app, 2, sargv);
    }
    T_RUN(the_apps_pool_is_the_pool);
    T_RUN(the_startup_line_at_every_level);
    T_RUN(sigint_drains_then_closes);
    return T_DONE();
}

#else

int main(void) {
    T_CHECK(1);   // the local provider is not linked: there is no serve to run an app under
    return T_DONE();
}

#endif
