// mqtt.c - `cbase-bench mqtt`: the MQTT stream end to end, against `cbase
// serve` or production (IoT Core behind the edge), with throwaway accounts.
//
//   1. The authz matrix, live: who may CONNECT (a live account's token as the
//      password, the same in the URL's token=, a forged token, an expired one
//      when CBASE_BENCH_JWT_SECRET is the server's secret, a deleted account's,
//      another user's client id; with --iot <host>, IoT's own host walked to
//      around the edge) and what one may SUBSCRIBE to (its own topics and the
//      spectators', never another user's or a wildcard over users). A refusal
//      is a closed connection with no CONNACK or SUBACK, as IoT answers.
//   2. --n connects, each timed from the dial: the 101, the CONNACK, the SUBACK,
//      and the first message (a chat posted the moment the SUBACK lands):
//      connect-to-first-message.
//   3. --pushes chat messages on one connection, each timed from its POST going
//      out to its PUBLISH arriving (send_to_push), and from the POST's answer
//      (push_after_answer).
//
// Every account is deleted at the end. Every body is the kernel's (netwire.h),
// every packet the shared codec's (shared/c/mqtt).
#include "bench.h"
#include "net.h"

#include "auth/jwt.h"
#include "client_table.h"
#include "net/ws.h"
#include "netwire.h"
#include "util/clock.h"
#include "util/uuid.h"

#include "../../shared/c/mqtt/mqtt.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TIMEOUT_MS 15000

typedef struct {
    bn_url  u;
    bn_http h;
    char    token[NW_ACCESS_MAX + 1];
    char    auth[NW_ACCESS_MAX + 64];
    char    uid[NW_ID_MAX + 1];
    char    gid[NW_GID_MAX + 1];
} acct;

static void sleep_ms(int ms) {
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static int signup(acct *a, const bn_url *u) {
    a->u = *u;
    bn_http_init(&a->h, u);
    uint8_t rnd[4];
    cb_random_bytes(rnd, sizeof rnd);
    NwRequest q;
    memset(&q, 0, sizeof q);
    q.kind = NW_SIGNUP;
    q.name_len = (uint8_t)snprintf(q.name, sizeof q.name, "BENCHM%02X%02X%02X%02X", rnd[0], rnd[1], rnd[2], rnd[3]);
    q.password_len = (uint8_t)snprintf(q.password, sizeof q.password, "bench-pass-123");
    uint8_t body[256];
    int n = nw_request_encode(&q, body, sizeof body);
    bn_resp r;
    bn_resp_init(&r);
    int rc = n > 0 ? bn_http_request(&a->h, "POST", "/v1/auth/signup", "Content-Type: application/octet-stream\r\n", body,
                                     (size_t)n, TIMEOUT_MS, &r) : -1;
    static NwSession s;
    bool ok = rc == 0 && r.status == 200 && nw_session_decode(r.body.p, (int)r.body.len, &s) == NW_OK;
    if (ok) {
        snprintf(a->token, sizeof a->token, "%.*s", (int)s.access_len, s.access);
        snprintf(a->auth, sizeof a->auth, "Authorization: Bearer %s\r\n", a->token);
        snprintf(a->uid, sizeof a->uid, "%.*s", (int)s.user_id_len, s.user_id);
    } else {
        fprintf(stderr, "bench: signup answered %d\n", r.status);
    }
    bn_resp_free(&r);
    return ok ? 0 : -1;
}

static int create_game(acct *a) {
    char headers[NW_ACCESS_MAX + 128];
    snprintf(headers, sizeof headers, "%sContent-Type: application/octet-stream\r\n", a->auth);
    bn_resp r;
    bn_resp_init(&r);
    static ClientSlot slot;
    static ClientTable ct;
    client_init(&ct, &slot);
    bool ok = bn_http_request(&a->h, "POST", "/v1/create", headers, NULL, 0, TIMEOUT_MS, &r) == 0 && r.status == 200 &&
              client_adopt_envelope(&ct, r.body.p, (int)r.body.len) >= 0 && ct.gid_len > 0;
    if (ok) snprintf(a->gid, sizeof a->gid, "%.*s", (int)ct.gid_len, ct.gid);
    else fprintf(stderr, "bench: create answered %d\n", r.status);
    bn_resp_free(&r);
    return ok ? 0 : -1;
}

static int delete_account(acct *a) {
    bn_resp r;
    bn_resp_init(&r);
    int st = bn_http_request(&a->h, "POST", "/v1/account/delete", a->auth, NULL, 0, TIMEOUT_MS, &r) == 0 ? r.status : -1;
    if (st < 200 || st >= 300) fprintf(stderr, "bench: account delete answered %d\n", st);
    bn_resp_free(&r);
    bn_http_close(&a->h);
    return st;
}

static int post_chat_status(acct *a, int k) {
    NwRequest q;
    memset(&q, 0, sizeof q);
    q.kind = NW_CHAT_SEND;
    q.message_len = (uint16_t)snprintf(q.message, sizeof q.message, "bench %d", k);
    uint8_t body[128];
    int n = nw_request_encode(&q, body, sizeof body);
    char path[16 + NW_GID_MAX], headers[NW_ACCESS_MAX + 128];
    snprintf(path, sizeof path, "/v1/chat/%s", a->gid);
    snprintf(headers, sizeof headers, "%sContent-Type: application/octet-stream\r\n", a->auth);
    bn_resp r;
    bn_resp_init(&r);
    int st = bn_http_request(&a->h, "POST", path, headers, body, (size_t)n, TIMEOUT_MS, &r) == 0 ? r.status : -1;
    bn_resp_free(&r);
    return st;
}

// One cell: opened, CONNECTed as uid with token (or the URL's), then SUBSCRIBEd
// to `filter` when given. 1 accepted, else what happened (-1 closed).
static int cell(const bn_url *u, const char *uid, const char *token, bool in_query, const char *filter) {
    bn_mq c = { 0 };
    cb_buf_init(&c.in);
    c.ws = bn_mq_open(u, in_query ? token : NULL, TIMEOUT_MS);
    if (!c.ws) { bn_mq_close(&c); return -2; }
    int r = bn_mq_connect(&c, uid, in_query ? NULL : token, TIMEOUT_MS);
    if (r == 1 && filter) r = bn_mq_subscribe(&c, 1, filter, TIMEOUT_MS);
    bn_mq_close(&c);
    return r;
}

typedef struct { acct *a; int k; int status; int64_t t_answer; } chat_job;
static void *post_chat(void *arg) {
    chat_job *j = arg;
    j->status = post_chat_status(j->a, j->k);
    j->t_answer = bn_now_us();
    return NULL;
}

// Waits for a PUBLISH on `topic`: the time it arrived, or 0.
static int64_t wait_publish(bn_mq *c, const char *topic, int timeout_ms) {
    MqttPacket p;
    cb_buf s;
    cb_buf_init(&s);
    int64_t until = bn_now_us() + (int64_t)timeout_ms * 1000, at = 0;
    while (!at && bn_now_us() < until) {
        int t = bn_mq_next(c, (int)((until - bn_now_us()) / 1000) + 1, &p, &s);
        if (t < 0) break;
        if (t == MQTT_PUBLISH && p.topic_len == (int)strlen(topic) && memcmp(p.topic, topic, p.topic_len) == 0) at = bn_now_us();
    }
    cb_buf_free(&s);
    return at;
}

static int g_fail;
static void expect(const char *what, int got, int want) {
    bool ok = want == 1 ? got == 1 : got != 1;
    printf("  %-58s %s%s\n", what, want == 1 ? "accepted" : "refused", ok ? "" : "   <-- WRONG");
    if (!ok) g_fail++;
}

int bench_mqtt(const bench_opts *o) {
    bn_url u;
    if (!bn_url_parse(o->url, &u)) { fprintf(stderr, "bench: not a base URL: %s\n", o->url); return 2; }
    acct A, B, C;
    memset(&A, 0, sizeof A);
    memset(&B, 0, sizeof B);
    memset(&C, 0, sizeof C);
    if (signup(&A, &u) != 0 || signup(&B, &u) != 0 || signup(&C, &u) != 0 || create_game(&A) != 0) return 1;
    char t[160];

    // 1. The matrix.
    printf("authz matrix (%s):\n", o->url);
    expect("A's token as the CONNECT password", cell(&u, A.uid, A.token, false, NULL), 1);
    expect("A's token in the URL (token=)", cell(&u, A.uid, A.token, true, NULL), 1);
    char forged[NW_ACCESS_MAX + 1];
    snprintf(forged, sizeof forged, "%s", A.token);
    char *sig = strrchr(forged, '.');
    if (sig && sig[1]) sig[1] = sig[1] == 'A' ? 'B' : 'A';
    expect("a forged token (signature changed)", cell(&u, A.uid, forged, false, NULL), 0);
    expect("no token", cell(&u, A.uid, NULL, false, NULL), 0);
    expect("A's token with B's client id", cell(&u, B.uid, A.token, false, NULL), 0);
    const char *secret = getenv("CBASE_BENCH_JWT_SECRET");
    if (secret && *secret) {
        cb_jwt_claims cl;
        memset(&cl, 0, sizeof cl);
        snprintf(cl.sub, sizeof cl.sub, "%s", A.uid);
        snprintf(cl.session_id, sizeof cl.session_id, "%s", A.uid);
        cl.iat = cb_now_ms() / 1000 - 7200;
        cl.exp = cl.iat + 3600;
        char expired[CB_JWT_MAX];
        if (cb_jwt_mint(secret, &cl, "bench", expired, sizeof expired) > 0)
            expect("an expired token signed with the server's key", cell(&u, A.uid, expired, false, NULL), 0);
    } else {
        printf("  (an expired token: skipped, CBASE_BENCH_JWT_SECRET unset)\n");
    }
    snprintf(t, sizeof t, "u/%s/#", A.uid);
    expect("A subscribes u/A/#", cell(&u, A.uid, A.token, false, t), 1);
    snprintf(t, sizeof t, "u/%s/v", A.uid);
    expect("A subscribes u/A/v", cell(&u, A.uid, A.token, false, t), 1);
    snprintf(t, sizeof t, "s/%s", A.gid);
    expect("B subscribes s/<A's game> (a spectator)", cell(&u, B.uid, B.token, false, t), 1);
    snprintf(t, sizeof t, "u/%s/v", A.uid);
    expect("B subscribes u/A/v", cell(&u, B.uid, B.token, false, t), 0);
    snprintf(t, sizeof t, "u/%s/#", A.uid);
    expect("B subscribes u/A/#", cell(&u, B.uid, B.token, false, t), 0);
    expect("B subscribes u/+/v", cell(&u, B.uid, B.token, false, "u/+/v"), 0);
    expect("B subscribes #", cell(&u, B.uid, B.token, false, "#"), 0);
    if (o->iot && *o->iot) {
        bn_url iu = { .tls = true, .port = 443 };
        snprintf(iu.host, sizeof iu.host, "%s", o->iot);
        bn_mq c = { 0 };
        cb_buf_init(&c.in);
        char path[NW_ACCESS_MAX + 128];
        snprintf(path, sizeof path, "/mqtt?x-amz-customauthorizer-name=cbase-stream");
        c.ws = bn_ws_open_with(&iu, path, "Sec-WebSocket-Protocol: mqtt\r\n", TIMEOUT_MS);
        int r = c.ws ? bn_mq_connect(&c, A.uid, A.token, TIMEOUT_MS) : -2;
        bn_mq_close(&c);
        expect("A's token straight to IoT's host (around the edge)", r, 0);
    }
    // A deleted account: its token, unexpired, connects nothing.
    expect("C's token while C exists", cell(&u, C.uid, C.token, false, NULL), 1);
    delete_account(&C);
    expect("C's token after C deleted the account", cell(&u, C.uid, C.token, false, NULL), 0);

    // 2. Connect to first message.
    int n = o->n > 0 ? o->n : 20, n_ok = 0;
    int64_t *t101 = calloc((size_t)n, sizeof(int64_t)), *tack = calloc((size_t)n, sizeof(int64_t)),
            *tsub = calloc((size_t)n, sizeof(int64_t)), *tfirst = calloc((size_t)n, sizeof(int64_t));
    char chat_topic[160];
    snprintf(chat_topic, sizeof chat_topic, "u/%s/m/%s", A.uid, A.gid);
    for (int i = 0; i < n; i++) {
        bn_mq c = { 0 };
        cb_buf_init(&c.in);
        int64_t t0 = bn_now_us();
        c.ws = bn_mq_open(&u, NULL, TIMEOUT_MS);
        int64_t a = bn_now_us();
        bool ok = c.ws && bn_mq_connect(&c, A.uid, A.token, TIMEOUT_MS) == 1;
        int64_t b = bn_now_us();
        ok = ok && bn_mq_subscribe(&c, 1, chat_topic, TIMEOUT_MS) == 1;
        int64_t s = bn_now_us();
        chat_job j = { &A, i, 0, 0 };
        pthread_t th;
        ok = ok && pthread_create(&th, NULL, post_chat, &j) == 0;
        int64_t f = ok ? wait_publish(&c, chat_topic, TIMEOUT_MS) : 0;
        if (ok) pthread_join(th, NULL);
        if (ok && f) {
            t101[n_ok] = a - t0;
            tack[n_ok] = b - t0;
            tsub[n_ok] = s - t0;
            tfirst[n_ok] = f - t0;
            n_ok++;
        } else {
            g_fail++;
        }
        bn_mq_close(&c);
        sleep_ms(o->gap_ms > 0 ? o->gap_ms : 300);
    }

    // 3. Push latency on one connection.
    int pushes = o->pushes, n_push = 0;
    int64_t *s2p = calloc((size_t)(pushes > 0 ? pushes : 1), sizeof(int64_t)),
            *a2p = calloc((size_t)(pushes > 0 ? pushes : 1), sizeof(int64_t));
    if (pushes > 0) {
        bn_mq c = { 0 };
        cb_buf_init(&c.in);
        c.ws = bn_mq_open(&u, NULL, TIMEOUT_MS);
        bool ok = c.ws && bn_mq_connect(&c, A.uid, A.token, TIMEOUT_MS) == 1 && bn_mq_subscribe(&c, 1, chat_topic, TIMEOUT_MS) == 1;
        for (int k = 0; ok && k < pushes; k++) {
            sleep_ms(o->gap_ms > 0 ? o->gap_ms : 300);
            chat_job j = { &A, 1000 + k, 0, 0 };
            int64_t t_send = bn_now_us();
            pthread_t th;
            if (pthread_create(&th, NULL, post_chat, &j) != 0) break;
            int64_t at = wait_publish(&c, chat_topic, TIMEOUT_MS);
            pthread_join(th, NULL);
            if (!at || j.status < 200 || j.status >= 300) { g_fail++; continue; }
            s2p[n_push] = at - t_send;
            a2p[n_push] = at - j.t_answer;
            n_push++;
        }
        if (!ok) g_fail++;
        bn_mq_close(&c);
    }
    delete_account(&A);
    delete_account(&B);

    char date[32], host[64], note[256];
    bench_date(date, sizeof date);
    bench_host(host, sizeof host);
    snprintf(note, sizeof note, "%s%sfailed=%d", o->note ? o->note : "", o->note && *o->note ? " " : "", g_fail);
    bench_note_load(note, sizeof note);
    bs_run run;
    bs_run_init(&run, "mqtt", date, host, o->url, note);
    bs_run_lat(&run, "ws_101", t101, n_ok, 0, 0);
    bs_run_lat(&run, "connack", tack, n_ok, 0, 0);
    bs_run_lat(&run, "suback", tsub, n_ok, 0, 0);
    bs_run_lat(&run, "first_message", tfirst, n_ok, 0, 0);
    if (pushes > 0) {
        bs_run_lat(&run, "send_to_push", s2p, n_push, 0, 0);
        bs_run_lat(&run, "push_after_answer", a2p, n_push, 0, 0);
    }
    int rc = bench_save(&run, o);
    free(t101); free(tack); free(tsub); free(tfirst); free(s2p); free(a2p);
    return rc != 0 ? 1 : g_fail ? 1 : 0;
}
