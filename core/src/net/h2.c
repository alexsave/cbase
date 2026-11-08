// h2.c - the HTTP/2 client's connections and channels: see h2.h.
//
// LOCKING: one mutex per channel guards the channel, its connections' sessions
// and every call's record. A connection's I/O thread alone touches its socket,
// its TLS state and its outgoing bytes (conn.out), and does that without the
// lock; it takes the lock to hand the session what it read and to take what
// the session has to send. The session's callbacks therefore run under the
// lock, on the I/O thread, and only fill the call's record and wake its caller.
//
// LIFETIME: a connection is referenced by its I/O thread and by each call on
// it (conn.refs). The thread drops its reference when it ends (the session dead
// and its GOAWAY written, a drained connection after a GOAWAY, or idle); the
// last reference frees it. The channel's free waits for every call and every
// connection to be gone.
#include "net/h2.h"

#include "net/tls.h"
#include "util/clock.h"
#include "util/err.h"
#include "util/log.h"

#include <openssl/err.h>
#include <openssl/x509v3.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define DEFAULT_CONNECT_MS 10000
#define DEFAULT_IDLE_MS    30000
#define DEFAULT_MAX_BODY   (32u << 20)
// The most outgoing bytes a connection holds before it stops asking the session for more.
#define OUT_HIGH_WATER     (256u << 10)
// How long a dead connection may take to write its GOAWAY before it is closed anyway.
#define GOAWAY_LINGER_MS   1000

typedef struct conn conn;

typedef struct {
    pthread_cond_t  cv;
    bool            done;
    cb_h2_end       end;
    conn           *c;
    cb_h2_stream   *st;
    cb_h2_response *resp;
    size_t          max_body;
    bool            too_big;
} call;

struct conn {
    cb_h2_channel *ch;
    int            fd;
    SSL           *ssl;
    cb_h2_sess    *sess;
    int            wake[2];
    cb_buf         out;          // the I/O thread's alone
    int            refs;
    bool           exited;
    int64_t        idle_since;   // when its streams fell to 0 (cb_mono_ms), 0 while busy
    cb_h2_why      fail_why;     // set before the I/O thread fails the session itself
    char           fail_err[120];
    conn          *next;         // the channel's list
};

struct cb_h2_channel {
    pthread_mutex_t mu;
    pthread_cond_t  cv;          // a connect finished, a connection or a call ended
    bool            tls;
    char            host[256];
    int             port;
    char            authority[300];
    SSL_CTX        *ctx;
    cb_h2_opts      o;
    conn           *cur;         // where new calls go
    bool            connecting;
    conn           *conns;
    int             n_calls;
    bool            closing;
    int             dials;
};

// ---- the response ---------------------------------------------------------------
void cb_h2_response_init(cb_h2_response *r) {
    memset(r, 0, sizeof *r);
    cb_buf_init(&r->fields);
    cb_buf_init(&r->body);
}

void cb_h2_response_free(cb_h2_response *r) {
    cb_buf_free(&r->fields);
    cb_buf_free(&r->body);
    cb_h2_response_init(r);
}

static void response_reset(cb_h2_response *r) {
    cb_buf_reset(&r->fields);
    cb_buf_reset(&r->body);
    r->status = 0;
    r->n_headers = r->n_trailers = 0;
    r->why = CB_H2_WHY_NONE;
    r->h2_code = 0;
    r->err[0] = 0;
}

bool cb_h2_response_get(const cb_h2_response *r, bool trailer, const char *name, cb_span *value) {
    size_t nl = strlen(name), at = 0;
    const char *p = (const char *)r->fields.p;
    for (int i = 0; i < r->n_headers + r->n_trailers && at < r->fields.len; i++) {
        const char *n = p + at;
        size_t n_len = strlen(n);
        const char *v = n + n_len + 1;
        size_t v_len = strlen(v);
        at += n_len + 1 + v_len + 1;
        if ((i >= r->n_headers) != trailer) continue;
        if (n_len == nl && memcmp(n, name, nl) == 0) {
            *value = cb_span_of(v, v_len);
            return true;
        }
    }
    return false;
}

// ---- the session's callbacks (under the channel's lock, on the I/O thread) ------------
static int on_header(void *user, bool trailer, const char *name, size_t nl, const char *value, size_t vl) {
    call *k = user;
    cb_h2_response *r = k->resp;
    // A field holding a NUL would cut the "name\0value\0" list: the session
    // already refuses those (RFC 9113 8.2.1), so this cannot happen.
    if (memchr(name, 0, nl) || memchr(value, 0, vl)) return CB_E_INVALID;
    cb_buf_append(&r->fields, name, nl);
    cb_buf_putc(&r->fields, 0);
    cb_buf_append(&r->fields, value, vl);
    cb_buf_putc(&r->fields, 0);
    if (trailer) r->n_trailers++;
    else r->n_headers++;
    if (!trailer && nl == 7 && memcmp(name, ":status", 7) == 0 && vl == 3)
        r->status = (value[0] - '0') * 100 + (value[1] - '0') * 10 + (value[2] - '0');
    return CB_OK;
}

static int on_data(void *user, const uint8_t *p, size_t n) {
    call *k = user;
    if (k->resp->body.len + n > k->max_body) {
        k->too_big = true;
        return CB_E_FULL;
    }
    cb_buf_append(&k->resp->body, p, n);
    return CB_OK;
}

static void on_end(void *user, const cb_h2_end *e) {
    call *k = user;
    k->end = *e;
    k->done = true;
    k->st = NULL;
    pthread_cond_signal(&k->cv);
}

static const cb_h2_callbacks CALLBACKS = { on_header, on_data, on_end };

// ---- connections ------------------------------------------------------------------
static void wake(conn *c) {
    char b = 1;
    ssize_t w = write(c->wake[1], &b, 1);   // a full pipe already holds a wake-up
    (void)w;
}

static void conn_free(conn *c) {
    cb_h2_channel *ch = c->ch;
    for (conn **pp = &ch->conns; *pp; pp = &(*pp)->next)
        if (*pp == c) { *pp = c->next; break; }
    cb_h2_sess_free(c->sess);
    if (c->ssl) SSL_free(c->ssl);
    if (c->fd >= 0) close(c->fd);
    close(c->wake[0]);
    close(c->wake[1]);
    cb_buf_free(&c->out);
    free(c);
    pthread_cond_broadcast(&ch->cv);
}

static void conn_unref(conn *c) {
    if (--c->refs == 0) conn_free(c);
}

// The I/O thread found the socket gone (or the channel closing): every call on
// the connection ends with why.
static void conn_fail(conn *c, cb_h2_why why, const char *err) {
    if (cb_h2_sess_dead(c->sess)) return;
    c->fail_why = why;
    snprintf(c->fail_err, sizeof c->fail_err, "%s", err);
    cb_h2_sess_fail(c->sess, CB_H2_NO_ERROR, err);
}

// Writes what it can of c->out. false when the socket failed.
static bool write_out(conn *c, bool *want_read) {
    *want_read = false;
    while (c->out.len) {
        if (c->ssl) {
            int w = SSL_write(c->ssl, c->out.p, c->out.len > INT32_MAX ? INT32_MAX : (int)c->out.len);
            if (w > 0) { cb_buf_consume(&c->out, (size_t)w); continue; }
            int e = SSL_get_error(c->ssl, w);
            if (e == SSL_ERROR_WANT_WRITE) return true;
            if (e == SSL_ERROR_WANT_READ) { *want_read = true; return true; }
            ERR_clear_error();
            return false;
        }
#ifdef MSG_NOSIGNAL
        ssize_t w = send(c->fd, c->out.p, c->out.len, MSG_NOSIGNAL);
#else
        ssize_t w = send(c->fd, c->out.p, c->out.len, 0);
#endif
        if (w > 0) { cb_buf_consume(&c->out, (size_t)w); continue; }
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return true;
        return false;
    }
    return true;
}

// Reads what is there into buf: > 0 bytes, 0 when nothing more for now, -1 at
// EOF or an error. *want_write: TLS needs the socket writable to read on.
static ssize_t read_in(conn *c, uint8_t *buf, size_t cap, bool *want_write) {
    *want_write = false;
    if (c->ssl) {
        int r = SSL_read(c->ssl, buf, (int)cap);
        if (r > 0) return r;
        int e = SSL_get_error(c->ssl, r);
        if (e == SSL_ERROR_WANT_READ) return 0;
        if (e == SSL_ERROR_WANT_WRITE) { *want_write = true; return 0; }
        ERR_clear_error();
        return -1;
    }
    ssize_t r = recv(c->fd, buf, cap, 0);
    if (r > 0) return r;
    if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return 0;
    return -1;
}

static void *io_main(void *arg) {
    conn *c = arg;
    cb_h2_channel *ch = c->ch;
    uint8_t *buf = malloc(64u << 10);
    bool read_wants_write = false, write_wants_read = false;
    int64_t dead_since = 0;
    pthread_mutex_lock(&ch->mu);
    for (;;) {
        if (ch->closing) conn_fail(c, CB_H2_WHY_CLOSED, "the channel was closed");
        if (!buf) conn_fail(c, CB_H2_WHY_PROTOCOL, "no memory for the connection's reads");
        while (c->out.len < OUT_HIGH_WATER && cb_h2_sess_flush(c->sess, &c->out) > 0) {
        }
        bool dead = cb_h2_sess_dead(c->sess);
        int streams = cb_h2_sess_streams(c->sess);
        int64_t now = cb_mono_ms();
        if (dead) {
            if (!dead_since) dead_since = now;
            if (!c->out.len || now - dead_since >= GOAWAY_LINGER_MS) break;
        } else if (!cb_h2_sess_accepting(c->sess) && streams == 0 && !c->out.len) {
            break;   // went away, or closed gracefully, and drained
        }
        int timeout = -1;
        if (dead) {
            timeout = (int)(GOAWAY_LINGER_MS - (now - dead_since));
        } else if (streams == 0) {
            if (!c->idle_since) c->idle_since = now;
            int64_t idle_left = c->idle_since + ch->o.idle_close_ms - now;
            if (idle_left <= 0 && cb_h2_sess_accepting(c->sess)) {
                // No new call comes here: get_conn takes only an accepting session.
                cb_h2_sess_goaway(c->sess);
                continue;
            }
            timeout = idle_left > 0 ? (int)idle_left : 0;
        } else {
            c->idle_since = 0;
        }
        bool want_write = c->out.len > 0 || read_wants_write;
        pthread_mutex_unlock(&ch->mu);

        struct pollfd pf[2] = { { c->fd, (short)(POLLIN | (want_write ? POLLOUT : 0)), 0 }, { c->wake[0], POLLIN, 0 } };
        // TLS may hold bytes already decrypted: read them without waiting.
        if (c->ssl && SSL_pending(c->ssl) > 0) timeout = 0;
        int pr = poll(pf, 2, timeout);
        if (pr < 0 && errno != EINTR) {
            pthread_mutex_lock(&ch->mu);
            conn_fail(c, CB_H2_WHY_PROTOCOL, "poll failed on the connection");
            continue;
        }
        if (pf[1].revents & POLLIN) {
            char drain[64];
            while (read(c->wake[0], drain, sizeof drain) > 0) {
            }
        }
        bool failed = false;
        const char *fail_err = NULL;
        if (c->out.len && ((pf[0].revents & POLLOUT) || write_wants_read)) {
            if (!write_out(c, &write_wants_read)) { failed = true; fail_err = "writing to the connection failed"; }
        }
        bool readable = (pf[0].revents & (POLLIN | POLLHUP | POLLERR)) || (read_wants_write && (pf[0].revents & POLLOUT)) ||
                        (c->ssl && SSL_pending(c->ssl) > 0);
        if (!failed && readable && buf) {
            // Read until the socket would block, handing each piece to the session.
            for (int spins = 0; spins < 64; spins++) {
                ssize_t r = read_in(c, buf, 64u << 10, &read_wants_write);
                if (r == 0) break;
                if (r < 0) { failed = true; fail_err = "the server closed the connection"; break; }
                pthread_mutex_lock(&ch->mu);
                cb_h2_sess_recv(c->sess, buf, (size_t)r);
                pthread_mutex_unlock(&ch->mu);
            }
        }
        pthread_mutex_lock(&ch->mu);
        if (failed) {
            conn_fail(c, CB_H2_WHY_PROTOCOL, fail_err);
            c->out.len = 0;   // nothing more can be written
        }
    }
    // Ended: anything still in hand fails, the socket closes, and the thread lets go.
    conn_fail(c, CB_H2_WHY_PROTOCOL, "the connection closed");
    if (ch->cur == c) ch->cur = NULL;
    if (c->ssl) { SSL_free(c->ssl); c->ssl = NULL; }
    if (c->fd >= 0) { close(c->fd); c->fd = -1; }
    c->exited = true;
    conn_unref(c);
    pthread_cond_broadcast(&ch->cv);
    pthread_mutex_unlock(&ch->mu);
    free(buf);
    return NULL;
}

static int64_t left_ms(int64_t deadline) {
    int64_t l = deadline - cb_mono_ms();
    return l > 0 ? l : 0;
}

// Opens a connection (TCP, then TLS with ALPN h2), without the channel's lock.
// CB_OK and *out, or CB_E_IO with why and err.
static int conn_open(cb_h2_channel *ch, int64_t deadline, conn **out, cb_h2_why *why, char *err, size_t cap) {
    int64_t cdl = cb_mono_ms() + ch->o.connect_timeout_ms;
    if (cdl > deadline) cdl = deadline;
    int fd = cb_net_dial(ch->host, ch->port, cdl, true);
    if (fd < 0) {
        *why = left_ms(deadline) ? CB_H2_WHY_CONNECT : CB_H2_WHY_TIMEOUT;
        snprintf(err, cap, "could not connect to %s:%d", ch->host, ch->port);
        return CB_E_IO;
    }
    SSL *ssl = NULL;
    if (ch->tls) {
        ssl = SSL_new(ch->ctx);
        if (!ssl || cb_tls_bind_host(ssl, ch->host) != CB_OK) {
            if (ssl) SSL_free(ssl);
            close(fd);
            *why = CB_H2_WHY_TLS;
            snprintf(err, cap, "TLS to %s:%d: the host name to verify could not be set", ch->host, ch->port);
            return CB_E_IO;
        }
        SSL_set_mode(ssl, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
        SSL_set_fd(ssl, fd);
        for (;;) {
            int r = SSL_connect(ssl);
            if (r == 1) break;
            int e = SSL_get_error(ssl, r);
            short ev = e == SSL_ERROR_WANT_READ ? POLLIN : e == SSL_ERROR_WANT_WRITE ? POLLOUT : 0;
            struct pollfd pf = { fd, ev, 0 };
            if (ev && left_ms(cdl) > 0 && poll(&pf, 1, (int)left_ms(cdl)) == 1) continue;
            long vr = SSL_get_verify_result(ssl);
            char e2[160] = "";
            unsigned long code = ERR_get_error();
            if (code) ERR_error_string_n(code, e2, sizeof e2);
            ERR_clear_error();
            *why = ev && !left_ms(deadline) ? CB_H2_WHY_TIMEOUT : CB_H2_WHY_TLS;
            snprintf(err, cap, "TLS to %s:%d failed%s%s%s%s", ch->host, ch->port, ev ? " (timed out)" : "",
                     e2[0] ? ": " : "", e2, vr != X509_V_OK ? "; the certificate did not verify" : "");
            if (vr != X509_V_OK) {
                size_t at = strlen(err);
                snprintf(err + at, cap - at, " (%s)", X509_verify_cert_error_string(vr));
            }
            SSL_free(ssl);
            close(fd);
            return CB_E_IO;
        }
        const unsigned char *proto = NULL;
        unsigned plen = 0;
        SSL_get0_alpn_selected(ssl, &proto, &plen);
        if (plen != 2 || memcmp(proto, "h2", 2) != 0) {
            SSL_free(ssl);
            close(fd);
            *why = CB_H2_WHY_TLS;
            snprintf(err, cap, "%s:%d did not agree to HTTP/2 (ALPN h2)", ch->host, ch->port);
            return CB_E_IO;
        }
    }
    conn *c = calloc(1, sizeof *c);
    int pfd[2] = { -1, -1 };
    if (!c || pipe(pfd) != 0) {
        free(c);
        if (ssl) SSL_free(ssl);
        close(fd);
        *why = CB_H2_WHY_PROTOCOL;
        snprintf(err, cap, "no resources for a connection");
        return CB_E_IO;
    }
    for (int i = 0; i < 2; i++) fcntl(pfd[i], F_SETFL, fcntl(pfd[i], F_GETFL, 0) | O_NONBLOCK);
    c->ch = ch;
    c->fd = fd;
    c->ssl = ssl;
    c->wake[0] = pfd[0];
    c->wake[1] = pfd[1];
    cb_buf_init(&c->out);
    c->sess = cb_h2_sess_new(&ch->o.limits, &CALLBACKS);
    if (!c->sess) {
        if (ssl) SSL_free(ssl);
        close(fd);
        close(pfd[0]);
        close(pfd[1]);
        free(c);
        *why = CB_H2_WHY_PROTOCOL;
        snprintf(err, cap, "no memory for a session");
        return CB_E_IO;
    }
    *out = c;
    return CB_OK;
}

// ---- the channel --------------------------------------------------------------------
#ifndef SO_NOSIGPIPE
static void ignore_sigpipe(void) { signal(SIGPIPE, SIG_IGN); }
#endif

int cb_h2_channel_new(const char *origin, const cb_h2_opts *opts, cb_h2_channel **out) {
#ifndef SO_NOSIGPIPE
    // Linux has no per-socket switch and OpenSSL writes with plain write(2):
    // a peer that closed must cost an error, not the process.
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, ignore_sigpipe);
#endif
    bool tls;
    const char *rest;
    if (strncmp(origin, "https://", 8) == 0) { tls = true; rest = origin + 8; }
    else if (strncmp(origin, "http://", 7) == 0) { tls = false; rest = origin + 7; }
    else return CB_E_INVALID;
    size_t n = strcspn(rest, "/");
    if (rest[n] && strcmp(rest + n, "/") != 0) return CB_E_INVALID;   // an origin: no path
    char hostport[300];
    if (n == 0 || n >= sizeof hostport) return CB_E_INVALID;
    memcpy(hostport, rest, n);
    hostport[n] = 0;
    char host[256];
    int port = tls ? 443 : 80;
    const char *colon = NULL;
    if (hostport[0] == '[') {   // [v6]:port
        const char *rb = strchr(hostport, ']');
        if (!rb || (size_t)(rb - hostport - 1) >= sizeof host) return CB_E_INVALID;
        memcpy(host, hostport + 1, (size_t)(rb - hostport - 1));
        host[rb - hostport - 1] = 0;
        if (rb[1] == ':') colon = rb + 1;
        else if (rb[1]) return CB_E_INVALID;
    } else {
        colon = strchr(hostport, ':');
        size_t hl = colon ? (size_t)(colon - hostport) : n;
        if (hl == 0 || hl >= sizeof host) return CB_E_INVALID;
        memcpy(host, hostport, hl);
        host[hl] = 0;
    }
    if (colon) {
        char *end;
        long p = strtol(colon + 1, &end, 10);
        if (*end || p < 1 || p > 65535 || end == colon + 1) return CB_E_INVALID;
        port = (int)p;
    }
    cb_h2_channel *ch = calloc(1, sizeof *ch);
    if (!ch) return CB_E_NOMEM;
    ch->tls = tls;
    snprintf(ch->host, sizeof ch->host, "%s", host);
    ch->port = port;
    snprintf(ch->authority, sizeof ch->authority, "%s", hostport);
    if (opts) ch->o = *opts;
    if (ch->o.connect_timeout_ms <= 0) ch->o.connect_timeout_ms = DEFAULT_CONNECT_MS;
    if (ch->o.idle_close_ms <= 0) ch->o.idle_close_ms = DEFAULT_IDLE_MS;
    cb_h2_limits zero;
    memset(&zero, 0, sizeof zero);
    if (memcmp(&ch->o.limits, &zero, sizeof zero) == 0) cb_h2_limits_default(&ch->o.limits);
    ch->o.ca_file = NULL;   // used now, not kept
    if (tls) {
        static const unsigned char ALPN[] = { 2, 'h', '2' };
        ch->ctx = cb_tls_client_ctx(opts ? opts->ca_file : NULL, ALPN, sizeof ALPN);
        if (!ch->ctx) { free(ch); return CB_E_IO; }
    }
    pthread_mutex_init(&ch->mu, NULL);
    pthread_cond_init(&ch->cv, NULL);
    *out = ch;
    return CB_OK;
}

void cb_h2_channel_free(cb_h2_channel *ch) {
    if (!ch) return;
    pthread_mutex_lock(&ch->mu);
    ch->closing = true;
    ch->cur = NULL;
    for (conn *c = ch->conns; c; c = c->next) wake(c);
    while (ch->conns || ch->n_calls || ch->connecting) pthread_cond_wait(&ch->cv, &ch->mu);
    pthread_mutex_unlock(&ch->mu);
    pthread_mutex_destroy(&ch->mu);
    pthread_cond_destroy(&ch->cv);
    if (ch->ctx) SSL_CTX_free(ch->ctx);
    free(ch);
}

int cb_h2_channel_dials(cb_h2_channel *ch) {
    pthread_mutex_lock(&ch->mu);
    int n = ch->dials;
    pthread_mutex_unlock(&ch->mu);
    return n;
}

// Waits on cv until deadline (cb_mono_ms). false when it passed.
static bool wait_until(pthread_cond_t *cv, pthread_mutex_t *mu, int64_t deadline) {
    int64_t l = left_ms(deadline);
    if (l <= 0) return false;
    // The condition variable's clock is the wall clock (macOS has no other):
    // the monotonic time left is added to the wall clock now.
    struct timeval tv;
    gettimeofday(&tv, NULL);
    int64_t ns = (int64_t)tv.tv_usec * 1000 + (l % 1000) * 1000000;
    struct timespec ts = { tv.tv_sec + (time_t)(l / 1000) + (time_t)(ns / 1000000000), (long)(ns % 1000000000) };
    pthread_cond_timedwait(cv, mu, &ts);
    return true;
}

static void set_why(cb_h2_response *out, cb_h2_why why, uint32_t code, const char *fmt, const char *arg) {
    out->why = why;
    out->h2_code = code;
    snprintf(out->err, sizeof out->err, fmt, arg);
}

// The connection new calls go to, opened when there is none. Under the lock;
// CB_OK and *out (referenced), or CB_E_IO with out's why.
static int get_conn(cb_h2_channel *ch, int64_t deadline, conn **outc, cb_h2_response *out) {
    for (;;) {
        if (ch->closing) { set_why(out, CB_H2_WHY_CLOSED, 0, "%s", "the channel was closed"); return CB_E_IO; }
        if (ch->cur && cb_h2_sess_accepting(ch->cur->sess)) {
            ch->cur->refs++;
            *outc = ch->cur;
            return CB_OK;
        }
        if (ch->cur) ch->cur = NULL;   // going away: it drains by itself
        if (ch->connecting) {
            if (!wait_until(&ch->cv, &ch->mu, deadline)) {
                set_why(out, CB_H2_WHY_TIMEOUT, 0, "%s", "no connection within the deadline");
                return CB_E_IO;
            }
            continue;
        }
        ch->connecting = true;
        ch->dials++;
        pthread_mutex_unlock(&ch->mu);
        conn *c = NULL;
        cb_h2_why why = CB_H2_WHY_NONE;
        char err[sizeof out->err];
        int rc = conn_open(ch, deadline, &c, &why, err, sizeof err);
        pthread_mutex_lock(&ch->mu);
        ch->connecting = false;
        pthread_cond_broadcast(&ch->cv);
        if (rc != CB_OK) {
            out->why = why;
            snprintf(out->err, sizeof out->err, "%s", err);
            return CB_E_IO;
        }
        c->refs = 1;   // the I/O thread's
        c->next = ch->conns;
        ch->conns = c;
        pthread_t th;
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&th, &at, io_main, c) != 0) {
            pthread_attr_destroy(&at);
            c->exited = true;
            conn_unref(c);
            set_why(out, CB_H2_WHY_PROTOCOL, 0, "%s", "could not start the connection's thread");
            return CB_E_IO;
        }
        pthread_attr_destroy(&at);
        if (ch->closing) continue;   // it ends by itself; the loop's top reports the close
        ch->cur = c;
    }
}

// conn_why: the connection's own failure, copied while the call still held it.
static cb_h2_why why_of(const call *k, cb_h2_why conn_why) {
    if (k->too_big) return CB_H2_WHY_TOO_BIG;
    switch (k->end.kind) {
    case CB_H2_END_PEER_RESET: return CB_H2_WHY_RESET;
    case CB_H2_END_GOAWAY: return CB_H2_WHY_GOAWAY;
    case CB_H2_END_FAILED: return conn_why ? conn_why : CB_H2_WHY_PROTOCOL;
    default: return CB_H2_WHY_PROTOCOL;
    }
}

int cb_h2_request(cb_h2_channel *ch, const char *method, const char *path, const cb_h2_field *fields, int n_fields,
                  const cb_h2_iov *body, int n_body, int timeout_ms, size_t max_body, cb_h2_response *out) {
    response_reset(out);
    int64_t deadline = cb_mono_ms() + (timeout_ms > 0 ? timeout_ms : 10000);
    pthread_mutex_lock(&ch->mu);
    ch->n_calls++;
    int rc = CB_E_IO;
    for (int attempt = 0; attempt < 2; attempt++) {
        conn *c;
        if ((rc = get_conn(ch, deadline, &c, out)) != CB_OK) break;
        call k;
        memset(&k, 0, sizeof k);
        pthread_cond_init(&k.cv, NULL);
        k.c = c;
        k.resp = out;
        k.max_body = max_body ? max_body : DEFAULT_MAX_BODY;
        rc = cb_h2_sess_submit(c->sess, method, ch->tls ? "https" : "http", ch->authority, path, fields, n_fields, body,
                               n_body, &k, &k.st);
        if (rc == CB_E_STATE) {   // it stopped taking streams since get_conn: another connection
            if (ch->cur == c) ch->cur = NULL;
            conn_unref(c);
            pthread_cond_destroy(&k.cv);
            attempt--;
            if (!left_ms(deadline)) { set_why(out, CB_H2_WHY_TIMEOUT, 0, "%s", "no connection within the deadline"); rc = CB_E_IO; break; }
            continue;
        }
        if (rc != CB_OK) {
            conn_unref(c);
            pthread_cond_destroy(&k.cv);
            if (rc == CB_E_FULL) snprintf(out->err, sizeof out->err, "too many requests in flight on one connection");
            break;
        }
        wake(c);
        bool timed_out = false;
        while (!k.done) {
            if (!wait_until(&k.cv, &ch->mu, deadline) && !k.done) {
                cb_h2_sess_cancel(c->sess, k.st);
                wake(c);
                timed_out = true;
                break;
            }
        }
        // The connection's failure, read before letting go of it.
        cb_h2_why conn_why = c->fail_why;
        char conn_err[sizeof c->fail_err];
        snprintf(conn_err, sizeof conn_err, "%s", c->fail_err);
        conn_unref(c);
        pthread_cond_destroy(&k.cv);
        if (timed_out) {
            char t[32];
            snprintf(t, sizeof t, "%d", timeout_ms > 0 ? timeout_ms : 10000);
            set_why(out, CB_H2_WHY_TIMEOUT, 0, "no answer within %s ms", t);
            rc = CB_E_IO;
            break;
        }
        if (k.end.ok) { rc = CB_OK; break; }
        // Never processed by the peer: once more, on a connection that takes it
        // (the same one after a REFUSED_STREAM, a fresh one after a GOAWAY). A
        // closing channel needs no test here: get_conn refuses it first.
        if (k.end.unprocessed && attempt == 0 && left_ms(deadline)) {
            response_reset(out);
            continue;
        }
        out->why = why_of(&k, conn_why);
        out->h2_code = k.end.h2_code;
        if (out->why == CB_H2_WHY_TOO_BIG)
            snprintf(out->err, sizeof out->err, "the response body is over %zu bytes", k.max_body);
        else if (k.end.kind == CB_H2_END_FAILED && conn_err[0])
            snprintf(out->err, sizeof out->err, "%s", conn_err);
        else
            snprintf(out->err, sizeof out->err, "%s (HTTP/2 code %u)", k.end.why ? k.end.why : "the stream failed",
                     (unsigned)k.end.h2_code);
        rc = CB_E_IO;
        break;
    }
    ch->n_calls--;
    pthread_cond_broadcast(&ch->cv);
    pthread_mutex_unlock(&ch->mu);
    return rc;
}
