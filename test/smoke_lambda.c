// smoke_lambda.c - the real `bootstrap` binary against a fake Lambda Runtime API.
//
//   build/test/smoke_lambda build/bootstrap
//
// This program IS the Runtime API: a cb_server on a free port answering
// /2018-06-01/runtime/invocation/next with a Function URL event for
// GET /health, recording what bootstrap POSTs back, and answering the second
// /next with 500 so bootstrap's loop ends (as it does when Lambda goes away).
// No AWS, no Docker: the Lambda path is tested on every machine `make check`
// runs on.
//
// TODAY aws/event.c is a stub, so the invocation is answered on /error with
// "event decode: CB_E_NOTIMPL"; that, a clean start (no init/error) and a
// clean exit are what is asserted. When event.c lands, the response lane
// asserts statusCode 200 and the health text, and the stub branch must go.
#include "t.h"

#include "lib_proc.h"
#include "net/loop.h"
#include "net/router.h"
#include "net/server.h"
#include "util/err.h"
#include "util/log.h"
#include "util/pool.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>

static cb_ctx g_ctx;
static cb_config g_cfg;
static atomic_int g_nexts;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_posted_path[256];
static cb_buf g_posted_body;
static atomic_int g_init_error;

static const char EVENT[] =
    "{\"version\":\"2.0\",\"routeKey\":\"$default\",\"rawPath\":\"/health\",\"rawQueryString\":\"\","
    "\"headers\":{\"host\":\"abc.lambda-url.us-west-1.on.aws\",\"user-agent\":\"smoke\"},"
    "\"requestContext\":{\"http\":{\"method\":\"GET\",\"path\":\"/health\",\"protocol\":\"HTTP/1.1\","
    "\"sourceIp\":\"127.0.0.1\"},\"requestId\":\"req-1\",\"stage\":\"$default\"},\"isBase64Encoded\":false}";

static void h_next(cb_ctx *c, const cb_http_req *q, cb_http_res *s) {
    if (atomic_fetch_add(&g_nexts, 1) > 0) { cb_res_text(s, 500, "no more\n"); return; }
    cb_res_header(s, "Lambda-Runtime-Aws-Request-Id", "req-1");
    cb_res_header(s, "Lambda-Runtime-Deadline-Ms", "9999999999999");
    cb_res_header(s, "Lambda-Runtime-Invoked-Function-Arn", "arn:aws:lambda:us-west-1:0:function:cbase");
    cb_res_send(s, 200, "application/json", EVENT, sizeof EVENT - 1);
}

static void h_post(cb_ctx *c, const cb_http_req *q, cb_http_res *s) {
    pthread_mutex_lock(&g_lock);
    snprintf(g_posted_path, sizeof g_posted_path, "%s", q->path);
    cb_buf_reset(&g_posted_body);
    cb_buf_append(&g_posted_body, q->body, q->body_len);
    pthread_mutex_unlock(&g_lock);
    cb_res_text(s, 202, "");
}

static void h_init_error(cb_ctx *c, const cb_http_req *q, cb_http_res *s) {
    atomic_store(&g_init_error, 1);
    fprintf(stderr, "init/error: %.*s\n", (int)q->body_len, (const char *)q->body);
    cb_res_text(s, 202, "");
}

static void *run(void *a) { cb_loop_run(g_ctx.loop); return NULL; }
static void stop(cb_loop *l, void *ud) { cb_loop_stop(l); }

static void one_invocation(const char *bin) {
    cb_log_init(CB_LOG_WARN, 0);
    strcpy(g_cfg.allowed_origins, "*");
    g_ctx.cfg = &g_cfg;
    g_ctx.loop = cb_loop_new();
    g_ctx.req_pool = cb_pool_new(2, "rt");
    g_ctx.router = cb_router_new();
    cb_router_add(g_ctx.router, "GET", "/2018-06-01/runtime/invocation/next", 0, h_next);
    cb_router_add(g_ctx.router, "POST", "/2018-06-01/runtime/invocation/*", 0, h_post);
    cb_router_add(g_ctx.router, "POST", "/2018-06-01/runtime/init/error", 0, h_init_error);
    g_ctx.server = cb_server_new(&g_ctx, NULL);
    int port = 0;
    T_EQ_INT(cb_server_listen(g_ctx.server, "127.0.0.1", 0, &port), CB_OK);
    pthread_t th;
    pthread_create(&th, NULL, run, NULL);

    char api[64];
    snprintf(api, sizeof api, "AWS_LAMBDA_RUNTIME_API=127.0.0.1:%d", port);
    char *env[] = { api, "CBASE_STORE=dynamo", "CBASE_JWT_SECRET=smoke-secret-smoke-secret-smoke-secret", NULL };
    char *argv[] = { (char *)bin, NULL };
    tp_child c;
    T_EQ_INT(tp_start(&c, argv, env), 0);
    // bootstrap exits by itself once /next answers 500.
    T_EQ_INT(tp_wait_line(&c, "invocation/next answered 500", 20000, NULL, 0), 0);
    T_EQ_INT(tp_stop(&c, SIGTERM, 5000), 0);

    T_EQ_INT(atomic_load(&g_init_error), 0);
    T_EQ_INT(atomic_load(&g_nexts), 2);
    pthread_mutex_lock(&g_lock);
    const char *body = cb_buf_cstr(&g_posted_body);
    if (strcmp(g_posted_path, "/2018-06-01/runtime/invocation/req-1/error") == 0 &&
        strstr(body, "event decode: CB_E_NOTIMPL")) {
        // The aws/event.c stub (see the header comment). Remove this branch with it.
        fprintf(stderr, "NOTE smoke_lambda: event decoding is a stub; the loop round trip is what passed\n");
        T_CHECK(strstr(body, "\"errorType\":\"Handler\"") != NULL);
    } else {
        T_EQ_STR(g_posted_path, "/2018-06-01/runtime/invocation/req-1/response");
        T_CHECK(strstr(body, "\"statusCode\":200") != NULL);
    }
    pthread_mutex_unlock(&g_lock);

    cb_loop_post(g_ctx.loop, stop, NULL);
    pthread_join(th, NULL);
    cb_pool_free(g_ctx.req_pool);
    cb_server_free(g_ctx.server);
    cb_router_free(g_ctx.router);
    cb_loop_free(g_ctx.loop);
    cb_buf_free(&g_posted_body);
}

static const char *g_bin;
static void lambda_round_trip(void) { one_invocation(g_bin); }

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: smoke_lambda <path to bootstrap>\n"); return 2; }
    g_bin = argv[1];
    T_RUN(lambda_round_trip);
    return T_DONE();
}
