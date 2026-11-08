// unit_spawn_serve.c - children started by an app's handlers under `cbase serve` (spawn.h through
// ctx->spawner): one waited for in its request, one that outlives its request and is ended when the
// server is SIGTERMed, so none outlives the server.
//
// This program runs ITSELF as the server (lib_proc.h: a scripted child is the test's own binary):
// its `serve-spawn` mode serves a small app through the serve ingress (cb_process_main).
#include "t.h"
#include "lib_client.h"
#include "lib_proc.h"

#include "cbase/app.h"
#include "cbase/provider.h"
#include "ctx.h"
#include "ingress_process.h"
#include "net/http.h"
#include "num_parse.h"
#include "spawn.h"
#include "util/err.h"
#include "util/log.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifdef CBASE_LINKS_LOCAL

// ---- the server's app ---------------------------------------------------------------------------

// GET /t/run: a child waited for in the request (a pool thread): its exit code and stdout.
static void h_run(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    cb_spawn_result r;
    const char *const argv[] = { "/bin/sh", "-c", "read x; echo \"got $x\"; exit 3", NULL };
    int rc = cb_spawn_wait(ctx->spawner, &(cb_spawn_opts){ .argv = argv, .stdin_data = "in\n", .stdin_n = 3 }, &r);
    char b[256];
    snprintf(b, sizeof b, "rc %d exit %d out %.*s", rc, r.exit_code, (int)r.out_n, r.out ? r.out : "");
    cb_spawn_result_free(&r);
    cb_res_text(res, 200, b);
}

// GET /t/start: a child that runs on after the request; its pid comes back on fd 3 first.
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static char g_pid[32];

static void on_pid(cb_spawner *sp, uint64_t id, const char *line, size_t n, void *ud) {
    pthread_mutex_lock(&g_mu);
    snprintf(g_pid, sizeof g_pid, "%s", line);
    pthread_cond_signal(&g_cv);
    pthread_mutex_unlock(&g_mu);
}

static void h_start(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    const char *const argv[] = { "/bin/sh", "-c", "echo $$ >&3; exec /bin/sleep 300", NULL };
    int rc = cb_spawn(ctx->spawner, &(cb_spawn_opts){ .argv = argv, .channel = true, .on_line = on_pid }, NULL);
    pthread_mutex_lock(&g_mu);
    for (int i = 0; rc == CB_OK && !g_pid[0] && i < 200; i++) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 50 * 1000000L;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&g_cv, &g_mu, &ts);
    }
    char b[64];
    snprintf(b, sizeof b, "%s", g_pid);
    pthread_mutex_unlock(&g_mu);
    cb_res_text(res, b[0] ? 200 : 500, b);
}

// GET /t/wait: a request that waits (cb_spawn_wait) for a child that would run 300 s; its pid is
// logged. The shutdown must end the child before it joins the pool this request holds.
static void on_wait_pid(cb_spawner *sp, uint64_t id, const char *line, size_t n, void *ud) {
    CB_INFO("t", "the waited-for child is pid %s", line);
}

static void h_wait(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    cb_spawn_result r;
    const char *const argv[] = { "/bin/sh", "-c", "echo $$ >&3; exec /bin/sleep 300", NULL };
    int rc = cb_spawn_wait(ctx->spawner, &(cb_spawn_opts){ .argv = argv, .channel = true, .on_line = on_wait_pid, .grace_ms = 200 }, &r);
    char b[128];
    snprintf(b, sizeof b, "rc %d killed %s", rc, cb_spawn_why_name(r.killed));
    cb_spawn_result_free(&r);
    cb_res_text(res, 200, b);
}

static const cb_route ROUTES[] = {
    { "GET", "/t/run", CB_ROUTE_POOL, h_run, 0 },
    { "GET", "/t/start", CB_ROUTE_POOL, h_start, 0 },
    { "GET", "/t/wait", CB_ROUTE_POOL, h_wait, 0 },
};
static const cb_route_list LIST = { ROUTES, 3 };
static const cb_route_list *const LISTS[] = { &LIST };
static const cb_app APP = { .name = "spawn-serve", .routes = LISTS, .n_route_lists = 1, .storeless = true };

// ---- the test -----------------------------------------------------------------------------------

static const char *g_self;

static int get(int port, const char *path, tc_response *r) {
    memset(r, 0, sizeof *r);
    if (tc_http(port, "GET", path, NULL, NULL, 0, r) != 0) return -1;
    return r->status;
}

static bool gone(int pid) { return tp_gone(pid, 5000) == 1; }

static void children_end_with_the_server(void) {
    char *env[] = { "CBASE_DEV=1", "CBASE_LISTEN=127.0.0.1:0", "CBASE_JWT_SECRET=spawn-serve-secret-spawn-serve-secret", "CBASE_STORE=",
                    "CBASE_REQ_WORKERS=2", NULL };
    char *argv[] = { (char *)g_self, "serve-spawn", NULL };
    tp_child c;
    T_EQ_INT(tp_start(&c, argv, env), 0);
    char line[512] = "";
    T_EQ_INT(tp_wait_line(&c, "listening on", 20000, line, sizeof line), 0);
    const char *colon = strrchr(line, ':');
    int64_t port = 0;
    T_CHECK(colon && num_parse_i64(colon + 1, strcspn(colon + 1, " \r\n"), 1, 65535, &port));

    tc_response r;
    T_EQ_INT(get((int)port, "/t/run", &r), 200);
    T_EQ_STR(cb_buf_cstr(&r.body), "rc 0 exit 3 out got in\n");
    tc_response_free(&r);

    T_EQ_INT(get((int)port, "/t/start", &r), 200);
    int64_t pid = 0;
    T_CHECK(num_parse_i64(cb_buf_cstr(&r.body), strlen(cb_buf_cstr(&r.body)), 2, INT32_MAX, &pid));
    tc_response_free(&r);
    T_CHECK(pid > 0 && kill((pid_t)pid, 0) == 0);   // it outlived its request

    // A request waiting for its child when the server is told to stop: the drain waits for it, the
    // loop stops, the child is ended, the request answers, and only then is its pool joined.
    int fd = tc_connect((int)port);
    T_CHECK(fd >= 0);
    const char *ask = "GET /t/wait HTTP/1.1\r\nHost: x\r\n\r\n";
    T_EQ_INT(write(fd, ask, strlen(ask)), (long)strlen(ask));
    T_EQ_INT(tp_wait_line(&c, "the waited-for child is pid", 20000, line, sizeof line), 0);
    const char *p = strstr(line, "pid ");
    int64_t wpid = 0;
    T_CHECK(p && num_parse_i64(p + 4, strcspn(p + 4, " \r\n"), 2, INT32_MAX, &wpid));

    T_EQ_INT(tp_stop(&c, SIGTERM, 30000), 0);
    T_CHECK(pid > 0 && gone((int)pid));     // and not the server
    T_CHECK(wpid > 0 && gone((int)wpid));
    char got[512] = "";
    ssize_t n = read(fd, got, sizeof got - 1);
    got[n > 0 ? n : 0] = 0;
    if (n > 0) T_CHECK(strstr(got, "killed shutdown") != NULL);   // answered, when the drain let it
    close(fd);
}

int main(int argc, char **argv) {
    g_self = argv[0];
    if (argc == 2 && strcmp(argv[1], "serve-spawn") == 0) {
        char *sargv[] = { argv[0], "serve", NULL };
        return cb_process_main(&cb_provider_local, &APP, 2, sargv);
    }
    T_RUN(children_end_with_the_server);
    return T_DONE();
}

#else

int main(void) {
    T_CHECK(1);   // the local provider is not linked: there is no serve to run children under
    return T_DONE();
}

#endif
