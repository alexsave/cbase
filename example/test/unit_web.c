// unit_web.c - what a local web app (amgi's shape: a single-page app, its API and its media,
// served from the user's own machine) needs from `cbase serve`, over a real serve.
//
// End to end, as `cbase serve` runs: this program runs ITSELF as the server (lib_proc.h: a scripted
// child is the test's own binary), whose `serve-web` mode serves the app below through the serve
// ingress (cb_process_main), storeless, on a port the kernel picks. Every check is a request over
// a socket, but the last test's, which calls the helpers in this process as a one-shot platform would.
//
//   * files (net/files.h): the content types, a directory under a prefix route and a media file
//     by name with every traversal refused (never a byte from outside), single-page routes that
//     answer index.html, HEAD, a missing file, a FIFO answered 404 at once (the worker never
//     held), files of many MB streamed whole, a request pipelined behind a file, and the same
//     helper reading the file whole off serve;
//   * the Host allow-list (cbase/app.h host_allow), against DNS rebinding: the listed hosts at
//     the bound port pass, IPv6 among them; another name, another port, no Host or two are refused
//     before any handler runs, on the app's routes, the platform's, a file and a WebSocket upgrade
//     (no 101); with no list, any Host goes ahead as before.
#include "t.h"
#include "lib_client.h"
#include "lib_proc.h"

#include "cbase/app.h"
#include "cbase/provider.h"
#include "ingress_process.h"
#include "net/files.h"
#include "net/http.h"
#include "net/router.h"
#include "num_parse.h"
#include "util/clock.h"
#include "util/err.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/resource.h>
#ifdef __APPLE__
#include <libproc.h>
#endif
#include <sys/stat.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#ifdef CBASE_LINKS_LOCAL

static const char *g_self;

// ---- the app ------------------------------------------------------------------------------------

// ---- files: amgi's shell, assets and media, from WEB_DIR ----
static const char *g_web;   // WEB_DIR
static char g_media[1024];  // WEB_DIR/media

// /, /decks, /settings and /deck/:id: the single-page app's shell, or / to /decks.
static void h_root(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    cb_res_status(res, 307);
    cb_res_header(res, "Location", "/decks");
    cb_res_header(res, "Cache-Control", "no-store");
}
static void h_shell(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    char p[1100];
    snprintf(p, sizeof p, "%s/index.html", g_web);
    cb_send_file(ctx, req, res, p, NULL, "no-store");
}
// /assets/*: the directory.
static void h_assets(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    cb_serve_dir(ctx, req, res, g_web, req->path + strlen("/assets/"), "no-store");
}
// /any/*: the same directory under a route of every method, where cb_serve_dir's own method rule
// is the one that holds.
static void h_any(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    cb_serve_dir(ctx, req, res, g_web, req->path + strlen("/any/"), "no-store");
}
// /media/:filename: one file by its name, which is user input.
static void h_media(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    char name[256], p[1100];
    if (!cb_route_param(req, "filename", name, sizeof name)) { cb_res_text(res, 400, "bad name\n"); return; }
    const int rc = cb_path_join(g_media, name, p, sizeof p);
    if (rc == CB_OK) cb_send_file(ctx, req, res, p, NULL, "private, max-age=31536000, immutable");
    else cb_res_text(res, rc == CB_E_FORBIDDEN ? 403 : 404, "no such media\n");
}

// /notmod: the shell's file answer turned into a 304, as a conditional GET would: no body may
// follow its head, file body or not (http.h cb_res_bodied).
static void h_notmod(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    char p[1100];
    snprintf(p, sizeof p, "%s/app.wasm", g_web);
    if (cb_send_file(ctx, req, res, p, NULL, "no-store") == 200) cb_res_status(res, 304);
}

// The app's own types: one of its own, and one over the platform's.
static const cb_mime MIME[] = { { "pack", "application/x-amgi-pack" }, { "txt", "text/x-amgi-notes" } };

static const cb_route ROUTES[] = {
    { .method = "GET", .pattern = "/", .fn = h_root },
    { .method = "GET", .pattern = "/decks", .fn = h_shell },
    { .method = "GET", .pattern = "/settings", .fn = h_shell },
    { .method = "GET", .pattern = "/deck/:id", .fn = h_shell },
    { .method = "GET", .pattern = "/assets/*", .fn = h_assets },
    { .method = "GET", .pattern = "/media/:filename", .fn = h_media },
    { .method = "GET", .pattern = "/notmod", .fn = h_notmod },
    { .method = "*", .pattern = "/any/*", .fn = h_any },
};
static const cb_route_list ROUTE_LIST = { ROUTES, (int)(sizeof ROUTES / sizeof ROUTES[0]) };
static const cb_route_list *const ROUTE_LISTS[] = { &ROUTE_LIST };

static cb_app g_app = {
    .name = "web",
    .routes = ROUTE_LISTS,
    .n_route_lists = 1,
    .storeless = true,
    .mime_types = MIME,
    .n_mime_types = (int)(sizeof MIME / sizeof MIME[0]),
};

// The hosts amgi answers to: its loopback names at the port it bound, and one named with its own port.
static const char *const HOSTS[] = { "127.0.0.1", "localhost", "[::1]", "app.example.test:9999" };

static int child_main(const char *variant) {
    if (strcmp(variant, "hosts") == 0) {
        g_app.host_allow = HOSTS;
        g_app.n_host_allow = (int)(sizeof HOSTS / sizeof HOSTS[0]);
    }
    if (strcmp(variant, "badhost") == 0) {
        static const char *const BAD[] = { "127.0.0.1", "http://localhost:8080" };
        g_app.host_allow = BAD;
        g_app.n_host_allow = 2;
    }
    g_web = getenv("WEB_DIR");
    if (g_web) snprintf(g_media, sizeof g_media, "%s/media", g_web);
    char *argv[] = { (char *)g_self, "serve", NULL };
    return cb_process_main(&cb_provider_local, &g_app, 2, argv);
}

// ---- the client ---------------------------------------------------------------------------------

static tp_child g_child;
static int g_port;

// Starts the server in `variant` (with `extra`, NULL or one more "NAME=value"), and reads its port
// from the startup line on stdout.
static int start(const char *variant, const char *extra) {
    char *env[] = { "CBASE_DEV=1", "CBASE_LISTEN=127.0.0.1:0", "CBASE_STORE=", (char *)extra, NULL };
    char *argv[] = { (char *)g_self, "serve-web", (char *)variant, NULL };
    if (tp_start_out(&g_child, argv, env) != 0) return -1;
    char line[256];
    size_t n = 0;
    while (n < sizeof line - 1) {
        struct pollfd p = { g_child.out_fd, POLLIN, 0 };
        if (poll(&p, 1, 20000) <= 0 || read(g_child.out_fd, line + n, 1) != 1) return -1;
        if (line[n++] == '\n') break;
    }
    line[n] = 0;
    // "cbase: listening <host>:<port> <url>": the port is the address word's, after its last ':'.
    const char *addr = strstr(line, "cbase: listening ");
    if (!addr) return -1;
    addr += strlen("cbase: listening ");
    size_t alen = strcspn(addr, " \r\n");
    const char *colon = NULL;
    for (size_t i = 0; i < alen; i++)
        if (addr[i] == ':') colon = addr + i;
    int64_t port = 0;
    if (!colon || !num_parse_i64(colon + 1, (size_t)(addr + alen - colon - 1), 1, 65535, &port)) return -1;
    g_port = (int)port;
    return 0;
}

static int stop(int sig) {
    close(g_child.out_fd);
    return tp_stop(&g_child, sig, 15000);
}

static int req(const char *method, const char *path, const char *headers, const void *body, size_t n, tc_response *r) {
    memset(r, 0, sizeof *r);
    if (tc_http(g_port, method, path, headers, body, n, r) != 0) return -1;
    return r->status;
}

// The response header's value, or "" when absent.
static const char *hdr(const tc_response *r, const char *name) {
    static char v[512];
    return tc_header(r, name, v, sizeof v) ? v : "";
}

// ---- files ------------------------------------------------------------------------------------

static char g_root[256], g_dir[512];   // a temporary directory, and WEB_DIR inside it
#define SECRET "SECRET-outside-the-web-directory"
#define WASM_SIZE (6u << 20)    // app.wasm-sized
#define BIG_SIZE  (40u << 20)   // and bigger than serve ever holds in memory at once

// n bytes of a pattern no window of which repeats soon, with NULs and CR LF in it.
static uint8_t *pattern(size_t n, uint32_t seed) {
    uint8_t *p = malloc(n);
    uint32_t x = seed;
    for (size_t i = 0; i < n; i++) {
        x = x * 1664525u + 1013904223u;
        p[i] = (uint8_t)(x >> 24);
    }
    return p;
}

static void put(const char *rel, const void *p, size_t n) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", g_dir, rel);
    FILE *f = fopen(path, "wb");
    T_CHECK(f != NULL);
    if (!f) return;
    T_EQ_INT((int)fwrite(p, 1, n, f), (int)n);
    fclose(f);
}

static void make_tree(void) {
    T_EQ_INT(tp_tempdir(g_root, sizeof g_root), 0);
    char p[1024], q[1024];
    snprintf(p, sizeof p, "%s/secret.txt", g_root);
    FILE *f = fopen(p, "w");
    fputs(SECRET, f);
    fclose(f);
    snprintf(g_dir, sizeof g_dir, "%s/web", g_root);
    T_EQ_INT(mkdir(g_dir, 0755), 0);
    snprintf(p, sizeof p, "%s/sub", g_dir);
    T_EQ_INT(mkdir(p, 0755), 0);
    snprintf(p, sizeof p, "%s/media", g_dir);
    T_EQ_INT(mkdir(p, 0755), 0);
    put("index.html", "<!doctype html><title>amgi</title>", 34);
    put("app.js", "export const x = 1;\n", 20);
    put("style.css", "body{margin:0}", 14);
    put("icon.svg", "<svg/>", 6);
    put("logo.png", "\x89PNG\r\n\x1a\n", 8);
    put("LOGO.PNG", "\x89PNG", 4);
    put("favicon.ico", "\0\0\1\0", 4);
    put("deck.pack", "AMGI", 4);
    put("notes.txt", "notes", 5);
    put("data.unknownext", "?", 1);
    put("noext", "?", 1);
    put("empty.js", "", 0);
    put("sub/inner.js", "inner", 5);
    put("media/a.mp3", "ID3", 3);
    put("media/clip.wav", "RIFF", 4);
    put("media/b.ogg", "OggS", 4);
    put("media/c.m4a", "ftyp", 4);
    uint8_t *w = pattern(WASM_SIZE, 1);
    put("app.wasm", w, WASM_SIZE);
    free(w);
    uint8_t *b = pattern(BIG_SIZE, 2);
    put("big.bin", b, BIG_SIZE);
    free(b);
    // Symlinks: out of the directory (refused), and within it (served).
    snprintf(p, sizeof p, "%s/out", g_dir);
    snprintf(q, sizeof q, "%s/secret.txt", g_root);
    T_EQ_INT(symlink(q, p), 0);
    snprintf(p, sizeof p, "%s/subout", g_dir);
    T_EQ_INT(symlink("..", p), 0);   // a directory link out: /assets/subout/secret.txt
    snprintf(p, sizeof p, "%s/media/link-out", g_dir);
    T_EQ_INT(symlink("../../secret.txt", p), 0);
    snprintf(p, sizeof p, "%s/media/link-in", g_dir);
    T_EQ_INT(symlink("a.mp3", p), 0);
}

static int start_web(void) {
    static char env[600];
    snprintf(env, sizeof env, "WEB_DIR=%s", g_dir);
    return start("plain", env);
}

// GET path: the status, with the body's file content type and Cache-Control checked when 200.
static void type_is(const char *path, const char *type) {
    tc_response r;
    T_EQ_INT(req("GET", path, NULL, NULL, 0, &r), 200);
    T_EQ_STR(hdr(&r, "content-type"), type);
    T_EQ_STR(hdr(&r, "cache-control"), "no-store");
    tc_response_free(&r);
}

static void serves_content_types(void) {
    make_tree();
    T_EQ_INT(start_web(), 0);
    type_is("/assets/index.html", "text/html; charset=utf-8");
    type_is("/assets/app.js", "text/javascript; charset=utf-8");
    type_is("/assets/style.css", "text/css; charset=utf-8");
    type_is("/assets/icon.svg", "image/svg+xml");
    type_is("/assets/logo.png", "image/png");
    type_is("/assets/LOGO.PNG", "image/png");   // the extension in any case
    type_is("/assets/favicon.ico", "image/x-icon");
    type_is("/assets/app.wasm", "application/wasm");
    type_is("/assets/data.unknownext", "application/octet-stream");
    type_is("/assets/noext", "application/octet-stream");
    type_is("/assets/deck.pack", "application/x-amgi-pack");   // the app's own
    type_is("/assets/notes.txt", "text/x-amgi-notes");         // the app's over the platform's
    type_is("/assets/sub/inner.js", "text/javascript; charset=utf-8");
    tc_response r;
    T_EQ_INT(req("GET", "/assets/empty.js", NULL, NULL, 0, &r), 200);
    T_EQ_STR(hdr(&r, "content-length"), "0");
    tc_response_free(&r);
    // Media by name, with the media types amgi plays.
    static const struct { const char *path, *type; } media[] = {
        { "/media/a.mp3", "audio/mpeg" }, { "/media/clip.wav", "audio/wav" }, { "/media/b.ogg", "audio/ogg" },
        { "/media/c.m4a", "audio/mp4" }, { "/media/link-in", "audio/mpeg" },   // by the file it resolves to
    };
    for (size_t i = 0; i < sizeof media / sizeof media[0]; i++) {
        T_EQ_INT(req("GET", media[i].path, NULL, NULL, 0, &r), 200);
        T_EQ_STR(hdr(&r, "content-type"), media[i].type);
        T_EQ_STR(hdr(&r, "cache-control"), "private, max-age=31536000, immutable");
        tc_response_free(&r);
    }
    T_EQ_INT(req("GET", "/media/link-in", NULL, NULL, 0, &r), 200);
    T_EQ_STR(cb_buf_cstr(&r.body), "ID3");
    tc_response_free(&r);
    T_EQ_INT(stop(SIGTERM), 0);
}

static void single_page_routes(void) {
    T_EQ_INT(start_web(), 0);
    static const char *const shell[] = { "/decks", "/settings", "/deck/42", "/deck/a%20b" };
    for (size_t i = 0; i < sizeof shell / sizeof shell[0]; i++) {
        tc_response r;
        T_EQ_INT(req("GET", shell[i], NULL, NULL, 0, &r), 200);
        T_EQ_STR(cb_buf_cstr(&r.body), "<!doctype html><title>amgi</title>");
        T_EQ_STR(hdr(&r, "content-type"), "text/html; charset=utf-8");
        T_EQ_STR(hdr(&r, "cache-control"), "no-store");
        T_EQ_STR(hdr(&r, "content-length"), "34");
        tc_response_free(&r);
    }
    tc_response r;
    T_EQ_INT(req("GET", "/", NULL, NULL, 0, &r), 307);
    T_EQ_STR(hdr(&r, "location"), "/decks");
    tc_response_free(&r);
    T_EQ_INT(stop(SIGTERM), 0);
}

// Every way out of the directory: refused with `status`, and never a byte of the secret.
static void refused(const char *method, const char *path, int status) {
    tc_response r;
    const int got = req(method, path, NULL, NULL, 0, &r);
    if (got != status) fprintf(stderr, "     %s %s: %d, not %d\n", method, path, got, status);
    T_EQ_INT(got, status);
    T_CHECK(!memmem(r.body.p ? (const void *)r.body.p : "", r.body.len, "SECRET", 6));
    tc_response_free(&r);
}

static void refuses_every_way_out(void) {
    T_EQ_INT(start_web(), 0);
    refused("GET", "/assets/../secret.txt", 404);
    refused("GET", "/assets/sub/../../secret.txt", 404);
    refused("GET", "/assets/%2e%2e/secret.txt", 404);
    refused("GET", "/assets/%2E%2E%2Fsecret.txt", 404);
    refused("GET", "/assets/..%2fsecret.txt", 404);
    refused("GET", "/assets/sub%2finner.js", 404);   // an encoded slash is never a separator
    refused("GET", "/assets/..%5csecret.txt", 404);  // nor a backslash
    refused("GET", "/assets/index.html%00.png", 404);
    refused("GET", "/assets/%00", 404);
    refused("GET", "/assets/.", 404);
    refused("GET", "/assets//etc/passwd", 404);
    refused("GET", "/assets/%2fetc%2fpasswd", 404);
    refused("GET", "/assets/", 404);        // a directory: never listed
    refused("GET", "/assets/sub", 404);
    refused("GET", "/assets/sub/", 404);
    refused("GET", "/assets/app.js/", 404);   // a file named as a directory
    refused("GET", "/assets/media", 404);
    refused("GET", "/assets/missing.js", 404);
    refused("GET", "/assets/out", 403);     // a symlink out
    refused("GET", "/assets/subout/secret.txt", 403);
    refused("POST", "/assets/app.js", 405);
    refused("DELETE", "/assets/app.js", 405);
    refused("POST", "/any/app.js", 405);   // the helper's own rule, under a route of any method
    tc_response r;
    T_EQ_INT(req("PUT", "/any/app.js", NULL, NULL, 0, &r), 405);
    T_EQ_STR(hdr(&r, "allow"), "GET, HEAD");
    tc_response_free(&r);
    T_EQ_INT(req("GET", "/any/app.js", NULL, NULL, 0, &r), 200);
    tc_response_free(&r);
    refused("GET", "/media/..%2Fsecret.txt", 404);
    refused("GET", "/media/..%2F..%2Fsecret.txt", 404);
    refused("GET", "/media/%2e%2e", 404);
    refused("GET", "/media/%2Fetc%2Fpasswd", 404);
    refused("GET", "/media/link-out", 403);
    refused("GET", "/media/a%00b", 400);   // cb_route_param refuses a decoded NUL
    refused("GET", "/media/nothing.mp3", 404);
    refused("GET", "/media/..", 404);   // the router's own: ".." is a segment like any other
    T_EQ_INT(stop(SIGTERM), 0);
}

// A whole response off fd into r (the head's Content-Length worth of body; HEAD: none).
static int read_response(int fd, bool head, tc_response *r, cb_buf *in) {
    memset(r, 0, sizeof *r);
    for (;;) {
        char *he = in->len ? memmem(in->p, in->len, "\r\n\r\n", 4) : NULL;
        if (he) {
            const size_t hl = (size_t)(he - (char *)in->p) + 4;
            cb_buf_init(&r->head);
            cb_buf_append(&r->head, in->p, hl);
            if (sscanf((char *)in->p, "HTTP/1.%*d %d", &r->status) != 1) return -1;
            char cl[32];
            const size_t want = head ? 0 : tc_header(r, "content-length", cl, sizeof cl) ? (size_t)strtoull(cl, NULL, 10) : 0;
            while (in->len < hl + want) {
                struct pollfd p = { fd, POLLIN, 0 };
                if (!cb_buf_reserve(in, 1 << 20) || poll(&p, 1, 10000) <= 0) return -1;
                const ssize_t n = recv(fd, in->p + in->len, in->cap - in->len - 1, 0);
                if (n <= 0) return -1;
                in->len += (size_t)n;
            }
            cb_buf_init(&r->body);
            cb_buf_append(&r->body, in->p + hl, want);
            cb_buf_consume(in, hl + want);
            return 0;
        }
        struct pollfd p = { fd, POLLIN, 0 };
        if (!cb_buf_reserve(in, 65536) || poll(&p, 1, 10000) <= 0) return -1;
        const ssize_t n = recv(fd, in->p + in->len, in->cap - in->len - 1, 0);
        if (n <= 0) return -1;
        in->len += (size_t)n;
    }
}

// The server's own memory high-water mark, in bytes, or -1: on Linux its mm's VmHWM, on macOS its
// lifetime peak physical footprint (proc_pid_rusage). Each is the server's alone, so its growth
// past the mark before the files counts neither this test's buffers nor a sanitizer's own memory
// (on macOS the whole-process peak under ASan came to more than the bound).
static long long server_hwm(void) {
#ifdef __APPLE__
    struct rusage_info_v4 ri;
    if (proc_pid_rusage((int)g_child.pid, RUSAGE_INFO_V4, (rusage_info_t *)&ri) != 0) return -1;
    return (long long)ri.ri_lifetime_max_phys_footprint;
#else
    long long kb = -1;
    char path[64], line[256];
    snprintf(path, sizeof path, "/proc/%d/status", (int)g_child.pid);
    FILE *st = fopen(path, "r");
    while (st && fgets(line, sizeof line, st))
        if (strncmp(line, "VmHWM:", 6) == 0) kb = strtoll(line + 6, NULL, 10);
    if (st) fclose(st);
    return kb < 0 ? -1 : kb * 1024;
#endif
}

static void streams_large_files_and_head(void) {
    T_EQ_INT(start_web(), 0);
    tc_response warm;
    T_EQ_INT(req("GET", "/assets/app.js", NULL, NULL, 0, &warm), 200);   // its buffers in use once
    tc_response_free(&warm);
    const long long base = server_hwm();
    T_CHECK(base > 0);
    uint8_t *w = pattern(WASM_SIZE, 1), *b = pattern(BIG_SIZE, 2);
    tc_response r;
    T_EQ_INT(req("GET", "/assets/app.wasm", NULL, NULL, 0, &r), 200);
    T_EQ_INT((int)r.body.len, (int)WASM_SIZE);
    T_CHECK(r.body.len == WASM_SIZE && memcmp(r.body.p, w, WASM_SIZE) == 0);
    tc_response_free(&r);
    T_EQ_INT(req("GET", "/assets/big.bin", NULL, NULL, 0, &r), 200);
    T_EQ_INT((int)r.body.len, (int)BIG_SIZE);
    T_CHECK(r.body.len == BIG_SIZE && memcmp(r.body.p, b, BIG_SIZE) == 0);
    tc_response_free(&r);

    // HEAD: GET's head and Content-Length, then no body: the next answer on the same connection
    // starts right after the head.
    int fd = tc_connect(g_port);
    T_CHECK(fd >= 0);
    T_EQ_INT(tc_request(fd, "HEAD", "/assets/app.wasm", NULL, NULL, 0, 10000, &r), 0);
    T_EQ_INT(r.status, 200);
    T_EQ_STR(hdr(&r, "content-length"), "6291456");
    T_EQ_STR(hdr(&r, "content-type"), "application/wasm");
    T_EQ_INT((int)r.body.len, 0);
    tc_response_free(&r);
    T_EQ_INT(tc_request(fd, "HEAD", "/decks", NULL, NULL, 0, 10000, &r), 0);
    T_EQ_INT(r.status, 200);
    T_EQ_STR(hdr(&r, "content-length"), "34");
    T_EQ_INT((int)r.body.len, 0);
    tc_response_free(&r);
    // A file answer made a 304: no Content-Length and none of the file's bytes, so the next
    // answer on the connection is the next request's.
    T_EQ_INT(tc_request(fd, "GET", "/notmod", NULL, NULL, 0, 10000, &r), 0);
    T_EQ_INT(r.status, 304);
    T_EQ_STR(hdr(&r, "content-length"), "");
    T_EQ_INT((int)r.body.len, 0);
    tc_response_free(&r);
    T_EQ_INT(tc_request(fd, "GET", "/assets/style.css", NULL, NULL, 0, 10000, &r), 0);
    T_EQ_INT(r.status, 200);
    T_EQ_STR(cb_buf_cstr(&r.body), "body{margin:0}");
    tc_response_free(&r);
    close(fd);

    // Pipelined: the big file and two more requests in one write. Each answer comes whole and in
    // order; none lands inside the file's bytes.
    fd = tc_connect(g_port);
    const char *three = "GET /assets/big.bin HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n"
                        "HEAD /assets/app.wasm HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n"
                        "GET /assets/app.js HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
    T_EQ_INT((int)send(fd, three, strlen(three), 0), (int)strlen(three));
    cb_buf in;
    cb_buf_init(&in);
    T_EQ_INT(read_response(fd, false, &r, &in), 0);
    T_EQ_INT(r.status, 200);
    T_CHECK(r.body.len == BIG_SIZE && memcmp(r.body.p, b, BIG_SIZE) == 0);
    tc_response_free(&r);
    T_EQ_INT(read_response(fd, true, &r, &in), 0);
    T_EQ_INT(r.status, 200);
    T_EQ_STR(hdr(&r, "content-length"), "6291456");
    tc_response_free(&r);
    T_EQ_INT(read_response(fd, false, &r, &in), 0);
    T_EQ_INT(r.status, 200);
    T_EQ_STR(cb_buf_cstr(&r.body), "export const x = 1;\n");
    tc_response_free(&r);
    T_EQ_INT((int)in.len, 0);
    cb_buf_free(&in);
    close(fd);
    free(w);
    free(b);
    // Streamed, not held: this server, having sent its 40 MB file four times, never grew by the
    // file's size past what it held before the files.
    const long long peak = server_hwm();
    T_CHECK(peak > 0);
    const long long grew = peak - base;
    T_EQ_INT(stop(SIGTERM), 0);
    fprintf(stderr, "     the server's resident growth: %lld KB\n", grew / 1024);
    T_CHECK(grew < (long long)BIG_SIZE / 2);
}

// A FIFO in the directory is no file, and asking for one costs nothing: open() on a FIFO with no
// writer blocks until one comes, so cb_send_file opens with O_NONBLOCK, and the FIFO fails its
// regular-file check at once. serve has one request worker (config.c THE THREADS): were it held
// in open(), the next request would never be answered.
static void a_fifo_is_no_file(void) {
    char p[1024];
    snprintf(p, sizeof p, "%s/pipe", g_dir);
    T_EQ_INT(mkfifo(p, 0644), 0);
    snprintf(p, sizeof p, "%s/media/pipe", g_dir);
    T_EQ_INT(mkfifo(p, 0644), 0);
    T_EQ_INT(start_web(), 0);
    static const char *const paths[] = { "/assets/pipe", "/media/pipe", "/assets/pipe" };
    for (size_t i = 0; i < sizeof paths / sizeof paths[0]; i++) {
        const int fd = tc_connect(g_port);
        T_CHECK(fd >= 0);
        tc_response r;
        const int64_t t0 = cb_mono_ms();
        const int rc = tc_request(fd, "GET", paths[i], NULL, NULL, 0, 3000, &r);
        const int64_t ms = cb_mono_ms() - t0;
        close(fd);
        if (rc != 0 || ms >= 1000) fprintf(stderr, "     GET %s: rc %d after %lld ms\n", paths[i], rc, (long long)ms);
        T_EQ_INT(rc, 0);
        T_EQ_INT(r.status, 404);
        T_CHECK(ms < 1000);
        tc_response_free(&r);
    }
    // The worker is free: a file right behind them.
    tc_response r;
    T_EQ_INT(req("GET", "/assets/app.js", NULL, NULL, 0, &r), 200);
    T_EQ_STR(cb_buf_cstr(&r.body), "export const x = 1;\n");
    tc_response_free(&r);
    T_EQ_INT(stop(SIGTERM), 0);
    // Off serve, a device is no file either. (The FIFO is not tried here: without O_NONBLOCK this
    // process, not a server, would wait in open() for ever, where the checks above fail in 3 s.)
    cb_http_req q;
    memset(&q, 0, sizeof q);
    strcpy(q.method, "GET");
    q.path = "/x";
    q.query = "";
    cb_http_res res;
    cb_res_init(&res);
    T_EQ_INT(cb_send_file(NULL, &q, &res, "/dev/null", NULL, NULL), 404);
    cb_res_free(&res);
}

// A rebuild on disk is what the next request gets: nothing is cached.
static void reads_the_file_per_request(void) {
    T_EQ_INT(start_web(), 0);
    tc_response r;
    T_EQ_INT(req("GET", "/decks", NULL, NULL, 0, &r), 200);
    T_EQ_STR(cb_buf_cstr(&r.body), "<!doctype html><title>amgi</title>");
    tc_response_free(&r);
    put("index.html", "<!doctype html><title>amgi 2</title>", 36);
    T_EQ_INT(req("GET", "/decks", NULL, NULL, 0, &r), 200);
    T_EQ_STR(cb_buf_cstr(&r.body), "<!doctype html><title>amgi 2</title>");
    tc_response_free(&r);
    T_EQ_INT(stop(SIGTERM), 0);
}

// Off serve (a one-shot platform, a socket's REQ: stream_ok false), the same call reads the
// file whole into the body; the join and the content types in this process.
// ---- the Host allow-list ------------------------------------------------------------------------

// Sends `text` (a whole request, every header its own) on a fresh connection and reads one answer.
static int raw(const char *text, tc_response *r) {
    memset(r, 0, sizeof *r);
    const int fd = tc_connect(g_port);
    if (fd < 0) return -1;
    cb_buf in;
    cb_buf_init(&in);
    int rc = send(fd, text, strlen(text), 0) == (ssize_t)strlen(text) ? read_response(fd, false, r, &in) : -1;
    cb_buf_free(&in);
    close(fd);
    return rc == 0 ? r->status : -1;
}

// GET path with exactly the Host line `host` ("" for none): the status.
static int with_host(const char *path, const char *host) {
    char t[1024];
    snprintf(t, sizeof t, "GET %s HTTP/1.1\r\n%s%s%sConnection: close\r\n\r\n", path, host[0] ? "Host: " : "", host,
             host[0] ? "\r\n" : "");
    tc_response r;
    const int st = raw(t, &r);
    // A refusal is the platform's own, never the handler's answer.
    if (st == 400 || st == 403) T_CHECK(!strstr(cb_buf_cstr(&r.body), "doctype"));
    tc_response_free(&r);
    return st;
}

static void holds_requests_to_the_host_list(void) {
    static char env[600];
    snprintf(env, sizeof env, "WEB_DIR=%s", g_dir);
    T_EQ_INT(start("hosts", env), 0);
    char h[128];
    // Listed, at the port it bound: the host in any case, IPv6 in brackets.
    snprintf(h, sizeof h, "127.0.0.1:%d", g_port);
    T_EQ_INT(with_host("/decks", h), 200);
    snprintf(h, sizeof h, "localhost:%d", g_port);
    T_EQ_INT(with_host("/decks", h), 200);
    snprintf(h, sizeof h, "LocalHost:%d", g_port);
    T_EQ_INT(with_host("/decks", h), 200);
    snprintf(h, sizeof h, "[::1]:%d", g_port);
    T_EQ_INT(with_host("/decks", h), 200);
    T_EQ_INT(with_host("/decks", "app.example.test:9999"), 200);   // its own port, not the bound one
    // An attacker's name that resolves to 127.0.0.1: refused on every kind of route.
    snprintf(h, sizeof h, "evil.example:%d", g_port);
    T_EQ_INT(with_host("/decks", h), 403);
    T_EQ_INT(with_host("/assets/app.js", h), 403);
    T_EQ_INT(with_host("/media/a.mp3", h), 403);
    T_EQ_INT(with_host("/health", h), 403);
    T_EQ_INT(with_host("/no/such/route", h), 403);
    T_EQ_INT(with_host("/decks", "evil.example"), 403);
    snprintf(h, sizeof h, "127.0.0.1.evil.example:%d", g_port);
    T_EQ_INT(with_host("/decks", h), 403);
    // A listed name at another port, or none (80), is another origin.
    snprintf(h, sizeof h, "127.0.0.1:%d", g_port + 1);
    T_EQ_INT(with_host("/decks", h), 403);
    T_EQ_INT(with_host("/decks", "127.0.0.1"), 403);
    snprintf(h, sizeof h, "app.example.test:%d", g_port);
    T_EQ_INT(with_host("/decks", h), 403);
    snprintf(h, sizeof h, "[::2]:%d", g_port);
    T_EQ_INT(with_host("/decks", h), 403);
    // No Host, two, or one that is no host[:port]: 400 (RFC 9112 3.2).
    T_EQ_INT(with_host("/decks", ""), 400);
    snprintf(h, sizeof h, "127.0.0.1:%d\r\nHost: evil.example:%d", g_port, g_port);
    T_EQ_INT(with_host("/decks", h), 400);
    snprintf(h, sizeof h, "127.0.0.1:%d\r\nHost: 127.0.0.1:%d", g_port, g_port);
    T_EQ_INT(with_host("/decks", h), 400);
    snprintf(h, sizeof h, "127.0.0.1:%dx", g_port);
    T_EQ_INT(with_host("/decks", h), 400);
    T_EQ_INT(with_host("/decks", "127.0.0.1:123456"), 400);
    T_EQ_INT(with_host("/decks", "user@127.0.0.1"), 400);
    T_EQ_INT(with_host("/decks", "[::1"), 400);
    T_EQ_INT(with_host("/decks", ":80"), 400);
    tc_response r;
    T_EQ_INT(raw("GET /decks HTTP/1.0\r\n\r\n", &r), 400);   // HTTP/1.0 with no Host is held to the list too
    tc_response_free(&r);

    // A WebSocket upgrade: 101 from a listed Host, 403 and no 101 from any other.
    char up[600];
    const char *ws = "GET /ws HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                     "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
    snprintf(h, sizeof h, "localhost:%d", g_port);
    snprintf(up, sizeof up, ws, h);
    T_EQ_INT(raw(up, &r), 101);
    tc_response_free(&r);
    snprintf(h, sizeof h, "evil.example:%d", g_port);
    snprintf(up, sizeof up, ws, h);
    T_EQ_INT(raw(up, &r), 403);
    T_CHECK(!strstr(cb_buf_cstr(&r.head), "Upgrade: websocket"));
    tc_response_free(&r);
    T_EQ_INT(stop(SIGTERM), 0);

    // No list: any Host goes ahead, as before.
    T_EQ_INT(start("plain", env), 0);
    T_EQ_INT(with_host("/decks", "evil.example"), 200);
    T_EQ_INT(with_host("/decks", ""), 200);
    T_EQ_INT(stop(SIGTERM), 0);

    // A list entry that is no host[:port] does not start: the sentence, and exit 1.
    T_EQ_INT(start("badhost", env), -1);
    char line[512];
    T_EQ_INT(tp_wait_line(&g_child, "is not host[:port]", 20000, line, sizeof line), 0);
    T_CHECK(strstr(line, "http://localhost:8080") != NULL);
    T_EQ_INT(stop(0), 1);
}

static void reads_whole_off_serve(void) {
    char p[1100];
    snprintf(p, sizeof p, "%s/app.wasm", g_dir);
    cb_http_req q;
    memset(&q, 0, sizeof q);
    strcpy(q.method, "GET");
    q.path = "/x";
    q.query = "";
    cb_http_res res;
    cb_res_init(&res);
    T_EQ_INT(cb_send_file(NULL, &q, &res, p, NULL, NULL), 200);
    T_CHECK(!res.file);
    uint8_t *w = pattern(WASM_SIZE, 1);
    T_CHECK(res.body.len == WASM_SIZE && memcmp(res.body.p, w, WASM_SIZE) == 0);
    free(w);
    cb_buf out;
    cb_buf_init(&out);
    cb_res_serialize(&res, true, &out);
    T_CHECK(memmem(out.p, out.len, "Content-Length: 6291456\r\n", 25) != NULL);
    T_EQ_INT((int)out.len, (int)(strstr((char *)out.p, "\r\n\r\n") - (char *)out.p) + 4 + (int)WASM_SIZE);
    cb_buf_free(&out);
    cb_res_free(&res);
    // A directory is no file.
    cb_res_init(&res);
    T_EQ_INT(cb_send_file(NULL, &q, &res, g_dir, NULL, NULL), 404);
    cb_res_free(&res);

    T_EQ_INT(cb_path_join(g_dir, "app.js", p, sizeof p), CB_OK);
    T_CHECK(strstr(p, "/web/app.js") != NULL);
    static const char *const bad[] = { "", ".", "..", "../secret.txt", "sub/inner.js", "/etc/passwd", "a\\b", "a\nb", "a\x7f" };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) T_EQ_INT(cb_path_join(g_dir, bad[i], p, sizeof p), CB_E_INVALID);
    T_EQ_INT(cb_path_join(g_dir, "out", p, sizeof p), CB_E_FORBIDDEN);
    T_EQ_INT(cb_path_join(g_dir, "nothing", p, sizeof p), CB_E_NOT_FOUND);
    T_EQ_INT(cb_path_join(g_dir, "app.js", p, 8), CB_E_FULL);
    T_EQ_STR(cb_mime_type(NULL, "a/b.WASM"), "application/wasm");
    T_EQ_STR(cb_mime_type(NULL, "a.b/noext"), "application/octet-stream");
    T_EQ_STR(cb_mime_type(NULL, "trailing."), "application/octet-stream");
    T_EQ_STR(cb_mime_type(&g_app, "x.txt"), "text/x-amgi-notes");
    T_EQ_STR(cb_mime_type(NULL, "x.txt"), "text/plain; charset=utf-8");
}

int main(int argc, char **argv) {
    g_self = argv[0];
    if (argc == 3 && strcmp(argv[1], "serve-web") == 0) return child_main(argv[2]);
    signal(SIGPIPE, SIG_IGN);
    T_RUN(serves_content_types);
    T_RUN(single_page_routes);
    T_RUN(refuses_every_way_out);
    T_RUN(streams_large_files_and_head);
    T_RUN(reads_the_file_per_request);
    T_RUN(a_fifo_is_no_file);
    T_RUN(holds_requests_to_the_host_list);
    T_RUN(reads_whole_off_serve);
    return T_DONE();
}

#else

int main(void) {
    T_CHECK(1);   // the local provider is not linked: there is no serve to run
    return T_DONE();
}

#endif
