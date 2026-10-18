// unit_rt_iot.c - realtime under Lambda on AWS IoT Core (realtime/rt_iot.h):
// a push is one publish per target to its MQTT topic, and the custom
// authorizer turns IoT's CONNECT request (decoded from the JSON IoT sends,
// aws/event.h) into the policy for the token's user, or a refusal. The
// authorizer's half of the authz matrix: a forged, expired or deleted
// account's token, another user's client id and a connection that did not
// come through the edge are refused; the policy names only the user's own
// topics and the spectators', and never iot:Publish.
#include "t.h"

#include "aws/event.h"
#include "config.h"
#include "json_surface.h"
#include "realtime/realtime.h"
#include "realtime/rt_deps.h"
#include "realtime/rt_iot.h"
#include "realtime/stream_mqtt.h"
#include "../../shared/c/b64/b64.h"
#include "util/err.h"
#include "util/json.h"
#include "app/fanout.h"
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
static pthread_mutex_t g_pub_mu = PTHREAD_MUTEX_INITIALIZER;
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
} g_pub;
static int fake_publish(const char *endpoint, const char *topic, const void *data, size_t len) {
    int64_t t0 = cb_mono_ms();
    if (g_pub.wait_ms) poll(NULL, 0, g_pub.wait_ms);
    pthread_mutex_lock(&g_pub_mu);
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

static int published_at(const char *topic) {
    for (int i = 0; i < g_pub.n && i < 48; i++)
        if (strcmp(g_pub.topic[i], topic) == 0) return i;
    return -1;
}

static void setup(void) {
    cb_log_init(CB_LOG_ERROR, 0);
    memset(&g_ctx, 0, sizeof g_ctx);
    memset(&g_cfg, 0, sizeof g_cfg);
    memset(&g_pub, 0, sizeof g_pub);
    g_pub.status = 200;
    g_ctx.cfg = &g_cfg;
    g_ctx.mode = CB_MODE_LAMBDA;
    cb_rt_deps_g.verify_token = fake_verify;
    cb_rt_deps_g.iot_publish = fake_publish;
}

static void pushes_are_publishes(void) {
    setup();
    cb_strlcpy(g_cfg.iot_endpoint, "abc123-ats.iot.us-east-1.amazonaws.com", sizeof g_cfg.iot_endpoint);
    cb_rt *rt;
    T_EQ_INT(cb_rt_iot_new(&g_ctx, &rt), CB_OK);
    const uint8_t cast[] = { 5, 6 };
    rt_publish_cast(rt, UA, G, 3, cast, 2);
    T_EQ_INT(g_pub.n, 1);
    T_EQ_STR(g_pub.endpoint, "abc123-ats.iot.us-east-1.amazonaws.com");
    T_EQ_STR(g_pub.topic[0], "u/" UA "/c/" G);
    const uint8_t want[] = { 1, NW_WS_CAST, 6, 'a', 'b', '1', '2', 'c', 'd', 3, 0, 0, 0, 2, 0, 0, 0, 5, 6 };
    T_EQ_MEM(g_pub.body, g_pub.len, want, sizeof want);   // the nw1 message, as it is
    rt_publish_cast(rt, NULL, G, 3, cast, 2);
    T_EQ_STR(g_pub.topic[1], "s/" G);
    // Chat: each member's own topic, never the whole game's (nw1 only).
    cb_chat_row c;
    memset(&c, 0, sizeof c);
    strcpy(c.id, "m");
    strcpy(c.user_id, UA);
    c.message = "hi";
    cb_uuid members[2] = { UA, UB };
    rt_publish_chat(rt, G, &c, members, 2);
    T_EQ_INT(g_pub.n, 4);
    T_CHECK(published_at("u/" UA "/m/" G) >= 2);   // the members' lanes run at once: either order
    T_CHECK(published_at("u/" UB "/m/" G) >= 2);
    T_EQ_INT(cb_rt_failures_take(), 0);
    // A refused publish counts as a failed delivery.
    g_pub.status = 403;
    rt_publish_cast(rt, UA, G, 4, cast, 2);
    T_EQ_INT(cb_rt_failures_take(), 1);
    g_pub.status = CB_E_IO;
    rt_publish_cast(rt, UA, G, 4, cast, 2);
    T_EQ_INT(cb_rt_failures_take(), 1);
    // No endpoint configured: nothing is sent, and nothing failed.
    g_cfg.iot_endpoint[0] = 0;
    int before = g_pub.n;
    rt_publish_cast(rt, UA, G, 5, cast, 2);
    T_EQ_INT(g_pub.n, before);
    T_EQ_INT(cb_rt_failures_take(), 0);
    // A user id that cannot be a topic level is never published.
    cb_strlcpy(g_cfg.iot_endpoint, "abc123-ats.iot.us-east-1.amazonaws.com", sizeof g_cfg.iot_endpoint);
    rt_publish_cast(rt, "u/+", G, 5, cast, 2);
    T_EQ_INT(g_pub.n, before);
    // forget is not this transport's: IoT names no connection to end.
    cb_rt_forget_user(rt, UA);
    cb_rt_free(rt);
}

// A commit's fan-out between two humans is five publishes: each human's view
// (the dashboard, and a boards host's game), each seat's cast, the
// spectators' cast. In one batch they go out on three lanes at once (one per
// user, one for the spectators), so with IoT taking WAIT ms a publish the
// whole fan-out takes about two publishes' time (a user's view, then its
// cast), not five; and a user still hears its view before its cast.
#define WAIT 60
static void a_fanout_overlaps(void) {
    setup();
    cb_strlcpy(g_cfg.iot_endpoint, "abc123-ats.iot.us-east-1.amazonaws.com", sizeof g_cfg.iot_endpoint);
    cb_rt *rt;
    T_EQ_INT(cb_rt_iot_new(&g_ctx, &rt), CB_OK);
    g_ctx.rt = rt;
    const uint8_t env[] = { 1, 2, 3 }, cast[] = { 4, 5 }, spec[] = { 6 };
    cb_fanout f;
    memset(&f, 0, sizeof f);
    f.game_id = G;
    f.version = 7;
    f.status = "playing";
    f.n_seats = 3;
    cb_strlcpy(f.seats[0].user_id, UA, sizeof f.seats[0].user_id);
    cb_strlcpy(f.seats[2].user_id, UB, sizeof f.seats[2].user_id);   // seat 1 is a bot: nothing for it
    for (int i = 0; i < 3; i++) {
        f.seats[i].envelope = env, f.seats[i].envelope_len = sizeof env;
        f.seats[i].cast = cast, f.seats[i].cast_len = sizeof cast;
    }
    f.spectator_cast = spec;
    f.spectator_cast_len = sizeof spec;
    g_pub.wait_ms = WAIT;
    int64_t t0 = cb_mono_ms();
    cb_fanout_publish(&g_ctx, &f);
    int64_t took = cb_mono_ms() - t0;
    T_EQ_INT(g_pub.n, 5);
    T_EQ_INT(cb_rt_failures_take(), 0);
    int va = published_at("u/" UA "/v"), vb = published_at("u/" UB "/v");
    int ca = published_at("u/" UA "/c/" G), cb = published_at("u/" UB "/c/" G), sp = published_at("s/" G);
    T_CHECK(va >= 0 && vb >= 0 && ca >= 0 && cb >= 0 && sp >= 0);
    // Each user's view was answered before its cast was sent, on one lane.
    T_CHECK(g_pub.end_ms[va] <= g_pub.start_ms[ca]);
    T_CHECK(g_pub.end_ms[vb] <= g_pub.start_ms[cb]);
    T_CHECK(pthread_equal(g_pub.thread[va], g_pub.thread[ca]));
    // Three lanes at once: two publishes' time and some, never five.
    T_CHECK(took >= 2 * WAIT);
    T_CHECK(took < 4 * WAIT);
    T_CHECK(!pthread_equal(g_pub.thread[va], g_pub.thread[vb]));
    T_CHECK(!pthread_equal(g_pub.thread[sp], g_pub.thread[va]) && !pthread_equal(g_pub.thread[sp], g_pub.thread[vb]));

    // A publish refused on one lane counts once, the rest still go.
    memset(&g_pub, 0, sizeof g_pub);
    g_pub.status = 200;
    g_pub.fail_topic = "u/" UB "/v";
    cb_fanout_publish(&g_ctx, &f);
    T_EQ_INT(g_pub.n, 5);
    T_EQ_INT(cb_rt_failures_take(), 1);

    // A seat that left hears the empty view on its own lane, and a batch
    // holds every message of the largest commit (8 humans, 8 gone): 25.
    memset(&g_pub, 0, sizeof g_pub);
    g_pub.status = 200;
    f.n_seats = CB_MAX_SEATS;
    static char ids[2 * CB_MAX_SEATS][40];
    for (int i = 0; i < CB_MAX_SEATS; i++) {
        snprintf(ids[i], sizeof ids[i], "aaaaaaaa-aaaa-4aaa-8aaa-%012d", i);
        cb_strlcpy(f.seats[i].user_id, ids[i], sizeof f.seats[i].user_id);
        f.seats[i].envelope = env, f.seats[i].envelope_len = sizeof env;
        f.seats[i].cast = cast, f.seats[i].cast_len = sizeof cast;
        snprintf(ids[CB_MAX_SEATS + i], sizeof ids[0], "cccccccc-cccc-4ccc-8ccc-%012d", i);
        cb_strlcpy(f.left[i], ids[CB_MAX_SEATS + i], sizeof f.left[i]);
    }
    f.n_left = CB_MAX_SEATS;
    g_pub.wait_ms = 20;
    t0 = cb_mono_ms();
    cb_fanout_publish(&g_ctx, &f);
    took = cb_mono_ms() - t0;
    T_EQ_INT(g_pub.n, 3 * CB_MAX_SEATS + 1);
    T_EQ_INT(cb_rt_failures_take(), 0);
    T_CHECK(took < 6 * 20);   // 17 lanes at once: a view and a cast each at most
    char t[128];
    snprintf(t, sizeof t, "u/%s/v", ids[CB_MAX_SEATS]);
    T_CHECK(published_at(t) >= 0);
    g_ctx.rt = NULL;
    cb_rt_free(rt);
}

// Without a batch (a single publish, or a transport with no lanes) a
// publish is sent at once, on the caller's thread; batches nest, and only the
// outermost end sends.
static void a_batch_nests(void) {
    setup();
    cb_strlcpy(g_cfg.iot_endpoint, "abc123-ats.iot.us-east-1.amazonaws.com", sizeof g_cfg.iot_endpoint);
    cb_rt *rt;
    T_EQ_INT(cb_rt_iot_new(&g_ctx, &rt), CB_OK);
    const uint8_t cast[] = { 5, 6 };
    rt_publish_cast(rt, UA, G, 3, cast, 2);
    T_EQ_INT(g_pub.n, 1);
    T_CHECK(pthread_equal(g_pub.thread[0], pthread_self()));
    cb_rt_batch_begin(rt);
    cb_rt_batch_begin(rt);
    rt_publish_cast(rt, UA, G, 4, cast, 2);
    rt_publish_cast(rt, UB, G, 4, cast, 2);
    cb_rt_batch_end(rt);
    T_EQ_INT(g_pub.n, 1);   // held until the outer end
    rt_publish_cast(rt, NULL, G, 4, cast, 2);
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
    rt_publish_cast(rt, long_id, G, 5, cast, 2);
    rt_publish_cast(rt, UA, G, 5, cast, 2);
    T_EQ_INT(g_pub.n, 4);
    cb_rt_batch_end(rt);
    T_EQ_INT(g_pub.n, 5);
    T_EQ_INT(cb_rt_failures_take(), 0);
    // More messages than a batch holds: the rest are sent at once, none lost.
    memset(&g_pub, 0, sizeof g_pub);
    g_pub.status = 200;
    cb_rt_batch_begin(rt);
    for (int i = 0; i < 40; i++) rt_publish_cast(rt, UA, G, 6 + i, cast, 2);
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
    rt_publish_cast(rt, UA, G, 50, cast, 2);
    cb_rt_batch_begin(rt2);
    rt_publish_cast(rt2, UB, G, 50, cast, 2);
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
    T_RUN(pushes_are_publishes);
    T_RUN(a_fanout_overlaps);
    T_RUN(a_batch_nests);
    T_RUN(decoded_events);
    T_RUN(the_policy);
    T_RUN(only_through_the_edge);
    T_RUN(the_encoder_refuses_what_a_policy_cannot_hold);
    T_RUN(the_endpoint_setting);
    return T_DONE();
}
