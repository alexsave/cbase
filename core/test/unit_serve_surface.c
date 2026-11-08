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

// The magic a wire body starts with.
static const uint8_t MAGIC[] = { 'A', 'W', 0, 1 };

// POST /api/wire: the product's own checks of a request, on the raw bytes. 415 unless the type is
// the wire's (parameters after it allowed, as the product's startsWith check allows them), 400
// unless the body starts with MAGIC, and else the body back as it came, with the type it came as.
static void h_wire(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    const char *ct = cb_http_header_get(req, "Content-Type");
    if (!ct || strncmp(ct, WIRE_MIME, strlen(WIRE_MIME)) != 0) {
        cb_res_send(res, 415, WIRE_MIME, "\0type", 5);
        return;
    }
    if (req->body_len < sizeof MAGIC || memcmp(req->body, MAGIC, sizeof MAGIC) != 0) {
        cb_res_send(res, 400, WIRE_MIME, "\0bytes", 6);
        return;
    }
    cb_res_send(res, 200, WIRE_MIME, req->body, req->body_len);
    cb_res_header(res, "X-Got-Type", ct);
}

// GET /t/headers: the product's five context headers, each asked for in a spelling of its own, a
// line each ("-" for one not sent).
static void h_headers(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    static const char *const NAMES[] = { "x-amgi-base-dir", "X-AMGI-COLLECTION", "X-Amgi-Profile",
                                         "x-AMGI-bridge-URL", "X-Amgi-Bridge-Token" };
    cb_buf b;
    cb_buf_init(&b);
    for (size_t i = 0; i < sizeof NAMES / sizeof NAMES[0]; i++) {
        const char *v = cb_http_header_get(req, NAMES[i]);
        cb_buf_printf(&b, "%s\n", v ? v : "-");
    }
    cb_res_send(res, 200, "text/plain", b.p, b.len);
    cb_buf_free(&b);
}

// /decks/:id, under GET, DELETE and a method the server has no name for.
static void h_deck(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) { cb_res_text(res, 200, "deck"); }

static const cb_route ROUTES[] = {
    { .method = "GET", .pattern = "/decks/:id", .flags = CB_ROUTE_POOL, .fn = h_deck },
    { .method = "DELETE", .pattern = "/decks/:id", .flags = CB_ROUTE_POOL, .fn = h_deck },
    { .method = "PURGE", .pattern = "/decks/:id", .flags = CB_ROUTE_POOL, .fn = h_deck },
    { .method = "GET", .pattern = "/t/headers", .flags = CB_ROUTE_POOL, .fn = h_headers },
    { .method = "GET", .pattern = "/t/answer", .flags = CB_ROUTE_POOL, .fn = h_answer },
    { .method = "POST", .pattern = "/api/wire", .flags = CB_ROUTE_POOL, .fn = h_wire },
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

// ---- 2. what a handler is given of a request ---------------------------------------------------

// Sends a POST /api/wire with `type` (NULL: no Content-Type) and body[0..n), Connection: close.
static long post_wire(const char *type, const void *body, size_t n, cb_buf *out) {
    cb_buf q;
    cb_buf_init(&q);
    cb_buf_printf(&q, "POST /api/wire HTTP/1.1\r\nHost: x\r\n");
    if (type) cb_buf_printf(&q, "Content-Type: %s\r\n", type);
    cb_buf_printf(&q, "Content-Length: %zu\r\nConnection: close\r\n\r\n", n);
    cb_buf_append(&q, body, n);
    long k = raw(q.p, q.len, out);
    cb_buf_free(&q);
    return k;
}

static void gets_the_raw_body_and_its_type(void) {
    // MAGIC, then every byte value twice over: NULs, CR LF, a blank line's bytes, high bytes, and
    // what would be a percent-escape or a form field if anything decoded it.
    uint8_t body[4 + 512 + 9];
    memcpy(body, MAGIC, sizeof MAGIC);
    for (int i = 0; i < 512; i++) body[4 + i] = (uint8_t)i;
    memcpy(body + 4 + 512, "a=%41&b=+", 9);
    cb_buf w;
    cb_buf_init(&w);
    char head[512];
    snprintf(head, sizeof head,
             "HTTP/1.1 200 OK\r\nContent-Type: " WIRE_MIME "\r\nX-Got-Type: " WIRE_MIME "; v=3\r\nVary: Origin\r\n"
             "Content-Length: %zu\r\nConnection: close\r\n\r\n",
             sizeof body);
    T_CHECK(post_wire(WIRE_MIME "; v=3", body, sizeof body, &w) > 0);
    T_CHECK(wire_is(&w, head, body, sizeof body));
    // The handler's own refusals: a body of another type, or of none, is its 415; bytes that are
    // not a message of its kind, its 400.
    static const char R415[] = "HTTP/1.1 415 Unsupported Media Type\r\nContent-Type: " WIRE_MIME
                               "\r\nVary: Origin\r\nContent-Length: 5\r\nConnection: close\r\n\r\n";
    T_CHECK(post_wire("application/json", body, sizeof body, &w) > 0);
    T_CHECK(wire_is(&w, R415, "\0type", 5));
    T_CHECK(post_wire(NULL, body, sizeof body, &w) > 0);
    T_CHECK(wire_is(&w, R415, "\0type", 5));
    static const char R400[] = "HTTP/1.1 400 Bad Request\r\nContent-Type: " WIRE_MIME
                               "\r\nVary: Origin\r\nContent-Length: 6\r\nConnection: close\r\n\r\n";
    body[2] = 'X';
    T_CHECK(post_wire(WIRE_MIME, body, sizeof body, &w) > 0);
    T_CHECK(wire_is(&w, R400, "\0bytes", 6));
    T_CHECK(post_wire(WIRE_MIME, "", 0, &w) > 0);
    T_CHECK(wire_is(&w, R400, "\0bytes", 6));
    cb_buf_free(&w);
}

// ---- 3. request headers by name ----------------------------------------------------------------

// A Windows path deep in a folder of Korean names, percent-encoded UTF-8 as the product sends it,
// cut at a whole escape near `n` bytes: the long header values a real profile can carry.
static void long_path(char *out, size_t n, const char *tail) {
    static const char SEG[] = "%ED%95%9C%EA%B5%AD%EC%96%B4%20%EC%B9%B4%EB%93%9C%5C";
    size_t k = (size_t)snprintf(out, n, "C%%3A%%5CUsers%%5C");
    while (k + sizeof SEG + strlen(tail) < n) k += (size_t)snprintf(out + k, n - k, "%s", SEG);
    snprintf(out + k, n - k, "%s", tail);
}

static void reads_request_headers_by_name_in_any_case(void) {
    // Five of about 2 KB each, beside what a browser sends of its own (a cookie jar shared by
    // everything on 127.0.0.1 among it): about 11 KB of head, inside CB_HTTP_MAX_HEAD's 16 KB,
    // which is Node's default too, the server the product moves from.
    char dir[2048], col[2048], prof[2048], url[2048], tok[2048], cookie[2048];
    long_path(dir, sizeof dir, "Anki2");
    long_path(col, sizeof col, "Anki2%5CUser%201%5Ccollection.anki2");
    long_path(prof, sizeof prof, "User%201");
    snprintf(url, sizeof url, "http%%3A%%2F%%2F127.0.0.1%%3A8798%%2F");
    memset(tok, 'k', sizeof tok - 1), tok[sizeof tok - 1] = 0;
    memset(cookie, 'c', sizeof cookie - 1), cookie[sizeof cookie - 1] = 0;
    cb_buf q, w;
    cb_buf_init(&q);
    cb_buf_init(&w);
    cb_buf_printf(&q,
                  "GET /t/headers HTTP/1.1\r\nHost: 127.0.0.1:3000\r\n"
                  "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
                  "Chrome/142.0.0.0 Safari/537.36\r\n"
                  "Accept: */*\r\nAccept-Language: ko-KR,ko;q=0.9,en-US;q=0.8,en;q=0.7\r\n"
                  "Accept-Encoding: gzip, deflate, br, zstd\r\nReferer: http://127.0.0.1:3000/decks\r\n"
                  "Sec-Fetch-Dest: empty\r\nSec-Fetch-Mode: cors\r\nSec-Fetch-Site: same-origin\r\n"
                  "Cookie: jar=%s\r\n"
                  "X-Amgi-Base-Dir: %s\r\nx-amgi-collection: %s\r\nX-AMGI-PROFILE: %s\r\n"
                  "X-Amgi-Bridge-Url: %s\r\nX-Amgi-Bridge-Token: \t %s  \r\n"
                  "Connection: close\r\n\r\n",
                  cookie, dir, col, prof, url, tok);
    T_CHECK(q.len > 10500 && q.len < CB_HTTP_MAX_HEAD);
    cb_buf want;
    cb_buf_init(&want);
    cb_buf_printf(&want, "%s\n%s\n%s\n%s\n%s\n", dir, col, prof, url, tok);   // the token's blanks trimmed
    char head[256];
    snprintf(head, sizeof head,
             "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nVary: Origin\r\nContent-Length: %zu\r\n"
             "Connection: close\r\n\r\n",
             want.len);
    T_CHECK(raw(q.p, q.len, &w) > 0);
    T_CHECK(wire_is(&w, head, want.p, want.len));
    // One not sent is not there.
    T_CHECK(raw_str("GET /t/headers HTTP/1.1\r\nHost: x\r\nx-amgi-profile: p\r\nConnection: close\r\n\r\n", &w) > 0);
    T_CHECK(wire_is(&w,
                    "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nVary: Origin\r\nContent-Length: 10\r\n"
                    "Connection: close\r\n\r\n",
                    "-\n-\np\n-\n-\n", 10));
    // Past the cap, the head is refused whole (431) before any handler runs.
    cb_buf_reset(&q);
    cb_buf_printf(&q, "GET /t/headers HTTP/1.1\r\nHost: x\r\n");
    for (int i = 0; i < 9; i++) cb_buf_printf(&q, "X-Amgi-Base-Dir-%d: %s\r\n", i, dir);
    cb_buf_puts(&q, "\r\n");
    T_CHECK(q.len > CB_HTTP_MAX_HEAD);
    T_CHECK(raw(q.p, q.len, &w) > 0);
    T_CHECK(cb_starts_with(cb_buf_cstr(&w), "HTTP/1.1 431 "));
    cb_buf_free(&q);
    cb_buf_free(&w);
    cb_buf_free(&want);
}

// ---- 4. 404 and 405 -----------------------------------------------------------------------------

static void unknown_path_404_wrong_method_405_with_allow(void) {
    cb_buf w;
    cb_buf_init(&w);
    T_CHECK(raw_str("GET /api/nothing HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n", &w) > 0);
    T_CHECK(wire_is(&w,
                    "HTTP/1.1 404 Not Found\r\nContent-Type: text/plain; charset=utf-8\r\nVary: Origin\r\n"
                    "Content-Length: 9\r\nConnection: close\r\n\r\n",
                    "not found", 9));
    // A path routed under other methods: 405, and Allow names them (RFC 9110 15.5.6).
    static const char W405[] = "HTTP/1.1 405 Method Not Allowed\r\nContent-Type: text/plain; charset=utf-8\r\n"
                               "Allow: %s\r\nVary: Origin\r\nContent-Length: 18\r\nConnection: close\r\n\r\n";
    static const struct { const char *req, *allow; } A[] = {
        { "GET /api/wire HTTP/1.1\r\n", "POST" },
        { "PUT /api/wire HTTP/1.1\r\n", "POST" },
        { "PROPFIND /api/wire HTTP/1.1\r\n", "POST" },
        { "POST /decks/7 HTTP/1.1\r\nContent-Length: 2\r\n", "GET, HEAD, DELETE, PURGE" },
        { "PATCH /decks/a%2Fb HTTP/1.1\r\n", "GET, HEAD, DELETE, PURGE" },
        { "POST /t/answer HTTP/1.1\r\n", "GET, HEAD" },
    };
    for (size_t i = 0; i < sizeof A / sizeof A[0]; i++) {
        char req[256], head[512];
        snprintf(req, sizeof req, "%sHost: x\r\nConnection: close\r\n\r\n%s", A[i].req,
                 strstr(A[i].req, "Content-Length") ? "{}" : "");
        snprintf(head, sizeof head, W405, A[i].allow);
        T_CHECK(raw_str(req, &w) > 0);
        T_CHECK(wire_is(&w, head, "method not allowed", 18));
    }
    // A HEAD the path has no route for: the same head, no body.
    char head[512];
    snprintf(head, sizeof head, W405, "POST");
    T_CHECK(raw_str("HEAD /api/wire HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n", &w) > 0);
    T_CHECK(wire_is(&w, head, NULL, 0));
    // And the methods it is routed under still answer.
    T_CHECK(raw_str("PURGE /decks/7 HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n", &w) > 0);
    T_CHECK(cb_starts_with(cb_buf_cstr(&w), "HTTP/1.1 200 OK\r\n"));
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
    T_RUN(gets_the_raw_body_and_its_type);
    T_RUN(reads_request_headers_by_name_in_any_case);
    T_RUN(unknown_path_404_wrong_method_405_with_allow);

    T_EQ_INT(tp_stop(&c, SIGTERM, 30000), 0);
    return T_DONE();
}

#else

int main(void) {
    T_CHECK(1);   // the local provider is not linked: there is no serve to run an app under
    return T_DONE();
}

#endif
