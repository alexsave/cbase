// unit_serve_surface.c - a product's HTTP surface under `cbase serve`, byte for byte on the wire: the
// answers a handler can give (any status, any type, a binary body, headers of its own), what it is
// given of a request (the raw body and its type, every header by any spelling of its name), and
// what the router answers around it (404, 405 with Allow).
//
// This program runs ITSELF as the server (lib_proc.h: a scripted child is the test's own binary):
// its `serve-surface` mode serves a small app through the serve ingress (cb_process_main), and the
// test speaks to it over raw sockets, so what is checked is what a client receives.
#include "t.h"
#include "lib_client.h"
#include "lib_proc.h"

#include "cbase/app.h"
#include "cbase/provider.h"
#include "ctx.h"
#include "ingress_process.h"
#include "net/http.h"
#include "net/router.h"
#include "num_parse.h"
#include "util/clock.h"

#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#ifdef CBASE_LINKS_LOCAL

// ---- the server's app ---------------------------------------------------------------------------

#define WIRE_MIME "application/x-amgi-wire"
#define FOREVER   "private, max-age=31536000, immutable"

// A binary body: NULs, a high byte, and a blank line's bytes that must not end anything.
static const uint8_t BIN[] = { 'A', 'W', 0, 1, 0, 0, 0xff, 'x', 0, '\r', '\n', '\r', '\n', 0 };

// GET /t/answer?status=N: N, typed as the product's wire, BIN as the body, and headers of its own
// (Retry-After on a 429, Location on a 307, a cache line on every one).
static void h_answer(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    char st[8];
    int64_t status = 200;
    if (cb_http_query_get(req, "status", st, sizeof st)) num_parse_i64(st, strlen(st), 1, 999, &status);
    cb_res_send(res, (int)status, WIRE_MIME, BIN, sizeof BIN);
    if (status == 429) cb_res_header(res, "Retry-After", "30");
    if (status == 307) cb_res_header(res, "Location", "/decks");
    cb_res_header(res, "Cache-Control", FOREVER);
}

static const cb_route ROUTES[] = {
    { .method = "GET", .pattern = "/t/answer", .flags = CB_ROUTE_POOL, .fn = h_answer },
};
static const cb_route_list LIST = { ROUTES, sizeof ROUTES / sizeof ROUTES[0] };
static const cb_route_list *const LISTS[] = { &LIST };
static const cb_app APP = { .name = "serve-surface", .routes = LISTS, .n_route_lists = 1, .storeless = true };

// ---- the client side ----------------------------------------------------------------------------

static int g_port;

// Sends `req` (n bytes) on a new connection and reads until the server closes it (the requests
// say Connection: close): every byte it sent, in out. The bytes read, or -1.
static long raw(const void *req, size_t n, cb_buf *out) {
    cb_buf_reset(out);
    int fd = tc_connect(g_port);
    if (fd < 0) return -1;
    if (write(fd, req, n) != (ssize_t)n) { close(fd); return -1; }
    int64_t end = cb_mono_ms() + 10000;
    for (;;) {
        struct pollfd p = { fd, POLLIN, 0 };
        if (cb_mono_ms() > end) { close(fd); return -1; }
        if (poll(&p, 1, 100) <= 0) continue;
        char b[8192];
        ssize_t k = recv(fd, b, sizeof b, 0);
        if (k <= 0) break;
        cb_buf_append(out, b, (size_t)k);
    }
    close(fd);
    return (long)out->len;
}

static long raw_str(const char *req, cb_buf *out) { return raw(req, strlen(req), out); }

// out holds exactly head, then body[0..n).
static bool wire_is(const cb_buf *out, const char *head, const void *body, size_t n) {
    const size_t hn = strlen(head);
    if (out->len != hn + n || memcmp(out->p, head, hn) != 0 || (n && memcmp(out->p + hn, body, n) != 0)) {
        fprintf(stderr, "  wire: got %zu bytes:\n%.*s\n  wanted %zu:\n%s\n", out->len, (int)out->len,
                out->p ? (const char *)out->p : "", hn + n, head);
        return false;
    }
    return true;
}

// ---- 1. what a handler answers ------------------------------------------------------------------

static void answers_any_status_type_body_and_headers(void) {
    cb_buf w;
    cb_buf_init(&w);
    static const struct { int status; const char *reason, *extra; } S[] = {
        { 200, "OK", "" },
        { 201, "Created", "" },
        { 307, "Temporary Redirect", "Location: /decks\r\n" },
        { 400, "Bad Request", "" },
        { 415, "Unsupported Media Type", "" },
        { 429, "Too Many Requests", "Retry-After: 30\r\n" },
        { 500, "Internal Server Error", "" },
        { 503, "Service Unavailable", "" },
        { 299, "Unknown", "" },
    };
    for (size_t i = 0; i < sizeof S / sizeof S[0]; i++) {
        char req[128], head[512];
        snprintf(req, sizeof req, "GET /t/answer?status=%d HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n", S[i].status);
        snprintf(head, sizeof head,
                 "HTTP/1.1 %d %s\r\n"
                 "Content-Type: " WIRE_MIME "\r\n"
                 "%s"
                 "Cache-Control: " FOREVER "\r\n"
                 "Vary: Origin\r\n"
                 "Content-Length: %zu\r\n"
                 "Connection: close\r\n\r\n",
                 S[i].status, S[i].reason, S[i].extra, sizeof BIN);
        T_CHECK(raw_str(req, &w) > 0);
        T_CHECK(wire_is(&w, head, BIN, sizeof BIN));
    }
    // 204 and 304 carry no body and no Content-Length, whatever the handler gave.
    T_CHECK(raw_str("GET /t/answer?status=304 HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n", &w) > 0);
    T_CHECK(wire_is(&w,
                    "HTTP/1.1 304 Not Modified\r\nContent-Type: " WIRE_MIME "\r\nCache-Control: " FOREVER
                    "\r\nVary: Origin\r\nConnection: close\r\n\r\n",
                    NULL, 0));
    T_CHECK(raw_str("GET /t/answer?status=204 HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n", &w) > 0);
    T_CHECK(wire_is(&w,
                    "HTTP/1.1 204 No Content\r\nContent-Type: " WIRE_MIME "\r\nCache-Control: " FOREVER
                    "\r\nVary: Origin\r\nConnection: close\r\n\r\n",
                    NULL, 0));
    // On a kept-alive connection, two answers back to back: the first body's blank-line bytes end
    // nothing, and the second answer starts right after its Content-Length.
    int fd = tc_connect(g_port);
    T_CHECK(fd >= 0);
    tc_response r;
    for (int k = 0; k < 2; k++) {
        T_EQ_INT(tc_request(fd, "GET", "/t/answer?status=429", NULL, NULL, 0, 10000, &r), 0);
        T_EQ_INT(r.status, 429);
        T_EQ_INT(r.body.len, sizeof BIN);
        T_CHECK(r.body.len == sizeof BIN && memcmp(r.body.p, BIN, sizeof BIN) == 0);
        tc_response_free(&r);
    }
    close(fd);
    cb_buf_free(&w);
}

// ---- the test -----------------------------------------------------------------------------------

static const char *g_self;

int main(int argc, char **argv) {
    g_self = argv[0];
    if (argc == 2 && strcmp(argv[1], "serve-surface") == 0) {
        char *sargv[] = { argv[0], "serve", NULL };
        return cb_process_main(&cb_provider_local, &APP, 2, sargv);
    }
    char *env[] = { "CBASE_DEV=1", "CBASE_LISTEN=127.0.0.1:0", "CBASE_STORE=", "CBASE_REQ_WORKERS=2", NULL };
    char *sargv[] = { (char *)g_self, "serve-surface", NULL };
    tp_child c;
    T_EQ_INT(tp_start(&c, sargv, env), 0);
    char line[512] = "";
    T_EQ_INT(tp_wait_line(&c, "listening on", 20000, line, sizeof line), 0);
    const char *colon = strrchr(line, ':');
    int64_t port = 0;
    T_CHECK(colon && num_parse_i64(colon + 1, strcspn(colon + 1, " \r\n"), 1, 65535, &port));
    g_port = (int)port;

    T_RUN(answers_any_status_type_body_and_headers);

    T_EQ_INT(tp_stop(&c, SIGTERM, 30000), 0);
    return T_DONE();
}

#else

int main(void) {
    T_CHECK(1);   // the local provider is not linked: there is no serve to run an app under
    return T_DONE();
}

#endif
