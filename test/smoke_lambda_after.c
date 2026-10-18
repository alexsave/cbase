// smoke_lambda_after.c - the real `bootstrap` answering first and pushing after.
//
//   build/test/smoke_lambda_after build/bootstrap
//
// smoke_lambda.c's events never commit, so they never reach main_lambda.c's
// after-response window (app/after.h). Here this program is both Lambda's
// Runtime API and API Gateway's management API (CBASE_WS_ENDPOINT points at
// it), and hands bootstrap, one at a time:
//   1  POST /v1/auth/signup             (a Function URL event, base64 body)
//   2  $connect of connection C1         (proto=nw1)
//   3  $default on C1: SUB_AS games      (the client's text frame, base64)
//   4  POST /v1/create                   (commits: its pushes go AFTER the answer)
//   5  $disconnect of C1
// and records every arrival in order. The create's /response must arrive
// before C1's push of the new game's view; the SUB's ACK, a WebSocket reply,
// arrives during its own invocation. bootstrap's timing line counts the push
// as after-response work.
#include "t.h"

#include "lib_proc.h"
#include "net/loop.h"
#include "net/router.h"
#include "net/server.h"
#include "util/b64.h"
#include "util/err.h"
#include "util/json.h"
#include "util/log.h"
#include "util/pool.h"

#include "ask.h"
#include "netwire.h"

#include <pthread.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>

static cb_ctx g_ctx;
static cb_config g_cfg;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;

// The next event to hand out ("" none yet), and what came back.
static char g_event[8192];
static int g_served, g_queued;
static char g_arrivals[32][128];
static int g_n_arrivals;
static cb_buf g_response[8];
static int g_n_responses;
static cb_buf g_pushes;   // the bodies of every @connections post, one per line
static bool g_done;

static void arrive(const char *what) {
    if (g_n_arrivals < 32) snprintf(g_arrivals[g_n_arrivals++], sizeof g_arrivals[0], "%s", what);
}

static void h_next(cb_ctx *c, const cb_http_req *q, cb_http_res *s) {
    pthread_mutex_lock(&g_lock);
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += 20;
    while (g_served == g_queued && !g_done)
        if (pthread_cond_timedwait(&g_cv, &g_lock, &until) != 0) break;
    if (g_served == g_queued) { pthread_mutex_unlock(&g_lock); cb_res_text(s, 500, "no more\n"); return; }
    g_served++;
    char id[16];
    snprintf(id, sizeof id, "req-%d", g_served);
    char ev[8192];
    snprintf(ev, sizeof ev, "%s", g_event);
    pthread_mutex_unlock(&g_lock);
    cb_res_header(s, "Lambda-Runtime-Aws-Request-Id", id);
    cb_res_header(s, "Lambda-Runtime-Deadline-Ms", "9999999999999");
    cb_res_header(s, "Lambda-Runtime-Invoked-Function-Arn", "arn:aws:lambda:us-east-1:0:function:cbase");
    cb_res_send(s, 200, "application/json", ev, strlen(ev));
}

static void h_post(cb_ctx *c, const cb_http_req *q, cb_http_res *s) {
    const char *r = strstr(q->path, "/req-");
    pthread_mutex_lock(&g_lock);
    char what[128];
    snprintf(what, sizeof what, "%s req-%d", strstr(q->path, "/error") ? "error" : "response", r ? atoi(r + 5) : -1);
    arrive(what);
    if (g_n_responses < 8) {
        cb_buf_init(&g_response[g_n_responses]);
        cb_buf_append(&g_response[g_n_responses++], q->body, q->body_len);
    }
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_lock);
    cb_res_text(s, 202, "");
}

// API Gateway's management API: POST /prod/@connections/<id>.
static void h_connections(cb_ctx *c, const cb_http_req *q, cb_http_res *s) {
    pthread_mutex_lock(&g_lock);
    char what[128];
    snprintf(what, sizeof what, "post %s", strrchr(q->path, '/') + 1);
    arrive(what);
    cb_buf_append(&g_pushes, q->body, q->body_len);
    cb_buf_putc(&g_pushes, '\n');
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_lock);
    cb_res_text(s, 200, "");
}

static void *run(void *a) { cb_loop_run(g_ctx.loop); return NULL; }
static void stop(cb_loop *l, void *ud) { cb_loop_stop(l); }

// Hands bootstrap the next event and waits until its answer has arrived.
static int give(const char *event) {
    pthread_mutex_lock(&g_lock);
    snprintf(g_event, sizeof g_event, "%s", event);
    int want = g_n_responses + 1;
    g_queued++;
    pthread_cond_broadcast(&g_cv);
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += 20;
    while (g_n_responses < want)
        if (pthread_cond_timedwait(&g_cv, &g_lock, &until) != 0) break;
    int got = g_n_responses >= want ? want - 1 : -1;
    pthread_mutex_unlock(&g_lock);
    return got;
}

static void url_event(char *out, size_t cap, const char *method, const char *path, const char *auth, const uint8_t *body, size_t n) {
    char b64[4096] = "";
    if (n) cb_b64_encode(body, n, b64, sizeof b64);
    char hdr[2400] = "";
    if (auth) snprintf(hdr, sizeof hdr, ",\"authorization\":\"%s\"", auth);
    snprintf(out, cap,
             "{\"version\":\"2.0\",\"routeKey\":\"$default\",\"rawPath\":\"%s\",\"rawQueryString\":\"\","
             "\"headers\":{\"host\":\"abc.lambda-url.us-east-1.on.aws\",\"content-type\":\"application/octet-stream\"%s},"
             "\"requestContext\":{\"http\":{\"method\":\"%s\",\"path\":\"%s\",\"protocol\":\"HTTP/1.1\",\"sourceIp\":\"127.0.0.1\"},"
             "\"requestId\":\"r\",\"stage\":\"$default\"},\"body\":\"%s\",\"isBase64Encoded\":true}",
             path, hdr, method, path, b64);
}

static void ws_event(char *out, size_t cap, const char *route, const char *query_json, const char *body) {
    char q[128] = "", bd[4200] = "";
    if (query_json) snprintf(q, sizeof q, ",\"queryStringParameters\":%s", query_json);
    if (body) snprintf(bd, sizeof bd, ",\"body\":\"%s\"", body);
    snprintf(out, cap,
             "{\"requestContext\":{\"routeKey\":\"%s\",\"stage\":\"prod\","
             "\"domainName\":\"abcd1234.execute-api.us-east-1.amazonaws.com\",\"connectionId\":\"C1=\",\"apiId\":\"abcd1234\"}"
             "%s%s,\"isBase64Encoded\":false}",
             route, q, bd);
}

// The HTTP response's body bytes (base64 in the encoded response), into out.
static int response_body(int i, uint8_t *out, int cap) {
    char b64[8192];
    if (!cb_json_get_str(cb_buf_cstr(&g_response[i]), g_response[i].len, "body", b64, sizeof b64)) return -1;
    return cb_b64_decode(b64, strlen(b64), out, (size_t)cap);
}

// route: CBASE_WS_ROUTE_REPLY=yes, where a frame's replies ride back in the
// $default invocation's own answer (rt_apigw.c), and step 3 is one batch: a
// SUB_AS with a token that does not verify, a good one, and a ping.
static void answers_then_pushes(const char *bin, bool route) {
    g_served = g_queued = g_n_arrivals = g_n_responses = 0;
    g_done = false;
    cb_log_init(CB_LOG_WARN, 0);
    signal(SIGPIPE, SIG_IGN);
    strcpy(g_cfg.allowed_origins, "*");
    g_ctx.cfg = &g_cfg;
    g_ctx.loop = cb_loop_new();
    g_ctx.req_pool = cb_pool_new(4, "rt");
    g_ctx.router = cb_router_new();
    cb_router_add(g_ctx.router, "GET", "/2018-06-01/runtime/invocation/next", 0, h_next);
    cb_router_add(g_ctx.router, "POST", "/2018-06-01/runtime/invocation/*", 0, h_post);
    cb_router_add(g_ctx.router, "POST", "/prod/@connections/*", 0, h_connections);
    g_ctx.server = cb_server_new(&g_ctx, NULL);
    int port = 0;
    T_EQ_INT(cb_server_listen(g_ctx.server, "127.0.0.1", 0, &port), CB_OK);
    pthread_t th;
    pthread_create(&th, NULL, run, NULL);
    cb_buf_init(&g_pushes);

    char api[64], db[256], envdb[300], wsep[96];
    snprintf(api, sizeof api, "AWS_LAMBDA_RUNTIME_API=127.0.0.1:%d", port);
    snprintf(wsep, sizeof wsep, "CBASE_WS_ENDPOINT=http://127.0.0.1:%d/prod", port);
    T_EQ_INT(tp_tempdir(db, sizeof db), 0);
    snprintf(envdb, sizeof envdb, "CBASE_DB=%s/cbase.sqlite", db);
    char *env[] = { api, "CBASE_STORE=sqlite", envdb, "CBASE_JWT_SECRET=after-secret-after-secret-after-secret", wsep,
                    "AWS_ACCESS_KEY_ID=AKIDEXAMPLE", "AWS_SECRET_ACCESS_KEY=secret", "AWS_REGION=us-east-1",
                    route ? "CBASE_WS_ROUTE_REPLY=yes" : "CBASE_WS_ROUTE_REPLY=no", NULL };
    char *argv[] = { (char *)bin, NULL };
    tp_child c;
    T_EQ_INT(tp_start(&c, argv, env), 0);

    // 1. Sign up.
    static char ev[8192];
    NwRequest rq;
    memset(&rq, 0, sizeof rq);
    rq.kind = NW_SIGNUP;
    rq.name_len = 5;
    memcpy(rq.name, "AFTER", 5);
    rq.password_len = 10;
    memcpy(rq.password, "after-pass", 10);
    uint8_t body[256];
    int bn = nw_request_encode(&rq, body, sizeof body);
    url_event(ev, sizeof ev, "POST", "/v1/auth/signup", NULL, body, (size_t)bn);
    int r = give(ev);
    T_EQ_INT(r, 0);
    uint8_t sess[4096];
    int sn = r >= 0 ? response_body(r, sess, sizeof sess) : -1;
    static NwSession s;
    T_CHECK(sn > 0 && nw_session_decode(sess, sn, &s) == NW_OK);
    char token[NW_ACCESS_MAX + 1], bearer[NW_ACCESS_MAX + 16];
    snprintf(token, sizeof token, "%.*s", (int)s.access_len, s.access);
    snprintf(bearer, sizeof bearer, "Bearer %s", token);

    // 2-3. A stream connection subscribing to the user's games.
    ws_event(ev, sizeof ev, "$connect", "{\"proto\":\"nw1\"}", NULL);
    T_EQ_INT(give(ev), 1);
    static NwFrame f;
    memset(&f, 0, sizeof f);
    f.kind = NW_WS_SUB_AS;
    f.ref = 1;
    f.what = NW_SUB_GAMES;
    f.token_len = (uint16_t)strlen(token);
    memcpy(f.token, token, f.token_len);
    static uint8_t fb[3 * (NW_ACCESS_MAX + 96) + 64];
    int fn = nw_frame_encode(&f, fb, sizeof fb);
    if (route) {
        // The batch: a forged token's SUB_AS, the good one, a ping.
        static uint8_t list[3 * (NW_ACCESS_MAX + 96)], one[NW_ACCESS_MAX + 64];
        int at = 0, n;
        static NwFrame bad;
        bad = f;
        bad.ref = 9;
        memcpy(bad.token, "not.a.token", 11);
        bad.token_len = 11;
        n = nw_frame_encode(&bad, one, sizeof one);
        at += nw_batch_add(one, n, list + at, (int)sizeof list - at);
        n = nw_frame_encode(&f, one, sizeof one);
        at += nw_batch_add(one, n, list + at, (int)sizeof list - at);
        static NwFrame ping;
        memset(&ping, 0, sizeof ping);
        ping.kind = NW_WS_PING;
        n = nw_frame_encode(&ping, one, sizeof one);
        at += nw_batch_add(one, n, list + at, (int)sizeof list - at);
        static NwFrame b;
        memset(&b, 0, sizeof b);
        b.kind = NW_WS_BATCH;
        b.blob.p = list;
        b.blob.len = (uint32_t)at;
        fn = nw_frame_encode(&b, fb, sizeof fb);
    }
    static char text[4 * sizeof fb / 3 + 8];
    int tn = nw_text_encode(fb, fn, text, sizeof text - 1);
    T_CHECK(tn > 0);
    text[tn > 0 ? tn : 0] = 0;
    ws_event(ev, sizeof ev, "$default", NULL, text);
    T_EQ_INT(give(ev), 2);
    if (route) {
        // The replies came back in the invocation's answer, as one batch: AUTH, accepted, PONG.
        char b64[8192] = "";
        T_CHECK(cb_json_get_str(cb_buf_cstr(&g_response[2]), g_response[2].len, "body", b64, sizeof b64));
        static uint8_t msg[8192];
        int mn = nw_text_decode(b64, (int)strlen(b64), msg, sizeof msg);
        static NwFrame got;
        T_CHECK(mn > 0 && nw_frame_decode(msg, mn, &got) == NW_OK && got.kind == NW_WS_BATCH);
        NwBlob all = got.blob, e;
        uint32_t pos = 0;
        int kinds[4] = { 0 }, whys[4] = { -1, -1, -1, -1 }, k = 0;
        static NwFrame ef;
        while (k < 4 && nw_batch_next(all, &pos, &e) == 1 && nw_frame_decode(e.p, (int)e.len, &ef) == NW_OK) {
            kinds[k] = ef.kind;
            whys[k] = ef.why;
            k++;
        }
        T_EQ_INT(k, 3);
        T_EQ_INT(kinds[0], NW_WS_ACK);
        T_EQ_INT(whys[0], ASK_WHY_AUTH);
        T_EQ_INT(kinds[1], NW_WS_ACK);
        T_EQ_INT(whys[1], 0);
        T_EQ_INT(kinds[2], NW_WS_PONG);
    }

    // 4. The create: its answer, then its push.
    url_event(ev, sizeof ev, "POST", "/v1/create", bearer, NULL, 0);
    T_EQ_INT(give(ev), 3);
    char timing[512] = "";
    T_EQ_INT(tp_wait_line(&c, "inv: http POST /v1/create 200", 20000, timing, sizeof timing), 0);
    // One outbound call is the push, made after the answer (plain HTTP here,
    // which the tally files as DynamoDB Local's: aws/https.c host_kind).
    T_CHECK(strstr(timing, " ddb=1/") != NULL);

    // 5. Gone.
    ws_event(ev, sizeof ev, "$disconnect", NULL, NULL);
    T_EQ_INT(give(ev), 4);
    pthread_mutex_lock(&g_lock);
    g_done = true;
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_lock);
    T_EQ_INT(tp_stop(&c, 0, 20000), 0);

    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_n_arrivals; i++) fprintf(stderr, "lambda-after: %s\n", g_arrivals[i]);
    int sub_ack = -1, create_resp = -1, push_after = -1;
    for (int i = 0; i < g_n_arrivals; i++) {
        if (strcmp(g_arrivals[i], "post C1%3D") == 0 && sub_ack < 0) sub_ack = i;
        else if (strcmp(g_arrivals[i], "response req-4") == 0) create_resp = i;
        else if (strcmp(g_arrivals[i], "post C1%3D") == 0 && create_resp >= 0 && push_after < 0) push_after = i;
    }
    pthread_mutex_unlock(&g_lock);
    if (route) {
        // No POST for the replies: the first post is the create's push, after its answer.
        T_CHECK(sub_ack > create_resp);
        push_after = sub_ack;
    } else {
        T_CHECK(sub_ack >= 0 && sub_ack < create_resp);   // the ACK, inside its own invocation
    }
    T_CHECK(create_resp >= 0);
    T_CHECK(push_after > create_resp);                // the view, once the answer was out
    for (int i = 0; i < g_n_responses; i++) T_CHECK(strstr(cb_buf_cstr(&g_response[i]), "\"statusCode\":") != NULL);
    // Two pushes reached C1: the ACK and the new game's view (a text frame of base64).
    int lines = 0;
    for (size_t i = 0; i < g_pushes.len; i++) lines += g_pushes.p[i] == '\n';
    T_EQ_INT(lines, route ? 1 : 2);
    cb_buf_free(&g_pushes);
    for (int i = 0; i < g_n_responses; i++) cb_buf_free(&g_response[i]);

    cb_loop_post(g_ctx.loop, stop, NULL);
    pthread_join(th, NULL);
}

static const char *g_bin;
static void the_answer_goes_first(void) { answers_then_pushes(g_bin, false); }
static void route_replies_ride_in_the_answer(void) { answers_then_pushes(g_bin, true); }

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: smoke_lambda_after <path to bootstrap>\n"); return 2; }
    g_bin = argv[1];
    T_RUN(the_answer_goes_first);
    T_RUN(route_replies_ride_in_the_answer);
    return T_DONE();
}
