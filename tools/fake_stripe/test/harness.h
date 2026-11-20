/* Shared by the fake_stripe tests: spawn the server, a tiny HTTP client, and a webhook receiver thread.
 * Built with -std=c11 and no -D: the feature macros below are the file's own, as the fake's are, and
 * on Darwin _POSIX_C_SOURCE alone hides INADDR_LOOPBACK, so _DARWIN_C_SOURCE comes with it. */
#ifndef FS_HARNESS_H
#define FS_HARNESS_H
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../../../lib/num_parse.h"

static int fails = 0;
static void msleep(int ms) { struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L}; nanosleep(&ts, NULL); }
static char *ci_find(char *hay, const char *needle) {
    size_t n = strlen(needle);
    for (; *hay; hay++) if (strncasecmp(hay, needle, n) == 0) return hay;
    return NULL;
}
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define FS_KEY "sk_test_fake_cbase"
#define FS_SECRET "whsec_test_secret"
#define BODY_MAX 16384

typedef struct { pid_t pid; int port; } fs_server;

/* ---- receiver: accepts webhook POSTs, stores them, answers 200 ---- */
typedef struct { char sig[256]; char body[BODY_MAX]; } hook_t;
static struct {
    int fd, port, stop, n;
    pthread_t th;
    pthread_mutex_t mu;
    hook_t h[64];
} HK;

static void *hook_thread(void *arg) {
    (void)arg;
    for (;;) {
        int c = accept(HK.fd, NULL, NULL);
        if (c < 0) break;
        if (HK.stop) { close(c); break; }
        static char buf[BODY_MAX * 2];
        size_t have = 0;
        char *he = NULL;
        while (!he && have < sizeof buf - 1) {
            ssize_t n = recv(c, buf + have, sizeof buf - 1 - have, 0);
            if (n <= 0) break;
            have += (size_t)n;
            buf[have] = 0;
            he = strstr(buf, "\r\n\r\n");
        }
        if (he) {
            size_t hlen = (size_t)(he - buf) + 4, clen = 0;
            char *cl = ci_find(buf, "Content-Length:");
            if (cl) {
                cl += 15;
                while (*cl == ' ') cl++;
                int64_t v;
                if (num_parse_i64(cl, strspn(cl, "0123456789"), 0, 1 << 20, &v)) clen = (size_t)v;
            }
            while (have < hlen + clen && have < sizeof buf - 1) {
                ssize_t n = recv(c, buf + have, sizeof buf - 1 - have, 0);
                if (n <= 0) break;
                have += (size_t)n;
            }
            buf[have] = 0;
            hook_t *h = &HK.h[HK.n % 64];
            memset(h, 0, sizeof *h);
            char *sg = ci_find(buf, "Stripe-Signature:");
            if (sg) { sg += 17; while (*sg == ' ') sg++; size_t l = strcspn(sg, "\r\n"); if (l > 255) l = 255; memcpy(h->sig, sg, l); }
            size_t bl = have - hlen < BODY_MAX - 1 ? have - hlen : BODY_MAX - 1;
            memcpy(h->body, buf + hlen, bl);
            pthread_mutex_lock(&HK.mu);
            HK.n++;
            pthread_mutex_unlock(&HK.mu);
        }
        const char *ok = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok";
        send(c, ok, strlen(ok), MSG_NOSIGNAL);
        close(c);
    }
    return NULL;
}
static void hook_start(void) {
    signal(SIGPIPE, SIG_IGN);
    pthread_mutex_init(&HK.mu, NULL);
    HK.fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(HK.fd, (struct sockaddr *)&sa, sizeof sa) < 0 || listen(HK.fd, 16) < 0) { perror("hook bind"); exit(1); }
    socklen_t sl = sizeof sa;
    getsockname(HK.fd, (struct sockaddr *)&sa, &sl);
    HK.port = ntohs(sa.sin_port);
    pthread_create(&HK.th, NULL, hook_thread, NULL);
}
static void hook_stop(void) {
    HK.stop = 1;
    int c = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = htons((uint16_t)HK.port);
    connect(c, (struct sockaddr *)&sa, sizeof sa);
    close(c);
    pthread_join(HK.th, NULL);
    close(HK.fd);
}
static int hook_count(void) { pthread_mutex_lock(&HK.mu); int n = HK.n; pthread_mutex_unlock(&HK.mu); return n; }
/* wait until n webhooks have arrived in total (up to 5 s) */
static int hook_wait(int n) {
    for (int i = 0; i < 500 && hook_count() < n; i++) msleep(10);
    return hook_count() >= n;
}
/* give a webhook that should NOT arrive a moment to show up */
static inline int hook_quiet(int n) { msleep(300); return hook_count() == n; }

/* ---- server process ---- */
static fs_server fs_start(const char *bin, int with_hook, ...) {
    int p[2];
    if (pipe(p) < 0) { perror("pipe"); exit(1); }
    fs_server s = {0, 0};
    s.pid = fork();
    if (s.pid == 0) {
        close(p[0]);
        dup2(p[1], 1);
        char *argv[24];
        int n = 0;
        char hook[128];
        snprintf(hook, sizeof hook, "http://127.0.0.1:%d/hooks/stripe", HK.port);
        argv[n++] = (char *)bin;
        argv[n++] = "--port"; argv[n++] = "0";
        argv[n++] = "--webhook-secret"; argv[n++] = FS_SECRET;
        if (with_hook) { argv[n++] = "--webhook-url"; argv[n++] = hook; }
        va_list ap;
        va_start(ap, with_hook);
        for (const char *e; (e = va_arg(ap, const char *)) != NULL && n < 22;) argv[n++] = (char *)e;
        va_end(ap);
        argv[n] = NULL;
        execv(bin, argv);
        _exit(127);
    }
    close(p[1]);
    char line[128] = "";
    size_t have = 0;
    while (have < sizeof line - 1) {
        ssize_t r = read(p[0], line + have, 1);
        if (r <= 0 || line[have] == '\n') break;
        have++;
    }
    line[have] = 0;
    close(p[0]);
    static const char LISTEN[] = "listening 127.0.0.1:";
    int64_t port;
    if (strncmp(line, LISTEN, sizeof LISTEN - 1) != 0 || !num_parse_i64_cstr(line + sizeof LISTEN - 1, 1, 65535, &port)) {
        printf("FAIL: server did not start (%s)\n", line);
        exit(1);
    }
    s.port = (int)port;
    return s;
}
/* A server that must refuse to start: its exit status (the NULL-ended options are the fake's own), -1 if it
 * does not end within 5 s (it is killed then) or ends by a signal. Its stdout and stderr are discarded. */
static inline int fs_start_fails(const char *bin, ...) {
    pid_t pid = fork();
    if (pid == 0) {
        char *argv[24];
        int n = 0;
        argv[n++] = (char *)bin;
        va_list ap;
        va_start(ap, bin);
        for (const char *e; (e = va_arg(ap, const char *)) != NULL && n < 22;) argv[n++] = (char *)e;
        va_end(ap);
        argv[n] = NULL;
        if (!freopen("/dev/null", "w", stdout) || !freopen("/dev/null", "w", stderr)) _exit(126);
        execv(bin, argv);
        _exit(127);
    }
    int st = 0;
    for (int i = 0; i < 500; i++) {
        pid_t w = waitpid(pid, &st, WNOHANG);
        if (w == pid) return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
        msleep(10);
    }
    kill(pid, SIGKILL);
    waitpid(pid, &st, 0);
    return -1;
}
static void fs_stop(fs_server *s) {
    if (s->pid > 0) { kill(s->pid, SIGTERM); waitpid(s->pid, NULL, 0); s->pid = 0; }
}

/* ---- http client ---- */
typedef struct { int status; char body[BODY_MAX]; char hdr[2048]; } reply;

/* Send len bytes of req as they are (a test can lie about Content-Length), read until the server closes,
 * and parse the answer. A send the server stops reading (it refused the request) ends the sending, not the
 * test: the answer is still read. */
static void exchange(int port, const char *req, size_t len, reply *out) {
    memset(out, 0, sizeof *out);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = htons((uint16_t)port);
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0) { perror("connect"); out->status = -1; close(fd); return; }
    for (size_t off = 0; off < len;) {
        ssize_t w = send(fd, req + off, len - off, MSG_NOSIGNAL);
        if (w <= 0) break;
        off += (size_t)w;
    }
    static _Thread_local char raw[BODY_MAX + 4096];   /* one a thread: test_client.c's clients at once */
    size_t have = 0;
    for (;;) {
        ssize_t n = recv(fd, raw + have, sizeof raw - 1 - have, 0);
        if (n <= 0) break;
        have += (size_t)n;
    }
    raw[have] = 0;
    close(fd);
    char *he = strstr(raw, "\r\n\r\n");
    int64_t st;
    if (strncmp(raw, "HTTP/1.1 ", 9) != 0 || !num_parse_i64(raw + 9, strspn(raw + 9, "0123456789"), 100, 599, &st) || !he) { out->status = -1; return; }
    out->status = (int)st;
    *he = 0;
    snprintf(out->hdr, sizeof out->hdr, "%.2047s", raw);
    snprintf(out->body, sizeof out->body, "%s", he + 4);
}

static void http(int port, const char *method, const char *path, const char *headers, const char *body, reply *out) {
    char req[BODY_MAX];
    int l = snprintf(req, sizeof req, "%s %s HTTP/1.1\r\nHost: 127.0.0.1\r\n%sContent-Length: %zu\r\nConnection: close\r\n\r\n%s",
                     method, path, headers ? headers : "", body ? strlen(body) : 0, body ? body : "");
    exchange(port, req, (size_t)l, out);
}

/* GET/POST with the API key, optional connected account, optional Idempotency-Key */
static void api(int port, const char *method, const char *path, const char *acct, const char *idem, const char *body, reply *out) {
    char h[600];
    int n = snprintf(h, sizeof h, "Authorization: Bearer %s\r\nContent-Type: application/x-www-form-urlencoded\r\n", FS_KEY);
    if (acct) n += snprintf(h + n, sizeof h - (size_t)n, "Stripe-Account: %s\r\n", acct);
    if (idem) snprintf(h + n, sizeof h - (size_t)n, "Idempotency-Key: %s\r\n", idem);
    http(port, method, path, h, body, out);
}

/* naive extraction from the fake's compact JSON: the string value after "key": (first occurrence) */
static int jstr(const char *json, const char *key, char *out, size_t cap) {
    char pat[96];
    snprintf(pat, sizeof pat, "\"%s\":\"", key);
    const char *p = strstr(json, pat);
    if (!p) return 0;
    p += strlen(pat);
    size_t l = strcspn(p, "\"");
    if (l >= cap) l = cap - 1;
    memcpy(out, p, l);
    out[l] = 0;
    return 1;
}
static int has(const char *json, const char *frag) { return strstr(json, frag) != NULL; }
#endif
