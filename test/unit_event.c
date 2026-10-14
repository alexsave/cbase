// unit_event.c - the Lambda envelope: events in, responses out.
//
// The fixtures are the event shapes AWS documents, written out by hand:
// a Function URL request ("Invoking Lambda function URLs", payload format
// 2.0), the API Gateway WebSocket $connect / $default / $disconnect events
// ("Setting up WebSocket API integrations"), an EventBridge scheduled event,
// and c_base's own two string literals.
#include "t.h"

#include "aws/event.h"
#include "fixture_cloudfront_event.h"
#include "util/b64.h"
#include "util/err.h"
#include "util/json.h"

#include <stdlib.h>

static int decode(const char *json, cb_event *ev) {
    // An exactly-sized heap copy, so a read past the end is an ASan error.
    size_t n = strlen(json);
    char *b = malloc(n ? n : 1);
    memcpy(b, json, n);
    int rc = cb_event_decode(b, n, ev);
    // Nothing the event keeps may point into the caller's buffer.
    memset(b, 'X', n);
    free(b);
    return rc;
}

// The docs' Function URL request, with the docs' own values.
static const char URL_POST[] =
    "{\n"
    "  \"version\": \"2.0\",\n"
    "  \"routeKey\": \"$default\",\n"
    "  \"rawPath\": \"/my/path\",\n"
    "  \"rawQueryString\": \"parameter1=value1&parameter1=value2&parameter2=value\",\n"
    "  \"cookies\": [\"cookie1\", \"cookie2\"],\n"
    "  \"headers\": {\"header1\": \"value1\", \"header2\": \"value1,value2\"},\n"
    "  \"queryStringParameters\": {\"parameter1\": \"value1,value2\", \"parameter2\": \"value\"},\n"
    "  \"requestContext\": {\n"
    "    \"accountId\": \"123456789012\",\n"
    "    \"apiId\": \"<urlid>\",\n"
    "    \"authentication\": null,\n"
    "    \"authorizer\": {\"iam\": {\"accessKey\": \"AKIA...\", \"accountId\": \"111122223333\", \"callerId\": \"AIDA...\",\n"
    "                    \"cognitoIdentity\": null, \"principalOrgId\": null, \"userArn\": \"arn:aws:iam::111122223333:user/example-user\",\n"
    "                    \"userId\": \"AIDA...\"}},\n"
    "    \"domainName\": \"<url-id>.lambda-url.us-west-2.on.aws\",\n"
    "    \"domainPrefix\": \"<url-id>\",\n"
    "    \"http\": {\"method\": \"POST\", \"path\": \"/my/path\", \"protocol\": \"HTTP/1.1\",\n"
    "             \"sourceIp\": \"123.123.123.123\", \"userAgent\": \"agent\"},\n"
    "    \"requestId\": \"id\",\n"
    "    \"routeKey\": \"$default\",\n"
    "    \"stage\": \"$default\",\n"
    "    \"time\": \"12/Mar/2020:19:03:58 +0000\",\n"
    "    \"timeEpoch\": 1583348638390\n"
    "  },\n"
    "  \"body\": \"Hello from client!\",\n"
    "  \"pathParameters\": null,\n"
    "  \"isBase64Encoded\": false,\n"
    "  \"stageVariables\": null\n"
    "}\n";

static void function_url_request(void) {
    cb_event ev;
    T_EQ_INT(decode(URL_POST, &ev), CB_OK);
    T_EQ_INT(ev.kind, CB_EVENT_HTTP);
    const cb_http_req *r = &ev.http;
    T_EQ_STR(r->method, "POST");
    T_EQ_STR(r->path, "/my/path");
    T_EQ_STR(r->query, "parameter1=value1&parameter1=value2&parameter2=value");
    char v[64];
    T_CHECK(cb_http_query_get(r, "parameter2", v, sizeof v));
    T_EQ_STR(v, "value");
    T_EQ_INT(r->n_headers, 3);
    T_EQ_STR(cb_http_header_get(r, "header1"), "value1");
    T_EQ_STR(cb_http_header_get(r, "Header2"), "value1,value2");
    T_EQ_STR(cb_http_header_get(r, "cookie"), "cookie1; cookie2");
    T_EQ_MEM(r->body, r->body_len, "Hello from client!", 18);
    T_EQ_STR(r->peer, "123.123.123.123");
    cb_event_free(&ev);
}

// What the web client's action POST looks like through a Function URL: an
// octet-stream body (base64 in the event, bytes 00 and ff included), headers
// with capitals and escapes, no query and no cookies.
static void function_url_binary(void) {
    const char *ev_json =
        "{\"version\":\"2.0\",\"routeKey\":\"$default\",\"rawPath\":\"/functions/v1/action\",\"rawQueryString\":\"\","
        "\"headers\":{\"Content-Type\":\"application/octet-stream\",\"authorization\":\"Bearer eyJ.a.b\","
        "\"apikey\":\"anon\",\"x-escaped\":\"\\u0041\\\"q\\\"\\u00e9\",\"x-number\":5},"
        "\"requestContext\":{\"http\":{\"method\":\"POST\",\"path\":\"/functions/v1/action\",\"protocol\":\"HTTP/1.1\","
        "\"sourceIp\":\"203.0.113.9\"}},\"body\":\"AAH/fgA=\",\"isBase64Encoded\":true}";
    cb_event ev;
    T_EQ_INT(decode(ev_json, &ev), CB_OK);
    T_EQ_INT(ev.kind, CB_EVENT_HTTP);
    T_EQ_STR(ev.http.query, "");
    T_EQ_STR(cb_http_header_get(&ev.http, "content-type"), "application/octet-stream");
    T_EQ_STR(ev.http.headers[0].name, "content-type");   // names lowercased
    T_EQ_STR(cb_http_bearer(&ev.http), "eyJ.a.b");
    T_EQ_STR(cb_http_header_get(&ev.http, "x-escaped"), "A\"q\"\xc3\xa9");
    T_CHECK(cb_http_header_get(&ev.http, "x-number") == NULL);   // not a string: not a header
    T_EQ_INT(ev.http.n_headers, 4);
    T_EQ_MEM(ev.http.body, ev.http.body_len, "\x00\x01\xff\x7e\x00", 5);
    cb_event_free(&ev);

    // A GET with neither body nor headers.
    const char *get = "{\"rawPath\":\"/health\",\"requestContext\":{\"http\":{\"method\":\"GET\"}}}";
    T_EQ_INT(decode(get, &ev), CB_OK);
    T_EQ_STR(ev.http.path, "/health");
    T_EQ_INT(ev.http.n_headers, 0);
    T_CHECK(ev.http.body == NULL);
    T_EQ_INT(ev.http.body_len, 0);
    cb_event_free(&ev);
}

static void too_many_headers_dropped(void) {
    cb_buf j;
    cb_buf_init(&j);
    cb_buf_puts(&j, "{\"rawPath\":\"/\",\"requestContext\":{\"http\":{\"method\":\"GET\"}},\"headers\":{");
    for (int i = 0; i < 70; i++) cb_buf_printf(&j, "%s\"h%d\":\"value number %d\"", i ? "," : "", i, i);
    cb_buf_puts(&j, "},\"cookies\":[\"a=1\"]}");
    cb_event ev;
    T_EQ_INT(decode(cb_buf_cstr(&j), &ev), CB_OK);
    T_EQ_INT(ev.http.n_headers, CB_HTTP_MAX_HEADERS);
    T_EQ_STR(cb_http_header_get(&ev.http, "h63"), "value number 63");
    T_CHECK(cb_http_header_get(&ev.http, "h64") == NULL);
    cb_event_free(&ev);
    cb_buf_free(&j);
}

// ---- WebSocket ---------------------------------------------------------------

static const char WS_CONNECT[] =
    "{\"headers\":{\"Host\":\"abcd123.execute-api.us-east-1.amazonaws.com\","
    "\"Sec-WebSocket-Extensions\":\"permessage-deflate; client_max_window_bits\","
    "\"Sec-WebSocket-Key\":\"...\",\"Sec-WebSocket-Version\":\"13\",\"X-Amzn-Trace-Id\":\"...\","
    "\"X-Forwarded-For\":\"192.0.2.1\",\"X-Forwarded-Port\":\"443\",\"X-Forwarded-Proto\":\"https\"},"
    "\"multiValueHeaders\":{\"Host\":[\"abcd123.execute-api.us-east-1.amazonaws.com\"]},"
    "\"queryStringParameters\":{\"apikey\":\"anon key+/=\",\"vsn\":\"2.0.0\"},"
    "\"multiValueQueryStringParameters\":{\"apikey\":[\"anon key+/=\"],\"vsn\":[\"2.0.0\"]},"
    "\"requestContext\":{\"routeKey\":\"$connect\",\"eventType\":\"CONNECT\",\"extendedRequestId\":\"ABCD1234=\","
    "\"requestTime\":\"09/Feb/2024:18:11:43 +0000\",\"messageDirection\":\"IN\",\"stage\":\"prod\","
    "\"connectedAt\":1707502303419,\"requestTimeEpoch\":1707502303420,\"identity\":{\"sourceIp\":\"192.0.2.1\"},"
    "\"requestId\":\"ABCD1234=\",\"domainName\":\"abcd1234.execute-api.us-east-1.amazonaws.com\","
    "\"connectionId\":\"AAAA1234=\",\"apiId\":\"abcd1234\"},\"isBase64Encoded\":false}";

typedef struct {
    char got[512];
} qs;
static bool collect(const char *k, const char *v, void *ud) {
    qs *q = ud;
    size_t n = strlen(q->got);
    snprintf(q->got + n, sizeof q->got - n, "[%s=%s]", k, v);
    return true;
}

static void ws_connect(void) {
    cb_event ev;
    T_EQ_INT(decode(WS_CONNECT, &ev), CB_OK);
    T_EQ_INT(ev.kind, CB_EVENT_WS);
    T_EQ_STR(ev.ws.route_key, "$connect");
    T_EQ_STR(ev.ws.connection_id, "AAAA1234=");
    T_EQ_STR(ev.ws.domain, "abcd1234.execute-api.us-east-1.amazonaws.com");
    T_EQ_STR(ev.ws.stage, "prod");
    T_EQ_STR(ev.ws.query, "apikey=anon%20key%2B%2F%3D&vsn=2.0.0");
    qs q = { "" };
    T_EQ_INT(cb_http_query_each(ev.ws.query, collect, &q), 2);   // and it decodes back exactly
    T_EQ_STR(q.got, "[apikey=anon key+/=][vsn=2.0.0]");
    T_EQ_INT(ev.ws.body_len, 0);
    cb_event_free(&ev);
}

static void ws_default_and_disconnect(void) {
    const char *msg =
        "{\"requestContext\":{\"routeKey\":\"$default\",\"messageId\":\"AAAAAAAAAAAAAAA=\",\"eventType\":\"MESSAGE\","
        "\"extendedRequestId\":\"ABCD1234=\",\"requestTime\":\"09/Feb/2024:18:13:04 +0000\",\"messageDirection\":\"IN\","
        "\"stage\":\"prod\",\"connectedAt\":1707502303419,\"requestTimeEpoch\":1707502384346,"
        "\"identity\":{\"sourceIp\":\"192.0.2.1\"},\"requestId\":\"ABCD1234=\","
        "\"domainName\":\"abcd1234.execute-api.us-east-1.amazonaws.com\",\"connectionId\":\"AAAA1234=\",\"apiId\":\"abcd1234\"},"
        "\"body\":\"[\\\"1\\\",\\\"1\\\",\\\"realtime:game-a1b2c3\\\",\\\"phx_join\\\",{}]\",\"isBase64Encoded\":false}";
    cb_event ev;
    T_EQ_INT(decode(msg, &ev), CB_OK);
    T_EQ_INT(ev.kind, CB_EVENT_WS);
    T_EQ_STR(ev.ws.route_key, "$default");
    T_EQ_STR(ev.ws.query, "");
    const char *frame = "[\"1\",\"1\",\"realtime:game-a1b2c3\",\"phx_join\",{}]";
    T_EQ_MEM(ev.ws.body, ev.ws.body_len, frame, strlen(frame));
    cb_event_free(&ev);

    const char *bin = "{\"requestContext\":{\"routeKey\":\"$default\",\"connectionId\":\"B=\",\"stage\":\"prod\","
                      "\"domainName\":\"d\"},\"body\":\"AP8=\",\"isBase64Encoded\":true}";
    T_EQ_INT(decode(bin, &ev), CB_OK);
    T_EQ_MEM(ev.ws.body, ev.ws.body_len, "\x00\xff", 2);
    cb_event_free(&ev);

    const char *bye =
        "{\"headers\":{\"Host\":\"abcd1234.execute-api.us-east-1.amazonaws.com\",\"x-api-key\":\"\","
        "\"X-Forwarded-For\":\"\",\"x-restapi\":\"\"},\"multiValueHeaders\":{},"
        "\"requestContext\":{\"routeKey\":\"$disconnect\",\"disconnectStatusCode\":1005,\"eventType\":\"DISCONNECT\","
        "\"extendedRequestId\":\"ABCD1234=\",\"requestTime\":\"09/Feb/2024:18:23:28 +0000\",\"messageDirection\":\"IN\","
        "\"disconnectReason\":\"Client-side close frame status not set\",\"stage\":\"prod\",\"connectedAt\":1707503007396,"
        "\"requestTimeEpoch\":1707503008941,\"identity\":{\"sourceIp\":\"192.0.2.1\"},\"requestId\":\"ABCD1234=\","
        "\"domainName\":\"abcd1234.execute-api.us-east-1.amazonaws.com\",\"connectionId\":\"AAAA1234=\",\"apiId\":\"abcd1234\"},"
        "\"isBase64Encoded\":false}";
    T_EQ_INT(decode(bye, &ev), CB_OK);
    T_EQ_INT(ev.kind, CB_EVENT_WS);
    T_EQ_STR(ev.ws.route_key, "$disconnect");
    T_EQ_STR(ev.ws.connection_id, "AAAA1234=");
    cb_event_free(&ev);
}

// ---- the schedule and the self-invoke ---------------------------------------------

static void schedule_and_bot_drive(void) {
    const char *eb =
        "{\"version\":\"0\",\"id\":\"53dc4d37-cffa-4f76-80c9-8b7d4a4d2eaa\",\"detail-type\":\"Scheduled Event\","
        "\"source\":\"aws.events\",\"account\":\"123456789012\",\"time\":\"2015-10-08T16:53:06Z\",\"region\":\"us-east-1\","
        "\"resources\":[\"arn:aws:events:us-east-1:123456789012:rule/my-scheduled-rule\"],\"detail\":{}}";
    cb_event ev;
    T_EQ_INT(decode(eb, &ev), CB_OK);
    T_EQ_INT(ev.kind, CB_EVENT_SCHEDULE);
    cb_event_free(&ev);
    T_EQ_INT(decode("\"scan\"", &ev), CB_OK);
    T_EQ_INT(ev.kind, CB_EVENT_SCHEDULE);
    cb_event_free(&ev);
    T_EQ_INT(decode("  \"scan\"\n", &ev), CB_OK);
    T_EQ_INT(ev.kind, CB_EVENT_SCHEDULE);
    cb_event_free(&ev);
    T_EQ_INT(decode("\"bot_drive:a1b2c3\"", &ev), CB_OK);
    T_EQ_INT(ev.kind, CB_EVENT_BOT_DRIVE);
    T_EQ_STR(ev.game_id, "a1b2c3");
    cb_event_free(&ev);

    const char *bad[] = {
        "\"bot_drive:\"", "\"bot_drive:a/b\"", "\"bot_drive:a1b2c3\\\"\"", "\"bot_drive:0123456789012345678901234567890123\"",
        "\"scan2\"", "\"scan", "\"Scan\"", "{\"cbase\":\"bot_drive\",\"game\":\"a1b2c3\"}", "{\"source\":\"aws.s3\"}",
        "{}", "[]", "", "{\"rawPath\":\"/\",\"requestContext\":{\"http\":{\"method\":7}}}",
        "{\"rawPath\":\"/\",\"requestContext\":{\"http\":{\"method\":\"GET\"}},\"body\":\"!!\",\"isBase64Encoded\":true}",
        "{\"rawPath\":\"/\",\"requestContext\":{\"http\":{\"method\":\"GET\"}}", "{\"requestContext\":{\"http\":{\"method\":\"GET\"}}}",
        "{\"requestContext\":{\"connectionId\":\"x\"}}",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        int rc = decode(bad[i], &ev);
        if (rc != CB_E_INVALID) fprintf(stderr, "     case %zu: %s\n", i, bad[i]);
        T_EQ_INT(rc, CB_E_INVALID);
        T_EQ_INT(ev.kind, CB_EVENT_UNKNOWN);
        cb_event_free(&ev);
    }
}

// ---- responses ----------------------------------------------------------------------

static void encode_text(void) {
    cb_http_res res;
    cb_res_init(&res);
    cb_res_text(&res, 200, "ok cbase \"0.1.0\"\n");
    cb_res_header(&res, "Cache-Control", "no-store");
    cb_res_header(&res, "Vary", "Origin");
    cb_res_header(&res, "vary", "Accept");
    cb_res_header(&res, "Set-Cookie", "a=1; Path=/");
    cb_res_header(&res, "Set-Cookie", "b=2");
    cb_buf out;
    cb_buf_init(&out);
    T_EQ_INT(cb_event_encode_http(&res, &out), CB_OK);
    T_EQ_STR(cb_buf_cstr(&out),
             "{\"statusCode\":200,\"headers\":{\"Content-Type\":\"text/plain; charset=utf-8\",\"Cache-Control\":\"no-store\","
             "\"Vary\":\"Origin, Accept\"},\"cookies\":[\"a=1; Path=/\",\"b=2\"],"
             "\"body\":\"ok cbase \\\"0.1.0\\\"\\n\",\"isBase64Encoded\":false}");
    cb_res_free(&res);

    // An empty 204: an empty body, never base64.
    cb_res_init(&res);
    cb_res_status(&res, 204);
    cb_buf_reset(&out);
    T_EQ_INT(cb_event_encode_http(&res, &out), CB_OK);
    T_EQ_STR(cb_buf_cstr(&out), "{\"statusCode\":204,\"headers\":{},\"body\":\"\",\"isBase64Encoded\":false}");
    cb_res_free(&res);
    cb_buf_free(&out);
}

static void encode_binary(void) {
    // Octet-stream goes base64 even when its bytes happen to be text.
    cb_http_res res;
    cb_res_init(&res);
    cb_res_send(&res, 200, "application/octet-stream", "\x01\x02" "abc", 5);
    cb_buf out;
    cb_buf_init(&out);
    T_EQ_INT(cb_event_encode_http(&res, &out), CB_OK);
    T_EQ_STR(cb_buf_cstr(&out),
             "{\"statusCode\":200,\"headers\":{\"Content-Type\":\"application/octet-stream\"},\"body\":\"AQJhYmM=\","
             "\"isBase64Encoded\":true}");
    cb_res_free(&res);

    // Text that is not UTF-8 goes base64 too, and every byte comes back.
    static const uint8_t bytes[] = { 'h', 'i', 0xff, 0xc3, 0x28, 0xed, 0xa0, 0x80, 0 };
    cb_res_init(&res);
    cb_res_send(&res, 400, "text/plain", bytes, sizeof bytes);
    cb_buf_reset(&out);
    T_EQ_INT(cb_event_encode_http(&res, &out), CB_OK);
    T_CHECK(cb_json_valid((const char *)out.p, out.len));
    bool b64 = false;
    T_CHECK(cb_json_get_bool((const char *)out.p, out.len, "isBase64Encoded", &b64) && b64);
    char body[64];
    uint8_t back[64];
    T_CHECK(cb_json_get_str((const char *)out.p, out.len, "body", body, sizeof body));
    T_EQ_INT(cb_b64_decode(body, strlen(body), back, sizeof back), (long long)sizeof bytes);
    T_EQ_MEM(back, sizeof bytes, bytes, sizeof bytes);
    cb_res_free(&res);

    // Valid UTF-8 with an escape-worthy control byte stays text, escaped.
    cb_res_init(&res);
    cb_res_send(&res, 200, "application/json", "{\"n\":\"\xc3\xa9\x01\"}", 11);
    cb_buf_reset(&out);
    T_EQ_INT(cb_event_encode_http(&res, &out), CB_OK);
    T_CHECK(strstr(cb_buf_cstr(&out), "\"body\":\"{\\\"n\\\":\\\"\xc3\xa9\\u0001\\\"}\",\"isBase64Encoded\":false") != NULL);
    cb_res_free(&res);
    cb_buf_free(&out);
}

// Each kind of invalid UTF-8 on its own sends the body base64; valid
// multi-byte text (2, 3 and 4 bytes, the top code point) does not.
static void utf8_rule(void) {
    static const struct { const char *b; size_t n; bool b64; } cases[] = {
        { "\xff", 1, true },                       // never a UTF-8 byte
        { "\xc3\x28", 2, true },                  // a lead without its continuation
        { "\xe2\x82", 2, true },                  // cut short
        { "\xc0\x80", 2, true },                  // overlong NUL
        { "\xe0\x80\xaf", 3, true },             // overlong '/' in three bytes
        { "\xed\xa0\x80", 3, true },             // a UTF-16 surrogate
        { "\xf4\x90\x80\x80", 4, true },        // past U+10FFFF
        { "\x80", 1, true },                       // a lone continuation
        { "\xc3\xa9\xe2\x82\xac\xf0\x9f\x83\x8f\xf4\x8f\xbf\xbf", 13, false },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        cb_http_res res;
        cb_res_init(&res);
        cb_res_send(&res, 200, "text/plain", cases[i].b, cases[i].n);
        cb_buf out;
        cb_buf_init(&out);
        T_EQ_INT(cb_event_encode_http(&res, &out), CB_OK);
        bool b64 = !cases[i].b64;
        T_CHECK(cb_json_get_bool((const char *)out.p, out.len, "isBase64Encoded", &b64));
        if (b64 != cases[i].b64) fprintf(stderr, "     utf8 case %zu\n", i);
        T_EQ_INT(b64, cases[i].b64);
        cb_buf_free(&out);
        cb_res_free(&res);
    }
}

static void encode_ws(void) {
    cb_buf out;
    cb_buf_init(&out);
    T_EQ_INT(cb_event_encode_ws(401, &out), CB_OK);
    T_EQ_STR(cb_buf_cstr(&out), "{\"statusCode\":401}");
    cb_buf_free(&out);
}

// A request through CloudFront, as Lambda delivered it: 30-odd cloudfront-*
// and x-amz* headers, the origin's own host, a JSON body. The decoder always
// read this shape; what broke in production was the chunked /next answer
// around it (unit_runtime_api.c).
static void function_url_via_cloudfront(void) {
    cb_event ev;
    T_EQ_INT(decode(CLOUDFRONT_SIGNUP_EVENT, &ev), CB_OK);
    T_EQ_INT(ev.kind, CB_EVENT_HTTP);
    T_EQ_STR(ev.http.method, "POST");
    T_EQ_STR(ev.http.path, "/auth/v1/signup");
    T_EQ_STR(ev.http.query, "");
    T_EQ_STR(cb_http_header_get(&ev.http, "apikey"), "test-anon-key");
    T_EQ_STR(cb_http_header_get(&ev.http, "content-type"), "application/json");
    T_EQ_STR(cb_http_header_get(&ev.http, "x-forwarded-for"), "203.0.113.7");
    T_EQ_STR(ev.http.peer, "198.51.100.72");
    static const char BODY[] = "{\"email\":\"probecf1@probe.invalid\",\"password\":\"probe-pass-123\","
                               "\"data\":{\"username\":\"probecf1\"}}";
    T_EQ_INT((int)ev.http.body_len, (int)strlen(BODY));
    T_CHECK(ev.http.body && memcmp(ev.http.body, BODY, strlen(BODY)) == 0);
    cb_event_free(&ev);
}

// The raw event a failed decode logs keeps its shape and loses its secrets.
static void redact_secrets(void) {
    static const char EV[] =
        "{\"rawQueryString\":\"apikey=SEKRIT1&vsn=2.0.0\","
        "\"cookies\":[\"sb=SEKRIT2\",\"x=SEKRIT3\"],"
        "\"headers\":{\"Authorization\":\"Bearer SEKRIT4\",\"apikey\":\"SEKRIT5\",\"host\":\"h.example\"},"
        "\"body\":\"{\\\"password\\\":\\\"SEKRIT6\\\"}\",\"isBase64Encoded\":false}";
    cb_buf r;
    cb_buf_init(&r);
    T_EQ_INT(cb_event_redact(EV, strlen(EV), 4096, &r), CB_OK);
    const char *s = cb_buf_cstr(&r);
    T_CHECK(strstr(s, "SEKRIT") == NULL);
    T_CHECK(strstr(s, "\"rawQueryString\":\"apikey=REDACTED&vsn=2.0.0\"") != NULL);
    T_CHECK(strstr(s, "\"host\":\"h.example\"") != NULL);
    T_CHECK(strstr(s, "\"isBase64Encoded\":false") != NULL);
    T_CHECK(strstr(s, "\"Authorization\":\"REDACTED 14\"") != NULL);
    cb_buf_free(&r);
    // The cap cuts the copy.
    cb_buf_init(&r);
    T_EQ_INT(cb_event_redact(EV, strlen(EV), 10, &r), CB_OK);
    T_EQ_INT((int)r.len, 10);
    cb_buf_free(&r);
}

int main(void) {
    T_RUN(function_url_via_cloudfront);
    T_RUN(redact_secrets);
    T_RUN(function_url_request);
    T_RUN(function_url_binary);
    T_RUN(too_many_headers_dropped);
    T_RUN(ws_connect);
    T_RUN(ws_default_and_disconnect);
    T_RUN(schedule_and_bot_drive);
    T_RUN(encode_text);
    T_RUN(encode_binary);
    T_RUN(utf8_rule);
    T_RUN(encode_ws);
    return T_DONE();
}
