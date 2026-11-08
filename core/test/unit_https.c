// unit_https.c - the HTTPS client: its response parser on recorded responses,
// TLS verification and keep-alive against a local TLS server, and one real
// request to https://example.com when this machine is online.
#include "t.h"
#include <pthread.h>

#include "net/https.h"
#include "lib_tls.h"
#include "util/clock.h"
#include "util/err.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
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
    if (strcmp(mode, "hang") == 0 || strncmp(q, "GET /hang ", 10) == 0) return TLS_HANG;
    if (strncmp(q, "GET /short ", 11) == 0) {
        // A body shorter than its length says, 200 ms late: the rest never comes.
        cb_buf_puts(resp, "HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nfive!");
        return TLS_LATE;
    }
    if (strncmp(q, "GET /slow ", 10) == 0) {
        cb_buf_puts(resp, "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\nslow");
        return TLS_LATE;   // 200 ms, then kept alive
    }
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

    int64_t sets = cb_https_timeout_sets();
    T_EQ_INT(cb_https_request("localhost", s.port, "GET", "/chunked", NULL, NULL, 0, 3000, &r), CB_OK);
    T_EQ_STR(cb_buf_cstr(&r.body), "MozillaDeveloperNetwork");
    cb_https_response_free(&r);
    // The pooled connection kept the timeouts its first request set: the
    // same 3 s, so no setsockopt at all.
    T_EQ_INT(cb_https_timeout_sets() - sets, 0);
    // Both requests rode one TLS connection.
    T_EQ_INT(atomic_load(&s.accepts) - before, 1);
    T_EQ_INT(atomic_load(&s.requests), 2);
    // The timing line's tally saw them: two calls on one new connection, to a
    // host no provider named a role, and some time.
    cb_https_tally_take(&tally);
    T_EQ_INT(tally.other.calls, 2);
    T_EQ_INT(tally.other.dials, 1);
    T_CHECK(tally.other.us > 0);
    T_EQ_INT(tally.store.calls + tally.push.calls, 0);
    // The roles a provider names (cb_https_tally_roles): a prefix makes a host the store's, a part
    // the pushes'.
    cb_https_tally_roles("local", NULL);
    T_EQ_INT(cb_https_request("localhost", s.port, "GET", "/chunked", NULL, NULL, 0, 3000, &r), CB_OK);
    cb_https_response_free(&r);
    cb_https_tally_roles("nothing.", "calho");
    T_EQ_INT(cb_https_request("localhost", s.port, "GET", "/chunked", NULL, NULL, 0, 3000, &r), CB_OK);
    cb_https_response_free(&r);
    cb_https_tally_roles("ocalhost", "x.");   // a prefix is a prefix, not a part
    T_EQ_INT(cb_https_request("localhost", s.port, "GET", "/chunked", NULL, NULL, 0, 3000, &r), CB_OK);
    cb_https_response_free(&r);
    cb_https_tally_roles(NULL, NULL);
    cb_https_tally_take(&tally);
    T_EQ_INT(tally.store.calls, 1);
    T_EQ_INT(tally.push.calls, 1);
    T_EQ_INT(tally.other.calls, 1);
    tls_srv_stop(&s);
}

// Requests from several threads at once each take a connection of their own
// (rt_iot.c's lanes): they overlap, and the pool keeps every one, so the next
// round of the same width dials nothing.
#define PAR 6
typedef struct { int port, rc, status; } par_arg;
static void *par_get(void *ud) {
    par_arg *a = ud;
    cb_https_response r;
    a->rc = cb_https_request("localhost", a->port, "GET", "/slow", NULL, NULL, 0, 3000, &r);
    a->status = r.status;
    cb_https_response_free(&r);
    return NULL;
}
static int64_t par_round(int port, par_arg *a) {
    pthread_t th[PAR];
    int64_t t0 = cb_mono_ms();
    for (int i = 0; i < PAR; i++) {
        a[i] = (par_arg){ port, -1, 0 };
        pthread_create(&th[i], NULL, par_get, &a[i]);
    }
    for (int i = 0; i < PAR; i++) pthread_join(th[i], NULL);
    return cb_mono_ms() - t0;
}
static void concurrent_requests_pool_their_connections(void) {
    tls_srv s;
    T_EQ_INT(tls_srv_start_concurrent(&s, answer, "keep"), 0);
    T_EQ_INT(cb_https_set_ca_file(s.ca_file), CB_OK);
    cb_https_tally tally;
    cb_https_tally_take(&tally);
    par_arg a[PAR];
    int64_t took = par_round(s.port, a);
    for (int i = 0; i < PAR; i++) T_CHECK(a[i].rc == CB_OK && a[i].status == 200);
    T_CHECK(took < 3 * 200);   // six 200 ms answers at once, not 1.2 s in a row
    T_EQ_INT(atomic_load(&s.open_max), PAR);
    cb_https_tally_take(&tally);
    T_EQ_INT(tally.other.calls, PAR);
    T_EQ_INT(tally.other.dials, PAR);
    took = par_round(s.port, a);
    for (int i = 0; i < PAR; i++) T_CHECK(a[i].rc == CB_OK && a[i].status == 200);
    T_CHECK(took < 3 * 200);
    cb_https_tally_take(&tally);
    T_EQ_INT(tally.other.calls, PAR);
    T_EQ_INT(tally.other.dials, 0);   // every connection came from the pool
    T_EQ_INT(atomic_load(&s.accepts), PAR);
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

// A request with a different timeout on a pooled connection sets it once, and
// one with the first timeout again sets it back once.
static void timeouts_follow_the_deadline(void) {
    tls_srv s;
    T_EQ_INT(tls_srv_start(&s, answer, "keep"), 0);
    T_EQ_INT(cb_https_set_ca_file(s.ca_file), CB_OK);
    cb_https_response r;
    int before = atomic_load(&s.accepts);
    int64_t sets = cb_https_timeout_sets();
    T_EQ_INT(cb_https_request("localhost", s.port, "GET", "/a", NULL, NULL, 0, 5000, &r), CB_OK);
    cb_https_response_free(&r);
    T_EQ_INT(cb_https_timeout_sets() - sets, 1);   // the new connection's
    sets = cb_https_timeout_sets();
    T_EQ_INT(cb_https_request("localhost", s.port, "GET", "/b", NULL, NULL, 0, 1000, &r), CB_OK);
    cb_https_response_free(&r);
    T_EQ_INT(cb_https_timeout_sets() - sets, 1);   // lowered to the shorter deadline
    sets = cb_https_timeout_sets();
    T_EQ_INT(cb_https_request("localhost", s.port, "GET", "/c", NULL, NULL, 0, 5000, &r), CB_OK);
    cb_https_response_free(&r);
    T_EQ_INT(cb_https_timeout_sets() - sets, 1);   // raised again
    sets = cb_https_timeout_sets();
    T_EQ_INT(cb_https_request("localhost", s.port, "GET", "/d", NULL, NULL, 0, 5050, &r), CB_OK);
    cb_https_response_free(&r);
    T_EQ_INT(cb_https_timeout_sets() - sets, 0);   // within the slack: kept
    T_EQ_INT(atomic_load(&s.accepts) - before, 1);
    tls_srv_stop(&s);
}

// A server that answers nothing: the request ends at its deadline, give or
// take the slack, on a new connection and on a pooled one whose timeouts were
// set for a longer request.
static void times_out_on_a_pooled_connection(void) {
    tls_srv s;
    T_EQ_INT(tls_srv_start(&s, answer, "keep"), 0);
    T_EQ_INT(cb_https_set_ca_file(s.ca_file), CB_OK);
    cb_https_response r;
    T_EQ_INT(cb_https_request("localhost", s.port, "GET", "/a", NULL, NULL, 0, 5000, &r), CB_OK);
    cb_https_response_free(&r);
    int64_t t0 = cb_mono_ms();
    T_EQ_INT(cb_https_request("localhost", s.port, "GET", "/hang", NULL, NULL, 0, 300, &r), CB_E_IO);
    int64_t took = cb_mono_ms() - t0;
    T_CHECK(took >= 190 && took < 600);
    cb_https_response_free(&r);
    // Part of an answer after 200 ms, then nothing: the read after it is
    // bounded by the 100 ms left, not by the 300 the request started with.
    t0 = cb_mono_ms();
    T_EQ_INT(cb_https_request("localhost", s.port, "GET", "/short", NULL, NULL, 0, 300, &r), CB_E_IO);
    took = cb_mono_ms() - t0;
    T_CHECK(took >= 190 && took < 440);
    cb_https_response_free(&r);
    tls_srv_stop(&s);
}

// A listener that completes TCP's handshake (the kernel's backlog) and never
// TLS's: the handshake is bounded by the deadline too.
static void handshake_times_out(void) {
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    socklen_t al = sizeof a;
    T_EQ_INT(bind(lfd, (struct sockaddr *)&a, sizeof a), 0);
    T_EQ_INT(listen(lfd, 4), 0);
    T_EQ_INT(getsockname(lfd, (struct sockaddr *)&a, &al), 0);
    cb_https_response r;
    int64_t t0 = cb_mono_ms();
    T_EQ_INT(cb_https_request("localhost", ntohs(a.sin_port), "GET", "/", NULL, NULL, 0, 300, &r), CB_E_IO);
    int64_t took = cb_mono_ms() - t0;
    T_CHECK(took >= 190 && took < 2000);
    T_EQ_STR(cb_https_fail_name(r.fail), "timeout");
    T_CHECK(!r.sent);
    cb_https_response_free(&r);
    close(lfd);
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

// With no file given, OpenSSL's own bundle ($SSL_CERT_FILE here) is the one used, ahead of the
// fixed list of system files (tls.h): on a Mac that list's /etc/ssl/cert.pem is Apple's lagging
// copy, which could not verify example.com on CI's runner while Homebrew's bundle could.
static void openssl_bundle_before_the_system_list(void) {
    tls_srv s;
    T_EQ_INT(tls_srv_start(&s, answer, "keep"), 0);
    const char *had = getenv("CBASE_CA_FILE");
    char *saved = had ? strdup(had) : NULL;
    unsetenv("CBASE_CA_FILE");
    setenv("SSL_CERT_FILE", s.ca_file, 1);
    T_EQ_INT(cb_https_set_ca_file(NULL), CB_OK);
    cb_https_response r;
    T_EQ_INT(cb_https_request("localhost", s.port, "GET", "/x", NULL, NULL, 0, 3000, &r), CB_OK);
    T_EQ_INT(r.status, 200);
    cb_https_response_free(&r);
    unsetenv("SSL_CERT_FILE");
    if (saved) { setenv("CBASE_CA_FILE", saved, 1); free(saved); }
    T_EQ_INT(cb_https_set_ca_file(NULL), CB_OK);
    tls_srv_stop(&s);
}

// ---- plain HTTP, for a product's loopback peer ----------------------------------------------------
//
// cb_http_plain_request against lib_tls's plain mode, as a product's bridge client uses it against a
// companion process on 127.0.0.1: its own header lines, binary bodies both ways, a short and a long
// timeout, one kept-alive connection, and a peer that closes after its answer.

#define BRIDGE_HEADERS "X-Amgi-Bridge-Token: tok-1\r\nX-Amgi-Bridge-Protocol: 3\r\nContent-Type: application/x-amgi-wire\r\n"

static tls_action bridge(const cb_buf *req, cb_buf *resp, void *ud) {
    const char *mode = ud;
    const char *q = (const char *)req->p;
    if (strncmp(q, "POST /hang ", 11) == 0) return TLS_HANG;
    if (strncmp(q, "POST /slow ", 11) == 0) cb_sleep_ms(1700);   // past 1.5 s, well inside 30
    if (strncmp(q, "POST /bye ", 10) == 0) {
        cb_buf_puts(resp, "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: 3\r\n\r\nbye");
        return TLS_CLOSE;
    }
    // The request's body back behind two bytes of its own, NUL and 0xff.
    const char *he = strstr(q, "\r\n\r\n");
    const size_t at = (size_t)(he + 4 - q), n = req->len - at;
    cb_buf_printf(resp, "HTTP/1.1 200 OK\r\nContent-Type: application/x-amgi-wire\r\nContent-Length: %zu\r\n\r\n", n + 2);
    cb_buf_append(resp, "\0\xff", 2);
    cb_buf_append(resp, req->p + at, n);
    return strcmp(mode, "close") == 0 ? TLS_CLOSE : TLS_KEEP;   // "close": shut after every answer, unannounced
}

static int bridge_post(int port, const char *path, const void *body, size_t n, int timeout_ms, cb_https_response *r) {
    return cb_http_plain_request(CB_HTTPS_OTHER, "127.0.0.1", port, "POST", path, BRIDGE_HEADERS, body, n, timeout_ms, r);
}

static void plain_binary_both_ways_with_its_headers(void) {
    tls_srv s;
    T_EQ_INT(tls_srv_start_plain(&s, bridge, "keep"), 0);
    uint8_t body[300];
    for (size_t i = 0; i < sizeof body; i++) body[i] = (uint8_t)(i * 37);
    body[0] = 0, body[1] = '\r', body[2] = '\n', body[3] = '\r', body[4] = '\n';
    cb_https_tally tally;
    cb_https_tally_take(&tally);   // from here on
    cb_https_response r;
    for (int k = 0; k < 2; k++) {
        T_EQ_INT(bridge_post(s.port, "/echo", body, sizeof body, 1500, &r), CB_OK);
        T_EQ_INT(r.status, 200);
        T_EQ_INT(r.body.len, sizeof body + 2);
        T_CHECK(r.body.len == sizeof body + 2 && memcmp(r.body.p, "\0\xff", 2) == 0 &&
                memcmp(r.body.p + 2, body, sizeof body) == 0);
        T_CHECK(strstr(cb_buf_cstr(&r.headers), "\r\nContent-Type: application/x-amgi-wire\r\n") != NULL);
        cb_https_response_free(&r);
        // The request as it went: the caller's lines as given, then the length, then the bytes.
        cb_buf want;
        cb_buf_init(&want);
        cb_buf_printf(&want, "POST /echo HTTP/1.1\r\nHost: 127.0.0.1:%d\r\n" BRIDGE_HEADERS "Content-Length: %zu\r\n\r\n",
                      s.port, sizeof body);
        cb_buf_append(&want, body, sizeof body);
        pthread_mutex_lock(&s.lock);
        T_EQ_MEM(s.last.p, s.last.len, want.p, want.len);
        pthread_mutex_unlock(&s.lock);
        cb_buf_free(&want);
    }
    T_EQ_INT(atomic_load(&s.accepts), 1);   // the second rode the first's connection
    // A product's call is the tally's other row, never the store's.
    cb_https_tally_take(&tally);
    T_EQ_INT(tally.other.calls, 2);
    T_EQ_INT(tally.other.dials, 1);
    T_EQ_INT(tally.store.calls + tally.push.calls, 0);
    // A stand-in for a store names its row.
    T_EQ_INT(cb_http_plain_request(CB_HTTPS_STORE, "127.0.0.1", s.port, "POST", "/echo", NULL, "x", 1, 1500, &r), CB_OK);
    cb_https_response_free(&r);
    cb_https_tally_take(&tally);
    T_EQ_INT(tally.store.calls, 1);
    T_EQ_INT(tally.other.calls + tally.push.calls, 0);
    tls_srv_stop(&s);
}

// The bridge's two timeouts: a ping's 1.5 s ends at 1.5 s against a peer that answers nothing; a
// sync's 30 s waits out a peer that takes 1.7 s.
static void plain_timeouts_short_and_long(void) {
    tls_srv s;
    T_EQ_INT(tls_srv_start_plain(&s, bridge, "keep"), 0);
    cb_https_response r;
    int64_t t0 = cb_mono_ms();
    T_EQ_INT(bridge_post(s.port, "/hang", "p", 1, 1500, &r), CB_E_IO);
    int64_t took = cb_mono_ms() - t0;
    T_CHECK(took >= 1400 && took < 2100);
    cb_https_response_free(&r);
    t0 = cb_mono_ms();
    T_EQ_INT(bridge_post(s.port, "/slow", "\0s", 2, 30000, &r), CB_OK);
    took = cb_mono_ms() - t0;
    T_CHECK(took >= 1650 && took < 10000);
    T_EQ_INT(r.status, 200);
    T_EQ_MEM(r.body.p, r.body.len, "\0\xff\0s", 4);
    cb_https_response_free(&r);
    tls_srv_stop(&s);
}

// A peer that closes after its answer, unannounced or with Connection: close: every request still
// answers, each on a connection of its own.
static void plain_peer_that_closes(void) {
    tls_srv s;
    T_EQ_INT(tls_srv_start_plain(&s, bridge, "close"), 0);
    cb_https_response r;
    for (int k = 0; k < 3; k++) {
        T_EQ_INT(bridge_post(s.port, "/echo", "\0", 1, 1500, &r), CB_OK);
        T_EQ_MEM(r.body.p, r.body.len, "\0\xff\0", 3);
        cb_https_response_free(&r);
        cb_sleep_ms(50);   // the close lands while the connection is idle in the pool
    }
    T_EQ_INT(atomic_load(&s.accepts), 3);
    T_EQ_INT(atomic_load(&s.requests), 3);   // none sent twice
    for (int k = 0; k < 2; k++) {
        T_EQ_INT(bridge_post(s.port, "/bye", NULL, 0, 1500, &r), CB_OK);
        T_EQ_STR(cb_buf_cstr(&r.body), "bye");
        cb_https_response_free(&r);
    }
    T_EQ_INT(atomic_load(&s.accepts), 5);
    T_EQ_INT(atomic_load(&s.requests), 5);   // not pooled, so never a write into a closed one
    tls_srv_stop(&s);
}

// ---- what is sent twice (https.h) ---------------------------------------------------------------
//
// A peer that answers the first request on a kept-alive connection, then reads the second and
// closes without a word (it may have acted on it): a safe method is sent again on a fresh
// connection, a write never is.

typedef struct {
    atomic_int  n;
    tls_action  second;   // what the second request gets
} once_peer;

static tls_action answer_then(const cb_buf *req, cb_buf *resp, void *ud) {
    once_peer *p = ud;
    if (atomic_fetch_add(&p->n, 1) + 1 == 2) return p->second;
    const char *q = (const char *)req->p;
    const char *sp = strchr(q, ' ');
    cb_buf_printf(resp, "HTTP/1.1 200 OK\r\nContent-Length: %d\r\n\r\n%.*s", (int)(sp - q), (int)(sp - q), q);
    return TLS_KEEP;
}

static int send_on(bool tls, unsigned flags, int port, const char *method, const char *path, cb_https_response *r) {
    const char *body = strcmp(method, "GET") == 0 ? NULL : "note";
    size_t n = body ? 4 : 0;
    return tls ? cb_https_request_flags(flags, "localhost", port, method, path, NULL, body, n, 3000, r)
               : cb_http_plain_request_flags(flags, CB_HTTPS_OTHER, "127.0.0.1", port, method, path, NULL, body, n,
                                             3000, r);
}

// The second request's outcome on a fresh peer: how many requests the peer read in all.
static int second_request(bool tls, tls_action second, unsigned flags, const char *method, int *rc,
                          cb_https_response *r) {
    once_peer p = { .second = second };
    atomic_init(&p.n, 0);
    tls_srv s;
    T_EQ_INT((tls ? tls_srv_start : tls_srv_start_plain)(&s, answer_then, &p), 0);
    if (tls) T_EQ_INT(cb_https_set_ca_file(s.ca_file), CB_OK);
    cb_https_response first;
    T_EQ_INT(send_on(tls, 0, s.port, "GET", "/first", &first), CB_OK);
    cb_https_response_free(&first);
    *rc = send_on(tls, flags, s.port, method, "/second", r);
    int seen = atomic_load(&s.requests);
    tls_srv_stop(&s);
    return seen;
}

static void writes_are_never_resent_on(bool tls) {
    static const char *const writes[] = { "POST", "PATCH", "PUT", "DELETE" };
    for (size_t i = 0; i < sizeof writes / sizeof writes[0]; i++) {
        int rc;
        cb_https_response r;
        T_EQ_INT(second_request(tls, TLS_SWALLOW, 0, writes[i], &rc, &r), 2);   // the write reached it once
        T_EQ_INT(rc, CB_E_IO);
        T_EQ_STR(cb_https_fail_name(r.fail), "reset");
        T_CHECK(r.sent);
        T_CHECK(strstr(r.why, "may or may not have been applied") != NULL);
        T_CHECK(strstr(r.why, "not resent") != NULL);
        cb_https_response_free(&r);
    }
    // A safe method is sent again, once, and answered there.
    static const char *const safe[] = { "GET", "HEAD", "OPTIONS" };
    for (size_t i = 0; i < sizeof safe / sizeof safe[0]; i++) {
        int rc;
        cb_https_response r;
        T_EQ_INT(second_request(tls, TLS_SWALLOW, 0, safe[i], &rc, &r), 3);
        T_EQ_INT(rc, CB_OK);
        T_EQ_INT(r.status, 200);
        T_EQ_STR(r.why, "");
        cb_https_response_free(&r);
    }
    // So is a write its caller vouches for, and a reset is the same as a close.
    int rc;
    cb_https_response r;
    T_EQ_INT(second_request(tls, TLS_RESET, CB_HTTP_RETRY_IDEMPOTENT, "POST", &rc, &r), 3);
    T_EQ_INT(rc, CB_OK);
    T_EQ_STR(cb_buf_cstr(&r.body), "POST");
    cb_https_response_free(&r);
    T_EQ_INT(second_request(tls, TLS_RESET, 0, "POST", &rc, &r), 2);
    T_EQ_INT(rc, CB_E_IO);
    cb_https_response_free(&r);
}

static void writes_are_never_resent(void) {
    writes_are_never_resent_on(false);
    writes_are_never_resent_on(true);
}

// A connection the peer closed while it sat idle is dropped before the request goes out, so a write
// on it is not lost to a close that had already happened.
static void idle_close_is_seen_before_a_write(void) {
    for (int tls = 0; tls < 2; tls++) {
        tls_srv s;
        T_EQ_INT((tls ? tls_srv_start : tls_srv_start_plain)(&s, answer, "close"), 0);
        if (tls) T_EQ_INT(cb_https_set_ca_file(s.ca_file), CB_OK);
        cb_https_response r;
        T_EQ_INT(send_on(tls, 0, s.port, "GET", "/a", &r), CB_OK);
        cb_https_response_free(&r);
        cb_sleep_ms(100);   // the close lands while the connection is idle in the pool
        T_EQ_INT(send_on(tls, 0, s.port, "POST", "/w", &r), CB_OK);
        T_EQ_STR(cb_buf_cstr(&r.body), "POST /w HTTP/1.1|note");
        cb_https_response_free(&r);
        T_EQ_INT(atomic_load(&s.requests), 2);
        T_EQ_INT(atomic_load(&s.accepts), 2);
        tls_srv_stop(&s);
    }
}

// What failed, told apart: a peer that never answers, a port nobody listens on, a peer that resets.
static void failures_say_which(void) {
    for (int tls = 0; tls < 2; tls++) {
        tls_srv s;
        T_EQ_INT((tls ? tls_srv_start : tls_srv_start_plain)(&s, answer, "hang"), 0);
        if (tls) T_EQ_INT(cb_https_set_ca_file(s.ca_file), CB_OK);
        cb_https_response r;
        T_EQ_INT(tls ? cb_https_request("localhost", s.port, "POST", "/", NULL, "x", 1, 300, &r)
                     : cb_http_plain_request(CB_HTTPS_OTHER, "127.0.0.1", s.port, "POST", "/", NULL, "x", 1, 300, &r),
                 CB_E_IO);
        T_EQ_STR(cb_https_fail_name(r.fail), "timeout");
        T_CHECK(r.sent);
        T_CHECK(strstr(r.why, "did not answer within 300 ms: the request may or may not have been applied") != NULL);
        cb_https_response_free(&r);
        tls_srv_stop(&s);

        // Reset after reading the request, on a fresh connection.
        once_peer p = { .second = TLS_RESET };
        atomic_init(&p.n, 1);   // the first request it sees is the "second"
        T_EQ_INT((tls ? tls_srv_start : tls_srv_start_plain)(&s, answer_then, &p), 0);
        if (tls) T_EQ_INT(cb_https_set_ca_file(s.ca_file), CB_OK);
        T_EQ_INT(send_on(tls, 0, s.port, "GET", "/", &r), CB_E_IO);
        T_EQ_STR(cb_https_fail_name(r.fail), "reset");
        T_CHECK(r.sent);
        cb_https_response_free(&r);
        T_EQ_INT(atomic_load(&s.requests), 1);   // a fresh connection's failure is not resent, even a GET's
        tls_srv_stop(&s);
    }
    // A port nobody listens on: refused, nothing sent.
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    socklen_t al = sizeof a;
    T_EQ_INT(bind(fd, (struct sockaddr *)&a, sizeof a), 0);
    T_EQ_INT(getsockname(fd, (struct sockaddr *)&a, &al), 0);
    close(fd);
    for (int tls = 0; tls < 2; tls++) {
        cb_https_response r;
        T_EQ_INT(send_on(tls, 0, ntohs(a.sin_port), "POST", "/", &r), CB_E_IO);
        T_EQ_STR(cb_https_fail_name(r.fail), "connect");
        T_CHECK(!r.sent);
        T_CHECK(strstr(r.why, "nothing was sent") != NULL);
        cb_https_response_free(&r);
    }
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
    T_RUN(concurrent_requests_pool_their_connections);
    T_RUN(dead_idle_connection_retried);
    T_RUN(times_out);
    T_RUN(timeouts_follow_the_deadline);
    T_RUN(times_out_on_a_pooled_connection);
    T_RUN(handshake_times_out);
    T_RUN(openssl_bundle_before_the_system_list);
    T_RUN(plain_binary_both_ways_with_its_headers);
    T_RUN(plain_timeouts_short_and_long);
    T_RUN(plain_peer_that_closes);
    T_RUN(writes_are_never_resent);
    T_RUN(idle_close_is_seen_before_a_write);
    T_RUN(failures_say_which);
    T_RUN(example_com);
    return T_DONE();
}
