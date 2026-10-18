// unit_aws_calls.c - the IoT publish, the Lambda self-invoke, the CloudFront
// invalidation, SSM and DynamoDB, against a local TLS server standing in for AWS.
//
// Each request is checked on the wire (method, path, headers, body) and its
// signature is recomputed from what arrived: the X-Amz-Date it carries, the
// Host it was sent to, the exact path and body. A client that signed one thing
// and sent another fails here, which no AWS test vector can show.
#include "t.h"

#include "app/edge.h"
#include "aws/aws_call.h"
#include "aws/cloudfront.h"
#include "aws/event.h"
#include "aws/https.h"
#include "aws/iot_data.h"
#include "aws/lambda_invoke.h"
#include "aws/sigv4.h"
#include "aws/ssm.h"
#include "config.h"
#include "json_surface.h"
#include "lib_aws_tls.h"
#include "store/store.h"
#include "util/err.h"

#include <stdlib.h>
#include <time.h>

static int g_status = 200;

static tls_action answer(const cb_buf *req, cb_buf *resp, void *ud) {
    if (g_status == 410) cb_buf_puts(resp, "HTTP/1.1 410 Gone\r\nContent-Length: 20\r\n\r\n{\"message\": \"Gone\"}\n");
    else cb_buf_printf(resp, "HTTP/1.1 %d X\r\nContent-Length: 0\r\n\r\n", g_status);
    return TLS_KEEP;
}

static void set_env(void) {
    setenv("AWS_ACCESS_KEY_ID", "AKIDEXAMPLE", 1);
    setenv("AWS_SECRET_ACCESS_KEY", "wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY", 1);
    setenv("AWS_SESSION_TOKEN", "SESSIONTOKEN", 1);
    setenv("AWS_REGION", "us-west-2", 1);
}

// The value of a header in a raw request ("" when absent).
static void hdr(const char *req, const char *name, char *out, size_t cap) {
    out[0] = 0;
    char pat[64];
    snprintf(pat, sizeof pat, "\r\n%s: ", name);
    const char *p = strstr(req, pat);
    if (!p) return;
    p += strlen(pat);
    const char *e = strstr(p, "\r\n");
    cb_strncpy0(out, cap, p, (size_t)(e - p));
}

// Recomputes the request's signature from the bytes that arrived.
static void signature_matches(const char *req, const char *service, const char *const *signed_extra, int n_extra) {
    char method[16], path[512], host[128], date[32], auth[1024];
    T_CHECK(sscanf(req, "%15s %511s HTTP/1.1", method, path) == 2);
    char *qs = strchr(path, '?');   // the query is signed apart from the path
    if (qs) *qs++ = 0;
    hdr(req, "Host", host, sizeof host);
    hdr(req, "X-Amz-Date", date, sizeof date);
    hdr(req, "Authorization", auth, sizeof auth);
    struct tm tm = { 0 };
    T_CHECK(strptime(date, "%Y%m%dT%H%M%SZ", &tm) != NULL);
    const char *body = strstr(req, "\r\n\r\n") + 4;
    cb_aws_creds c;
    T_EQ_INT(cb_aws_creds_from_env(&c), CB_OK);
    cb_sigv4_req r = {
        .method = method, .host = host, .path = path, .query = qs ? qs : "", .service = service, .headers = signed_extra,
        .n_headers = n_extra, .payload = body, .payload_len = strlen(body), .now_ms = (int64_t)timegm(&tm) * 1000,
    };
    cb_buf out;
    cb_buf_init(&out);
    T_EQ_INT(cb_sigv4_sign(&c, &r, &out), CB_OK);
    char want[1024];
    const char *first = cb_buf_cstr(&out);
    T_CHECK(cb_starts_with(first, "Authorization: "));
    const char *e = strstr(first, "\r\n");
    cb_strncpy0(want, sizeof want, first + 15, (size_t)(e - first - 15));
    T_EQ_STR(auth, want);
    cb_buf_free(&out);
}

// A push to IoT Core: POST /topics/<topic, one encoded segment>?qos=0 with the
// raw message, signed for iotdata (aws/iot_data.h).
static void iot_publish(void) {
    set_env();
    tls_srv s;
    T_EQ_INT(tls_srv_start(&s, answer, NULL), 0);
    T_EQ_INT(cb_https_set_ca_file(s.ca_file), CB_OK);
    char ep[64];
    snprintf(ep, sizeof ep, "localhost:%d", s.port);
    g_status = 200;
    const uint8_t msg[] = { 1, 0x05, 'b', 0x00 };
    T_EQ_INT(cb_iot_publish(ep, "u/aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa/c/ab12cd", msg, 3), 200);
    char *req = tls_srv_last(&s);
    T_CHECK(cb_starts_with(req, "POST /topics/u%2Faaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa%2Fc%2Fab12cd?qos=0 HTTP/1.1\r\n"));
    T_CHECK(strstr(req, "\r\n\r\n") && memcmp(strstr(req, "\r\n\r\n") + 4, msg, 3) == 0);
    char v[512];
    hdr(req, "Authorization", v, sizeof v);
    T_CHECK(strstr(v, "/us-west-2/iotdata/aws4_request, ") != NULL);
    signature_matches(req, "iotdata", NULL, 0);
    free(req);
    g_status = 403;
    T_EQ_INT(cb_iot_publish(ep, "s/ab12cd", msg, 3), 403);
    T_EQ_INT(cb_iot_publish("", "s/ab12cd", msg, 3), CB_E_INVALID);
    T_EQ_INT(cb_iot_publish("localhost/x", "s/ab12cd", msg, 3), CB_E_INVALID);
    T_EQ_INT(cb_iot_publish("localhost:0", "s/ab12cd", msg, 3), CB_E_INVALID);
    T_EQ_INT(cb_iot_publish(ep, "", msg, 3), CB_E_INVALID);
    T_EQ_INT(cb_iot_publish(NULL, "s/a", msg, 3), CB_E_INVALID);
    // AWS_ENDPOINT_URL_IOT_DATA_PLANE replaces the endpoint: https only, no path.
    char url[96];
    snprintf(url, sizeof url, "https://localhost:%d", s.port);
    setenv("AWS_ENDPOINT_URL_IOT_DATA_PLANE", url, 1);
    g_status = 200;
    T_EQ_INT(cb_iot_publish("a1b2c3-ats.iot.us-west-2.amazonaws.com", "s/ab12cd", msg, 3), 200);
    snprintf(url, sizeof url, "http://localhost:%d", s.port);
    setenv("AWS_ENDPOINT_URL_IOT_DATA_PLANE", url, 1);
    T_EQ_INT(cb_iot_publish("a1b2c3-ats.iot.us-west-2.amazonaws.com", "s/ab12cd", msg, 3), CB_E_INVALID);
    snprintf(url, sizeof url, "https://localhost:%d/x", s.port);
    setenv("AWS_ENDPOINT_URL_IOT_DATA_PLANE", url, 1);
    T_EQ_INT(cb_iot_publish("a1b2c3-ats.iot.us-west-2.amazonaws.com", "s/ab12cd", msg, 3), CB_E_INVALID);
    unsetenv("AWS_ENDPOINT_URL_IOT_DATA_PLANE");
    tls_srv_stop(&s);
    T_EQ_INT(cb_iot_publish(ep, "s/ab12cd", msg, 3), CB_E_IO);   // nobody listening
}

static void lambda_self_invoke(void) {
    set_env();
    tls_srv s;
    T_EQ_INT(tls_srv_start(&s, answer, NULL), 0);
    T_EQ_INT(cb_https_set_ca_file(s.ca_file), CB_OK);
    char ep[128];
    snprintf(ep, sizeof ep, "https://localhost:%d", s.port);
    setenv("AWS_ENDPOINT_URL_LAMBDA", ep, 1);
    setenv("AWS_LAMBDA_FUNCTION_NAME", "cbase-Fn", 1);

    g_status = 202;
    T_EQ_INT(cb_lambda_invoke_bot_drive("a1b2c3"), CB_OK);
    char *req = tls_srv_last(&s);
    T_CHECK(cb_starts_with(req, "POST /2015-03-31/functions/cbase-Fn/invocations HTTP/1.1\r\n"));
    char v[512];
    hdr(req, "X-Amz-Invocation-Type", v, sizeof v);
    T_EQ_STR(v, "Event");
    T_EQ_STR(strstr(req, "\r\n\r\n") + 4, "\"bot_drive:a1b2c3\"");
    hdr(req, "Authorization", v, sizeof v);
    T_CHECK(strstr(v, "/us-west-2/lambda/aws4_request, SignedHeaders=host;x-amz-date;x-amz-invocation-type;x-amz-security-token, ") != NULL);
    static const char *const extra[] = { "X-Amz-Invocation-Type: Event" };
    signature_matches(req, "lambda", extra, 1);
    free(req);

    // What it sends is what the decoder reads back.
    char payload[64];
    T_EQ_INT(cb_event_bot_drive_payload("a1b2c3", payload, sizeof payload), 18);
    cb_event ev;
    T_EQ_INT(cb_event_decode(payload, strlen(payload), &ev), CB_OK);
    T_EQ_INT(ev.kind, CB_EVENT_BOT_DRIVE);
    T_EQ_STR(ev.game_id, "a1b2c3");
    cb_event_free(&ev);

    g_status = 500;
    T_EQ_INT(cb_lambda_invoke_bot_drive("a1b2c3"), CB_E_IO);
    int before = atomic_load(&s.requests);
    T_EQ_INT(cb_lambda_invoke_bot_drive("a\"b"), CB_E_INVALID);
    T_EQ_INT(cb_lambda_invoke_bot_drive(""), CB_E_INVALID);
    T_EQ_INT(atomic_load(&s.requests), before);   // refused before anything is sent
    unsetenv("AWS_LAMBDA_FUNCTION_NAME");
    T_EQ_INT(cb_lambda_invoke_bot_drive("a1b2c3"), CB_E_IO);
    tls_srv_stop(&s);
    unsetenv("AWS_ENDPOINT_URL_LAMBDA");
}

// CreateInvalidation: the recorded request (API reference, CreateInvalidation,
// "Request Syntax", with our two paths), signed for cloudfront in us-east-1
// whatever AWS_REGION says, and 201 is its only success.
static const char CF_BODY[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
    "<InvalidationBatch xmlns=\"http://cloudfront.amazonaws.com/doc/2020-05-31/\">"
    "<Paths><Quantity>2</Quantity><Items><Path>/v1/leaderboard</Path><Path>/v1/bots</Path></Items></Paths>"
    "<CallerReference>cbase-1760700000000-ref</CallerReference></InvalidationBatch>";

static void cloudfront_invalidation(void) {
    // The bytes, pure.
    const char *paths[2];
    T_EQ_INT(cb_edge_paths(CB_EDGE_ALL, paths), 2);
    T_EQ_STR(paths[0], "/v1/leaderboard");
    T_EQ_STR(paths[1], "/v1/bots");
    T_EQ_INT(cb_edge_paths(CB_EDGE_BOARD, paths), 1);
    T_EQ_STR(paths[0], "/v1/leaderboard");
    T_EQ_INT(cb_edge_paths(CB_EDGE_BOTS, paths), 1);
    T_EQ_STR(paths[0], "/v1/bots");
    T_EQ_INT(cb_edge_paths(CB_EDGE_ALL, paths), 2);
    cb_buf path, body;
    cb_buf_init(&path);
    cb_buf_init(&body);
    T_EQ_INT(cb_cloudfront_invalidation("E2ABCDEF123456", paths, 2, "cbase-1760700000000-ref", &path, &body), CB_OK);
    T_EQ_STR(cb_buf_cstr(&path), "/2020-05-31/distribution/E2ABCDEF123456/invalidation");
    T_EQ_STR(cb_buf_cstr(&body), CF_BODY);
    cb_buf_free(&path);
    cb_buf_free(&body);
    cb_buf_init(&body);   // text is escaped
    const char *odd[1] = { "/a&b<c>" };
    T_CHECK(cf_invalidation_xml(&body, odd, 1, "r\"'"));
    T_CHECK(strstr(cb_buf_cstr(&body), "<Path>/a&amp;b&lt;c&gt;</Path>") != NULL);
    T_CHECK(strstr(cb_buf_cstr(&body), "<CallerReference>r&quot;&apos;</CallerReference>") != NULL);
    cb_buf_free(&body);
    cb_buf_init(&path);
    cb_buf_init(&body);
    T_EQ_INT(cb_cloudfront_invalidation("", paths, 2, "r", &path, &body), CB_E_INVALID);
    T_EQ_INT(cb_cloudfront_invalidation("E1", paths, 0, "r", &path, &body), CB_E_INVALID);
    cb_buf_free(&path);
    cb_buf_free(&body);

    // On the wire.
    set_env();
    tls_srv s;
    T_EQ_INT(tls_srv_start(&s, answer, NULL), 0);
    T_EQ_INT(cb_https_set_ca_file(s.ca_file), CB_OK);
    char ep[128];
    snprintf(ep, sizeof ep, "https://localhost:%d", s.port);
    setenv("AWS_ENDPOINT_URL_CLOUDFRONT", ep, 1);
    g_status = 201;
    T_EQ_INT(cb_cloudfront_invalidate("E2ABCDEF123456", paths, 2, "cbase-1760700000000-ref"), CB_OK);
    char *req = tls_srv_last(&s);
    T_CHECK(cb_starts_with(req, "POST /2020-05-31/distribution/E2ABCDEF123456/invalidation HTTP/1.1\r\n"));
    T_EQ_STR(strstr(req, "\r\n\r\n") + 4, CF_BODY);
    char v[512];
    hdr(req, "Content-Type", v, sizeof v);
    T_EQ_STR(v, "application/xml");
    hdr(req, "Authorization", v, sizeof v);
    T_CHECK(strstr(v, "/us-east-1/cloudfront/aws4_request, SignedHeaders=content-type;host;x-amz-date;x-amz-security-token, ") != NULL);
    setenv("AWS_REGION", "us-east-1", 1);   // what it was signed for, so the signature can be recomputed
    static const char *const extra[] = { "Content-Type: application/xml" };
    signature_matches(req, "cloudfront", extra, 1);
    free(req);
    set_env();
    // Anything but 201 is a failure (a 200 included).
    g_status = 200;
    T_EQ_INT(cb_cloudfront_invalidate("E2ABCDEF123456", paths, 1, "r2"), CB_E_IO);

    // The flush: one request for everything marked, then nothing pending.
    cb_config cfg;
    memset(&cfg, 0, sizeof cfg);
    snprintf(cfg.cf_distribution, sizeof cfg.cf_distribution, "E2ABCDEF123456");
    int before = atomic_load(&s.requests);
    T_EQ_INT(cb_edge_flush(&cfg), CB_OK);   // nothing marked: nothing sent
    T_EQ_INT(atomic_load(&s.requests), before);
    g_status = 201;
    cb_edge_stale(CB_EDGE_BOARD);
    cb_edge_stale(CB_EDGE_BOARD);
    cb_edge_stale(CB_EDGE_BOTS);
    T_EQ_INT(cb_edge_flush(&cfg), CB_OK);
    T_EQ_INT(atomic_load(&s.requests), before + 1);
    req = tls_srv_last(&s);
    T_CHECK(strstr(req, "<Quantity>2</Quantity><Items><Path>/v1/leaderboard</Path><Path>/v1/bots</Path></Items>") != NULL);
    T_CHECK(strstr(req, "<CallerReference>cbase-") != NULL);
    free(req);
    T_EQ_INT(cb_edge_pending(), 0);
    // No distribution: the marks are dropped, nothing is sent.
    cb_edge_stale(CB_EDGE_BOARD);
    cfg.cf_distribution[0] = 0;
    T_EQ_INT(cb_edge_flush(&cfg), CB_OK);
    T_EQ_INT(atomic_load(&s.requests), before + 1);
    T_EQ_INT(cb_edge_pending(), 0);
    tls_srv_stop(&s);
    unsetenv("AWS_ENDPOINT_URL_CLOUDFRONT");
}

// The DynamoDB backend's production transport is cb_aws_call too: a GetItem
// reaches the endpoint signed for "dynamodb" with its target and content type
// among the signed headers, and the answer comes back through the store.
static tls_action answer_dynamo(const cb_buf *req, cb_buf *resp, void *ud) {
    static const char B[] = "{\"Item\":{\"pk\":{\"S\":\"USER#u1\"},\"sk\":{\"S\":\"PROFILE\"},\"id\":{\"S\":\"u1\"},"
                            "\"email\":{\"S\":\"a@x.io\"},\"password_hash\":{\"S\":\"h\"},\"created_at\":{\"N\":\"5\"}}}";
    cb_buf_printf(resp, "HTTP/1.1 200 OK\r\nContent-Type: application/x-amz-json-1.0\r\nContent-Length: %zu\r\n\r\n%s",
                  sizeof B - 1, B);
    return TLS_KEEP;
}

static void dynamo_through_aws_call(void) {
    set_env();
    tls_srv s;
    T_EQ_INT(tls_srv_start(&s, answer_dynamo, NULL), 0);
    T_EQ_INT(cb_https_set_ca_file(s.ca_file), CB_OK);
    char ep[128];
    snprintf(ep, sizeof ep, "https://localhost:%d", s.port);
    setenv("AWS_ENDPOINT_URL_DYNAMODB", ep, 1);
    cb_store *st = NULL;
    T_EQ_INT(cb_store_open_dynamo("cbase", &st), CB_OK);
    cb_user_row u;
    T_EQ_INT(CB_STORE_CALL(st, user_get, "u1", &u), CB_OK);
    T_EQ_STR(u.email, "a@x.io");
    T_EQ_INT(u.created_at, 5);
    char *req = tls_srv_last(&s);
    T_CHECK(cb_starts_with(req, "POST / HTTP/1.1\r\n"));
    char v[512];
    hdr(req, "X-Amz-Target", v, sizeof v);
    T_EQ_STR(v, "DynamoDB_20120810.GetItem");
    hdr(req, "Authorization", v, sizeof v);
    T_CHECK(strstr(v, "/us-west-2/dynamodb/aws4_request, SignedHeaders=content-type;host;x-amz-date;x-amz-security-token;x-amz-target, ") != NULL);
    static const char *const extra[] = { "Content-Type: application/x-amz-json-1.0", "X-Amz-Target: DynamoDB_20120810.GetItem" };
    signature_matches(req, "dynamodb", extra, 2);
    T_CHECK(strstr(req, "\"TableName\":\"cbase\"") != NULL);
    free(req);
    cb_store_close(st);
    tls_srv_stop(&s);
    setenv("AWS_ENDPOINT_URL_DYNAMODB", "https://h/p", 1);   // a path is not an endpoint
    T_EQ_INT(cb_store_open_dynamo("cbase", &st), CB_E_INVALID);
    unsetenv("AWS_ENDPOINT_URL_DYNAMODB");
}

static void url_parse(void) {
    char host[64], base[64];
    int port;
    bool tls = false;
    T_EQ_INT(cb_aws_url_parse("https://abc.execute-api.us-west-2.amazonaws.com/prod", host, sizeof host, &port, &tls, base, sizeof base), CB_OK);
    T_EQ_STR(host, "abc.execute-api.us-west-2.amazonaws.com");
    T_EQ_INT(port, 443);
    T_CHECK(tls);
    T_EQ_STR(base, "/prod");
    T_EQ_INT(cb_aws_url_parse("wss://h:8443/a/b//", host, sizeof host, &port, &tls, base, sizeof base), CB_OK);
    T_EQ_STR(host, "h");
    T_EQ_INT(port, 8443);
    T_EQ_STR(base, "/a/b");
    T_EQ_INT(cb_aws_url_parse("https://h", host, sizeof host, &port, &tls, base, sizeof base), CB_OK);
    T_EQ_STR(base, "");
    T_EQ_INT(cb_aws_url_parse("https://h:0/x", host, sizeof host, &port, &tls, base, sizeof base), CB_E_INVALID);
    T_EQ_INT(cb_aws_url_parse("https://:443/x", host, sizeof host, &port, &tls, base, sizeof base), CB_E_INVALID);
    T_EQ_INT(cb_aws_url_parse("h/x", host, sizeof host, &port, &tls, base, sizeof base), CB_E_INVALID);
    // http:// is a local stand-in: plain, port 80 unless given.
    T_EQ_INT(cb_aws_url_parse("http://127.0.0.1:8000", host, sizeof host, &port, &tls, base, sizeof base), CB_OK);
    T_EQ_STR(host, "127.0.0.1");
    T_EQ_INT(port, 8000);
    T_CHECK(!tls);
    T_EQ_INT(cb_aws_url_parse("http://dynamo-local", host, sizeof host, &port, &tls, base, sizeof base), CB_OK);
    T_EQ_INT(port, 80);
    T_CHECK(!tls);
    // ... and never for AWS itself.
    T_EQ_INT(cb_aws_url_parse("http://dynamodb.us-east-1.amazonaws.com", host, sizeof host, &port, &tls, base, sizeof base), CB_E_INVALID);
    T_EQ_INT(cb_aws_url_parse("http://dynamodb.us-east-1.AmazonAWS.com:80", host, sizeof host, &port, &tls, base, sizeof base), CB_E_INVALID);
    // Nor its other spellings (finding L12): a trailing dot, the China partition, the bare domain.
    static const char *const aws[] = { "http://dynamodb.us-east-1.amazonaws.com.", "http://dynamodb.cn-north-1.amazonaws.com.cn",
                                       "http://x.amazonaws.com.cn.:80", "http://amazonaws.com", "http://AMAZONAWS.COM." };
    for (size_t i = 0; i < sizeof aws / sizeof aws[0]; i++)
        T_EQ_INT(cb_aws_url_parse(aws[i], host, sizeof host, &port, &tls, base, sizeof base), CB_E_INVALID);
    // A name that only contains it is not AWS.
    T_EQ_INT(cb_aws_url_parse("http://notamazonaws.com", host, sizeof host, &port, &tls, base, sizeof base), CB_OK);
}

// AWS_ENDPOINT_URL_DYNAMODB=http://... (DynamoDB Local): the store speaks plain
// HTTP to it, signed exactly as over TLS, with the port in the Host it signs.
static void dynamo_over_plain_http(void) {
    set_env();
    tls_srv s;
    T_EQ_INT(tls_srv_start_plain(&s, answer_dynamo, NULL), 0);
    char ep[128];
    snprintf(ep, sizeof ep, "http://127.0.0.1:%d", s.port);
    setenv("AWS_ENDPOINT_URL_DYNAMODB", ep, 1);
    cb_store *st = NULL;
    T_EQ_INT(cb_store_open_dynamo("cbase", &st), CB_OK);
    cb_user_row u;
    T_EQ_INT(CB_STORE_CALL(st, user_get, "u1", &u), CB_OK);
    T_EQ_STR(u.email, "a@x.io");
    T_EQ_INT(CB_STORE_CALL(st, user_get, "u1", &u), CB_OK);   // the kept-alive connection, reused
    T_EQ_INT(atomic_load(&s.accepts), 1);
    T_EQ_INT(atomic_load(&s.requests), 2);
    char *req = tls_srv_last(&s);
    char v[512], want[64];
    hdr(req, "Host", v, sizeof v);
    snprintf(want, sizeof want, "127.0.0.1:%d", s.port);
    T_EQ_STR(v, want);
    static const char *const extra[] = { "Content-Type: application/x-amz-json-1.0", "X-Amz-Target: DynamoDB_20120810.GetItem" };
    signature_matches(req, "dynamodb", extra, 2);
    free(req);
    cb_store_close(st);
    tls_srv_stop(&s);
    setenv("AWS_ENDPOINT_URL_DYNAMODB", "http://dynamodb.us-west-2.amazonaws.com", 1);
    T_EQ_INT(cb_store_open_dynamo("cbase", &st), CB_E_INVALID);
    unsetenv("AWS_ENDPOINT_URL_DYNAMODB");
}

// The secrets from SSM (aws/ssm.h): one GetParameters, signed for ssm,
// decrypted, every name under the prefix; each listed value lands in its
// field, an unlisted one leaves the field alone, and nothing about a value
// ever reaches the error text.
static const char *g_ssm_answer;
static tls_action ssm_answer(const cb_buf *req, cb_buf *resp, void *ud) {
    cb_buf_printf(resp, "HTTP/1.1 %d X\r\nContent-Type: application/x-amz-json-1.1\r\nContent-Length: %zu\r\n\r\n%s",
                  g_status, strlen(g_ssm_answer), g_ssm_answer);
    return TLS_KEEP;
}

static void ssm_secrets(void) {
    set_env();
    tls_srv s;
    T_EQ_INT(tls_srv_start(&s, ssm_answer, NULL), 0);
    T_EQ_INT(cb_https_set_ca_file(s.ca_file), CB_OK);
    char ep[128];
    snprintf(ep, sizeof ep, "https://localhost:%d", s.port);
    setenv("AWS_ENDPOINT_URL_SSM", ep, 1);
    cb_config c;
    memset(&c, 0, sizeof c);
    strcpy(c.secrets_ssm, "/cbase/");
    strcpy(c.admin_token, "from-the-environment");
    c.jwt_secret_generated = true;
    g_status = 200;
    g_ssm_answer = "{\"InvalidParameters\":[\"/cbase/admin-token\"],\"Parameters\":["
                   "{\"Name\":\"/cbase/origin-secret\",\"Type\":\"SecureString\",\"Value\":\"origin-0123456789abcdef0123456789\"},"
                   "{\"Name\":\"/cbase/jwt-secret\",\"Type\":\"SecureString\",\"Value\":\"jwt-\\u00410123456789abcdef0123456789\"}]}";
    char err[512] = "";
    T_EQ_INT(cb_ssm_load_secrets(&c, err, sizeof err), CB_OK);
    T_EQ_STR(c.jwt_secret, "jwt-A0123456789abcdef0123456789");
    T_CHECK(!c.jwt_secret_generated);
    T_EQ_STR(c.origin_secret, "");   // not one SSM holds: a name it was not asked for is ignored
    T_EQ_STR(c.admin_token, "from-the-environment");   // not listed: left alone
    char *req = tls_srv_last(&s);
    T_CHECK(cb_starts_with(req, "POST / HTTP/1.1\r\n"));
    char v[512];
    hdr(req, "X-Amz-Target", v, sizeof v);
    T_EQ_STR(v, "AmazonSSM.GetParameters");
    T_EQ_STR(strstr(req, "\r\n\r\n") + 4,
             "{\"Names\":[\"/cbase/jwt-secret\",\"/cbase/admin-token\"],\"WithDecryption\":true}");
    hdr(req, "Authorization", v, sizeof v);
    T_CHECK(strstr(v, "/us-west-2/ssm/aws4_request") != NULL);
    static const char *const extra[] = { "X-Amz-Target: AmazonSSM.GetParameters", "Content-Type: application/x-amz-json-1.1" };
    signature_matches(req, "ssm", extra, 2);
    free(req);

    // A value with a NUL in it is not a secret this process can hold whole: refused.
    memset(&c, 0, sizeof c);
    strcpy(c.secrets_ssm, "/cbase/");
    g_ssm_answer = "{\"Parameters\":[{\"Name\":\"/cbase/jwt-secret\",\"Value\":\"abc\\u0000def\"}]}";
    T_EQ_INT(cb_ssm_load_secrets(&c, err, sizeof err), CB_E_IO);
    T_EQ_STR(c.jwt_secret, "");
    T_CHECK(strstr(err, "/cbase/jwt-secret") != NULL && strstr(err, "abc") == NULL);
    // Refused (no access, say): an error, and its text holds no value.
    g_status = 400;
    g_ssm_answer = "{\"__type\":\"AccessDeniedException\",\"Message\":\"not authorized to perform ssm:GetParameters\"}";
    T_EQ_INT(cb_ssm_load_secrets(&c, err, sizeof err), CB_E_IO);
    T_CHECK(strstr(err, "AccessDenied") != NULL);
    // A value too long for its field.
    g_status = 200;
    static char big[1200];
    char longv[600];
    memset(longv, 'x', sizeof longv - 1);
    longv[sizeof longv - 1] = 0;
    snprintf(big, sizeof big, "{\"Parameters\":[{\"Name\":\"/cbase/admin-token\",\"Value\":\"%s\"}]}", longv);
    g_ssm_answer = big;
    T_EQ_INT(cb_ssm_load_secrets(&c, err, sizeof err), CB_E_IO);
    T_CHECK(strstr(err, "/cbase/admin-token") != NULL && strstr(err, "xxxx") == NULL);
    unsetenv("AWS_ENDPOINT_URL_SSM");
    tls_srv_stop(&s);
}

int main(void) {
    T_RUN(url_parse);
    T_RUN(iot_publish);
    T_RUN(lambda_self_invoke);
    T_RUN(cloudfront_invalidation);
    T_RUN(ssm_secrets);
    T_RUN(dynamo_through_aws_call);
    T_RUN(dynamo_over_plain_http);
    return T_DONE();
}
