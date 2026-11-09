// unit_socket_activation.c - `cbase serve` on a listening socket it inherits (systemd's socket
// activation, net/server.c THE INHERITED LISTENER), restarted on that socket the way a deploy
// restarts it (vm_unit.socket): no connection is refused across the restart, the request that was
// arriving when the drain began is answered, and the connections made while no serve ran are
// answered by the next one.
//
// The test is the service manager: it makes the listening socket and keeps its own copy for the
// whole test, and each serve is this program ITSELF (lib_proc.h) in its `serve-activated` mode,
// which does what systemd does between fork and exec (the socket at fd 3, LISTEN_FDS=1,
// LISTEN_PID its own pid) and then runs the serve ingress (cb_process_main).
#include "t.h"
#include "lib_proc.h"

#include "cbase/app.h"
#include "cbase/provider.h"
#include "ctx.h"
#include "ingress_process.h"
#include "net/http.h"
#include "num_parse.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#ifdef CBASE_LINKS_LOCAL

// ---- the server's app ---------------------------------------------------------------------------

// GET /t/pid: which serve answered.
static void h_pid(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    char b[32];
    snprintf(b, sizeof b, "%d", (int)getpid());
    cb_res_text(res, 200, b);
}

// GET /t/slow: a request still in flight when the drain begins.
static void h_slow(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    struct timespec ts = { 0, 600 * 1000000L };
    nanosleep(&ts, NULL);
    cb_res_text(res, 200, "slow");
}

static const cb_route ROUTES[] = {
    { .method = "GET", .pattern = "/t/pid", .flags = CB_ROUTE_POOL, .fn = h_pid },
    { .method = "GET", .pattern = "/t/slow", .flags = CB_ROUTE_POOL, .fn = h_slow },
};
static const cb_route_list LIST = { ROUTES, 2 };
static const cb_route_list *const LISTS[] = { &LIST };
static const cb_app APP = { .name = "socket-activation", .routes = LISTS, .n_route_lists = 1, .storeless = true };

// ---- the test -----------------------------------------------------------------------------------

static const char *g_self;
static int g_lfd = -1, g_port;

static void ms(int n) {
    struct timespec ts = { n / 1000, (long)(n % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

// A connection to the socket: the kernel completes it from the backlog whether or not a serve is
// running. The fd, or -1 (refused).
static int dial(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)g_port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (fd >= 0 && connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) { close(fd); fd = -1; }
    return fd;
}

static bool say(int fd, const char *s) { return fd >= 0 && write(fd, s, strlen(s)) == (ssize_t)strlen(s); }

// The response on fd, read to its close within timeout_ms: its status, its body into body. -1 when
// none came.
static int answer(int fd, int timeout_ms, char *body, size_t cap) {
    char b[4096];
    size_t n = 0;
    body[0] = 0;
    for (int waited = 0; fd >= 0 && n < sizeof b - 1 && waited < timeout_ms;) {
        struct pollfd p = { fd, POLLIN, 0 };
        int r = poll(&p, 1, 100);
        if (r < 0 && errno == EINTR) continue;
        if (r == 0) { waited += 100; continue; }
        ssize_t got = read(fd, b + n, sizeof b - 1 - n);
        if (got <= 0) break;
        n += (size_t)got;
    }
    b[n] = 0;
    int64_t status = 0;
    if (n < 12 || strncmp(b, "HTTP/1.1 ", 9) != 0 || !num_parse_i64(b + 9, 3, 100, 599, &status)) return -1;
    const char *e = strstr(b, "\r\n\r\n");
    snprintf(body, cap, "%s", e ? e + 4 : "");
    return (int)status;
}

static const char *const GET_PID = "GET /t/pid HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";

// One serve on the test's socket: started, and its startup line read (the port it names is the
// socket's).
static void serve_on_the_socket(tp_child *c) {
    char fdenv[32];
    snprintf(fdenv, sizeof fdenv, "T_LISTEN_FD=%d", g_lfd);
    char *env[] = { "CBASE_DEV=1", "CBASE_LISTEN=127.0.0.1:0", "CBASE_JWT_SECRET=socket-activation-secret-socket-act", "CBASE_STORE=",
                    "CBASE_REQ_WORKERS=2", fdenv, NULL };
    char *argv[] = { (char *)g_self, "serve-activated", NULL };
    T_EQ_INT(tp_start(c, argv, env), 0);
    char line[512] = "";
    T_EQ_INT(tp_wait_line(c, "serve: listening on", 20000, line, sizeof line), 0);
    const char *colon = strrchr(line, ':');
    int64_t port = 0;
    T_CHECK(colon && num_parse_i64(colon + 1, strcspn(colon + 1, " \r\n"), 1, 65535, &port));
    T_EQ_INT((int)port, g_port);   // the inherited socket's, not a port of its own
}

static void a_restart_on_the_socket_refuses_nobody(void) {
    g_lfd = socket(AF_INET, SOCK_STREAM, 0);
    T_CHECK(g_lfd >= 0);
    int one = 1;
    setsockopt(g_lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    T_EQ_INT(bind(g_lfd, (struct sockaddr *)&sa, sizeof sa), 0);
    T_EQ_INT(listen(g_lfd, 64), 0);
    socklen_t sl = sizeof sa;
    getsockname(g_lfd, (struct sockaddr *)&sa, &sl);
    g_port = ntohs(sa.sin_port);

    tp_child a;
    serve_on_the_socket(&a);
    char body[256], apid[32];
    int fd = dial();
    T_CHECK(say(fd, GET_PID));
    T_EQ_INT(answer(fd, 10000, body, sizeof body), 200);
    close(fd);
    snprintf(apid, sizeof apid, "%d", (int)a.pid);
    T_EQ_STR(body, apid);

    // The drain begins with one request in flight and one half arrived.
    int slow = dial(), half = dial();
    T_CHECK(say(slow, "GET /t/slow HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"));
    T_CHECK(say(half, "GET /t/pid HTTP/1.1\r\nHost: x\r\n"));
    ms(200);   // both read by the serve
    T_EQ_INT(kill(a.pid, SIGTERM), 0);
    char line[512];
    T_EQ_INT(tp_wait_line(&a, "draining", 10000, line, sizeof line), 0);
    // A connection while the old serve drains: it has closed its listener, and the socket queues it.
    int during = dial();
    T_CHECK(during >= 0);
    T_CHECK(say(during, GET_PID));
    T_CHECK(say(half, "Connection: close\r\n\r\n"));
    T_EQ_INT(answer(half, 10000, body, sizeof body), 200);   // the half-arrived request, answered by the old serve
    T_EQ_STR(body, apid);
    T_EQ_INT(answer(slow, 10000, body, sizeof body), 200);
    T_EQ_STR(body, "slow");
    close(half);
    close(slow);
    T_EQ_INT(tp_stop(&a, 0, 15000), 0);

    // A connection while no serve runs at all: queued, not refused.
    int gap = dial();
    T_CHECK(gap >= 0);
    T_CHECK(say(gap, GET_PID));

    tp_child b;
    serve_on_the_socket(&b);
    char bpid[32];
    snprintf(bpid, sizeof bpid, "%d", (int)b.pid);
    T_EQ_INT(answer(during, 10000, body, sizeof body), 200);
    T_EQ_STR(body, bpid);
    T_EQ_INT(answer(gap, 10000, body, sizeof body), 200);
    T_EQ_STR(body, bpid);
    close(during);
    close(gap);
    T_EQ_INT(tp_stop(&b, SIGTERM, 15000), 0);
    close(g_lfd);
}

// What systemd does between fork and exec: the socket at fd 3 and the two variables naming it.
static int serve_activated(void) {
    int64_t fd = 0;
    if (!num_parse_i64_cstr(getenv("T_LISTEN_FD"), 0, 1023, &fd)) return 2;
    if (fd != 3 && (dup2((int)fd, 3) != 3 || close((int)fd) != 0)) return 2;
    char pid[32];
    snprintf(pid, sizeof pid, "%d", (int)getpid());
    if (setenv("LISTEN_FDS", "1", 1) != 0 || setenv("LISTEN_PID", pid, 1) != 0) return 2;
    char *sargv[] = { (char *)g_self, "serve", NULL };
    return cb_process_main(&cb_provider_local, &APP, 2, sargv);
}

int main(int argc, char **argv) {
    g_self = argv[0];
    if (argc == 2 && strcmp(argv[1], "serve-activated") == 0) return serve_activated();
    signal(SIGPIPE, SIG_IGN);
    T_RUN(a_restart_on_the_socket_refuses_nobody);
    return T_DONE();
}

#else

int main(void) {
    T_CHECK(1);   // the local provider is not linked: there is no serve to restart
    return T_DONE();
}

#endif
