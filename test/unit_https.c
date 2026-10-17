// unit_https.c - the HTTPS client: its response parser on recorded responses,
// TLS verification and keep-alive against a local TLS server, and one real
// request to https://example.com when this machine is online.
#include "t.h"

#include "aws/https.h"
#include "lib_aws_tls.h"
#include "util/clock.h"
#include "util/err.h"

#include <netdb.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

// ---- the parser --------------------------------------------------------------

// A DynamoDB GetItem answer, as AWS writes it (Content-Length).
static const char DDB[] =
    "HTTP/1.1 200 OK\r\n"
    "Server: Server\r\n"
    "Date: Tue, 13 Oct 2026 17:00:00 GMT\r\n"
    "Content-Type: application/x-amz-json-1.0\r\n"
    "Content-Length: 42\r\n"
    "Connection: keep-alive\r\n"
    "x-amzn-RequestId: 4KBNVRGD25RG1KEO9UT4V3FQDJVV4KQNSO5AEMVJF66Q9ASUAAJG\r\n"
    "x-amz-crc32: 2745614147\r\n"
    "\r\n"
    "{\"Item\":{\"pk\":{\"S\":\"GAME#a1b2c3\"},\"v\":{}}}";

// A chunked answer with a chunk extension and a trailer.
static const char CHUNKED[] =
    "HTTP/1.1 200 OK\r\n"
    "Transfer-Encoding: chunked\r\n"
    "\r\n"
    "7\r\nMozilla\r\n"
    "9;ext=1\r\nDeveloper\r\n"
    "7\r\nNetwork\r\n"
    "0\r\n"
    "X-Trailer: yes\r\n"
    "\r\n";

static int parse(const char *text, size_t n, bool head, bool eof, cb_https_response *r, bool *ka) {
    // An exactly-sized heap copy, so a read past the end is an ASan error.
    uint8_t *b = malloc(n ? n : 1);
    memcpy(b, text, n);
    int k = cb_https_parse_response(b, n, head, eof, r, ka);
    free(b);
    return k;
}

static void content_length(void) {
    cb_https_response r = { 0 };
    bool ka = false;
    T_EQ_INT(parse(DDB, sizeof DDB - 1, false, false, &r, &ka), (long long)sizeof DDB - 1);
    T_EQ_INT(r.status, 200);
    T_CHECK(ka);
    T_EQ_STR(cb_buf_cstr(&r.body), "{\"Item\":{\"pk\":{\"S\":\"GAME#a1b2c3\"},\"v\":{}}}");
    T_CHECK(strstr(cb_buf_cstr(&r.headers), "x-amz-crc32: 2745614147\r\n") != NULL);
    // Every shorter prefix is "not yet", even at EOF it is an error, never a short body.
    for (size_t n = 0; n < sizeof DDB - 1; n++) {
        T_EQ_INT(parse(DDB, n, false, false, &r, &ka), 0);
        T_EQ_INT(parse(DDB, n, false, true, &r, &ka), CB_E_IO);
    }
    // Pipelined bytes after the body are not consumed.
    char two[512];
    snprintf(two, sizeof two, "%sHTTP/1.1 200", DDB);
    T_EQ_INT(parse(two, strlen(two), false, false, &r, &ka), (long long)sizeof DDB - 1);
    cb_https_response_free(&r);
}

static void chunked_body(void) {
    cb_https_response r = { 0 };
    bool ka = false;
    T_EQ_INT(parse(CHUNKED, sizeof CHUNKED - 1, false, false, &r, &ka), (long long)sizeof CHUNKED - 1);
    T_EQ_INT(r.status, 200);
    T_EQ_STR(cb_buf_cstr(&r.body), "MozillaDeveloperNetwork");
    T_CHECK(ka);
    for (size_t n = 0; n < sizeof CHUNKED - 1; n++) T_EQ_INT(parse(CHUNKED, n, false, false, &r, &ka), 0);
    // Malformed: a size that is not hex, data not followed by CRLF.
    const char *bad1 = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\nab\r\n0\r\n\r\n";
    const char *bad2 = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nabX\r\n0\r\n\r\n";
    T_EQ_INT(parse(bad1, strlen(bad1), false, false, &r, &ka), CB_E_IO);
    T_EQ_INT(parse(bad2, strlen(bad2), false, false, &r, &ka), CB_E_IO);
    cb_https_response_free(&r);
}

static void special_statuses(void) {
    cb_https_response r = { 0 };
    bool ka = true;
    // 100 Continue is skipped; the real answer follows.
    const char *cont = "HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 202 Accepted\r\nContent-Length: 0\r\n\r\n";
    T_EQ_INT(parse(cont, strlen(cont), false, false, &r, &ka), (long long)strlen(cont));
    T_EQ_INT(r.status, 202);
    // 204 and a HEAD answer have no body whatever Content-Length says.
    const char *nc = "HTTP/1.1 204 No Content\r\nContent-Length: 10\r\n\r\n";
    T_EQ_INT(parse(nc, strlen(nc), false, false, &r, &ka), (long long)strlen(nc));
    T_EQ_INT(r.body.len, 0);
    const char *hd = "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\n";
    T_EQ_INT(parse(hd, strlen(hd), true, false, &r, &ka), (long long)strlen(hd));
    // Neither length nor chunks: the body runs to the close, and the connection is done.
    const char *eof = "HTTP/1.0 410 Gone\r\n\r\n{\"message\":\"Gone\"}";
    T_EQ_INT(parse(eof, strlen(eof), false, false, &r, &ka), 0);
    T_EQ_INT(parse(eof, strlen(eof), false, true, &r, &ka), (long long)strlen(eof));
    T_EQ_INT(r.status, 410);
    T_EQ_STR(cb_buf_cstr(&r.body), "{\"message\":\"Gone\"}");
    T_CHECK(!ka);
    // Connection: close on 1.1.
    const char *cl = "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: 2\r\n\r\nok";
    T_EQ_INT(parse(cl, strlen(cl), false, false, &r, &ka), (long long)strlen(cl));
    T_CHECK(!ka);
    cb_https_response_free(&r);
}

static void malformed(void) {
    cb_https_response r = { 0 };
    bool ka;
    const char *cases[] = {
        "HTTP/2 200 OK\r\n\r\n",
        "HTTP/1.1 2x0 OK\r\nContent-Length: 0\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: 1a\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: 99999999999\r\n\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip\r\n\r\n",
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++)
        T_EQ_INT(parse(cases[i], strlen(cases[i]), false, false, &r, &ka), CB_E_IO);
    cb_https_response_free(&r);
}

// ---- against a local TLS server ------------------------------------------------

static tls_action answer(const cb_buf *req, cb_buf *resp, void *ud) {
    const char *mode = ud;
    const char *q = (const char *)req->p;
    if (strcmp(mode, "hang") == 0) return TLS_HANG;
    if (strncmp(q, "GET /chunked ", 13) == 0) {
        cb_buf_puts(resp, CHUNKED);
        return TLS_KEEP;
    }
    // Echo the request line and the body.
    const char *nl = strstr(q, "\r\n");
    const char *body = strstr(q, "\r\n\r\n") + 4;
    cb_buf_printf(resp, "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\n\r\n%.*s|%s",
                  (size_t)(nl - q) + 1 + strlen(body), (int)(nl - q), q, body);
    if (strcmp(mode, "close") == 0) return TLS_CLOSE;   // a clean TLS close the response never announced
    return strcmp(mode, "drop") == 0 ? TLS_DROP : TLS_KEEP;
}

static void verified_and_kept_alive(void) {
    tls_srv s;
    T_EQ_INT(tls_srv_start(&s, answer, "keep"), 0);
    cb_https_response r;

    // The system CAs do not know this certificate.
    T_EQ_INT(cb_https_set_ca_file(NULL), CB_OK);
    T_EQ_INT(cb_https_request("localhost", s.port, "GET", "/x", NULL, NULL, 0, 3000, &r), CB_E_IO);
    cb_https_response_free(&r);

    T_EQ_INT(cb_https_set_ca_file(s.ca_file), CB_OK);
    // Trusted, but the name does not match: 127.0.0.1 is not "localhost".
    T_EQ_INT(cb_https_request("127.0.0.1", s.port, "GET", "/x", NULL, NULL, 0, 3000, &r), CB_E_IO);
    cb_https_response_free(&r);
    int before = atomic_load(&s.accepts);
    cb_https_tally tally;
    cb_https_tally_take(&tally);   // from here on

    T_EQ_INT(cb_https_request("localhost", s.port, "POST", "/echo", "X-Amz-Target: T\r\n", "hello", 5, 3000, &r), CB_OK);
    T_EQ_INT(r.status, 200);
    T_EQ_STR(cb_buf_cstr(&r.body), "POST /echo HTTP/1.1|hello");
    char *last = tls_srv_last(&s);
    char host[64];
    snprintf(host, sizeof host, "\r\nHost: localhost:%d\r\n", s.port);
    T_CHECK(strstr(last, host) != NULL);
    T_CHECK(strstr(last, "\r\nX-Amz-Target: T\r\nContent-Length: 5\r\n\r\nhello") != NULL);
    free(last);
    cb_https_response_free(&r);

    T_EQ_INT(cb_https_request("localhost", s.port, "GET", "/chunked", NULL, NULL, 0, 3000, &r), CB_OK);
    T_EQ_STR(cb_buf_cstr(&r.body), "MozillaDeveloperNetwork");
    cb_https_response_free(&r);
    // Both requests rode one TLS connection.
    T_EQ_INT(atomic_load(&s.accepts) - before, 1);
    T_EQ_INT(atomic_load(&s.requests), 2);
    // The timing line's tally saw them: two calls on one new connection, to a
    // host that is neither DynamoDB nor API Gateway, and some time.
    cb_https_tally_take(&tally);
    T_EQ_INT(tally.other.calls, 2);
    T_EQ_INT(tally.other.dials, 1);
    T_CHECK(tally.other.us > 0);
    T_EQ_INT(tally.ddb.calls + tally.push.calls, 0);
    tls_srv_stop(&s);
}

// A pooled connection the server dropped while idle is replaced, once, unseen:
// whether the server reset it ("drop") or closed it cleanly ("close").
static void dead_idle_connection_retried_as(const char *mode) {
    tls_srv s;
    T_EQ_INT(tls_srv_start(&s, answer, (void *)mode), 0);
    T_EQ_INT(cb_https_set_ca_file(s.ca_file), CB_OK);
    cb_https_response r;
    T_EQ_INT(cb_https_request("localhost", s.port, "GET", "/a", NULL, NULL, 0, 3000, &r), CB_OK);
    cb_https_response_free(&r);
    cb_sleep_ms(100);
    T_EQ_INT(cb_https_request("localhost", s.port, "GET", "/b", NULL, NULL, 0, 3000, &r), CB_OK);
    T_EQ_INT(r.status, 200);
    T_EQ_STR(cb_buf_cstr(&r.body), "GET /b HTTP/1.1|");
    cb_https_response_free(&r);
    T_EQ_INT(atomic_load(&s.accepts), 2);
    tls_srv_stop(&s);
}

static void dead_idle_connection_retried(void) {
    dead_idle_connection_retried_as("drop");
    dead_idle_connection_retried_as("close");
}

static void times_out(void) {
    tls_srv s;
    T_EQ_INT(tls_srv_start(&s, answer, "hang"), 0);
    T_EQ_INT(cb_https_set_ca_file(s.ca_file), CB_OK);
    cb_https_response r;
    int64_t t0 = cb_mono_ms();
    T_EQ_INT(cb_https_request("localhost", s.port, "GET", "/", NULL, NULL, 0, 300, &r), CB_E_IO);
    int64_t took = cb_mono_ms() - t0;
    T_CHECK(took >= 250 && took < 2000);
    cb_https_response_free(&r);
    tls_srv_stop(&s);
}

// ---- the real internet, when there is one ---------------------------------------

static bool online(void) {
    struct addrinfo hints = { 0 }, *ai = NULL;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo("example.com", "443", &hints, &ai) != 0) return false;
    freeaddrinfo(ai);
    return true;
}

static void example_com(void) {
    if (!online()) { fprintf(stderr, "SKIP unit_https example_com: offline\n"); return; }
    T_EQ_INT(cb_https_set_ca_file(NULL), CB_OK);
    cb_https_response r;
    T_EQ_INT(cb_https_request("example.com", 443, "GET", "/", "User-Agent: cbase-test\r\n", NULL, 0, 10000, &r), CB_OK);
    T_EQ_INT(r.status, 200);
    T_CHECK(strstr(cb_buf_cstr(&r.body), "Example Domain") != NULL);
    cb_https_response_free(&r);
}

int main(void) {
    T_RUN(content_length);
    T_RUN(chunked_body);
    T_RUN(special_statuses);
    T_RUN(malformed);
    T_RUN(verified_and_kept_alive);
    T_RUN(dead_idle_connection_retried);
    T_RUN(times_out);
    T_RUN(example_com);
    return T_DONE();
}
