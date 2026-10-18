// smoke_lambda.c - the real `bootstrap` binary against a fake Lambda Runtime API.
//
//   build/test/smoke_lambda build/bootstrap
//
// This program IS the Runtime API: a cb_server on a free port answering
// /2018-06-01/runtime/invocation/next with one event after another, recording
// what bootstrap POSTs back for each, and answering /next with 500 once they
// are used up so bootstrap's loop ends (as it does when Lambda goes away).
// No AWS, no Docker: the Lambda path is tested on every machine `make check`
// runs on. The events:
//   req-1  a Function URL GET /health: answered on /response with the
//          encoded HTTP response (statusCode 200, the health text, not base64)
//   req-2  the schedule's string literal "scan": answered on /response with {}
//   req-3  a literal no one sends ("nonsense"): answered on /error, as an
//          event that does not decode
//   req-4  a drive for a game that is gone: answered {}
//   req-5  IoT Core's custom authorizer request with a token that does not
//          verify: answered {"isAuthenticated":false}
// bootstrap runs on SQLite in a temporary directory, so the scan touches no AWS.

#include "t.h"

#include "lib_proc.h"
#include "net/loop.h"
#include "net/router.h"
#include "net/server.h"
#include "util/err.h"
#include "util/json.h"
#include "util/log.h"
#include "util/pool.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

static cb_ctx g_ctx;
static cb_config g_cfg;
static atomic_int g_nexts;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static atomic_int g_init_error;

static const char HEALTH[] =
    "{\"version\":\"2.0\",\"routeKey\":\"$default\",\"rawPath\":\"/health\",\"rawQueryString\":\"\","
    "\"headers\":{\"host\":\"abc.lambda-url.us-west-1.on.aws\",\"user-agent\":\"smoke\"},"
    "\"requestContext\":{\"http\":{\"method\":\"GET\",\"path\":\"/health\",\"protocol\":\"HTTP/1.1\","
    "\"sourceIp\":\"127.0.0.1\"},\"requestId\":\"req-1\",\"stage\":\"$default\"},\"isBase64Encoded\":false}";

// IoT Core's custom authorizer request at an MQTT CONNECT, with a token that
// does not verify (base64 of "not-a-jwt" as the MQTT password).
static const char IOT_AUTH[] =
    "{\"token\":\"\",\"signatureVerified\":false,\"protocols\":[\"tls\",\"http\",\"mqtt\"],"
    "\"protocolData\":{\"tls\":{\"serverName\":\"api.foolish.cards\"},\"http\":{\"headers\":{},\"queryString\":\"\"},"
    "\"mqtt\":{\"username\":\"jwt\",\"password\":\"bm90LWEtand0\",\"clientId\":\"x-1\"}},"
    "\"connectionMetadata\":{\"id\":\"c1\"}}";

#define N_EVENTS 5
static const char *const EVENTS[N_EVENTS] = { HEALTH, "\"scan\"", "\"nonsense\"", "\"bot_drive:abc123\"", IOT_AUTH };
static char g_posted_path[N_EVENTS][256];
static cb_buf g_posted_body[N_EVENTS];

static void h_next(cb_ctx *c, const cb_http_req *q, cb_http_res *s) {
    int i = atomic_fetch_add(&g_nexts, 1);
    if (i >= N_EVENTS) { cb_res_text(s, 500, "no more\n"); return; }
    char id[16];
    snprintf(id, sizeof id, "req-%d", i + 1);
    cb_res_header(s, "Lambda-Runtime-Aws-Request-Id", id);
    cb_res_header(s, "Lambda-Runtime-Deadline-Ms", "9999999999999");
    cb_res_header(s, "Lambda-Runtime-Invoked-Function-Arn", "arn:aws:lambda:us-west-1:0:function:cbase");
    cb_res_send(s, 200, "application/json", EVENTS[i], strlen(EVENTS[i]));
}

// /2018-06-01/runtime/invocation/req-<n>/{response,error}
static void h_post(cb_ctx *c, const cb_http_req *q, cb_http_res *s) {
    const char *r = strstr(q->path, "/req-");
    int i = r ? atoi(r + 5) - 1 : -1;
    if (i < 0 || i >= N_EVENTS) { cb_res_text(s, 400, "unknown request id\n"); return; }
    pthread_mutex_lock(&g_lock);
    snprintf(g_posted_path[i], sizeof g_posted_path[i], "%s", q->path);
    cb_buf_reset(&g_posted_body[i]);
    cb_buf_append(&g_posted_body[i], q->body, q->body_len);
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

    char api[64], db[256], envdb[300];
    snprintf(api, sizeof api, "AWS_LAMBDA_RUNTIME_API=127.0.0.1:%d", port);
    T_EQ_INT(tp_tempdir(db, sizeof db), 0);
    snprintf(envdb, sizeof envdb, "CBASE_DB=%s/cbase.sqlite", db);
    char *env[] = { api, "CBASE_STORE=sqlite", envdb, "CBASE_JWT_SECRET=smoke-secret-smoke-secret-smoke-secret", NULL };
    char *argv[] = { (char *)bin, NULL };
    tp_child c;
    T_EQ_INT(tp_start(&c, argv, env), 0);
    // Every invocation logs its timing line (main_lambda.c), in order.
    char hl[512];
    T_EQ_INT(tp_wait_line(&c, "inv: http GET /health 200 ms=", 20000, hl, sizeof hl), 0);
    T_CHECK(strstr(hl, " ip=") != NULL);   // the access log's viewer (M9)
    // Each names its Lambda request id, the one the REPORT line carries.
    char line[512] = "";
    T_EQ_INT(tp_wait_line(&c, "inv: scan ms=", 20000, line, sizeof line), 0);
    T_CHECK(strstr(line, " rid=req-2") != NULL);
    T_EQ_INT(tp_wait_line(&c, "inv: drive abc123 ms=", 20000, line, sizeof line), 0);
    T_CHECK(strstr(line, " rid=req-4") != NULL);
    T_EQ_INT(tp_wait_line(&c, "inv: iot connect deny ms=", 20000, line, sizeof line), 0);
    T_CHECK(strstr(line, " rid=req-5") != NULL);
    // bootstrap exits by itself once /next answers 500.
    T_EQ_INT(tp_wait_line(&c, "invocation/next answered 500", 20000, NULL, 0), 0);
    T_EQ_INT(tp_stop(&c, 0, 10000), 0);   // and it exits 0 by itself

    T_EQ_INT(atomic_load(&g_init_error), 0);
    T_EQ_INT(atomic_load(&g_nexts), N_EVENTS + 1);
    pthread_mutex_lock(&g_lock);

    // req-1: the encoded HTTP response.
    const char *body = cb_buf_cstr(&g_posted_body[0]);
    size_t n = g_posted_body[0].len;
    T_EQ_STR(g_posted_path[0], "/2018-06-01/runtime/invocation/req-1/response");
    T_CHECK(cb_json_valid(body, n));
    int64_t status = 0;
    bool b64 = true;
    char text[256] = "", ctype[64] = "";
    T_CHECK(cb_json_get_i64(body, n, "statusCode", &status));
    T_EQ_INT(status, 200);
    T_CHECK(cb_json_get_bool(body, n, "isBase64Encoded", &b64));
    T_CHECK(!b64);
    T_CHECK(cb_json_get_str(body, n, "body", text, sizeof text));
    T_CHECK(cb_starts_with(text, "ok cbase "));
    T_CHECK(strstr(text, " mode=lambda ") != NULL);
    T_CHECK(cb_json_get_str(body, n, "headers.Content-Type", ctype, sizeof ctype));
    T_EQ_STR(ctype, "text/plain; charset=utf-8");

    // req-2: the scan literal reached cb_bots_scan, which answered {}.
    body = cb_buf_cstr(&g_posted_body[1]);
    T_EQ_STR(g_posted_path[1], "/2018-06-01/runtime/invocation/req-2/response");
    T_EQ_STR(body, "{}");

    // req-3: an event that does not decode is the invocation's error.
    body = cb_buf_cstr(&g_posted_body[2]);
    T_EQ_STR(g_posted_path[2], "/2018-06-01/runtime/invocation/req-3/error");
    T_EQ_STR(body, "{\"errorMessage\":\"event decode: CB_E_INVALID\",\"errorType\":\"Handler\"}");

    // req-4: a drive for a game that is gone answers {}, not an error, so
    // Lambda does not retry an async invoke that can never succeed.
    body = cb_buf_cstr(&g_posted_body[3]);
    T_EQ_STR(g_posted_path[3], "/2018-06-01/runtime/invocation/req-4/response");
    T_EQ_STR(body, "{}");

    // req-5: the authorizer refuses a token that does not verify, as an
    // answer (IoT then refuses the CONNECT), not an error.
    body = cb_buf_cstr(&g_posted_body[4]);
    T_EQ_STR(g_posted_path[4], "/2018-06-01/runtime/invocation/req-5/response");
    T_EQ_STR(body, "{\"isAuthenticated\":false}");
    pthread_mutex_unlock(&g_lock);
    char cmd[300];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", db);
    T_EQ_INT(system(cmd), 0);

    cb_loop_post(g_ctx.loop, stop, NULL);
    pthread_join(th, NULL);
    cb_pool_free(g_ctx.req_pool);
    cb_server_free(g_ctx.server);
    cb_router_free(g_ctx.router);
    cb_loop_free(g_ctx.loop);
    for (int i = 0; i < N_EVENTS; i++) cb_buf_free(&g_posted_body[i]);
}

static const char *g_bin;
static void lambda_round_trip(void) { one_invocation(g_bin); }

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: smoke_lambda <path to bootstrap>\n"); return 2; }
    g_bin = argv[1];
    T_RUN(lambda_round_trip);
    return T_DONE();
}
