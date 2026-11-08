// unit_grpc.c - gRPC unary calls (net/grpc.h) over real sockets against
// lib_h2_srv's /test.Echo/* routes: an echo in cleartext and over TLS, the
// metadata as the server saw it (the API key never-indexed, grpc-timeout from
// the deadline), a trailers-only error with its message percent-decoded and
// its status details base64-decoded, an answer that is not gRPC, the malformed
// replies a unary call refuses, an empty request, and twelve calls at once on
// one TLS connection. Then the pieces on their own: the grpc-timeout units,
// the message framing, and grpc-message's percent-decoding.
#include "t.h"

#include "lib_h2_srv.h"
#include "net/grpc.h"
#include "util/err.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static cb_h2_channel *channel(h2srv *s) {
    char origin[128];
    cb_h2_opts opts;
    memset(&opts, 0, sizeof opts);
    if (h2srv_ca_file(s)[0]) {
        snprintf(origin, sizeof origin, "https://localhost:%d", h2srv_port(s));
        opts.ca_file = h2srv_ca_file(s);
    } else {
        snprintf(origin, sizeof origin, "http://127.0.0.1:%d", h2srv_port(s));
    }
    cb_h2_channel *ch = NULL;
    T_EQ_INT(cb_h2_channel_new(origin, &opts, &ch), CB_OK);
    return ch;
}

static int call(cb_h2_channel *ch, const char *path, const void *req, size_t n, cb_grpc_reply *r) {
    return cb_grpc_unary(ch, path, NULL, 0, req, n, 5000, 0, r);
}

static void echo(void) {
    cb_grpc_reply fresh;
    cb_grpc_reply_init(&fresh);
    T_EQ_INT(fresh.code, -1);   // no answer yet
    T_CHECK(!fresh.has_msg);
    for (int tls = 0; tls < 2; tls++) {
        h2srv_opts so = { .tls = tls };
        h2srv *s = h2srv_start(&so);
        T_CHECK(s != NULL);
        cb_h2_channel *ch = channel(s);
        cb_grpc_reply r;
        cb_grpc_reply_init(&r);
        T_EQ_INT(call(ch, "/test.Echo/Echo", "\x0a\x03hey", 5, &r), CB_OK);
        T_EQ_INT(r.code, CB_GRPC_OK);
        T_EQ_INT(r.http_status, 200);
        T_CHECK(r.has_msg);
        T_EQ_MEM(r.msg, r.msg_len, "\x0a\x03hey", 5);
        T_EQ_INT((int)r.message.len, 0);
        T_EQ_STR((const char *)r.message.p, "");
        T_EQ_INT((int)r.details.len, 0);
        cb_span v;
        T_CHECK(cb_h2_response_get(&r.resp, false, "content-type", &v) && v.n == 16);
        // An empty request message: a prefix and nothing after it, and an empty reply.
        T_EQ_INT(call(ch, "/test.Echo/Echo", NULL, 0, &r), CB_OK);
        T_EQ_INT(r.code, CB_GRPC_OK);
        T_CHECK(r.has_msg);
        T_EQ_INT((int)r.msg_len, 0);
        // Long enough that every byte of the length prefix counts (the server checks it).
        static uint8_t big[0x01020304 & 0xfffff];
        for (size_t i = 0; i < sizeof big; i++) big[i] = (uint8_t)(i * 13);
        T_EQ_INT(cb_grpc_unary(ch, "/test.Echo/Echo", NULL, 0, big, sizeof big, 20000, 0, &r), CB_OK);
        T_EQ_INT(r.code, CB_GRPC_OK);
        T_EQ_MEM(r.msg, r.msg_len, big, sizeof big);
        T_EQ_INT(cb_h2_channel_dials(ch), 1);
        cb_grpc_reply_free(&r);
        cb_h2_channel_free(ch);
        h2srv_stop(s);
    }
}

// The key goes never-indexed, as gemini.ts sends it, and the deadline as grpc-timeout.
static void meta(void) {
    h2srv_opts so = { .tls = true };
    h2srv *s = h2srv_start(&so);
    cb_h2_channel *ch = channel(s);
    cb_grpc_reply r;
    cb_grpc_reply_init(&r);
    cb_h2_field key = { "x-goog-api-key", "k-123", 14, 5, true };
    T_EQ_INT(cb_grpc_unary(ch, "/test.Echo/Meta", &key, 1, "x", 1, 1500, 0, &r), CB_OK);
    T_EQ_INT(r.code, CB_GRPC_OK);
    T_EQ_MEM(r.msg, r.msg_len, "k-123|1500m|never", 17);
    // Indexable when the caller says so: the flag is the caller's, carried through.
    key.never = false;
    T_EQ_INT(cb_grpc_unary(ch, "/test.Echo/Meta", &key, 1, "x", 1, 2000, 0, &r), CB_OK);
    T_EQ_MEM(r.msg, r.msg_len, "k-123|2000m|indexed", 19);
    // No deadline given: the 10 s default goes to the server too.
    T_EQ_INT(cb_grpc_unary(ch, "/test.Echo/Meta", NULL, 0, "x", 1, 0, 0, &r), CB_OK);
    T_EQ_MEM(r.msg, r.msg_len, "|10000m|indexed", 15);
    // Metadata past the cap never leaves.
    cb_h2_field many[33];
    for (int i = 0; i < 33; i++) many[i] = (cb_h2_field){ "x-a", "b", 3, 1, false };
    T_EQ_INT(cb_grpc_unary(ch, "/test.Echo/Meta", many, 33, "x", 1, 1000, 0, &r), CB_E_INVALID);
    T_CHECK(strstr(r.resp.err, "metadata") != NULL);
    T_EQ_INT(cb_grpc_unary(ch, "/test.Echo/Meta", many, 32, "x", 1, 1000, 0, &r), CB_OK);
    T_EQ_INT(cb_grpc_unary(ch, "/test.Echo/Meta", many, -1, "x", 1, 1000, 0, &r), CB_E_INVALID);
    T_EQ_INT(h2srv_requests(s), 4);
    cb_grpc_reply_free(&r);
    cb_h2_channel_free(ch);
    h2srv_stop(s);
}

// Trailers-only RESOURCE_EXHAUSTED, the way Gemini says a quota is spent.
static void fail(void) {
    h2srv_opts so = { 0 };
    h2srv *s = h2srv_start(&so);
    cb_h2_channel *ch = channel(s);
    cb_grpc_reply r;
    cb_grpc_reply_init(&r);
    T_EQ_INT(call(ch, "/test.Echo/Fail", "x", 1, &r), CB_OK);
    T_EQ_INT(r.code, CB_GRPC_RESOURCE_EXHAUSTED);
    T_EQ_INT(r.http_status, 200);
    T_EQ_STR((const char *)r.message.p, "quota used up: try later %zz 100%");
    T_EQ_INT((int)r.message.len, 33);
    static const uint8_t details[] = { 0x08, 0x08, 0x12, 0x04, 'q', 'u', 'o', 't' };
    T_EQ_MEM(r.details.p, r.details.len, details, sizeof details);
    T_CHECK(!r.has_msg);
    T_CHECK(r.msg == NULL);
    T_EQ_INT(r.resp.n_trailers, 0);
    // A second call on the same reply: nothing of the first is left in it.
    T_EQ_INT(call(ch, "/test.Echo/Echo", "y", 1, &r), CB_OK);
    T_EQ_INT(r.code, 0);
    T_EQ_INT((int)r.message.len, 0);
    T_EQ_INT((int)r.details.len, 0);
    cb_grpc_reply_free(&r);
    cb_h2_channel_free(ch);
    h2srv_stop(s);
}

// Not gRPC: what a proxy or a load balancer answers. Code -1 and the HTTP status.
static void not_grpc(void) {
    h2srv_opts so = { 0 };
    h2srv *s = h2srv_start(&so);
    cb_h2_channel *ch = channel(s);
    cb_grpc_reply r;
    cb_grpc_reply_init(&r);
    T_EQ_INT(call(ch, "/status/503", "x", 1, &r), CB_OK);
    T_EQ_INT(r.code, -1);
    T_EQ_INT(r.http_status, 503);
    T_CHECK(!r.has_msg);
    T_EQ_INT(call(ch, "/nowhere", "x", 1, &r), CB_OK);
    T_EQ_INT(r.code, -1);
    T_EQ_INT(r.http_status, 404);
    // No answer at all: the transport's error, untouched.
    T_EQ_INT(call(ch, "/reset", "x", 1, &r), CB_E_IO);
    T_EQ_INT(r.resp.why, CB_H2_WHY_RESET);
    T_EQ_INT(r.code, -1);   // no answer: no code, not even OK
    // A reply over max_reply: the HTTP/2 cap, the message's prefix allowed for.
    T_EQ_INT(cb_grpc_unary(ch, "/test.Echo/Echo", NULL, 0, "abcdefgh", 8, 5000, 8, &r), CB_OK);
    T_EQ_MEM(r.msg, r.msg_len, "abcdefgh", 8);
    T_EQ_INT(cb_grpc_unary(ch, "/test.Echo/Echo", NULL, 0, "abcdefghi", 9, 5000, 8, &r), CB_E_IO);
    T_EQ_INT(r.resp.why, CB_H2_WHY_TOO_BIG);
    cb_grpc_reply_free(&r);
    cb_h2_channel_free(ch);
    h2srv_stop(s);
}

// Answers the server shapes from the path: the status's parse, an error with a
// message (which the caller is not shown), and gRPC fields on an HTTP error.
static void raw(void) {
    h2srv_opts so = { 0 };
    h2srv *s = h2srv_start(&so);
    cb_h2_channel *ch = channel(s);
    cb_grpc_reply r;
    cb_grpc_reply_init(&r);
    T_EQ_INT(call(ch, "/test.Echo/Raw/200/14/-/-", "x", 1, &r), CB_OK);
    T_EQ_INT(r.code, CB_GRPC_UNAVAILABLE);
    T_EQ_INT(call(ch, "/test.Echo/Raw/200/123/-/-", "x", 1, &r), CB_OK);
    T_EQ_INT(r.code, 123);
    static const char *const bad[] = { "/test.Echo/Raw/200//-/-", "/test.Echo/Raw/200/1234/-/-",
                                       "/test.Echo/Raw/200/8x/-/-", "/test.Echo/Raw/200/x8/-/-",
                                       "/test.Echo/Raw/200/-1/-/-", "/test.Echo/Raw/200/:/-/-" };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        T_EQ_INT(call(ch, bad[i], "x", 1, &r), CB_E_INVALID);
        T_CHECK(strstr(r.resp.err, "grpc-status") != NULL);
    }
    // Details that are not base64.
    T_EQ_INT(call(ch, "/test.Echo/Raw/200/8/!!!!/-", "x", 1, &r), CB_E_INVALID);
    T_CHECK(strstr(r.resp.err, "base64") != NULL);
    // Padded details decode the same as unpadded.
    T_EQ_INT(call(ch, "/test.Echo/Raw/200/8/AQI=/-", "x", 1, &r), CB_OK);
    T_EQ_MEM(r.details.p, r.details.len, "\x01\x02", 2);
    // An error that carries a message anyway: the caller is not handed it.
    T_EQ_INT(call(ch, "/test.Echo/Raw/200/9/-/hello", "x", 1, &r), CB_OK);
    T_EQ_INT(r.code, CB_GRPC_FAILED_PRECONDITION);
    T_CHECK(!r.has_msg && r.msg == NULL && r.msg_len == 0);
    // OK with a message in the body and the status in the trailers.
    T_EQ_INT(call(ch, "/test.Echo/Raw/200/0/-/hello", "x", 1, &r), CB_OK);
    T_EQ_MEM(r.msg, r.msg_len, "hello", 5);
    // OK with no message is no unary reply.
    T_EQ_INT(call(ch, "/test.Echo/Raw/200/0/-/-", "x", 1, &r), CB_E_INVALID);
    T_CHECK(strstr(r.resp.err, "without a message") != NULL);
    // A gRPC status on an HTTP error: the status counts, the body is not read as messages.
    T_EQ_INT(call(ch, "/test.Echo/Raw/503/14/-/!oops", "x", 1, &r), CB_OK);
    T_EQ_INT(r.code, CB_GRPC_UNAVAILABLE);
    T_EQ_INT(r.http_status, 503);
    T_CHECK(!r.has_msg);
    // A request message over 4 GB never leaves (the length is refused before the bytes are read).
    if (sizeof(size_t) > 4) {
        T_EQ_INT(cb_grpc_unary(ch, "/test.Echo/Echo", NULL, 0, "", (size_t)0x100000000ull, 1000, 0, &r), CB_E_INVALID);
        T_CHECK(strstr(r.resp.err, "4 GB") != NULL);
    }
    T_EQ_INT(h2srv_requests(s), 14);
    cb_grpc_reply_free(&r);
    cb_h2_channel_free(ch);
    h2srv_stop(s);
}

// gRPC, but malformed for a unary call.
static void malformed(void) {
    h2srv_opts so = { 0 };
    h2srv *s = h2srv_start(&so);
    cb_h2_channel *ch = channel(s);
    cb_grpc_reply r;
    cb_grpc_reply_init(&r);
    T_EQ_INT(call(ch, "/test.Echo/Two", "x", 1, &r), CB_E_INVALID);
    T_CHECK(strstr(r.resp.err, "framing") != NULL);
    T_CHECK(!r.has_msg);
    T_EQ_INT(call(ch, "/test.Echo/Zip", "x", 1, &r), CB_E_INVALID);
    T_CHECK(strstr(r.resp.err, "framing") != NULL);
    T_EQ_INT(call(ch, "/test.Echo/Echo", "x", 1, &r), CB_OK);   // the connection carries on
    T_EQ_INT(cb_h2_channel_dials(ch), 1);
    cb_grpc_reply_free(&r);
    cb_h2_channel_free(ch);
    h2srv_stop(s);
}

typedef struct {
    cb_h2_channel *ch;
    int            i, rc, code;
    char           got[32];
} worker;

static void *work(void *arg) {
    worker *w = arg;
    char req[32];
    int n = snprintf(req, sizeof req, "call %d", w->i);
    cb_grpc_reply r;
    cb_grpc_reply_init(&r);
    w->rc = cb_grpc_unary(w->ch, "/test.Echo/Echo", NULL, 0, req, (size_t)n, 20000, 0, &r);
    w->code = r.code;
    snprintf(w->got, sizeof w->got, "%.*s", (int)r.msg_len, r.msg ? (const char *)r.msg : "");
    cb_grpc_reply_free(&r);
    return NULL;
}

static void concurrent(void) {
    h2srv_opts so = { .tls = true };
    h2srv *s = h2srv_start(&so);
    cb_h2_channel *ch = channel(s);
    pthread_t th[12];
    worker w[12];
    for (int i = 0; i < 12; i++) {
        w[i] = (worker){ .ch = ch, .i = i };
        pthread_create(&th[i], NULL, work, &w[i]);
    }
    for (int i = 0; i < 12; i++) {
        pthread_join(th[i], NULL);
        char want[32];
        snprintf(want, sizeof want, "call %d", i);
        T_EQ_INT(w[i].rc, CB_OK);
        T_EQ_INT(w[i].code, 0);
        T_EQ_STR(w[i].got, want);
    }
    T_EQ_INT(cb_h2_channel_dials(ch), 1);
    T_EQ_INT(h2srv_accepts(s), 1);
    T_EQ_INT(h2srv_requests(s), 12);
    cb_h2_channel_free(ch);
    h2srv_stop(s);
}

static void timeout_units(void) {
    char b[16];
    static const struct { int64_t ms; const char *want; } cases[] = {
        { -5, "1m" }, { 0, "1m" }, { 1, "1m" }, { 1500, "1500m" }, { 99999999, "99999999m" },
        { 100000000, "100000S" }, { 100000001, "100001S" },                   // seconds round up
        { 99999999000, "99999999S" }, { 99999999001, "27778H" },             // 8 digits of seconds, then hours
        { 359999996400000, "99999999H" }, { 359999996400001, "99999999H" },  // hours cap at 8 digits
        { INT64_MAX, "99999999H" },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        cb_grpc_timeout(cases[i].ms, b, sizeof b);
        T_EQ_STR(b, cases[i].want);
    }
}

static void unframe(void) {
    const uint8_t *m;
    size_t n;
    bool has;
    // Nothing: no message, and that is well formed (the caller decides).
    T_EQ_INT(cb_grpc_unframe((const uint8_t *)"", 0, &m, &n, &has), CB_OK);
    T_CHECK(!has);
    T_EQ_INT((int)n, 0);
    T_CHECK(m == NULL);
    static const uint8_t one[] = { 0, 0, 0, 0, 3, 'a', 'b', 'c' };
    T_EQ_INT(cb_grpc_unframe(one, sizeof one, &m, &n, &has), CB_OK);
    T_CHECK(has);
    T_EQ_MEM(m, n, "abc", 3);
    T_CHECK(m == one + 5);
    // A zero-length message is a message.
    static const uint8_t zero[] = { 0, 0, 0, 0, 0 };
    T_EQ_INT(cb_grpc_unframe(zero, sizeof zero, &m, &n, &has), CB_OK);
    T_CHECK(has);
    T_EQ_INT((int)n, 0);
    // A prefix cut short, at every length.
    for (size_t k = 1; k < 5; k++) T_EQ_INT(cb_grpc_unframe(one, k, &m, &n, &has), CB_E_INVALID);
    // A message cut short.
    T_EQ_INT(cb_grpc_unframe(one, sizeof one - 1, &m, &n, &has), CB_E_INVALID);
    static const uint8_t huge[] = { 0, 0xff, 0xff, 0xff, 0xff, 'a' };
    T_EQ_INT(cb_grpc_unframe(huge, sizeof huge, &m, &n, &has), CB_E_INVALID);
    // 16 MB claimed by the top byte alone, 64 KB there: cut short.
    static uint8_t top[5 + 65536] = { 0, 1, 0, 0, 0 };
    T_EQ_INT(cb_grpc_unframe(top, sizeof top, &m, &n, &has), CB_E_INVALID);
    // Flagged compressed (1), and a reserved bit.
    static const uint8_t zip[] = { 1, 0, 0, 0, 1, 'z' };
    T_EQ_INT(cb_grpc_unframe(zip, sizeof zip, &m, &n, &has), CB_E_INVALID);
    static const uint8_t res[] = { 0x80, 0, 0, 0, 1, 'z' };
    T_EQ_INT(cb_grpc_unframe(res, sizeof res, &m, &n, &has), CB_E_INVALID);
    // Two messages, the second empty or not.
    static const uint8_t two[] = { 0, 0, 0, 0, 1, 'a', 0, 0, 0, 0, 1, 'b' };
    T_EQ_INT(cb_grpc_unframe(two, sizeof two, &m, &n, &has), CB_E_INVALID);
    T_CHECK(!has && m == NULL && n == 0);   // a refused body hands back no message
    static const uint8_t two0[] = { 0, 0, 0, 0, 1, 'a', 0, 0, 0, 0, 0 };
    T_EQ_INT(cb_grpc_unframe(two0, sizeof two0, &m, &n, &has), CB_E_INVALID);
    // The length is big-endian across all four bytes.
    static uint8_t big[5 + 0x010203];
    memset(big, 0, sizeof big);
    big[2] = 1; big[3] = 2; big[4] = 3;
    T_EQ_INT(cb_grpc_unframe(big, sizeof big, &m, &n, &has), CB_OK);
    T_EQ_INT((int)n, 0x010203);
}

static void pct(const char *in, const char *want) {
    cb_buf b;
    cb_buf_init(&b);
    cb_grpc_percent_decode(in, strlen(in), &b);
    cb_buf_putc(&b, 0);
    T_EQ_STR((const char *)b.p, want);
    cb_buf_free(&b);
}

static void percent(void) {
    pct("", "");
    pct("plain text", "plain text");
    pct("a%20b%3Ac%3a", "a b:c:");                 // both cases of hex
    pct("%41%42%43", "ABC");                      // back to back, the last at the very end
    pct("%E2%9C%93", "\xe2\x9c\x93");             // UTF-8 bytes
    pct("%zz %G1 %1g", "%zz %G1 %1g");            // not hex: kept as is
    pct("100%", "100%");                          // truncated: kept
    pct("1%4", "1%4");
    pct("%", "%");
    pct("%%41", "%A");                            // a stray % then a good escape
    // A decoded NUL is a byte like any other.
    cb_buf b;
    cb_buf_init(&b);
    cb_grpc_percent_decode("a%00b", 5, &b);
    T_EQ_MEM(b.p, b.len, "a\0b", 3);
    // The length is the bound, not a NUL: an escape cut by n stays as is.
    cb_buf_reset(&b);
    cb_grpc_percent_decode("%414", 2, &b);
    T_EQ_MEM(b.p, b.len, "%4", 2);
    cb_buf_free(&b);
}

int main(void) {
    T_RUN(timeout_units);
    T_RUN(unframe);
    T_RUN(percent);
    T_RUN(echo);
    T_RUN(meta);
    T_RUN(fail);
    T_RUN(not_grpc);
    T_RUN(raw);
    T_RUN(malformed);
    T_RUN(concurrent);
    return T_DONE();
}
