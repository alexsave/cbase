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
#include "util/b64.h"
#include "util/err.h"
#include "util/json.h"
#include "util/log.h"

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

static struct { int n; char endpoint[256], topic[8][128]; uint8_t body[256]; size_t len; int status; } g_pub;
static int fake_publish(const char *endpoint, const char *topic, const void *data, size_t len) {
    if (g_pub.n < 8) cb_strlcpy(g_pub.topic[g_pub.n], topic, sizeof g_pub.topic[0]);
    g_pub.n++;
    cb_strlcpy(g_pub.endpoint, endpoint, sizeof g_pub.endpoint);
    g_pub.len = len < sizeof g_pub.body ? len : 0;
    memcpy(g_pub.body, data, g_pub.len);
    return g_pub.status;
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
    T_EQ_STR(g_pub.topic[2], "u/" UA "/m/" G);
    T_EQ_STR(g_pub.topic[3], "u/" UB "/m/" G);
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

// ---- the authorizer --------------------------------------------------------------------------

// An IoT custom authorizer request, as AWS documents it, with the given parts.
static int event_of(cb_event *ev, const char *password, const char *token, const char *query, const char *client_id,
                    const char *origin_header) {
    char pw64[256] = "";
    if (password) cb_b64_encode((const uint8_t *)password, strlen(password), pw64, sizeof pw64);
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
    T_RUN(decoded_events);
    T_RUN(the_policy);
    T_RUN(only_through_the_edge);
    T_RUN(the_encoder_refuses_what_a_policy_cannot_hold);
    T_RUN(the_endpoint_setting);
    return T_DONE();
}
