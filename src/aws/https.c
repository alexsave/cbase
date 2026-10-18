// https.c - the HTTPS/1.1 client (https.h).
//
// Blocking sockets with per-call deadlines: the connect is non-blocking plus
// poll(2), and every TLS read and write runs under SO_RCVTIMEO/SO_SNDTIMEO set
// to the time left. Idle connections wait in a small pool keyed by host and
// port; a caller takes one out (so two threads never share an SSL), and puts
// it back only after a response that allows keep-alive.
#include "https.h"

#include "util/clock.h"
#include "util/err.h"
#include "util/log.h"

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define POOL_MAX     8
#define IDLE_MAX_MS  50000   // AWS front ends drop idle connections after about a minute

typedef struct {
    SSL    *ssl;      // NULL on a plain-HTTP connection
    bool    tls;
    int     fd;
    char    host[256];
    int     port;
    int64_t idle_since;
} conn;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static SSL_CTX *g_ctx;
static char g_ca_file[1024];   // cb_https_set_ca_file's, "" for the default search
static conn g_pool[POOL_MAX];
static int g_npool;

// ---- the tally (https.h) ----------------------------------------------------------

enum { K_DDB, K_PUSH, K_OTHER, K_N };
static _Atomic int     g_calls[K_N], g_dials[K_N];
static _Atomic int64_t g_us[K_N];

static int host_kind(const char *host, bool tls) {
    if (!tls || strncmp(host, "dynamodb.", 9) == 0) return K_DDB;
    if (strstr(host, ".execute-api.") || strstr(host, ".iot.")) return K_PUSH;
    return K_OTHER;
}

static int64_t mono_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

void cb_https_tally_take(cb_https_tally *out) {
    cb_https_count *rows[K_N] = { &out->ddb, &out->push, &out->other };
    for (int k = 0; k < K_N; k++) {
        rows[k]->calls = atomic_exchange_explicit(&g_calls[k], 0, memory_order_relaxed);
        rows[k]->dials = atomic_exchange_explicit(&g_dials[k], 0, memory_order_relaxed);
        rows[k]->us = atomic_exchange_explicit(&g_us[k], 0, memory_order_relaxed);
    }
}

void cb_https_response_free(cb_https_response *r) {
    cb_buf_free(&r->headers);
    cb_buf_free(&r->body);
}

// ---- the parser ---------------------------------------------------------------

// The end of the line starting at p (the CR of its CRLF), or NULL when no CRLF yet.
static const uint8_t *line_end(const uint8_t *p, const uint8_t *end) {
    for (const uint8_t *q = p; q + 1 < end; q++)
        if (q[0] == '\r' && q[1] == '\n') return q;
    return NULL;
}

// Header `name` in a head (from the line after the status line), value span into v.
static bool head_value(const uint8_t *head, const uint8_t *head_end, const char *name, const uint8_t **v, size_t *vn) {
    size_t nl = strlen(name);
    const uint8_t *p = line_end(head, head_end);
    while (p && p + 2 < head_end) {
        const uint8_t *s = p + 2;
        const uint8_t *e = line_end(s, head_end);
        if (!e) return false;
        if ((size_t)(e - s) > nl && s[nl] == ':' && strncasecmp((const char *)s, name, nl) == 0) {
            s += nl + 1;
            while (s < e && (*s == ' ' || *s == '\t')) s++;
            const uint8_t *t = e;
            while (t > s && (t[-1] == ' ' || t[-1] == '\t')) t--;
            *v = s;
            *vn = (size_t)(t - s);
            return true;
        }
        p = e;
    }
    return false;
}

static bool span_has_token(const uint8_t *v, size_t n, const char *tok) {
    size_t tl = strlen(tok);
    for (size_t i = 0; i + tl <= n; i++)
        if (strncasecmp((const char *)v + i, tok, tl) == 0) return true;
    return false;
}

// Walks a chunked body at p. Returns the bytes it spans (through the last
// CRLF of the trailers), 0 when incomplete, -1 when malformed. When `out` is
// set, appends the data.
static long chunked(const uint8_t *p, const uint8_t *end, cb_buf *out) {
    const uint8_t *s = p;
    size_t total = 0;
    for (;;) {
        const uint8_t *e = line_end(s, end);
        if (!e) return (end - s) > 64 ? -1 : 0;
        size_t sz = 0;
        int digits = 0;
        const uint8_t *q = s;
        for (; q < e; q++) {
            int d;
            if (*q >= '0' && *q <= '9') d = *q - '0';
            else if (*q >= 'a' && *q <= 'f') d = *q - 'a' + 10;
            else if (*q >= 'A' && *q <= 'F') d = *q - 'A' + 10;
            else break;
            if (++digits > 8) return -1;
            sz = sz * 16 + (size_t)d;
        }
        if (digits == 0 || (q < e && *q != ';' && *q != ' ' && *q != '\t')) return -1;
        s = e + 2;
        if (sz == 0) {
            // Trailers: header lines until an empty one.
            for (;;) {
                const uint8_t *te = line_end(s, end);
                if (!te) return 0;
                bool empty = te == s;
                s = te + 2;
                if (empty) return (long)(s - p);
            }
        }
        total += sz;
        if (total > CB_HTTPS_MAX_BODY) return -1;
        if ((size_t)(end - s) < sz + 2) return 0;
        if (s[sz] != '\r' || s[sz + 1] != '\n') return -1;
        if (out) cb_buf_append(out, s, sz);
        s += sz + 2;
    }
}

int cb_https_parse_response(const uint8_t *buf, size_t len, bool head_only, bool eof, cb_https_response *out,
                            bool *keep_alive) {
    const uint8_t *end = buf + len, *p = buf;
    for (;;) {
        // The head ends at the first empty line.
        const uint8_t *he = NULL;
        for (const uint8_t *q = p; q + 3 < end; q++)
            if (q[0] == '\r' && q[1] == '\n' && q[2] == '\r' && q[3] == '\n') { he = q + 4; break; }
        if (!he) return eof || (end - p) > 65536 ? CB_E_IO : 0;
        int minor = -1, status = 0;
        if ((size_t)(he - p) < 12 || memcmp(p, "HTTP/1.", 7) != 0 || (p[7] != '0' && p[7] != '1') || p[8] != ' ')
            return CB_E_IO;
        minor = p[7] - '0';
        for (int i = 9; i < 12; i++) {
            if (p[i] < '0' || p[i] > '9') return CB_E_IO;
            status = status * 10 + (p[i] - '0');
        }
        if (status < 100) return CB_E_IO;
        if (status < 200) { p = he; continue; }   // 100 Continue and friends carry no body

        const uint8_t *v;
        size_t vn;
        bool ka = minor == 1;
        if (head_value(p, he, "connection", &v, &vn)) {
            if (span_has_token(v, vn, "close")) ka = false;
            else if (span_has_token(v, vn, "keep-alive")) ka = true;
        }
        bool no_body = head_only || status == 204 || status == 304;
        const uint8_t *body = he;
        size_t consumed;
        bool is_chunked = false;
        size_t clen = 0;
        bool have_len = false;
        if (!no_body && head_value(p, he, "transfer-encoding", &v, &vn)) {
            if (!span_has_token(v, vn, "chunked")) return CB_E_IO;
            is_chunked = true;
        } else if (!no_body && head_value(p, he, "content-length", &v, &vn)) {
            if (vn == 0 || vn > 10) return CB_E_IO;
            for (size_t i = 0; i < vn; i++) {
                if (v[i] < '0' || v[i] > '9') return CB_E_IO;
                clen = clen * 10 + (size_t)(v[i] - '0');
            }
            if (clen > CB_HTTPS_MAX_BODY) return CB_E_IO;
            have_len = true;
        }
        if (no_body) {
            consumed = (size_t)(he - buf);
        } else if (is_chunked) {
            long k = chunked(body, end, NULL);
            if (k < 0) return CB_E_IO;
            if (k == 0) return eof ? CB_E_IO : 0;
            consumed = (size_t)(body - buf) + (size_t)k;
        } else if (have_len) {
            if ((size_t)(end - body) < clen) return eof ? CB_E_IO : 0;
            consumed = (size_t)(body - buf) + clen;
        } else {
            // Neither: the body runs to the close.
            if ((size_t)(end - body) > CB_HTTPS_MAX_BODY) return CB_E_IO;
            if (!eof) return 0;
            ka = false;
            consumed = len;
        }
        cb_buf_reset(&out->headers);
        cb_buf_reset(&out->body);
        out->status = status;
        cb_buf_append(&out->headers, p, (size_t)(he - p));
        if (is_chunked) chunked(body, end, &out->body);
        else if (!no_body) cb_buf_append(&out->body, body, consumed - (size_t)(body - buf));
        if (out->headers.oom || out->body.oom) return CB_E_NOMEM;
        *keep_alive = ka;
        return (int)consumed;
    }
}

// ---- TLS ----------------------------------------------------------------------

static bool file_exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

static void conn_close(conn *c) {
    if (c->ssl) SSL_free(c->ssl);   // no close_notify: the peer may be long gone
    if (c->fd >= 0) close(c->fd);
    c->ssl = NULL;
    c->fd = -1;
}

// Builds g_ctx under g_lock. CB_OK or CB_E_IO.
static int ctx_build(void) {
    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) return CB_E_IO;
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
#ifdef SSL_OP_IGNORE_UNEXPECTED_EOF
    // AWS sometimes closes without close_notify; the parser decides completeness.
    SSL_CTX_set_options(ctx, SSL_OP_IGNORE_UNEXPECTED_EOF);
#endif
    const char *ca = g_ca_file[0] ? g_ca_file : getenv("CBASE_CA_FILE");
    static const char *const KNOWN[] = {
        "/etc/pki/tls/certs/ca-bundle.crt",     // Amazon Linux 2023: the Lambda runtime
        "/etc/ssl/certs/ca-certificates.crt",   // Debian, Ubuntu, Alpine
        "/etc/ssl/cert.pem",                    // Alpine, macOS's system copy
    };
    int ok = 0;
    if (ca && *ca) {
        ok = SSL_CTX_load_verify_locations(ctx, ca, NULL);
        if (!ok) CB_ERROR("https", "cannot load CA bundle %s", ca);
    } else {
        for (size_t i = 0; i < sizeof KNOWN / sizeof KNOWN[0] && !ok; i++)
            if (file_exists(KNOWN[i])) ok = SSL_CTX_load_verify_locations(ctx, KNOWN[i], NULL);
        if (!ok) ok = SSL_CTX_set_default_verify_paths(ctx);
    }
    if (!ok) { SSL_CTX_free(ctx); ERR_clear_error(); return CB_E_IO; }
    if (g_ctx) SSL_CTX_free(g_ctx);
    g_ctx = ctx;
    return CB_OK;
}

int cb_https_set_ca_file(const char *path) {
    pthread_mutex_lock(&g_lock);
    if (path && cb_strlcpy(g_ca_file, path, sizeof g_ca_file) >= sizeof g_ca_file) {
        pthread_mutex_unlock(&g_lock);
        return CB_E_INVALID;
    }
    if (!path) g_ca_file[0] = 0;
    for (int i = 0; i < g_npool; i++) conn_close(&g_pool[i]);
    g_npool = 0;
    int rc = ctx_build();
    pthread_mutex_unlock(&g_lock);
    return rc;
}

static int64_t left(int64_t deadline) {
    int64_t l = deadline - cb_mono_ms();
    return l > 0 ? l : 0;
}

static void set_timeouts(int fd, int64_t ms) {
    if (ms < 1) ms = 1;
    struct timeval tv = { (time_t)(ms / 1000), (suseconds_t)((ms % 1000) * 1000) };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
}

static int dial(const char *host, int port, int64_t deadline) {
    struct addrinfo hints, *ai = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_socktype = SOCK_STREAM;
    char ps[16];
    snprintf(ps, sizeof ps, "%d", port);
    if (getaddrinfo(host, ps, &hints, &ai) != 0) return -1;
    int fd = -1;
    for (struct addrinfo *a = ai; a && fd < 0 && left(deadline) > 0; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) continue;
        int one = 1;
#ifdef SO_NOSIGPIPE
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
        int fl = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, fl | O_NONBLOCK);
        int rc = connect(fd, a->ai_addr, a->ai_addrlen);
        if (rc != 0 && errno == EINPROGRESS) {
            struct pollfd pf = { fd, POLLOUT, 0 };
            int err = 0;
            socklen_t el = sizeof err;
            if (poll(&pf, 1, (int)left(deadline)) == 1 && getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) == 0 && err == 0)
                rc = 0;
        }
        if (rc != 0) { close(fd); fd = -1; continue; }
        fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
        (void)one;
    }
    freeaddrinfo(ai);
    return fd;
}

static int conn_open(conn *c, const char *host, int port, bool tls, int64_t deadline) {
    memset(c, 0, sizeof *c);
    c->fd = -1;
    cb_strlcpy(c->host, host, sizeof c->host);
    c->port = port;
    c->tls = tls;
    if (!tls) {
        c->fd = dial(host, port, deadline);
        if (c->fd < 0) { CB_WARN("https", "connect %s:%d failed", host, port); return CB_E_IO; }
        return CB_OK;
    }
    pthread_mutex_lock(&g_lock);
    int rc = g_ctx ? CB_OK : ctx_build();
    if (rc == CB_OK) {
        c->ssl = SSL_new(g_ctx);
        if (!c->ssl) rc = CB_E_NOMEM;
    }
    pthread_mutex_unlock(&g_lock);
    if (rc != CB_OK) return rc;
    c->fd = dial(host, port, deadline);
    if (c->fd < 0) { CB_WARN("https", "connect %s:%d failed", host, port); conn_close(c); return CB_E_IO; }
    set_timeouts(c->fd, left(deadline));
    SSL_set_fd(c->ssl, c->fd);
    SSL_set_tlsext_host_name(c->ssl, host);   // SNI
    // The certificate must name the host. If the name cannot be set (finding
    // L10: SSL_set1_host's answer was ignored), no connection is made at all,
    // rather than one whose certificate is checked against nothing.
    if (SSL_set1_host(c->ssl, host) != 1) {
        CB_WARN("https", "TLS to %s:%d: the host name to verify could not be set", host, port);
        ERR_clear_error();
        conn_close(c);
        return CB_E_IO;
    }
    if (SSL_connect(c->ssl) != 1) {
        long vr = SSL_get_verify_result(c->ssl);
        char e[256];
        ERR_error_string_n(ERR_get_error(), e, sizeof e);
        CB_WARN("https", "TLS to %s:%d failed: %s%s%s", host, port, e, vr != X509_V_OK ? "; verify: " : "",
                vr != X509_V_OK ? X509_verify_cert_error_string(vr) : "");
        ERR_clear_error();
        conn_close(c);
        return CB_E_IO;
    }
    return CB_OK;
}

// An idle pooled connection to host:port, or false.
static bool pool_take(const char *host, int port, bool tls, conn *out) {
    bool found = false;
    int64_t now = cb_mono_ms();
    pthread_mutex_lock(&g_lock);
    for (int i = g_npool - 1; i >= 0; i--) {
        conn *c = &g_pool[i];
        bool stale = now - c->idle_since > IDLE_MAX_MS;
        bool match = c->port == port && c->tls == tls && strcmp(c->host, host) == 0;
        if (stale || (match && !found)) {
            if (stale) conn_close(c);
            else { *out = *c; found = true; }
            g_pool[i] = g_pool[--g_npool];
        }
    }
    pthread_mutex_unlock(&g_lock);
    return found;
}

static void pool_put(conn *c) {
    c->idle_since = cb_mono_ms();
    pthread_mutex_lock(&g_lock);
    if (g_npool < POOL_MAX) { g_pool[g_npool++] = *c; c->ssl = NULL; c->fd = -1; }
    pthread_mutex_unlock(&g_lock);
    conn_close(c);
}

static int write_all(conn *c, const void *p, size_t n) {
    const char *b = p;
    while (n) {
        int chunk = n > (1u << 30) ? (1 << 30) : (int)n;
        long w;
        if (c->ssl) w = SSL_write(c->ssl, b, chunk);
        else {
#ifdef MSG_NOSIGNAL
            w = send(c->fd, b, (size_t)chunk, MSG_NOSIGNAL);
#else
            w = send(c->fd, b, (size_t)chunk, 0);
#endif
            if (w < 0 && errno == EINTR) continue;
        }
        if (w <= 0) return -1;
        b += w;
        n -= (size_t)w;
    }
    return 0;
}

// One read on a plain connection, in SSL_read's terms: > 0 bytes, 0 the peer
// closed, -1 an error, -2 timed out.
static int plain_read(conn *c, uint8_t *p, size_t cap) {
    for (;;) {
        ssize_t r = recv(c->fd, p, cap, 0);
        if (r >= 0) return (int)r;
        if (errno == EINTR) continue;
        return errno == EAGAIN || errno == EWOULDBLOCK ? -2 : -1;
    }
}

// One exchange on c. CB_OK (and *ka), CB_E_IO, or 1 meaning "failed before
// any response byte" (the caller may retry a reused connection).
static int exchange(conn *c, const cb_buf *req, bool head, int64_t deadline, cb_https_response *out, bool *ka) {
    set_timeouts(c->fd, left(deadline));
    if (write_all(c, req->p, req->len) != 0) { ERR_clear_error(); return 1; }
    cb_buf in;
    cb_buf_init(&in);
    int rc = CB_E_IO;
    for (;;) {
        if (left(deadline) <= 0) { CB_WARN("https", "%s: timed out", c->host); break; }
        if (!cb_buf_reserve(&in, 16384)) { rc = CB_E_NOMEM; break; }
        set_timeouts(c->fd, left(deadline));
        bool eof = false;
        if (!c->ssl) {
            int r = plain_read(c, in.p + in.len, in.cap - in.len - 1);
            if (r == -2) { CB_WARN("https", "%s: timed out", c->host); break; }
            if (r < 0) { if (in.len == 0) rc = 1; break; }
            if (r == 0) eof = true;
            else in.len += (size_t)r;
        } else {
            int r = SSL_read(c->ssl, in.p + in.len, (int)(in.cap - in.len - 1));
            if (r > 0) {
                in.len += (size_t)r;
            } else {
                int e = SSL_get_error(c->ssl, r);
                ERR_clear_error();
                if (e == SSL_ERROR_ZERO_RETURN || (e == SSL_ERROR_SYSCALL && errno == 0)) eof = true;
                else if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) { CB_WARN("https", "%s: timed out", c->host); break; }
                    continue;
                } else {
                    if (in.len == 0) rc = 1;
                    break;
                }
            }
        }
        // A clean close before any byte: the peer had dropped the idle connection.
        if (eof && in.len == 0) { rc = 1; break; }
        int k = cb_https_parse_response(in.p, in.len, head, eof, out, ka);
        if (k > 0) { rc = CB_OK; if ((size_t)k != in.len) *ka = false; break; }
        if (k < 0) { rc = k; break; }
        if (eof) break;
    }
    cb_buf_free(&in);
    return rc;
}

#ifndef SO_NOSIGPIPE
static void ignore_sigpipe(void) { signal(SIGPIPE, SIG_IGN); }
#endif

static int request(bool tls, const char *host, int port, const char *method, const char *path, const char *headers,
                   const void *body, size_t body_len, int timeout_ms, cb_https_response *out) {
    memset(out, 0, sizeof *out);
    cb_buf_init(&out->headers);
    cb_buf_init(&out->body);
#ifndef SO_NOSIGPIPE
    // Linux has no per-socket switch and OpenSSL writes with plain write(2):
    // a peer that closed must cost an error, not the process.
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, ignore_sigpipe);
#endif
    int64_t deadline = cb_mono_ms() + (timeout_ms > 0 ? timeout_ms : 10000);
    bool head = strcmp(method, "HEAD") == 0;
    cb_buf req;
    cb_buf_init(&req);
    cb_buf_printf(&req, "%s %s HTTP/1.1\r\n", method, path);
    if (port == 443) cb_buf_printf(&req, "Host: %s\r\n", host);
    else cb_buf_printf(&req, "Host: %s:%d\r\n", host, port);
    if (headers) cb_buf_puts(&req, headers);
    if (body_len || strcmp(method, "POST") == 0 || strcmp(method, "PUT") == 0)
        cb_buf_printf(&req, "Content-Length: %zu\r\n", body_len);
    cb_buf_puts(&req, "\r\n");
    if (body_len) cb_buf_append(&req, body, body_len);
    if (req.oom) { cb_buf_free(&req); return CB_E_NOMEM; }

    const int kind = host_kind(host, tls);
    const int64_t t0 = mono_us();
    int rc = CB_E_IO;
    for (int attempt = 0; attempt < 2; attempt++) {
        conn c;
        bool reused = attempt == 0 && pool_take(host, port, tls, &c);
        if (!reused) {
            atomic_fetch_add_explicit(&g_dials[kind], 1, memory_order_relaxed);
            rc = conn_open(&c, host, port, tls, deadline);
            if (rc != CB_OK) break;
        }
        bool ka = false;
        rc = exchange(&c, &req, head, deadline, out, &ka);
        if (rc == CB_OK && ka) pool_put(&c);
        else conn_close(&c);
        if (rc == 1 && reused) continue;   // the idle connection was dead: once more, fresh
        if (rc == 1) rc = CB_E_IO;
        break;
    }
    atomic_fetch_add_explicit(&g_calls[kind], 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_us[kind], mono_us() - t0, memory_order_relaxed);
    cb_buf_free(&req);
    return rc;
}

int cb_https_request(const char *host, int port, const char *method, const char *path, const char *headers,
                     const void *body, size_t body_len, int timeout_ms, cb_https_response *out) {
    return request(true, host, port, method, path, headers, body, body_len, timeout_ms, out);
}

int cb_http_plain_request(const char *host, int port, const char *method, const char *path, const char *headers,
                          const void *body, size_t body_len, int timeout_ms, cb_https_response *out) {
    return request(false, host, port, method, path, headers, body, body_len, timeout_ms, out);
}
