// smoke_lambda_after.c - the real `bootstrap` answering first and pushing after.
//
//   build/test/smoke_lambda_after build/bootstrap
//
// smoke_lambda.c's events never commit, so they never reach main_lambda.c's
// after-response window (app/after.h). Here this program is Lambda's Runtime
// API, and a TLS server beside it is AWS IoT Core's publish endpoint
// (AWS_ENDPOINT_URL_IOT_DATA_PLANE points at it). It hands bootstrap, one at a time:
//   1  POST /v1/auth/signup             (a Function URL event, base64 body)
//   2  POST /v1/create                   (commits: its pushes go AFTER the answer)
// and records every arrival in order. The create's /response must arrive
// before the publish of the new game's view to the user's own topic, and
// bootstrap's timing line counts that publish as after-response work.
#include "t.h"

#include "lib_aws_tls.h"
#include "lib_fake_runtime.h"
#include "lib_proc.h"
#include "../../../../shared/c/b64/b64.h"
#include "util/err.h"
#include "util/json.h"
#include "util/log.h"

#include "ask.h"
#include "netwire.h"

#include <pthread.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>

static fr_runtime *g_fr;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_n_pushes;     // IoT publishes
static char g_push_path[256];   // the first one's request line
static uint8_t g_push_body[512];
static size_t g_push_len;

// IoT Core's publish: POST /topics/<topic>?qos=0, the message as the body.
static tls_action h_publish(const cb_buf *req, cb_buf *resp, void *ud) {
    fr_note(g_fr, "publish");
    pthread_mutex_lock(&g_lock);
    if (g_n_pushes++ == 0) {
        const char *r = (const char *)req->p, *eol = strstr(r, "\r\n"), *he = strstr(r, "\r\n\r\n");
        snprintf(g_push_path, sizeof g_push_path, "%.*s", eol ? (int)(eol - r) : 0, r);
        size_t hl = he ? (size_t)(he - r) + 4 : req->len;
        g_push_len = req->len - hl < sizeof g_push_body ? req->len - hl : 0;
        memcpy(g_push_body, req->p + hl, g_push_len);
    }
    pthread_mutex_unlock(&g_lock);
    cb_buf_puts(resp, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 2\r\n\r\n{}");
    return TLS_KEEP;
}

// Hands bootstrap the next event and waits until its answer has arrived: its req index, or -1.
static int give(const char *event) {
    int want = fr_n_posted(g_fr) + 1;
    fr_queue(g_fr, event);
    return fr_wait_posted(g_fr, want, 20000) ? fr_posted_order(g_fr, want - 1) : -1;
}

static void url_event(char *out, size_t cap, const char *method, const char *path, const char *auth, const uint8_t *body, size_t n) {
    char b64[4096] = "";
    if (n) b64_encode(body, n, b64, sizeof b64);
    char hdr[2400] = "";
    if (auth) snprintf(hdr, sizeof hdr, ",\"authorization\":\"%s\"", auth);
    snprintf(out, cap,
             "{\"version\":\"2.0\",\"routeKey\":\"$default\",\"rawPath\":\"%s\",\"rawQueryString\":\"\","
             "\"headers\":{\"host\":\"abc.lambda-url.us-east-1.on.aws\",\"content-type\":\"application/octet-stream\"%s},"
             "\"requestContext\":{\"http\":{\"method\":\"%s\",\"path\":\"%s\",\"protocol\":\"HTTP/1.1\",\"sourceIp\":\"127.0.0.1\"},"
             "\"requestId\":\"r\",\"stage\":\"$default\"},\"body\":\"%s\",\"isBase64Encoded\":true}",
             path, hdr, method, path, b64);
}

// The HTTP response's body bytes (base64 in the encoded response), into out.
static int response_body(int i, uint8_t *out, int cap) {
    char b64[8192];
    cb_buf *r = fr_posted_body(g_fr, i);
    if (!cb_json_get_str(cb_buf_cstr(r), r->len, "body", b64, sizeof b64)) return -1;
    return b64_decode(b64, strlen(b64), out, (size_t)cap);
}

static void answers_then_pushes(const char *bin) {
    g_n_pushes = 0;
    cb_log_init(CB_LOG_WARN, 0);
    signal(SIGPIPE, SIG_IGN);
    int port = 0;
    g_fr = fr_start("arn:aws:lambda:us-east-1:0:function:cbase", &port);
    T_CHECK(g_fr != NULL);
    if (!g_fr) return;
    static tls_srv iot;
    T_EQ_INT(tls_srv_start(&iot, h_publish, NULL), 0);

    char api[64], db[256], envdb[300], iotep[96], ca[300];
    snprintf(api, sizeof api, "AWS_LAMBDA_RUNTIME_API=127.0.0.1:%d", port);
    snprintf(iotep, sizeof iotep, "AWS_ENDPOINT_URL_IOT_DATA_PLANE=https://localhost:%d", iot.port);
    snprintf(ca, sizeof ca, "CBASE_CA_FILE=%s", iot.ca_file);
    T_EQ_INT(tp_tempdir(db, sizeof db), 0);
    snprintf(envdb, sizeof envdb, "CBASE_DB=%s/cbase.sqlite", db);
    char *env[] = { api, "CBASE_STORE=sqlite", envdb, "CBASE_JWT_SECRET=after-secret-after-secret-after-secret", iotep, ca,
                    "CBASE_IOT_ENDPOINT=a1b2c3-ats.iot.us-east-1.amazonaws.com",
                    "AWS_ACCESS_KEY_ID=AKIDEXAMPLE", "AWS_SECRET_ACCESS_KEY=secret", "AWS_REGION=us-east-1", NULL };
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
    char token[NW_ACCESS_MAX + 1], bearer[NW_ACCESS_MAX + 16], uid[NW_ID_MAX + 1];
    snprintf(token, sizeof token, "%.*s", (int)s.access_len, s.access);
    snprintf(bearer, sizeof bearer, "Bearer %s", token);
    snprintf(uid, sizeof uid, "%.*s", (int)s.user_id_len, s.user_id);

    // 2. The create: its answer, then its push.
    url_event(ev, sizeof ev, "POST", "/v1/create", bearer, NULL, 0);
    T_EQ_INT(give(ev), 1);
    char timing[512] = "";
    T_EQ_INT(tp_wait_line(&c, "inv: http POST /v1/create 200", 20000, timing, sizeof timing), 0);
    // One outbound call is the publish, made after the answer (to the stand-in,
    // a host that is not *.iot.*, so the tally files it as other: aws/https.c host_kind).
    T_CHECK(strstr(timing, " other=1/") != NULL);

    fr_done(g_fr);
    T_EQ_INT(tp_stop(&c, 0, 20000), 0);

    int n_arrivals = fr_n_arrivals(g_fr);
    for (int i = 0; i < n_arrivals; i++) fprintf(stderr, "lambda-after: %s\n", fr_arrival(g_fr, i));
    int create_resp = -1, push_after = -1;
    for (int i = 0; i < n_arrivals; i++) {
        if (strcmp(fr_arrival(g_fr, i), "response req-2") == 0) create_resp = i;
        else if (strcmp(fr_arrival(g_fr, i), "publish") == 0 && create_resp >= 0 && push_after < 0) push_after = i;
    }
    pthread_mutex_lock(&g_lock);
    T_CHECK(create_resp >= 0);
    T_CHECK(push_after > create_resp);                // the view, once the answer was out
    T_EQ_INT(g_n_pushes, 1);
    for (int i = 0; i < fr_n_posted(g_fr); i++) T_CHECK(strstr(cb_buf_cstr(fr_posted_body(g_fr, i)), "\"statusCode\":") != NULL);
    // The user's own views topic, QoS 0, the nw1 view as the body.
    char want[200];
    snprintf(want, sizeof want, "POST /topics/u%%2F%s%%2Fv?qos=0 HTTP/1.1", uid);
    T_EQ_STR(g_push_path, want);
    static NwFrame f;
    T_EQ_INT(nw_frame_decode(g_push_body, (int)g_push_len, &f), NW_OK);
    T_EQ_INT(f.kind, NW_WS_VIEW);
    T_CHECK(f.blob.len > 0);
    pthread_mutex_unlock(&g_lock);

    tls_srv_stop(&iot);
    fr_stop(g_fr);
    g_fr = NULL;
}

static const char *g_bin;
static void the_answer_goes_first(void) { answers_then_pushes(g_bin); }

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: smoke_lambda_after <path to bootstrap>\n"); return 2; }
    g_bin = argv[1];
    T_RUN(the_answer_goes_first);
    return T_DONE();
}
