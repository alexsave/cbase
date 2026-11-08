// unit_rt_iot.c - realtime under Lambda on AWS IoT Core (realtime/rt_iot.h):
// a push is one publish per target to its MQTT topic, and the custom
// authorizer turns IoT's CONNECT request (decoded from the JSON IoT sends,
// aws/event.h) into the policy for the token's user, or a refusal. The
// authorizer's half of the authz matrix: a forged, expired or deleted
// account's token, another user's client id and a connection that did not
// come through the edge are refused; the policy names only the user's own
// topics and the spectators', and never iot:Publish.
#include "t.h"
#include "cbase/provider.h"

#include "aws/event.h"
#include "config.h"
#include "aws_json.h"
#include "realtime/realtime.h"
#include "realtime/rt_deps.h"
#include "realtime/rt_iot.h"
#include "realtime/stream_mqtt.h"
#include "b64/b64.h"
#include "util/err.h"
#include "util/json.h"
#include "util/clock.h"
#include "util/log.h"

#include <poll.h>
#include <pthread.h>
#include <stdlib.h>

#define UA "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"
#define UB "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb"
#define G  "ab12cd"
#define NOW_MS 1700000000000LL
#define NOW_S  (NOW_MS / 1000)
#define ARN "arn:aws:lambda:us-east-1:487337210065:function:cbase"
#define SECRET "0123456789abcdef0123456789abcdef"

static cb_ctx g_ctx;
static cb_config g_cfg;

static int fake_verify(cb_ctx *ctx, const char *jwt, cb_user *out) {
    if (strcmp(jwt, "tok-db") == 0) return CB_E_DB;
    if (strncmp(jwt, "tok-", 4) != 0 || strcmp(jwt, "tok-gone") == 0) return CB_E_AUTH;
    memset(out, 0, sizeof *out);
    cb_strlcpy(out->id, jwt + 4, sizeof out->id);
    out->expires_at = NOW_S + 1800;
    return CB_OK;
}

// The publishes as IoT would take them. A batch's lanes call this from
// several threads at once: everything is under the lock but the wait, which
// stands for IoT's answer time (wait_ms), so concurrent publishes overlap.
// With `gather` set, each publish first waits (5 s at most, once) until that many are
// in flight at once: the lanes' concurrency is a count, not a wall-clock bound a
// loaded machine breaks.
static pthread_mutex_t g_pub_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_pub_cv = PTHREAD_COND_INITIALIZER;
static struct {
    int n;
    char endpoint[256], topic[48][128];
    uint8_t body[256];
    size_t len;
    int status;
    int wait_ms;
    const char *fail_topic;   // this topic answers 500
    int64_t start_ms[48], end_ms[48];
    pthread_t thread[48];
    int gather, inflight, peak;   // publishes in flight at once, and the most there were
} g_pub;
static int fake_publish(const char *endpoint, const char *topic, const void *data, size_t len) {
    int64_t t0 = cb_mono_ms();
    pthread_mutex_lock(&g_pub_mu);
    if (++g_pub.inflight > g_pub.peak) g_pub.peak = g_pub.inflight;
    pthread_cond_broadcast(&g_pub_cv);
    const int64_t until = cb_mono_ms() + 5000;
    while (g_pub.peak < g_pub.gather && cb_mono_ms() < until) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 10 * 1000000;
        if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
        pthread_cond_timedwait(&g_pub_cv, &g_pub_mu, &ts);
    }
    if (g_pub.peak < g_pub.gather) g_pub.gather = 0;   // never gathered: the rest go at once (peak says so)
    pthread_mutex_unlock(&g_pub_mu);
    if (g_pub.wait_ms) poll(NULL, 0, g_pub.wait_ms);
    pthread_mutex_lock(&g_pub_mu);
    g_pub.inflight--;
    int i = g_pub.n++;
    if (i < 48) {
        cb_strlcpy(g_pub.topic[i], topic, sizeof g_pub.topic[0]);
        g_pub.start_ms[i] = t0;
        g_pub.end_ms[i] = cb_mono_ms();
        g_pub.thread[i] = pthread_self();
    }
    cb_strlcpy(g_pub.endpoint, endpoint, sizeof g_pub.endpoint);
    g_pub.len = len < sizeof g_pub.body ? len : 0;
    memcpy(g_pub.body, data, g_pub.len);
    int st = g_pub.fail_topic && strcmp(topic, g_pub.fail_topic) == 0 ? 500 : g_pub.status;
    pthread_mutex_unlock(&g_pub_mu);
    return st;
}

// Two topic families as an app declares them (cbase/app.h): a seat's own pushes and a room's.
static const cb_topic_family SEAT = { "seat", "u/{user}/c/{doc}", false };
static const cb_topic_family WATCH = { "watch", "s/{doc}", false };

// One push of `cast` for the seat of `user` in room `doc`, or the room's watchers' (user NULL):
// its own bytes, as a product's frame would be (the payload is opaque to the platform).
static void push(cb_rt *rt, const char *user, const char *doc, int64_t version, const uint8_t *cast, size_t len) {
    uint8_t b[64];
    b[0] = (uint8_t)version;
    memcpy(b + 1, cast, len);
    cb_rt_to to = { user ? &SEAT : &WATCH, user, doc, NULL };
    cb_rt_publish(rt, &to, 1, b, len + 1);
}

static void setup(void) {
    cb_log_init(CB_LOG_ERROR, 0);
    memset(&g_ctx, 0, sizeof g_ctx);
    memset(&g_cfg, 0, sizeof g_cfg);
    memset(&g_pub, 0, sizeof g_pub);
    g_pub.status = 200;
    g_ctx.cfg = &g_cfg;
    g_ctx.prov = &cb_provider_aws;
    cb_rt_deps_g.verify_token = fake_verify;
    cb_rt_iot_publish = fake_publish;
}

// A commit's fan-out between two humans is five publishes: each human's view
// (the dashboard, and a boards host's game), each seat's cast, the
// spectators' cast. In one batch they go out on three lanes at once (one per
// user, one for the spectators), so with IoT taking WAIT ms a publish the
// whole fan-out takes about two publishes' time (a user's view, then its
// cast), not five; and a user still hears its view before its cast.
#define WAIT 60

// Without a batch (a single publish, or a transport with no lanes) a
// publish is sent at once, on the caller's thread; batches nest, and only the
// outermost end sends.
static void a_batch_nests(void) {
    setup();
    cb_strlcpy(g_cfg.iot_endpoint, "abc123-ats.iot.us-east-1.amazonaws.com", sizeof g_cfg.iot_endpoint);
    cb_rt *rt;
    T_EQ_INT(cb_rt_iot_new(&g_ctx, &rt), CB_OK);
    const uint8_t cast[] = { 5, 6 };
    push(rt, UA, G, 3, cast, 2);
    T_EQ_INT(g_pub.n, 1);
    T_CHECK(pthread_equal(g_pub.thread[0], pthread_self()));
    cb_rt_batch_begin(rt);
    cb_rt_batch_begin(rt);
    push(rt, UA, G, 4, cast, 2);
    push(rt, UB, G, 4, cast, 2);
    cb_rt_batch_end(rt);
    T_EQ_INT(g_pub.n, 1);   // held until the outer end
    push(rt, NULL, G, 4, cast, 2);
    cb_rt_batch_end(rt);
    T_EQ_INT(g_pub.n, 4);
    // An end with no batch open does nothing.
    cb_rt_batch_end(rt);
    T_EQ_INT(g_pub.n, 4);
    // A message whose id cannot be held (longer than any topic level) is
    // delivered at once, and is no topic: nothing is published or failed.
    cb_rt_batch_begin(rt);
    char long_id[100];
    memset(long_id, 'a', sizeof long_id - 1);
    long_id[sizeof long_id - 1] = 0;
    push(rt, long_id, G, 5, cast, 2);
    push(rt, UA, G, 5, cast, 2);
    T_EQ_INT(g_pub.n, 4);
    cb_rt_batch_end(rt);
    T_EQ_INT(g_pub.n, 5);
    T_EQ_INT(cb_rt_failures_take(), 0);
    // More messages than a batch holds: the rest are sent at once, none lost.
    memset(&g_pub, 0, sizeof g_pub);
    g_pub.status = 200;
    cb_rt_batch_begin(rt);
    for (int i = 0; i < 40; i++) push(rt, UA, G, 6 + i, cast, 2);
    T_EQ_INT(g_pub.n, 8);
    cb_rt_batch_end(rt);
    T_EQ_INT(g_pub.n, 40);
    // In one lane the order is the order published.
    int prev = -1;
    bool ordered = true;
    for (int i = 8; i < 40; i++) ordered &= g_pub.start_ms[i] >= (prev < 0 ? 0 : g_pub.end_ms[prev]), prev = i;
    T_CHECK(ordered);

    // A batch belongs to its transport: one opened on another while it is
    // open holds nothing of that one's, and neither carries the other's.
    static cb_ctx ctx2;
    static cb_config cfg2;
    ctx2 = g_ctx;
    cfg2 = g_cfg;
    ctx2.cfg = &cfg2;
    cb_strlcpy(cfg2.iot_endpoint, "other-ats.iot.us-east-1.amazonaws.com", sizeof cfg2.iot_endpoint);
    cb_rt *rt2;
    T_EQ_INT(cb_rt_iot_new(&ctx2, &rt2), CB_OK);
    memset(&g_pub, 0, sizeof g_pub);
    g_pub.status = 200;
    cb_rt_batch_begin(rt);
    push(rt, UA, G, 50, cast, 2);
    cb_rt_batch_begin(rt2);
    push(rt2, UB, G, 50, cast, 2);
    T_EQ_INT(g_pub.n, 1);   // rt2's, at once
    T_EQ_STR(g_pub.endpoint, "other-ats.iot.us-east-1.amazonaws.com");
    cb_rt_batch_end(rt2);
    T_EQ_INT(g_pub.n, 1);
    cb_rt_batch_end(rt);
    T_EQ_INT(g_pub.n, 2);   // rt's, held to its own end, through its own endpoint
    T_EQ_STR(g_pub.endpoint, "abc123-ats.iot.us-east-1.amazonaws.com");
    T_EQ_STR(g_pub.topic[1], "u/" UA "/c/" G);
    cb_rt_free(rt2);
    cb_rt_free(rt);
}

// ---- the authorizer --------------------------------------------------------------------------

// An IoT custom authorizer request, as AWS documents it, with the given parts.
static int event_of(cb_event *ev, const char *password, const char *token, const char *query, const char *client_id,
                    const char *origin_header) {
    char pw64[256] = "";
    if (password) b64_encode((const uint8_t *)password, strlen(password), pw64, sizeof pw64);
    char json[4096];
    int n = snprintf(json, sizeof json,
        "{%s%s%s\"signatureVerified\":false,\"protocols\":[\"tls\",\"http\",\"mqtt\"],"
        "\"protocolData\":{\"tls\":{\"serverName\":\"api.foolish.cards\"},"
        "\"http\":{\"headers\":{\"Host\":\"api.foolish.cards\"%s%s%s},\"queryString\":\"%s\"},"
        "\"mqtt\":{\"username\":\"jwt\"%s%s%s%s%s%s}},"
        "\"connectionMetadata\":{\"id\":\"e2a0c7d6-0000-4000-8000-000000000001\"}}",
        token ? "\"token\":\"" : "", token ? token : "", token ? "\"," : "",
        origin_header ? ",\"" : "", origin_header ? origin_header : "", origin_header ? "\"" : "",
        query ? query : "",
        password ? ",\"password\":\"" : "", password ? pw64 : "", password ? "\"" : "",
        client_id ? ",\"clientId\":\"" : "", client_id ? client_id : "", client_id ? "\"" : "");
    return cb_event_decode(json, (size_t)n, ev);
}

static void decoded_events(void) {
    setup();
    cb_event ev;
    T_EQ_INT(event_of(&ev, "tok-" UA, NULL, "?x-amz-customauthorizer-name=cbase-stream", UA "-t", NULL), CB_OK);
    T_EQ_INT(ev.kind, CB_EVENT_IOT_AUTH);
    T_EQ_MEM(ev.iot.token, ev.iot.token_len, "tok-" UA, 40);
    T_EQ_STR(ev.iot.client_id, UA "-t");
    T_CHECK(!ev.iot.has_origin);
    cb_event_free(&ev);
    // The password wins; then the event's token; then the query string's.
    T_EQ_INT(event_of(&ev, "tok-" UA, "tok-" UB, "?token=tok-x", NULL, NULL), CB_OK);
    T_EQ_MEM(ev.iot.token, ev.iot.token_len, "tok-" UA, 40);
    T_EQ_STR(ev.iot.client_id, "");
    cb_event_free(&ev);
    T_EQ_INT(event_of(&ev, NULL, "tok-" UB, "?token=tok-x", NULL, NULL), CB_OK);
    T_EQ_MEM(ev.iot.token, ev.iot.token_len, "tok-" UB, 40);
    cb_event_free(&ev);
    T_EQ_INT(event_of(&ev, NULL, NULL, "?x-amz-customauthorizer-name=a&token=tok-" UA, NULL, NULL), CB_OK);
    T_EQ_MEM(ev.iot.token, ev.iot.token_len, "tok-" UA, 40);
    cb_event_free(&ev);
    T_EQ_INT(event_of(&ev, NULL, NULL, "?token=tok-" UB "&x=1", NULL, NULL), CB_OK);   // IoT's leading '?'
    T_EQ_MEM(ev.iot.token, ev.iot.token_len, "tok-" UB, 40);
    cb_event_free(&ev);
    T_EQ_INT(event_of(&ev, NULL, NULL, "", NULL, NULL), CB_OK);
    T_EQ_INT(ev.iot.token_len, 0);
    cb_event_free(&ev);
    // The origin header, whatever case its name arrives in.
    T_EQ_INT(event_of(&ev, NULL, NULL, "", NULL, "x-CBASE-origin\":\"" SECRET), CB_OK);
    T_CHECK(ev.iot.has_origin);
    T_EQ_STR(ev.iot.origin, SECRET);
    cb_event_free(&ev);
    // A password that is not base64 is no token (never a cut or garbled one).
    const char *bad = "{\"protocolData\":{\"mqtt\":{\"password\":\"***\"}}}";
    T_EQ_INT(cb_event_decode(bad, strlen(bad), &ev), CB_OK);
    T_EQ_INT(ev.iot.token_len, 0);
    cb_event_free(&ev);
    // base64 of bytes holding a NUL: no token.
    const char *nul = "{\"protocolData\":{\"mqtt\":{\"password\":\"dG9rAC14\"}}}";
    T_EQ_INT(cb_event_decode(nul, strlen(nul), &ev), CB_OK);
    T_EQ_INT(ev.iot.token_len, 0);
    cb_event_free(&ev);
    // IoT's "token" that does not unescape (a lone surrogate after a byte that
    // did): no token, and nothing of it left in the buffer (fuzz/corpus/event
    // crash-03e40c70, which had a password that did not decode either).
    const char *lone = "{\"token\":\"r\\udae7\\ueae1\",\"protocolData\":{\"mqtt\":{\"password\":\"***\"}}}";
    T_EQ_INT(cb_event_decode(lone, strlen(lone), &ev), CB_OK);
    T_EQ_INT(ev.iot.token_len, 0);
    T_EQ_STR(ev.iot.token, "");
    cb_event_free(&ev);
    // A "token" holding \u0000 is no token, never the part before the NUL.
    const char *cut = "{\"token\":\"tok\\u0000x\",\"protocolData\":{}}";
    T_EQ_INT(cb_event_decode(cut, strlen(cut), &ev), CB_OK);
    T_EQ_INT(ev.iot.token_len, 0);
    T_EQ_STR(ev.iot.token, "");
    cb_event_free(&ev);
    // A whole one is the token.
    const char *whole = "{\"token\":\"tok-\\u0041\",\"protocolData\":{}}";
    T_EQ_INT(cb_event_decode(whole, strlen(whole), &ev), CB_OK);
    T_EQ_INT(ev.iot.token_len, 5);
    T_EQ_STR(ev.iot.token, "tok-A");
    cb_event_free(&ev);
}

// The answer for one event: allowed, and the answer's JSON in *out.
static int answer(const char *password, const char *client_id, const char *origin_header, cb_buf *out, bool *allowed) {
    cb_event ev;
    int rc = event_of(&ev, password, NULL, "", client_id, origin_header);
    if (rc == CB_OK) rc = cb_iot_authorize(&g_ctx, &ev.iot, ARN, NOW_MS, out, allowed);
    cb_event_free(&ev);
    return rc;
}

static bool has(const cb_buf *b, const char *s) { return strstr(cb_buf_cstr((cb_buf *)b), s) != NULL; }

static void the_policy(void) {
    setup();
    cb_buf out;
    cb_buf_init(&out);
    bool ok;
    T_EQ_INT(answer("tok-" UA, UA "-t", NULL, &out, &ok), CB_OK);
    T_CHECK(ok);
    const char *j = cb_buf_cstr(&out);
    T_CHECK(cb_json_valid(j, out.len));
    char s[64];
    T_CHECK(cb_json_get_str(j, out.len, "principalId", s, sizeof s));
    T_EQ_STR(s, "aaaaaaaaaaaa4aaa8aaaaaaaaaaaaaaa");
    int64_t life = 0;
    T_CHECK(cb_json_get_i64(j, out.len, "disconnectAfterInSeconds", &life));
    T_EQ_INT(life, 1800);   // the token's remaining life
    T_CHECK(cb_json_get_i64(j, out.len, "refreshAfterInSeconds", &life));
    T_EQ_INT(life, 1800);
    // The policy: a JSON string inside the answer, read back whole.
    cb_span docs;
    T_EQ_INT(cb_json_find(j, out.len, "policyDocuments", &docs), CB_JSON_ARR);
    char pol[2048];
    T_CHECK(cb_json_get_str(j, out.len, "policyDocuments.0", pol, sizeof pol));
    T_CHECK(cb_json_valid(pol, strlen(pol)));
    T_CHECK(strstr(pol, "\"arn:aws:iot:us-east-1:487337210065:client/" UA "-*\"") != NULL);
    T_CHECK(strstr(pol, "\"arn:aws:iot:us-east-1:487337210065:topicfilter/u/" UA "/*\"") != NULL);
    T_CHECK(strstr(pol, "\"arn:aws:iot:us-east-1:487337210065:topicfilter/s/*\"") != NULL);
    T_CHECK(strstr(pol, "\"arn:aws:iot:us-east-1:487337210065:topic/u/" UA "/*\"") != NULL);
    T_CHECK(strstr(pol, "\"arn:aws:iot:us-east-1:487337210065:topic/s/*\"") != NULL);
    T_CHECK(strstr(pol, "iot:Connect") && strstr(pol, "iot:Subscribe") && strstr(pol, "iot:Receive"));
    T_CHECK(!strstr(pol, "iot:Publish"));            // no client may publish
    T_CHECK(!strstr(pol, UB));
    T_CHECK(!strstr(pol, "topicfilter/u/*") && !strstr(pol, "topic/u/*") && !strstr(pol, "\"*\""));
    T_CHECK(!strstr(pol, "Deny"));

    // Refused: each answers {"isAuthenticated":false} and nothing else.
    const char *refused_tokens[] = { "forged", "tok-gone", "" };
    for (int i = 0; i < 3; i++) {
        T_EQ_INT(answer(refused_tokens[i][0] ? refused_tokens[i] : NULL, UA "-t", NULL, &out, &ok), CB_OK);
        T_CHECK(!ok);
        T_EQ_STR(cb_buf_cstr(&out), "{\"isAuthenticated\":false}");
    }
    T_EQ_INT(answer("tok-" UA, UB "-t", NULL, &out, &ok), CB_OK);   // another user's client id
    T_CHECK(!ok);
    T_CHECK(!has(&out, "policyDocuments"));
    // The check could not run: the invocation fails (IoT refuses the CONNECT).
    T_EQ_INT(answer("tok-db", UA "-t", NULL, &out, &ok), CB_E_DB);
    T_CHECK(!ok);
    // No region and account to write the policy under.
    cb_event ev;
    T_EQ_INT(event_of(&ev, "tok-" UA, NULL, "", NULL, NULL), CB_OK);
    T_EQ_INT(cb_iot_authorize(&g_ctx, &ev.iot, "not-an-arn", NOW_MS, &out, &ok), CB_E_STATE);
    T_EQ_INT(cb_iot_authorize(&g_ctx, &ev.iot, "arn:aws:lambda:us-east-1:487337210065:function:x*", NOW_MS, &out, &ok), CB_OK);
    T_CHECK(ok);
    T_EQ_INT(cb_iot_authorize(&g_ctx, &ev.iot, "arn:aws:lambda:us-east-*:487337210065:function:x", NOW_MS, &out, &ok),
             CB_E_INVALID);   // a wildcard can never reach a policy
    T_CHECK(!ok);
    cb_event_free(&ev);
    cb_buf_free(&out);
}

// Only through the edge: with CBASE_ORIGIN_SECRET set, a CONNECT that did not
// come through CloudFront (which adds X-Cbase-Origin) is refused.
static void only_through_the_edge(void) {
    setup();
    cb_strlcpy(g_cfg.origin_secret, SECRET, sizeof g_cfg.origin_secret);
    cb_buf out;
    cb_buf_init(&out);
    bool ok;
    T_EQ_INT(answer("tok-" UA, UA "-t", NULL, &out, &ok), CB_OK);
    T_CHECK(!ok);
    T_EQ_INT(answer("tok-" UA, UA "-t", "X-Cbase-Origin\":\"wrong-secret-wrong-secret-wrong-secret", &out, &ok), CB_OK);
    T_CHECK(!ok);
    T_EQ_INT(answer("tok-" UA, UA "-t", "X-Cbase-Origin\":\"" SECRET, &out, &ok), CB_OK);
    T_CHECK(ok);
    cb_buf_free(&out);
}

static void the_encoder_refuses_what_a_policy_cannot_hold(void) {
    cb_buf out;
    cb_buf_init(&out);
    T_EQ_INT(cb_iot_auth_encode(true, UA, 600, "us-east-1", "487337210065", &out), CB_OK);
    T_EQ_INT(cb_iot_auth_encode(true, "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaa*", 600, "us-east-1", "1", &out), CB_E_INVALID);
    T_EQ_INT(cb_iot_auth_encode(true, UA "/x", 600, "us-east-1", "1", &out), CB_E_INVALID);
    T_EQ_INT(cb_iot_auth_encode(true, NULL, 600, "us-east-1", "1", &out), CB_E_INVALID);
    T_EQ_INT(cb_iot_auth_encode(true, UA, 600, "us-east-1:*", "1", &out), CB_E_INVALID);
    T_EQ_INT(cb_iot_auth_encode(true, UA, 600, "us-east-1", "", &out), CB_E_INVALID);
    T_EQ_INT(cb_iot_auth_encode(true, UA, 299, "us-east-1", "1", &out), CB_E_INVALID);    // IoT's floor
    T_EQ_INT(cb_iot_auth_encode(true, UA, 86401, "us-east-1", "1", &out), CB_E_INVALID);  // and cap
    cb_buf_reset(&out);
    T_EQ_INT(cb_iot_auth_encode(false, NULL, 0, NULL, NULL, &out), CB_OK);
    T_EQ_STR(cb_buf_cstr(&out), "{\"isAuthenticated\":false}");
    cb_buf_free(&out);
}

// CBASE_IOT_ENDPOINT is a host name and nothing else: a path or a scheme in it
// would move where signed publishes go.
static void the_endpoint_setting(void) {
    cb_config c;
    char err[256];
    setenv("CBASE_IOT_ENDPOINT", "a1b2c3-ats.iot.us-east-1.amazonaws.com", 1);
    T_EQ_INT(cb_config_from_env(&c, err, sizeof err), CB_OK);
    T_EQ_STR(c.iot_endpoint, "a1b2c3-ats.iot.us-east-1.amazonaws.com");
    const char *bad[] = { "https://a.iot.us-east-1.amazonaws.com", "a.iot.amazonaws.com/x", "a b", "A.iot", "a.iot:443", "a?b" };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        setenv("CBASE_IOT_ENDPOINT", bad[i], 1);
        T_EQ_INT(cb_config_from_env(&c, err, sizeof err), CB_E_INVALID);
    }
    unsetenv("CBASE_IOT_ENDPOINT");
    T_EQ_INT(cb_config_from_env(&c, err, sizeof err), CB_OK);
    T_EQ_STR(c.iot_endpoint, "");
}

int main(void) {
    T_RUN(a_batch_nests);
    T_RUN(decoded_events);
    T_RUN(the_policy);
    T_RUN(only_through_the_edge);
    T_RUN(the_encoder_refuses_what_a_policy_cannot_hold);
    T_RUN(the_endpoint_setting);
    return T_DONE();
}
