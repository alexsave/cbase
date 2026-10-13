// unit_aws_calls.c - the API Gateway management calls and the Lambda
// self-invoke, against a local TLS server standing in for AWS.
//
// Each request is checked on the wire (method, path, headers, body) and its
// signature is recomputed from what arrived: the X-Amz-Date it carries, the
// Host it was sent to, the exact path and body. A client that signed one thing
// and sent another fails here, which no AWS test vector can show.
#include "t.h"

#include "aws/apigw_mgmt.h"
#include "aws/aws_call.h"
#include "aws/event.h"
#include "aws/https.h"
#include "aws/lambda_invoke.h"
#include "aws/sigv4.h"
#include "lib_aws_tls.h"
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
    hdr(req, "Host", host, sizeof host);
    hdr(req, "X-Amz-Date", date, sizeof date);
    hdr(req, "Authorization", auth, sizeof auth);
    struct tm tm = { 0 };
    T_CHECK(strptime(date, "%Y%m%dT%H%M%SZ", &tm) != NULL);
    const char *body = strstr(req, "\r\n\r\n") + 4;
    cb_aws_creds c;
    T_EQ_INT(cb_aws_creds_from_env(&c), CB_OK);
    cb_sigv4_req r = {
        .method = method, .host = host, .path = path, .query = "", .service = service, .headers = signed_extra,
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

static void apigw_post_and_delete(void) {
    set_env();
    tls_srv s;
    T_EQ_INT(tls_srv_start(&s, answer, NULL), 0);
    T_EQ_INT(cb_https_set_ca_file(s.ca_file), CB_OK);
    char ep[128];
    snprintf(ep, sizeof ep, "https://localhost:%d/prod/", s.port);

    const char *frame = "[null,null,\"realtime:game-a1b2c3\",\"broadcast\",{\"b\":\"AAE=\"}]";
    g_status = 200;
    T_EQ_INT(cb_apigw_post(ep, "AAAA1234=", frame, strlen(frame)), 200);
    char *req = tls_srv_last(&s);
    T_CHECK(cb_starts_with(req, "POST /prod/@connections/AAAA1234%3D HTTP/1.1\r\n"));
    T_CHECK(strstr(req, "\r\n\r\n") && strcmp(strstr(req, "\r\n\r\n") + 4, frame) == 0);
    char v[512];
    hdr(req, "Authorization", v, sizeof v);
    T_CHECK(strstr(v, "Credential=AKIDEXAMPLE/") != NULL);
    T_CHECK(strstr(v, "/us-west-2/execute-api/aws4_request, SignedHeaders=host;x-amz-date;x-amz-security-token, ") != NULL);
    hdr(req, "X-Amz-Security-Token", v, sizeof v);
    T_EQ_STR(v, "SESSIONTOKEN");
    signature_matches(req, "execute-api", NULL, 0);
    free(req);

    g_status = 410;
    T_EQ_INT(cb_apigw_post(ep, "gone/+=", "x", 1), CB_APIGW_GONE);
    req = tls_srv_last(&s);
    T_CHECK(cb_starts_with(req, "POST /prod/@connections/gone%2F%2B%3D HTTP/1.1\r\n"));
    signature_matches(req, "execute-api", NULL, 0);
    free(req);

    g_status = 204;
    T_EQ_INT(cb_apigw_delete(ep, "AAAA1234="), 204);
    req = tls_srv_last(&s);
    T_CHECK(cb_starts_with(req, "DELETE /prod/@connections/AAAA1234%3D HTTP/1.1\r\n"));
    T_CHECK(strstr(req, "Content-Length") == NULL);
    signature_matches(req, "execute-api", NULL, 0);
    free(req);

    T_EQ_INT(cb_apigw_post("http://localhost/prod", "A", "x", 1), CB_E_IO);   // not https
    T_EQ_INT(cb_apigw_post(ep, "", "x", 1), CB_E_IO);
    tls_srv_stop(&s);
    // Nobody listening any more.
    T_EQ_INT(cb_apigw_post(ep, "A", "x", 1), CB_E_IO);
    unsetenv("AWS_SECRET_ACCESS_KEY");
    T_EQ_INT(cb_apigw_post(ep, "A", "x", 1), CB_E_IO);   // no credentials
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

static void url_parse(void) {
    char host[64], base[64];
    int port;
    T_EQ_INT(cb_aws_url_parse("https://abc.execute-api.us-west-2.amazonaws.com/prod", host, sizeof host, &port, base, sizeof base), CB_OK);
    T_EQ_STR(host, "abc.execute-api.us-west-2.amazonaws.com");
    T_EQ_INT(port, 443);
    T_EQ_STR(base, "/prod");
    T_EQ_INT(cb_aws_url_parse("wss://h:8443/a/b//", host, sizeof host, &port, base, sizeof base), CB_OK);
    T_EQ_STR(host, "h");
    T_EQ_INT(port, 8443);
    T_EQ_STR(base, "/a/b");
    T_EQ_INT(cb_aws_url_parse("https://h", host, sizeof host, &port, base, sizeof base), CB_OK);
    T_EQ_STR(base, "");
    T_EQ_INT(cb_aws_url_parse("https://h:0/x", host, sizeof host, &port, base, sizeof base), CB_E_INVALID);
    T_EQ_INT(cb_aws_url_parse("https://:443/x", host, sizeof host, &port, base, sizeof base), CB_E_INVALID);
    T_EQ_INT(cb_aws_url_parse("h/x", host, sizeof host, &port, base, sizeof base), CB_E_INVALID);
}

int main(void) {
    T_RUN(url_parse);
    T_RUN(apigw_post_and_delete);
    T_RUN(lambda_self_invoke);
    return T_DONE();
}
