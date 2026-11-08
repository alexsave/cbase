// smoke_boot.c - the two binaries refusing to start, as an operator meets it.
//
//   build/test/smoke_boot build/cbase build/bootstrap
//   build/test/smoke_boot build/cbase        (a build with the local provider alone)
//   build/test/smoke_boot - build/bootstrap  (a build with the aws provider alone)
//
// `cbase` with no command, a setting that does not parse, a database it cannot
// open and a port already taken; `bootstrap` outside Lambda, and under a
// Runtime API (a socket here) with a bad setting and with a store it cannot
// open, where it must tell Lambda through /runtime/init/error, typed, before
// it exits. Each exits non-zero with one line that says why.
//
// It holds for every product, and adapts to the app the product links (cb_main_app, the same app
// the binaries serve): a STORELESS app (cbase/app.h storeless) opens no store, so every boot here
// runs without CBASE_STORE and CBASE_DB (store_env), and in place of the store's refusals it checks
// the storeless boot: outside dev it starts with no JWT secret and no public URL (it signs nothing
// and issues nothing), says its startup line, makes no database file, and refuses CBASE_STORE with
// its sentence. An app with a store gets the store's refusals, as ever.
//
// No serve default a product may change is assumed (cbase/app.h). Every child is started through
// spawn, which sets CBASE_LOG_LEVEL=info: the lines waited for are INFO, and the variable beats
// the app's log_level by design, so they are written whatever level the app ships. Every request
// carries a Host the app's allow-list answers (probe_host), and GET /health is held to what the
// app declares (the_port_answers): the platform's 200 when it declares health_route, and else
// what the app's own routes make of the path, its 404 for a path it does not route.
#include "t.h"
#include "lib_proc.h"

#include "cbase/app.h"
#include "net/router.h"
#include "util/json.h"

#include <dirent.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static const char *g_cbase, *g_bootstrap;
static bool g_storeless;   // cb_main_app keeps no platform store

// `env` as this app boots with it: for a storeless app its CBASE_STORE and CBASE_DB lines dropped
// (in place), since naming a store is what such an app refuses. The one place the two kinds differ
// in what every boot below is given.
static char **store_env(char **env) {
    if (!g_storeless) return env;
    int k = 0;
    for (int i = 0; env[i]; i++)
        if (strncmp(env[i], "CBASE_STORE=", 12) != 0 && strncmp(env[i], "CBASE_DB=", 9) != 0) env[k++] = env[i];
    env[k] = NULL;
    return env;
}

// THE ONE WAY A CHILD STARTS: `env` and CBASE_LOG_LEVEL=info after it, whatever log_level the app
// ships (an app's "warn" would keep every line this smoke waits for unwritten), with its stdout a
// pipe (c->out_fd) when `out`. Every cbase and bootstrap this smoke runs starts here.
static int spawn(tp_child *c, char *const argv[], char *const env[], bool out) {
    char *e[32];
    int k = 0;
    for (int i = 0; env[i]; i++) {
        if (k == 30) return -1;
        e[k++] = env[i];
    }
    e[k++] = "CBASE_LOG_LEVEL=info";
    e[k] = NULL;
    return out ? tp_start_out(c, argv, e) : tp_start(c, argv, e);
}

// Runs `bin` with `args` and `env` to its exit; true when it exits `status`
// having said `says` on stderr.
static bool exits(const char *bin, const char *arg, char *const env[], int status, const char *says) {
    tp_child c;
    char *argv[] = { (char *)bin, (char *)arg, NULL };
    if (!arg) argv[1] = NULL;
    if (spawn(&c, argv, env, false) != 0) return false;
    char line[512] = "";
    int found = says ? tp_wait_line(&c, says, 15000, line, sizeof line) : 0;
    int st = tp_stop(&c, 0, 15000);
    if (found != 0 || st != status) fprintf(stderr, "     %s %s: exit %d (want %d), \"%s\" %s\n", bin, arg ? arg : "", st, status, says, found ? "never said" : "said");
    return found == 0 && st == status;
}

// The names in dir but . and .., counted.
static int entries(const char *dir) {
    DIR *d = opendir(dir);
    int n = 0;
    for (struct dirent *e; d && (e = readdir(d));) n += strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0;
    if (d) closedir(d);
    return d ? n : -1;
}

// A storeless app's boot, outside dev: no JWT secret and no public URL asked for, the startup line,
// no database file where CBASE_DB would put one; and CBASE_STORE named is refused with its sentence.
static void storeless_boots(void) {
    char dir[256], db[400];
    T_EQ_INT(tp_tempdir(dir, sizeof dir), 0);
    snprintf(db, sizeof db, "CBASE_DB=%s/cbase.sqlite", dir);
    char *env[] = { "CBASE_LISTEN=127.0.0.1:0", db, NULL };
    char *argv[] = { (char *)g_cbase, "serve", NULL };
    tp_child c;
    T_EQ_INT(spawn(&c, argv, env, true), 0);
    char line[256] = "";
    size_t n = 0;
    struct pollfd p = { c.out_fd, POLLIN, 0 };
    while (n + 1 < sizeof line && !memchr(line, '\n', n) && poll(&p, 1, 15000) == 1) {
        ssize_t r = read(c.out_fd, line + n, sizeof line - 1 - n);
        if (r <= 0) break;
        n += (size_t)r;
        line[n] = 0;
    }
    if (!strstr(line, "cbase: listening 127.0.0.1:")) fprintf(stderr, "     storeless serve said \"%s\" on stdout\n", line);
    T_CHECK(strstr(line, "cbase: listening 127.0.0.1:") != NULL);
    T_EQ_INT(tp_stop(&c, SIGTERM, 15000), 0);
    close(c.out_fd);
    T_EQ_INT(entries(dir), 0);   // no database file, nor its -wal or -shm
    rmdir(dir);
    char want[256];
    snprintf(want, sizeof want, "CBASE_STORE=sqlite names a store, and the app %s keeps none", cb_main_app->name ? cb_main_app->name : "");
    char *named[] = { "CBASE_LISTEN=127.0.0.1:0", "CBASE_STORE=sqlite", "CBASE_DB=:memory:", "CBASE_DEV=1", NULL };
    T_CHECK(exits(g_cbase, "serve", named, 1, want));
}

static void cbase_refuses(void) {
    char *none[] = { NULL };
    T_CHECK(exits(g_cbase, NULL, none, 2, "usage: cbase serve"));
    T_CHECK(exits(g_cbase, "nonsense", none, 2, "usage: cbase serve"));
    char *bad_listen[] = { "CBASE_LISTEN=nowhere", NULL };
    T_CHECK(exits(g_cbase, "serve", bad_listen, 2, "CBASE_LISTEN=nowhere is not host:port"));
    if (g_storeless) storeless_boots();
    else {
        // Without CBASE_DEV=1 serve wants the JWT secret and the public URL before it does anything.
        char *no_secret[] = { "CBASE_LISTEN=127.0.0.1:0", "CBASE_STORE=sqlite", "CBASE_DB=:memory:", NULL };
        T_CHECK(exits(g_cbase, "serve", no_secret, 2, "no CBASE_JWT_SECRET"));
        char *no_url[] = { "CBASE_LISTEN=127.0.0.1:0", "CBASE_STORE=sqlite", "CBASE_DB=:memory:",
                           "CBASE_JWT_SECRET=boot-secret-boot-secret-boot-secret!", NULL };
        T_CHECK(exits(g_cbase, "serve", no_url, 2, "no CBASE_PUBLIC_URL"));
        char *bad_db[] = { "CBASE_LISTEN=127.0.0.1:0", "CBASE_STORE=sqlite", "CBASE_DB=/dev/null/cbase.sqlite", "CBASE_DEV=1", NULL };
        T_CHECK(exits(g_cbase, "serve", bad_db, 1, "store sqlite"));
    }
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
    char *busy[] = { listen_env, "CBASE_STORE=sqlite", "CBASE_DB=:memory:", "CBASE_DEV=1", NULL };
    T_CHECK(exits(g_cbase, "serve", store_env(busy), 1, "listen"));
    close(fd);
}

// THE SATURATION KNOB (util/affinity.h): where threads can be pinned, serve pins its loop and
// every other thread, says so, serves and stops as ever, and refuses a CPU it may not use. Unset,
// the knob is auto: with two CPUs or more the loop takes the lowest and every other thread the
// next; with one it pins nothing and still boots; `off` pins nothing. Elsewhere serve refuses a
// CPU given by number before it starts anything, and auto or `off` boot unpinned.
#if defined(__linux__)
#include <dirent.h>
#include <sched.h>
// Each of the child's threads' allowed CPUs (/proc/<pid>/task/<tid>/status Cpus_allowed_list):
// how many threads say exactly `want`, and how many in all; the main thread (the loop) apart.
static void threads_on(pid_t pid, const char *loop_want, const char *bg_want, int *loop_ok, int *bg_ok, int *bg_all) {
    char dir[64], path[384], line[256];
    snprintf(dir, sizeof dir, "/proc/%d/task", (int)pid);
    *loop_ok = *bg_ok = *bg_all = 0;
    DIR *d = opendir(dir);
    for (struct dirent *e; d && (e = readdir(d));) {
        if (e->d_name[0] == '.') continue;
        snprintf(path, sizeof path, "%s/%s/status", dir, e->d_name);
        FILE *f = fopen(path, "r");
        char cpus[64] = "";
        while (f && fgets(line, sizeof line, f))
            if (sscanf(line, "Cpus_allowed_list: %63s", cpus) == 1) break;
        if (f) fclose(f);
        if (atoi(e->d_name) == (int)pid) *loop_ok = strcmp(cpus, loop_want) == 0;
        else (*bg_all)++, *bg_ok += strcmp(cpus, bg_want) == 0;
    }
    if (d) closedir(d);
}

// This thread's own Cpus_allowed_list ("0-3"): what an unpinned child's threads all show.
static void own_cpus(char *out, size_t cap) {
    char line[256];
    FILE *f = fopen("/proc/thread-self/status", "r");
    out[0] = 0;
    while (f && fgets(line, sizeof line, f))
        if (sscanf(line, "Cpus_allowed_list: %63s", out) == 1) break;
    if (f) fclose(f);
}

// Starts serve with `env`, waits for `says` and then for it to listen, and checks every thread:
// the loop on `loop_want`, the two or more others on `bg_want`. Stops it with SIGTERM, cleanly.
static void boots_with(char *const env[], const char *says, const char *loop_want, const char *bg_want) {
    tp_child c;
    char *argv[] = { (char *)g_cbase, "serve", NULL };
    T_EQ_INT(spawn(&c, argv, env, false), 0);
    T_EQ_INT(tp_wait_line(&c, says, 15000, NULL, 0), 0);
    T_EQ_INT(tp_wait_line(&c, "listening on", 15000, NULL, 0), 0);
    int loop_ok, bg_ok, bg_all;
    threads_on(c.pid, loop_want, bg_want, &loop_ok, &bg_ok, &bg_all);
    if (!loop_ok || bg_ok != bg_all) fprintf(stderr, "     want the loop on %s and %d threads on %s: loop %s, %d of them\n",
                                             loop_want, bg_all, bg_want, loop_ok ? "yes" : "no", bg_ok);
    T_CHECK(loop_ok);                          // the loop (the main thread) on its CPU
    T_CHECK(bg_all >= 2 && bg_ok == bg_all);   // the pools' workers and the rest on theirs
    T_EQ_INT(tp_stop(&c, SIGTERM, 15000), 0);
}
#endif

static void the_cpu_knob_boots_or_refuses(void) {
#if defined(__linux__)
    // The CPUs this test may use (a cpuset need not hold CPU 0 or 1): the lowest two.
    cpu_set_t all, one;
    T_EQ_INT(sched_getaffinity(0, sizeof all, &all), 0);
    int low[2] = { -1, -1 }, n = 0;
    for (int i = 0; i < CPU_SETSIZE && n < 2; i++) if (CPU_ISSET(i, &all)) low[n++] = i;
    if (n < 2) low[1] = low[0];
    char mine[64], lo0[8], lo1[8], says[128];
    own_cpus(mine, sizeof mine);
    snprintf(lo0, sizeof lo0, "%d", low[0]);
    snprintf(lo1, sizeof lo1, "%d", low[1]);

    // By hand: the loop on the lowest, the rest on the next (both on the one there is).
    char loop_env[32], bg_env[32];
    snprintf(loop_env, sizeof loop_env, "CBASE_LOOP_CPU=%d", low[0]);
    snprintf(bg_env, sizeof bg_env, "CBASE_BG_CPU=%d", low[1]);
    snprintf(says, sizeof says, "pinned: the loop to CPU %d, every other thread to CPU %d", low[0], low[1]);
    char *pinned[] = { "CBASE_LISTEN=127.0.0.1:0", "CBASE_STORE=sqlite", "CBASE_DB=:memory:", "CBASE_DEV=1",
                       loop_env, bg_env, NULL };
    boots_with(store_env(pinned), says, lo0, lo1);
    char *no_cpu[] = { "CBASE_LISTEN=127.0.0.1:0", "CBASE_STORE=sqlite", "CBASE_DB=:memory:", "CBASE_DEV=1",
                       "CBASE_BG_CPU=1023", NULL };
    T_CHECK(exits(g_cbase, "serve", store_env(no_cpu), 2, "CBASE_BG_CPU: CPU 1023 is not one this process may use"));

    // Auto: the lowest two, or nothing pinned with one.
    char *autov[] = { "CBASE_LISTEN=127.0.0.1:0", "CBASE_STORE=sqlite", "CBASE_DB=:memory:", "CBASE_DEV=1", NULL };
    store_env(autov);
    if (n >= 2) {
        snprintf(says, sizeof says, "pinned: the loop to CPU %d, every other thread to CPU %d (auto)", low[0], low[1]);
        boots_with(autov, says, lo0, lo1);
    } else {
        boots_with(autov, "CPU knob auto: this process may use 1 CPU, so nothing is pinned", mine, mine);
    }
    // Auto with one CPU (as `taskset -c <lowest>` starts it; the child inherits this thread's mask):
    // nothing to pin, and it boots.
    CPU_ZERO(&one);
    CPU_SET(low[0], &one);
    T_EQ_INT(sched_setaffinity(0, sizeof one, &one), 0);
    boots_with(autov, "CPU knob auto: this process may use 1 CPU, so nothing is pinned", lo0, lo0);
    T_EQ_INT(sched_setaffinity(0, sizeof all, &all), 0);
    // Off: nothing pinned, every thread on every CPU this test may use.
    char *off[] = { "CBASE_LISTEN=127.0.0.1:0", "CBASE_STORE=sqlite", "CBASE_DB=:memory:", "CBASE_DEV=1",
                    "CBASE_LOOP_CPU=off", "CBASE_BG_CPU=off", NULL };
    boots_with(store_env(off), "CBASE_BG_CPU=off (not pinned)", mine, mine);
#else
    char *pinned[] = { "CBASE_LISTEN=127.0.0.1:0", "CBASE_STORE=sqlite", "CBASE_DB=:memory:", "CBASE_DEV=1",
                       "CBASE_LOOP_CPU=0", NULL };
    T_CHECK(exits(g_cbase, "serve", store_env(pinned), 2, "needs Linux"));
    // Auto and off cost no start here: auto says once why nothing is pinned, and both serve.
    char *argv[] = { (char *)g_cbase, "serve", NULL };
    char *autov[] = { "CBASE_LISTEN=127.0.0.1:0", "CBASE_STORE=sqlite", "CBASE_DB=:memory:", "CBASE_DEV=1", NULL };
    char *off[] = { "CBASE_LISTEN=127.0.0.1:0", "CBASE_STORE=sqlite", "CBASE_DB=:memory:", "CBASE_DEV=1",
                    "CBASE_LOOP_CPU=off", "CBASE_BG_CPU=off", NULL };
    store_env(autov);
    store_env(off);
    tp_child c;
    T_EQ_INT(spawn(&c, argv, autov, false), 0);
    T_EQ_INT(tp_wait_line(&c, "CPU knob auto: picking CPUs needs Linux, so nothing is pinned", 15000, NULL, 0), 0);
    T_EQ_INT(tp_wait_line(&c, "listening on", 15000, NULL, 0), 0);
    T_EQ_INT(tp_stop(&c, SIGTERM, 15000), 0);
    T_EQ_INT(spawn(&c, argv, off, false), 0);
    T_EQ_INT(tp_wait_line(&c, "CBASE_LOOP_CPU=off (not pinned)", 15000, NULL, 0), 0);
    T_EQ_INT(tp_wait_line(&c, "listening on", 15000, NULL, 0), 0);
    T_EQ_INT(tp_stop(&c, SIGTERM, 15000), 0);
#endif
}

// Reads the child's stdout into buf until a newline or `ms` pass: the bytes read (buf NUL-ended).
static size_t read_out_line(int fd, char *buf, size_t cap, int ms) {
    size_t n = 0;
    struct pollfd p = { fd, POLLIN, 0 };
    while (n + 1 < cap && !memchr(buf, '\n', n) && poll(&p, 1, ms) == 1) {
        ssize_t r = read(fd, buf + n, cap - 1 - n);
        if (r <= 0) break;
        n += (size_t)r;
        buf[n] = 0;
    }
    buf[n] = 0;
    return n;
}

// The Host this smoke sends to serve on 127.0.0.1:port: one the app answers. With no allow-list
// declared every Host is answered; with one (cbase/app.h host_allow), its first entry, at the port
// serve bound when the entry names none (a bare entry means that port), as the entry is when it
// names one (the router holds the Host's port to the entry's, not to the socket's).
static void probe_host(int port, char *out, size_t cap) {
    const cb_app *app = cb_main_app;
    if (!app || app->n_host_allow <= 0 || !app->host_allow[0]) { snprintf(out, cap, "127.0.0.1:%d", port); return; }
    const char *h = app->host_allow[0], *e = h[0] == '[' ? strchr(h, ']') : h;
    if (strchr(e ? e : h, ':')) snprintf(out, cap, "%s", h);
    else snprintf(out, cap, "%s:%d", h, port);
}

// GET /health on 127.0.0.1:port, with the Host the app answers: the status line's code, or -1, and
// the body (NUL-ended) in body. Asked from loopback, /health passes an origin secret the app may
// set (handler.c is_local_probe), so it reaches the router in every product.
static int get_health(int port, char *body, size_t cap) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port) };
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    body[0] = 0;
    if (fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof a) != 0) { if (fd >= 0) close(fd); return -1; }
    char host[300], q[400], b[2048];
    probe_host(port, host, sizeof host);
    snprintf(q, sizeof q, "GET /health HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", host);
    size_t n = 0;
    int code = -1;
    if (write(fd, q, strlen(q)) == (ssize_t)strlen(q)) {
        for (ssize_t r; n + 1 < sizeof b && (r = read(fd, b + n, sizeof b - 1 - n)) > 0;) n += (size_t)r;   // to the close
    }
    b[n] = 0;
    close(fd);
    if (n > 12) sscanf(b, "HTTP/1.1 %d", &code);
    const char *e = strstr(b, "\r\n\r\n");
    if (e) snprintf(body, cap, "%s", e + 4);
    return code;
}

// What GET /health is in this app, as its routes alone make it: 200 when it declares the platform's
// (health_route); else 404 when none of its routes has the path, 405 when one has it for another
// method, and 0 when one of its own answers GET there (its handler's to say, any status).
static int health_want(void) {
    const cb_app *app = cb_main_app;
    if (app && app->health_route) return 200;
    cb_router *r = cb_router_new();
    for (int l = 0; r && app && l < app->n_route_lists; l++) cb_router_add_all(r, app->routes[l]->routes, app->routes[l]->n);
    bool known = false;
    const cb_route *m = r ? cb_router_match(r, CB_M_GET, "/health", &known) : NULL;
    cb_router_free(r);
    return m ? 0 : known ? 405 : 404;
}

// The port serve named answers, as this app declares it: GET /health is the platform's ("ok cbase
// ...", 200) when the app declares it, and else the app's own answer: its 404 "not found" for a
// path it does not route, never the platform's /health. A Host refused (400, 403) fails here too.
static void the_port_answers(int port) {
    char body[1024];
    const int want = health_want(), got = get_health(port, body, sizeof body);
    if (want == 200) {
        T_EQ_INT(got, 200);
        T_CHECK(strncmp(body, "ok cbase ", 9) == 0);
    } else if (want == 404) {
        T_EQ_INT(got, 404);
        T_EQ_STR(body, "not found");
    } else if (want == 405) {
        T_EQ_INT(got, 405);
    } else {
        T_CHECK(got >= 100 && got < 600);       // the app's own handler answered
        T_CHECK(strncmp(body, "ok cbase ", 9) != 0);
    }
}

// THE STARTUP LINE (ingress_process.h): with port 0, serve binds a free port and says which on
// stdout, in one line, once, after the bind: the port it names answers, and nothing else is ever
// written there, through a drain to the exit.
static void the_startup_line_names_the_port(void) {
    char *env[] = { "CBASE_LISTEN=127.0.0.1:0", "CBASE_STORE=sqlite", "CBASE_DB=:memory:", "CBASE_DEV=1", NULL };
    store_env(env);
    char *argv[] = { (char *)g_cbase, "serve", NULL };
    for (int round = 0; round < 2; round++) {   // twice: two free ports, each named
        tp_child c;
        T_EQ_INT(spawn(&c, argv, env, true), 0);
        char line[256];
        size_t n = read_out_line(c.out_fd, line, sizeof line, 15000);
        int port = 0;
        char tail[8] = "";
        T_EQ_INT(sscanf(line, "cbase: listening 127.0.0.1:%d%7[^x]", &port, tail), 2);
        T_EQ_STR(tail, "\n");                  // the line ends at the port: nothing after it
        T_CHECK(n > 0 && strchr(line, '\n') == line + n - 1);   // one line, and only one
        T_CHECK(port > 0 && port < 65536);
        if (port <= 0) { tp_stop(&c, SIGKILL, 5000); close(c.out_fd); continue; }
        // The log says the same port, and the port answers: the line came after the bind.
        char logl[256] = "", want[64];
        T_EQ_INT(tp_wait_line(&c, "listening on", 15000, logl, sizeof logl), 0);
        snprintf(want, sizeof want, "listening on 127.0.0.1:%d ", port);
        T_CHECK(strstr(logl, want) != NULL);
        the_port_answers(port);
        T_EQ_INT(tp_stop(&c, SIGTERM, 15000), 0);
        // Exactly once: after the drain and the exit, stdout holds nothing more.
        char rest[256];
        T_EQ_INT((long long)read_out_line(c.out_fd, rest, sizeof rest, 2000), 0);
        close(c.out_fd);
    }
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
    // Under Lambda no JWT secret is no start (finding L8): a random one per
    // container would sign users out at random.
    // A storeless app's too: the secret is asked for before the app is.
    char *no_secret[] = { "CBASE_STORE=sqlite", NULL };
    T_CHECK(init_error(store_env(no_secret), "Config", "no CBASE_JWT_SECRET", false));
    char *bad_boot[] = { "CBASE_STORE=sqlite", "CBASE_DB=/dev/null/cbase.sqlite",
                         "CBASE_JWT_SECRET=boot-secret-boot-secret-boot-secret!", NULL };
    // A store it cannot open; for a storeless app, a store named at all.
    T_CHECK(init_error(bad_boot, "Boot", g_storeless ? "names a store" : "store sqlite", true));
}

int main(int argc, char **argv) {
    if (argc != 2 && argc != 3) {
        fprintf(stderr, "usage: smoke_boot <path to cbase, or -> [<path to bootstrap>]\n");
        return 2;
    }
    // A build links the providers it names (cbase.mk CBASE_PROVIDERS): each binary it has is tried.
    g_cbase = strcmp(argv[1], "-") == 0 ? NULL : argv[1];
    g_bootstrap = argc == 3 ? argv[2] : NULL;
    signal(SIGPIPE, SIG_IGN);
    g_storeless = cb_main_app && cb_main_app->storeless;
    printf("smoke_boot: the app %s %s\n", cb_main_app && cb_main_app->name ? cb_main_app->name : "",
           g_storeless ? "is storeless" : "keeps a store");
    if (g_cbase) T_RUN(cbase_refuses);
    if (g_cbase) T_RUN(the_cpu_knob_boots_or_refuses);
    if (g_cbase) T_RUN(the_startup_line_names_the_port);
    if (g_bootstrap) T_RUN(bootstrap_refuses);
    return T_DONE();
}
