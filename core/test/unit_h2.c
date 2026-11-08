// unit_h2.c - the HTTP/2 client (net/h2.h) over real sockets against
// lib_h2_srv: cleartext and TLS (ALPN h2, the certificate verified), one
// connection shared by many threads, both flow-control directions at size,
// deadlines, resets, the one retry for an unprocessed request, a dropped
// connection, the body cap, idle close, and freeing a channel under a call.
#include "t.h"

#include "lib_h2_srv.h"
#include "net/h2.h"
#include "util/clock.h"
#include "util/err.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static cb_h2_channel *channel(h2srv *s, const cb_h2_opts *o) {
    char origin[128];
    cb_h2_opts opts;
    if (o) opts = *o;
    else memset(&opts, 0, sizeof opts);
    if (h2srv_ca_file(s)[0]) {
        snprintf(origin, sizeof origin, "https://localhost:%d", h2srv_port(s));
        if (!opts.ca_file) opts.ca_file = h2srv_ca_file(s);
    } else {
        snprintf(origin, sizeof origin, "http://127.0.0.1:%d", h2srv_port(s));
    }
    cb_h2_channel *ch = NULL;
    T_EQ_INT(cb_h2_channel_new(origin, &opts, &ch), CB_OK);
    return ch;
}

static int get(cb_h2_channel *ch, const char *path, int timeout_ms, size_t max_body, cb_h2_response *r) {
    return cb_h2_request(ch, "GET", path, NULL, 0, NULL, 0, timeout_ms, max_body, r);
}

static void sleep_ms(int ms) {
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000 };
    nanosleep(&ts, NULL);
}

static void echo_both_ways(void) {
    for (int tls = 0; tls < 2; tls++) {
        h2srv_opts so = { .tls = tls };
        h2srv *s = h2srv_start(&so);
        T_CHECK(s != NULL);
        cb_h2_channel *ch = channel(s, NULL);
        cb_h2_response r;
        cb_h2_response_init(&r);
        cb_h2_field f = { "x-test", "1", 6, 1, false };
        cb_h2_iov body[2] = { { "hello ", 6 }, { "world", 5 } };
        T_EQ_INT(cb_h2_request(ch, "POST", "/echo", &f, 1, body, 2, 5000, 0, &r), CB_OK);
        T_EQ_INT(r.status, 200);
        T_EQ_MEM(r.body.p, r.body.len, "hello world", 11);
        cb_span v;
        T_CHECK(cb_h2_response_get(&r, false, "content-type", &v) && v.n == 24);
        T_CHECK(cb_h2_response_get(&r, true, "x-trailer", &v) && v.n == 4 && memcmp(v.p, "done", 4) == 0);
        T_CHECK(!cb_h2_response_get(&r, false, "x-trailer", &v));
        T_EQ_INT(r.n_trailers, 1);
        // The second call reuses the connection.
        T_EQ_INT(get(ch, "/status/204", 5000, 0, &r), CB_OK);
        T_EQ_INT(r.status, 204);
        T_EQ_INT(cb_h2_channel_dials(ch), 1);
        T_EQ_INT(h2srv_accepts(s), 1);
        if (tls) {   // the client offers h2 and nothing else
            const unsigned char *offer;
            int n = h2srv_alpn_offer(s, &offer);
            T_EQ_MEM(offer, n, "\x02h2", 3);
        }
        cb_h2_response_free(&r);
        cb_h2_channel_free(ch);
        h2srv_stop(s);
    }
}

// TLS that must fail: a certificate no bundle vouches for, a server that will
// not speak h2, nobody listening.
static void refusals(void) {
    h2srv_opts so = { .tls = true };
    h2srv *s = h2srv_start(&so);
    char origin[128];
    snprintf(origin, sizeof origin, "https://localhost:%d", h2srv_port(s));
    cb_h2_channel *ch = NULL;
    // The default bundles (tls.h's search) do not vouch for a test certificate.
    cb_h2_opts o;
    memset(&o, 0, sizeof o);
    T_EQ_INT(cb_h2_channel_new(origin, &o, &ch), CB_OK);
    cb_h2_response r;
    cb_h2_response_init(&r);
    T_EQ_INT(get(ch, "/status/200", 5000, 0, &r), CB_E_IO);
    T_EQ_INT(r.why, CB_H2_WHY_TLS);
    T_CHECK(strstr(r.err, "certificate") != NULL);
    cb_h2_channel_free(ch);
    // The right certificate, but a host name it does not carry.
    snprintf(origin, sizeof origin, "https://127.0.0.1:%d", h2srv_port(s));
    o.ca_file = h2srv_ca_file(s);
    T_EQ_INT(cb_h2_channel_new(origin, &o, &ch), CB_OK);
    T_EQ_INT(get(ch, "/status/200", 5000, 0, &r), CB_E_IO);
    T_EQ_INT(r.why, CB_H2_WHY_TLS);
    cb_h2_channel_free(ch);
    h2srv_stop(s);

    so.alpn_http11_only = true;
    s = h2srv_start(&so);
    ch = channel(s, NULL);
    T_EQ_INT(get(ch, "/status/200", 5000, 0, &r), CB_E_IO);
    T_EQ_INT(r.why, CB_H2_WHY_TLS);
    T_CHECK(strstr(r.err, "HTTP/2") != NULL);
    int port = h2srv_port(s);
    cb_h2_channel_free(ch);
    h2srv_stop(s);

    // A peer that takes the connection and never answers the handshake: the call's
    // deadline bounds it, not the 10 s connect timeout.
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    socklen_t sl = sizeof sa;
    T_CHECK(bind(lfd, (struct sockaddr *)&sa, sizeof sa) == 0 && listen(lfd, 8) == 0);
    getsockname(lfd, (struct sockaddr *)&sa, &sl);
    snprintf(origin, sizeof origin, "https://localhost:%d", ntohs(sa.sin_port));
    o.ca_file = NULL;
    T_EQ_INT(cb_h2_channel_new(origin, &o, &ch), CB_OK);
    int64_t t0 = cb_mono_ms();
    T_EQ_INT(get(ch, "/", 300, 0, &r), CB_E_IO);
    T_EQ_INT(r.why, CB_H2_WHY_TIMEOUT);
    T_CHECK(cb_mono_ms() - t0 < 2500);
    cb_h2_channel_free(ch);
    close(lfd);

    snprintf(origin, sizeof origin, "http://127.0.0.1:%d", port);   // closed now
    T_EQ_INT(cb_h2_channel_new(origin, NULL, &ch), CB_OK);
    T_EQ_INT(get(ch, "/", 2000, 0, &r), CB_E_IO);
    T_EQ_INT(r.why, CB_H2_WHY_CONNECT);
    cb_h2_channel_free(ch);
    cb_h2_response_free(&r);
}

// Twelve threads on one connection, against a server that takes four streams
// at a time: one dial, never more than four open, every call answered.
typedef struct {
    cb_h2_channel *ch;
    int            rc, status;
    char           body[16];
} worker;

static void *work(void *arg) {
    worker *w = arg;
    cb_h2_response r;
    cb_h2_response_init(&r);
    w->rc = get(w->ch, "/slow/50", 20000, 0, &r);
    w->status = r.status;
    snprintf(w->body, sizeof w->body, "%.*s", (int)r.body.len, r.body.p ? (const char *)r.body.p : "");
    cb_h2_response_free(&r);
    return NULL;
}

static void concurrency(void) {
    h2srv_opts so = { .tls = true, .max_concurrent = 4 };
    h2srv *s = h2srv_start(&so);
    cb_h2_channel *ch = channel(s, NULL);
    pthread_t th[12];
    worker w[12];
    for (int i = 0; i < 12; i++) {
        w[i] = (worker){ .ch = ch };
        pthread_create(&th[i], NULL, work, &w[i]);
    }
    for (int i = 0; i < 12; i++) {
        pthread_join(th[i], NULL);
        T_EQ_INT(w[i].rc, CB_OK);
        T_EQ_INT(w[i].status, 200);
        T_EQ_STR(w[i].body, "slow");
    }
    T_EQ_INT(cb_h2_channel_dials(ch), 1);
    T_CHECK(h2srv_max_open(s) <= 4);
    T_CHECK(h2srv_max_open(s) >= 2);
    T_EQ_INT(h2srv_requests(s), 12);
    cb_h2_channel_free(ch);
    h2srv_stop(s);
}

// 5 MB down (past the 4 MB stream window, so WINDOW_UPDATEs must flow), and
// 1 MB up against a server whose windows start at 65535.
static void big_bodies(void) {
    h2srv_opts so = { .tls = true };
    h2srv *s = h2srv_start(&so);
    cb_h2_channel *ch = channel(s, NULL);
    cb_h2_response r;
    cb_h2_response_init(&r);
    size_t n = 5u << 20;
    char path[32];
    snprintf(path, sizeof path, "/big/%zu", n);
    T_EQ_INT(get(ch, path, 30000, 0, &r), CB_OK);
    T_EQ_INT((int)r.body.len, (int)n);
    bool same = r.body.len == n;
    for (size_t i = 0; same && i < n; i++) same = r.body.p[i] == (uint8_t)(i % 251);
    T_CHECK(same);
    static uint8_t up[1u << 20];
    for (size_t i = 0; i < sizeof up; i++) up[i] = (uint8_t)(i * 7);
    cb_h2_iov iov = { up, sizeof up };
    T_EQ_INT(cb_h2_request(ch, "POST", "/echo", NULL, 0, &iov, 1, 30000, 0, &r), CB_OK);
    T_EQ_MEM(r.body.p, r.body.len, up, sizeof up);
    // The cap: a body over max_body is refused, and the connection carries on.
    T_EQ_INT(get(ch, "/big/1000", 5000, 100, &r), CB_E_IO);
    T_EQ_INT(r.why, CB_H2_WHY_TOO_BIG);
    T_EQ_INT(get(ch, "/big/10", 5000, 100, &r), CB_OK);
    T_EQ_INT((int)r.body.len, 10);
    T_EQ_INT(get(ch, "/big/100", 5000, 100, &r), CB_OK);   // the cap itself is allowed
    T_EQ_INT((int)r.body.len, 100);
    T_EQ_INT(get(ch, "/big/101", 5000, 100, &r), CB_E_IO);
    T_EQ_INT(r.why, CB_H2_WHY_TOO_BIG);
    T_EQ_INT(cb_h2_channel_dials(ch), 1);
    cb_h2_response_free(&r);
    cb_h2_channel_free(ch);
    h2srv_stop(s);
}

static void failures(void) {
    cb_span v;
    h2srv_opts so = { 0 };
    h2srv *s = h2srv_start(&so);
    cb_h2_channel *ch = channel(s, NULL);
    cb_h2_response r;
    cb_h2_response_init(&r);
    // A deadline: the call ends at it, and the connection serves the next.
    int64_t t0 = cb_mono_ms();
    T_EQ_INT(get(ch, "/slow/3000", 300, 0, &r), CB_E_IO);
    T_EQ_INT(r.why, CB_H2_WHY_TIMEOUT);
    T_CHECK(strstr(r.err, "300 ms") != NULL);
    T_CHECK(cb_mono_ms() - t0 < 2500);
    // The stream given up on is reset (CANCEL), so the server stops on it.
    for (int i = 0; i < 300 && h2srv_resets(s) < 1; i++) sleep_ms(10);
    T_EQ_INT(h2srv_resets(s), 1);
    T_EQ_INT(get(ch, "/status/200", 5000, 0, &r), CB_OK);
    T_EQ_INT(cb_h2_channel_dials(ch), 1);
    // A reset: it may have been processed, so it is not sent again.
    int before = h2srv_requests(s);
    T_EQ_INT(get(ch, "/reset", 5000, 0, &r), CB_E_IO);
    T_EQ_INT(r.why, CB_H2_WHY_RESET);
    T_EQ_INT(r.h2_code, CB_H2_INTERNAL_ERROR);
    T_EQ_INT(h2srv_requests(s) - before, 1);
    // Refused every time: sent twice, never a third time, and the refusal is the answer.
    before = h2srv_requests(s);
    T_EQ_INT(get(ch, "/refuse", 5000, 0, &r), CB_E_IO);
    T_EQ_INT(r.why, CB_H2_WHY_RESET);
    T_EQ_INT(r.h2_code, CB_H2_REFUSED_STREAM);
    T_EQ_INT(h2srv_requests(s) - before, 2);
    // Refused once: sent again, and answered.
    before = h2srv_requests(s);
    T_EQ_INT(get(ch, "/refuse-once", 5000, 0, &r), CB_OK);
    T_EQ_MEM(r.body.p, r.body.len, "again", 5);
    T_EQ_INT(h2srv_requests(s) - before, 2);
    // Refused after its headers came: still never processed, sent again, and the
    // answer holds nothing of the first attempt.
    before = h2srv_requests(s);
    T_EQ_INT(get(ch, "/refuse-after-headers-once", 5000, 0, &r), CB_OK);
    T_EQ_INT(r.status, 200);
    T_EQ_MEM(r.body.p, r.body.len, "again", 5);
    T_EQ_INT(r.n_headers, 1);
    T_CHECK(!cb_h2_response_get(&r, false, "x-first", &v));
    T_EQ_INT(h2srv_requests(s) - before, 2);
    // A GOAWAY below the stream: sent again on a fresh connection.
    int dials = cb_h2_channel_dials(ch);
    T_EQ_INT(get(ch, "/goaway-once", 5000, 0, &r), CB_OK);
    T_EQ_MEM(r.body.p, r.body.len, "again", 5);
    T_EQ_INT(cb_h2_channel_dials(ch), dials + 1);
    // The connection drops under a call: no retry (it may have been processed),
    // and the next call dials again.
    dials = cb_h2_channel_dials(ch);
    before = h2srv_requests(s);
    T_EQ_INT(get(ch, "/close", 5000, 0, &r), CB_E_IO);
    T_EQ_INT(r.why, CB_H2_WHY_PROTOCOL);
    T_EQ_INT(h2srv_requests(s) - before, 1);
    T_EQ_INT(get(ch, "/status/200", 5000, 0, &r), CB_OK);
    T_EQ_INT(cb_h2_channel_dials(ch), dials + 1);
    // Going away every time: sent again on a fresh connection, never a third time.
    dials = cb_h2_channel_dials(ch);
    before = h2srv_requests(s);
    T_EQ_INT(get(ch, "/goaway", 5000, 0, &r), CB_E_IO);
    T_EQ_INT(r.why, CB_H2_WHY_GOAWAY);
    T_EQ_INT(h2srv_requests(s) - before, 2);
    T_EQ_INT(cb_h2_channel_dials(ch), dials + 1);
    // A field HTTP/2 refuses never leaves.
    cb_h2_field bad = { "Upper", "x", 5, 1, false };
    T_EQ_INT(cb_h2_request(ch, "GET", "/", &bad, 1, NULL, 0, 1000, 0, &r), CB_E_INVALID);
    cb_h2_response_free(&r);
    cb_h2_channel_free(ch);
    h2srv_stop(s);
}

static void idle_close(void) {
    h2srv_opts so = { 0 };
    h2srv *s = h2srv_start(&so);
    cb_h2_opts o;
    memset(&o, 0, sizeof o);
    o.idle_close_ms = 150;
    cb_h2_channel *ch = channel(s, &o);
    cb_h2_response r;
    cb_h2_response_init(&r);
    T_EQ_INT(get(ch, "/status/200", 5000, 0, &r), CB_OK);
    T_EQ_INT(get(ch, "/status/200", 5000, 0, &r), CB_OK);
    T_EQ_INT(cb_h2_channel_dials(ch), 1);
    sleep_ms(600);
    T_EQ_INT(h2srv_closes(s), 1);   // closed by the client, not merely left
    T_EQ_INT(get(ch, "/status/200", 5000, 0, &r), CB_OK);
    T_EQ_INT(cb_h2_channel_dials(ch), 2);
    cb_h2_response_free(&r);
    cb_h2_channel_free(ch);

    // The idle clock restarts after each call: three gaps of 600 ms under a
    // 1500 ms idle close keep one connection, though together they pass it.
    o.idle_close_ms = 1500;
    ch = channel(s, &o);
    for (int i = 0; i < 3; i++) {
        T_EQ_INT(get(ch, "/slow/20", 5000, 0, &r), CB_OK);
        sleep_ms(600);
    }
    T_EQ_INT(get(ch, "/status/200", 5000, 0, &r), CB_OK);
    T_EQ_INT(cb_h2_channel_dials(ch), 1);
    cb_h2_response_free(&r);
    cb_h2_channel_free(ch);
    h2srv_stop(s);
}

// Freeing a channel ends the call in flight at once.
typedef struct {
    cb_h2_channel *ch;
    int            rc;
    cb_h2_why      why;
    int64_t        took;
} slow_call;

static void *do_slow(void *arg) {
    slow_call *c = arg;
    cb_h2_response r;
    cb_h2_response_init(&r);
    int64_t t0 = cb_mono_ms();
    c->rc = get(c->ch, "/slow/5000", 20000, 0, &r);
    c->took = cb_mono_ms() - t0;
    c->why = r.why;
    cb_h2_response_free(&r);
    return NULL;
}

static void free_under_call(void) {
    h2srv_opts so = { 0 };
    h2srv *s = h2srv_start(&so);
    cb_h2_channel *ch = channel(s, NULL);
    slow_call c = { .ch = ch };
    pthread_t th;
    pthread_create(&th, NULL, do_slow, &c);
    sleep_ms(200);
    cb_h2_channel_free(ch);
    pthread_join(th, NULL);
    T_EQ_INT(c.rc, CB_E_IO);
    T_EQ_INT(c.why, CB_H2_WHY_CLOSED);
    T_CHECK(c.took < 3000);
    h2srv_stop(s);
}

static void origins(void) {
    static const char *const bad[] = { "ftp://x", "http://", "http://h:0", "http://h:99999", "http://h/path",
                                       "https://[::1", "http://h:", "h:80" };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        cb_h2_channel *ch = NULL;
        T_EQ_INT(cb_h2_channel_new(bad[i], NULL, &ch), CB_E_INVALID);
    }
    static const char *const good[] = { "http://h:80", "http://h/", "https://example.com", "https://[::1]:8443" };
    for (size_t i = 0; i < sizeof good / sizeof good[0]; i++) {
        cb_h2_channel *ch = NULL;
        T_EQ_INT(cb_h2_channel_new(good[i], NULL, &ch), CB_OK);
        cb_h2_channel_free(ch);
    }
}

int main(void) {
    T_RUN(origins);
    T_RUN(echo_both_ways);
    T_RUN(refusals);
    T_RUN(concurrency);
    T_RUN(big_bodies);
    T_RUN(failures);
    T_RUN(idle_close);
    T_RUN(free_under_call);
    return T_DONE();
}
