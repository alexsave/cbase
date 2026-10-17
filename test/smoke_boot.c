// smoke_boot.c - the two binaries refusing to start, as an operator meets it.
//
//   build/test/smoke_boot build/cbase build/bootstrap
//
// `cbase` with no command, a setting that does not parse, a database it cannot
// open and a port already taken; `bootstrap` outside Lambda, and under a
// Runtime API (a socket here) with a bad setting and with a store it cannot
// open, where it must tell Lambda through /runtime/init/error, typed, before
// it exits. Each exits non-zero with one line that says why.
#include "t.h"
#include "lib_proc.h"

#include "util/json.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static const char *g_cbase, *g_bootstrap;

// Runs `bin` with `args` and `env` to its exit; true when it exits `status`
// having said `says` on stderr.
static bool exits(const char *bin, const char *arg, char *const env[], int status, const char *says) {
    tp_child c;
    char *argv[] = { (char *)bin, (char *)arg, NULL };
    if (!arg) argv[1] = NULL;
    if (tp_start(&c, argv, env) != 0) return false;
    char line[512] = "";
    int found = says ? tp_wait_line(&c, says, 15000, line, sizeof line) : 0;
    int st = tp_stop(&c, 0, 15000);
    if (found != 0 || st != status) fprintf(stderr, "     %s %s: exit %d (want %d), \"%s\" %s\n", bin, arg ? arg : "", st, status, says, found ? "never said" : "said");
    return found == 0 && st == status;
}

static void cbase_refuses(void) {
    char *none[] = { NULL };
    T_CHECK(exits(g_cbase, NULL, none, 2, "usage: cbase serve"));
    T_CHECK(exits(g_cbase, "nonsense", none, 2, "usage: cbase serve"));
    char *bad_listen[] = { "CBASE_LISTEN=nowhere", NULL };
    T_CHECK(exits(g_cbase, "serve", bad_listen, 2, "CBASE_LISTEN=nowhere is not host:port"));
    char *bad_db[] = { "CBASE_LISTEN=127.0.0.1:0", "CBASE_STORE=sqlite", "CBASE_DB=/dev/null/cbase.sqlite", NULL };
    T_CHECK(exits(g_cbase, "serve", bad_db, 1, "store sqlite"));
    // A port someone else holds.
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t al = sizeof a;
    T_EQ_INT(bind(fd, (struct sockaddr *)&a, sizeof a), 0);
    T_EQ_INT(listen(fd, 1), 0);
    T_EQ_INT(getsockname(fd, (struct sockaddr *)&a, &al), 0);
    char listen_env[64];
    snprintf(listen_env, sizeof listen_env, "CBASE_LISTEN=127.0.0.1:%d", ntohs(a.sin_port));
    char *busy[] = { listen_env, "CBASE_STORE=sqlite", "CBASE_DB=:memory:", NULL };
    T_CHECK(exits(g_cbase, "serve", busy, 1, "listen"));
    close(fd);
}

// One request to the fake Runtime API, recorded, answered 202.
typedef struct { int fd; char req[4096]; } one_req;
static void *take_one(void *ud) {
    one_req *o = ud;
    int c = accept(o->fd, NULL, NULL);
    if (c < 0) return NULL;
    size_t n = 0;
    while (n + 1 < sizeof o->req) {
        ssize_t r = recv(c, o->req + n, sizeof o->req - 1 - n, 0);
        if (r <= 0) break;
        n += (size_t)r;
        o->req[n] = 0;
        const char *h = strstr(o->req, "\r\n\r\n"), *cl = strstr(o->req, "Content-Length: ");
        if (h && cl && n >= (size_t)(h + 4 - o->req) + (size_t)atoi(cl + 16)) break;
    }
    static const char OK[] = "HTTP/1.1 202 Accepted\r\nContent-Length: 0\r\n\r\n";
    if (send(c, OK, sizeof OK - 1, 0) < 0) {}
    close(c);
    return NULL;
}

// bootstrap under a Runtime API that hears one request: the init error it posts.
// `says`: what the error message holds (and stderr, when `logged`).
static bool init_error(char *const env_extra[], const char *type, const char *says, bool logged) {
    one_req o;
    memset(&o, 0, sizeof o);
    o.fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t al = sizeof a;
    if (bind(o.fd, (struct sockaddr *)&a, sizeof a) != 0 || listen(o.fd, 1) != 0 || getsockname(o.fd, (struct sockaddr *)&a, &al) != 0) return false;
    char api[64];
    snprintf(api, sizeof api, "AWS_LAMBDA_RUNTIME_API=127.0.0.1:%d", ntohs(a.sin_port));
    char *env[8];
    int k = 0;
    env[k++] = api;
    for (int i = 0; env_extra[i] && k < 7; i++) env[k++] = env_extra[i];
    env[k] = NULL;
    pthread_t th;
    pthread_create(&th, NULL, take_one, &o);
    bool ok = exits(g_bootstrap, NULL, env, 1, logged ? says : NULL);
    shutdown(o.fd, SHUT_RDWR);
    close(o.fd);
    pthread_join(th, NULL);
    static const char LINE[] = "POST /2018-06-01/runtime/init/error HTTP/1.1\r\n";
    T_CHECK(strncmp(o.req, LINE, sizeof LINE - 1) == 0);
    const char *body = strstr(o.req, "\r\n\r\n");
    char v[256] = "";
    T_CHECK(body && cb_json_get_str(body + 4, strlen(body + 4), "errorType", v, sizeof v));
    T_EQ_STR(v, type);
    T_CHECK(body && cb_json_get_str(body + 4, strlen(body + 4), "errorMessage", v, sizeof v) && strstr(v, says) != NULL);
    return ok;
}

static void bootstrap_refuses(void) {
    char *none[] = { NULL };
    // Run where the variable is unset (the lane may export one for other tests).
    unsetenv("AWS_LAMBDA_RUNTIME_API");
    T_CHECK(exits(g_bootstrap, NULL, none, 2, "AWS_LAMBDA_RUNTIME_API is not set"));
    char *bad_config[] = { "CBASE_STORE=postgres", NULL };
    // Before the log exists: said to Lambda only, which logs it as the init failure.
    T_CHECK(init_error(bad_config, "Config", "CBASE_STORE=postgres", false));
    char *bad_boot[] = { "CBASE_STORE=sqlite", "CBASE_DB=/dev/null/cbase.sqlite", NULL };
    T_CHECK(init_error(bad_boot, "Boot", "store sqlite", true));
}

int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "usage: smoke_boot <path to cbase> <path to bootstrap>\n"); return 2; }
    g_cbase = argv[1];
    g_bootstrap = argv[2];
    signal(SIGPIPE, SIG_IGN);
    T_RUN(cbase_refuses);
    T_RUN(bootstrap_refuses);
    return T_DONE();
}
