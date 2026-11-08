// lib_h2_srv.c - a local HTTP/2 server for the client's tests: see lib_h2_srv.h.
#include "lib_h2_srv.h"

#include "b64/b64.h"
#include "lib_tls.h"
#include "net/h2_session.h"
#include "net/hpack.h"
#include "util/err.h"
#include "util/str.h"

#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define MAX_STREAMS 512

typedef struct sconn sconn;

typedef struct {
    uint32_t id;
    bool     used, open, reset;
    bool     key_never;  // x-goog-api-key came never-indexed
    int64_t  window;     // what this side may still send on it
    cb_buf   path, api_key, timeout, body;
} sstream;

struct h2srv {
    h2srv_opts      o;
    int             lfd, port;
    char            ca_file[256];
    SSL_CTX        *ctx;
    pthread_t       acceptor;
    atomic_int      stop, accepts, requests, resets, closes, max_open, refused_once, goaway_once, headers_once;
    unsigned char   alpn_offer[64];   // the client's last ALPN list, as sent
    atomic_int      alpn_offer_len;
    pthread_mutex_t mu;      // the threads' count and the connection list
    pthread_cond_t  cv;
    int             threads;
    sconn          *conns;
};

struct sconn {
    h2srv          *s;
    int             fd;
    SSL            *ssl;
    pthread_mutex_t wmu;     // writes, the windows, the streams
    pthread_cond_t  wcv;     // a window opened, or the connection died
    bool            dead;
    int64_t         conn_window;
    uint32_t        peer_initial;
    sstream         st[MAX_STREAMS];
    int             open_now;
    int             refs;    // the reader and each responder
    sconn          *next;
};

// ---- I/O --------------------------------------------------------------------------
static int s_write(sconn *c, const void *p, size_t n) {
    const uint8_t *b = p;
    while (n) {
        ssize_t w;
        if (c->ssl) {
            int k = SSL_write(c->ssl, b, (int)n);
            w = k > 0 ? k : -1;
        } else {
#ifdef MSG_NOSIGNAL
            w = send(c->fd, b, n, MSG_NOSIGNAL);
#else
            w = send(c->fd, b, n, 0);
#endif
        }
        if (w <= 0) return -1;
        b += w;
        n -= (size_t)w;
    }
    return 0;
}

static ssize_t s_read(sconn *c, uint8_t *b, size_t n) {
    if (c->ssl) {
        int k = SSL_read(c->ssl, b, (int)n);
        return k > 0 ? k : -1;
    }
    return recv(c->fd, b, n, 0);
}

static void send_buf(sconn *c, cb_buf *b) {   // under wmu
    if (!c->dead && s_write(c, b->p, b->len) != 0) c->dead = true;
    cb_buf_reset(b);
}

static void unref(sconn *c) {
    h2srv *s = c->s;
    pthread_mutex_lock(&s->mu);
    bool last = --c->refs == 0;
    if (last) {
        for (sconn **pp = &s->conns; *pp; pp = &(*pp)->next)
            if (*pp == c) { *pp = c->next; break; }
    }
    pthread_mutex_unlock(&s->mu);
    if (!last) return;
    if (c->ssl) SSL_free(c->ssl);
    close(c->fd);
    for (int i = 0; i < MAX_STREAMS; i++) {
        cb_buf_free(&c->st[i].path);
        cb_buf_free(&c->st[i].api_key);
        cb_buf_free(&c->st[i].timeout);
        cb_buf_free(&c->st[i].body);
    }
    pthread_mutex_destroy(&c->wmu);
    pthread_cond_destroy(&c->wcv);
    free(c);
}

static void thread_done(h2srv *s) {
    pthread_mutex_lock(&s->mu);
    s->threads--;
    pthread_cond_broadcast(&s->cv);
    pthread_mutex_unlock(&s->mu);
}

static sstream *find(sconn *c, uint32_t id) {
    for (int i = 0; i < MAX_STREAMS; i++)
        if (c->st[i].used && c->st[i].id == id) return &c->st[i];
    return NULL;
}

// ---- responding (a thread per request) ------------------------------------------------
typedef struct {
    sconn   *c;
    uint32_t id;
    char     path[256];
    char     api_key[256], timeout[32];
    bool     key_never;
    cb_buf   body;
} job;

static void field(cb_buf *block, const char *n, const char *v) { cb_hpack_encode(block, n, strlen(n), v, strlen(v), false); }

static void headers(sconn *c, uint32_t id, cb_buf *block, bool end) {   // under wmu
    cb_buf f;
    cb_buf_init(&f);
    cb_h2_frame_headers(&f, id, block->p, block->len, end, CB_H2_DEFAULT_FRAME);
    send_buf(c, &f);
    cb_buf_free(&f);
}

// DATA within the client's windows, waiting for them to open. Under wmu.
static void data(sconn *c, uint32_t id, const uint8_t *p, size_t n, bool end) {
    cb_buf f;
    cb_buf_init(&f);
    size_t at = 0;
    do {
        sstream *st = find(c, id);
        while (!c->dead && st && !st->reset && n - at > 0 && (c->conn_window <= 0 || st->window <= 0))
            pthread_cond_wait(&c->wcv, &c->wmu);
        if (c->dead || !st || st->reset) break;
        size_t k = n - at;
        if (k > CB_H2_DEFAULT_FRAME) k = CB_H2_DEFAULT_FRAME;
        if ((int64_t)k > c->conn_window) k = (size_t)c->conn_window;
        if ((int64_t)k > st->window) k = (size_t)st->window;
        bool last = end && at + k == n;
        cb_h2_frame_head(&f, (uint32_t)k, CB_H2_DATA, last ? CB_H2_F_END_STREAM : 0, id);
        cb_buf_append(&f, p + at, k);
        send_buf(c, &f);
        c->conn_window -= (int64_t)k;
        st->window -= (int64_t)k;
        at += k;
    } while (at < n);
    cb_buf_free(&f);
}

static void close_stream(sconn *c, uint32_t id) {   // under wmu
    sstream *st = find(c, id);
    if (!st) return;
    if (st->open) c->open_now--;
    st->used = st->open = false;
}

static void grpc_message(cb_buf *out, const void *p, size_t n, uint8_t flag) {
    uint8_t pre[5] = { flag, (uint8_t)(n >> 24), (uint8_t)(n >> 16), (uint8_t)(n >> 8), (uint8_t)n };
    cb_buf_append(out, pre, 5);
    cb_buf_append(out, p, n);
}

static void *respond(void *arg) {
    job *j = arg;
    sconn *c = j->c;
    h2srv *s = c->s;
    cb_buf block, body;
    cb_buf_init(&block);
    cb_buf_init(&body);
    const char *path = j->path;
    if (strncmp(path, "/slow/", 6) == 0) {
        long ms = strtol(path + 6, NULL, 10);
        struct timespec ts = { ms / 1000, (ms % 1000) * 1000000 };
        nanosleep(&ts, NULL);
    }
    pthread_mutex_lock(&c->wmu);
    bool grpc = strncmp(path, "/test.Echo/", 11) == 0;
    if (strcmp(path, "/echo") == 0) {
        field(&block, ":status", "200");
        field(&block, "content-type", "application/octet-stream");
        headers(c, j->id, &block, false);
        data(c, j->id, j->body.p, j->body.len, false);
        cb_buf_reset(&block);
        field(&block, "x-trailer", "done");
        headers(c, j->id, &block, true);
    } else if (strncmp(path, "/big/", 5) == 0) {
        size_t n = (size_t)strtoul(path + 5, NULL, 10);
        for (size_t i = 0; i < n; i++) cb_buf_putc(&body, (char)(i % 251));
        field(&block, ":status", "200");
        headers(c, j->id, &block, false);
        data(c, j->id, body.p, body.len, true);
    } else if (strncmp(path, "/slow/", 6) == 0) {
        field(&block, ":status", "200");
        headers(c, j->id, &block, false);
        data(c, j->id, (const uint8_t *)"slow", 4, true);
    } else if (strncmp(path, "/status/", 8) == 0) {
        field(&block, ":status", path + 8);
        field(&block, "content-type", "text/plain");
        headers(c, j->id, &block, false);
        data(c, j->id, (const uint8_t *)"status", 6, true);
    } else if (strcmp(path, "/reset") == 0) {
        cb_h2_frame_u32(&block, CB_H2_RST_STREAM, j->id, CB_H2_INTERNAL_ERROR);
        send_buf(c, &block);
    } else if (strcmp(path, "/refuse-after-headers-once") == 0 && atomic_fetch_add(&s->headers_once, 1) == 0) {
        // Headers, then REFUSED_STREAM: still never processed, so the client sends it again.
        field(&block, ":status", "503");
        field(&block, "x-first", "1");
        headers(c, j->id, &block, false);
        cb_buf_reset(&block);
        cb_h2_frame_u32(&block, CB_H2_RST_STREAM, j->id, CB_H2_REFUSED_STREAM);
        send_buf(c, &block);
    } else if (strcmp(path, "/refuse-after-headers-once") == 0) {
        field(&block, ":status", "200");
        headers(c, j->id, &block, false);
        data(c, j->id, (const uint8_t *)"again", 5, true);
    } else if (strcmp(path, "/refuse") == 0) {
        cb_h2_frame_u32(&block, CB_H2_RST_STREAM, j->id, CB_H2_REFUSED_STREAM);
        send_buf(c, &block);
    } else if (strcmp(path, "/refuse-once") == 0 && atomic_fetch_add(&s->refused_once, 1) == 0) {
        cb_h2_frame_u32(&block, CB_H2_RST_STREAM, j->id, CB_H2_REFUSED_STREAM);
        send_buf(c, &block);
    } else if (strcmp(path, "/goaway") == 0 || (strcmp(path, "/goaway-once") == 0 && atomic_fetch_add(&s->goaway_once, 1) == 0)) {
        cb_h2_frame_goaway(&block, j->id > 2 ? j->id - 2 : 0, CB_H2_NO_ERROR);   // below it (stream 1: none)
        send_buf(c, &block);
        shutdown(c->fd, SHUT_RDWR);
        c->dead = true;
    } else if (strcmp(path, "/close") == 0) {
        shutdown(c->fd, SHUT_RDWR);
        c->dead = true;
    } else if (strcmp(path, "/refuse-once") == 0 || strcmp(path, "/goaway-once") == 0) {
        field(&block, ":status", "200");
        headers(c, j->id, &block, false);
        data(c, j->id, (const uint8_t *)"again", 5, true);
    } else if (grpc && strcmp(path, "/test.Echo/Fail") == 0) {
        // Trailers-only: everything in the one header block.
        uint8_t details[] = { 0x08, 0x08, 0x12, 0x04, 'q', 'u', 'o', 't' };
        char b64[32];
        int k = b64_encode(details, sizeof details, b64, sizeof b64);
        while (k > 0 && b64[k - 1] == '=') b64[--k] = 0;   // gRPC sends it unpadded
        field(&block, ":status", "200");
        field(&block, "content-type", "application/grpc");
        field(&block, "grpc-status", "8");
        field(&block, "grpc-message", "quota used up%3A try l%61ter %zz 100%");
        field(&block, "grpc-status-details-bin", b64);
        headers(c, j->id, &block, true);
    } else if (grpc && strncmp(path, "/test.Echo/Raw/", 15) == 0) {
        // /test.Echo/Raw/<http>/<grpc-status>/<details>/<msg>: "-" leaves a part out.
        char part[4][64] = { "", "", "", "" };
        const char *q = path + 15;
        for (int i = 0; i < 4; i++) {
            size_t k = strcspn(q, "/");
            snprintf(part[i], sizeof part[i], "%.*s", (int)k, q);
            q += k + (q[k] == '/');
        }
        bool has_msg = strcmp(part[3], "-") != 0;
        field(&block, ":status", part[0]);
        field(&block, "content-type", "application/grpc");
        cb_buf grpc_fields;
        cb_buf_init(&grpc_fields);
        if (strcmp(part[1], "-") != 0) field(&grpc_fields, "grpc-status", part[1]);
        if (strcmp(part[2], "-") != 0) field(&grpc_fields, "grpc-status-details-bin", part[2]);
        if (!has_msg) {   // trailers-only
            cb_buf_append(&block, grpc_fields.p, grpc_fields.len);
            headers(c, j->id, &block, true);
        } else {
            if (part[3][0] == '!') cb_buf_puts(&body, part[3] + 1);   // not framed
            else grpc_message(&body, part[3], strlen(part[3]), 0);
            headers(c, j->id, &block, false);
            data(c, j->id, body.p, body.len, false);
            headers(c, j->id, &grpc_fields, true);
        }
        cb_buf_free(&grpc_fields);
    } else if (grpc && (j->body.len < 5 || j->body.p[0] != 0 ||
                        ((uint32_t)j->body.p[1] << 24 | (uint32_t)j->body.p[2] << 16 | (uint32_t)j->body.p[3] << 8 |
                         j->body.p[4]) != j->body.len - 5)) {
        // The request must be one framed message, its prefix true to its length.
        field(&block, ":status", "200");
        field(&block, "content-type", "application/grpc");
        field(&block, "grpc-status", "13");
        field(&block, "grpc-message", "the request is not one framed message");
        headers(c, j->id, &block, true);
    } else if (grpc) {
        const uint8_t *msg = j->body.p + 5;
        size_t ml = j->body.len - 5;
        if (strcmp(path, "/test.Echo/Echo") == 0) grpc_message(&body, msg, ml, 0);
        else if (strcmp(path, "/test.Echo/Meta") == 0) {
            char t[400];
            int tl = snprintf(t, sizeof t, "%s|%s|%s", j->api_key, j->timeout, j->key_never ? "never" : "indexed");
            grpc_message(&body, t, (size_t)tl, 0);
        } else if (strcmp(path, "/test.Echo/Two") == 0) {
            grpc_message(&body, "a", 1, 0);
            grpc_message(&body, "b", 1, 0);
        } else if (strcmp(path, "/test.Echo/Zip") == 0) grpc_message(&body, "z", 1, 1);
        field(&block, ":status", "200");
        field(&block, "content-type", "application/grpc");
        headers(c, j->id, &block, false);
        data(c, j->id, body.p, body.len, false);
        cb_buf_reset(&block);
        field(&block, "grpc-status", "0");
        headers(c, j->id, &block, true);
    } else {
        field(&block, ":status", "404");
        headers(c, j->id, &block, true);
    }
    close_stream(c, j->id);
    pthread_mutex_unlock(&c->wmu);
    cb_buf_free(&block);
    cb_buf_free(&body);
    cb_buf_free(&j->body);
    free(j);
    unref(c);
    thread_done(s);
    return NULL;
}

// ---- reading (a thread per connection) ------------------------------------------------
typedef struct {
    sstream *st;
} hctx;

static int on_field(void *ctx, const char *n, size_t nl, const char *v, size_t vl, bool never) {
    hctx *h = ctx;
    if (!h->st) return CB_OK;
    cb_buf *dst = NULL;
    if (nl == 5 && memcmp(n, ":path", 5) == 0) dst = &h->st->path;
    else if (nl == 14 && memcmp(n, "x-goog-api-key", 14) == 0) { dst = &h->st->api_key; h->st->key_never = never; }
    else if (nl == 12 && memcmp(n, "grpc-timeout", 12) == 0) dst = &h->st->timeout;
    if (dst) {
        cb_buf_reset(dst);
        cb_buf_append(dst, v, vl);
    }
    return CB_OK;
}

static void start_job(sconn *c, sstream *st) {   // under wmu
    h2srv *s = c->s;
    job *j = calloc(1, sizeof *j);
    j->c = c;
    j->id = st->id;
    snprintf(j->path, sizeof j->path, "%.*s", (int)st->path.len, st->path.p ? (const char *)st->path.p : "");
    snprintf(j->api_key, sizeof j->api_key, "%.*s", (int)st->api_key.len, st->api_key.p ? (const char *)st->api_key.p : "");
    j->key_never = st->key_never;
    snprintf(j->timeout, sizeof j->timeout, "%.*s", (int)st->timeout.len, st->timeout.p ? (const char *)st->timeout.p : "");
    cb_buf_init(&j->body);
    cb_buf_append(&j->body, st->body.p, st->body.len);
    atomic_fetch_add(&s->requests, 1);
    pthread_mutex_lock(&s->mu);
    c->refs++;
    s->threads++;
    pthread_mutex_unlock(&s->mu);
    pthread_t th;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_create(&th, &at, respond, j);
    pthread_attr_destroy(&at);
}

static void *reader(void *arg) {
    sconn *c = arg;
    h2srv *s = c->s;
    cb_hpack_dec dec;
    cb_hpack_dec_init(&dec, CB_HPACK_TABLE_DEFAULT);
    cb_buf in, block, out;
    cb_buf_init(&in);
    cb_buf_init(&block);
    cb_buf_init(&out);
    uint32_t block_stream = 0;
    uint8_t block_flags = 0;
    bool preface = false;
    uint8_t buf[16384];
    // Our SETTINGS first.
    uint16_t ids[2];
    uint32_t vals[2];
    int n = 0;
    if (s->o.max_concurrent) { ids[n] = CB_H2_S_MAX_CONCURRENT_STREAMS; vals[n++] = s->o.max_concurrent; }
    if (s->o.initial_window) { ids[n] = CB_H2_S_INITIAL_WINDOW_SIZE; vals[n++] = s->o.initial_window; }
    pthread_mutex_lock(&c->wmu);
    cb_h2_frame_settings(&out, ids, vals, n, false);
    send_buf(c, &out);
    pthread_mutex_unlock(&c->wmu);
    for (;;) {
        ssize_t r = s_read(c, buf, sizeof buf);
        if (r <= 0) {
            pthread_mutex_lock(&c->wmu);
            if (!c->dead) atomic_fetch_add(&s->closes, 1);   // the client closed it, not this side
            pthread_mutex_unlock(&c->wmu);
            break;
        }
        cb_buf_append(&in, buf, (size_t)r);
        if (!preface) {
            if (in.len < CB_H2_PREFACE_LEN) continue;
            if (memcmp(in.p, CB_H2_PREFACE, CB_H2_PREFACE_LEN) != 0) break;
            cb_buf_consume(&in, CB_H2_PREFACE_LEN);
            preface = true;
        }
        size_t at = 0;
        pthread_mutex_lock(&c->wmu);
        for (;;) {
            cb_h2_frame f;
            int k = cb_h2_frame_parse(in.p + at, in.len - at, CB_H2_MAX_FRAME, &f);
            if (k <= 0) break;
            const uint8_t *p = in.p + at + CB_H2_FRAME_HEAD;
            at += (size_t)k;
            if (f.type == CB_H2_SETTINGS && !(f.flags & CB_H2_F_ACK)) {
                for (uint32_t i = 0; i + 6 <= f.len; i += 6) {
                    uint16_t id = (uint16_t)(p[i] << 8 | p[i + 1]);
                    uint32_t v = (uint32_t)p[i + 2] << 24 | (uint32_t)p[i + 3] << 16 | (uint32_t)p[i + 4] << 8 | p[i + 5];
                    if (id == CB_H2_S_INITIAL_WINDOW_SIZE) {
                        int64_t delta = (int64_t)v - c->peer_initial;
                        for (int j = 0; j < MAX_STREAMS; j++)
                            if (c->st[j].used) c->st[j].window += delta;
                        c->peer_initial = v;
                    }
                }
                cb_h2_frame_settings(&out, NULL, NULL, 0, true);
                send_buf(c, &out);
                pthread_cond_broadcast(&c->wcv);
            } else if (f.type == CB_H2_WINDOW_UPDATE && f.len == 4) {
                uint32_t inc = ((uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]) & 0x7fffffffu;
                if (f.stream == 0) c->conn_window += inc;
                else {
                    sstream *st = find(c, f.stream);
                    if (st) st->window += inc;
                }
                pthread_cond_broadcast(&c->wcv);
            } else if (f.type == CB_H2_PING && !(f.flags & CB_H2_F_ACK)) {
                cb_h2_frame_ping(&out, p, true);
                send_buf(c, &out);
            } else if (f.type == CB_H2_RST_STREAM) {
                sstream *st = find(c, f.stream);
                atomic_fetch_add(&s->resets, 1);
                if (st) { st->reset = true; pthread_cond_broadcast(&c->wcv); }
            } else if (f.type == CB_H2_HEADERS || f.type == CB_H2_CONTINUATION) {
                if (f.type == CB_H2_HEADERS) {
                    const uint8_t *frag;
                    size_t fl;
                    if (cb_h2_frame_unpad(&f, p, &frag, &fl) != CB_OK) break;
                    cb_buf_reset(&block);
                    cb_buf_append(&block, frag, fl);
                    block_stream = f.stream;
                    block_flags = f.flags;
                    sstream *st = NULL;
                    for (int j = 0; j < MAX_STREAMS && !st; j++)
                        if (!c->st[j].used) st = &c->st[j];
                    if (st) {
                        st->used = st->open = true;
                        st->reset = false;
                        st->id = f.stream;
                        st->window = c->peer_initial;
                        cb_buf_reset(&st->path);
                        cb_buf_reset(&st->api_key);
                        st->key_never = false;
                        cb_buf_reset(&st->timeout);
                        cb_buf_reset(&st->body);
                        c->open_now++;
                        int m = atomic_load(&s->max_open);
                        while (c->open_now > m && !atomic_compare_exchange_weak(&s->max_open, &m, c->open_now)) {
                        }
                    }
                } else {
                    cb_buf_append(&block, p, f.len);
                    block_flags |= f.flags & CB_H2_F_END_HEADERS;
                }
                if (block_flags & CB_H2_F_END_HEADERS) {
                    hctx h = { find(c, block_stream) };
                    cb_hpack_decode(&dec, block.p, block.len, on_field, &h);
                    if (h.st && (block_flags & CB_H2_F_END_STREAM)) start_job(c, h.st);
                }
            } else if (f.type == CB_H2_DATA) {
                sstream *st = find(c, f.stream);
                const uint8_t *d;
                size_t dl;
                if (cb_h2_frame_unpad(&f, p, &d, &dl) == CB_OK && st) cb_buf_append(&st->body, d, dl);
                if (f.len) {   // credit back at once, for the connection and the stream
                    cb_h2_frame_u32(&out, CB_H2_WINDOW_UPDATE, 0, f.len);
                    if (st && !(f.flags & CB_H2_F_END_STREAM)) cb_h2_frame_u32(&out, CB_H2_WINDOW_UPDATE, f.stream, f.len);
                    send_buf(c, &out);
                }
                if (st && (f.flags & CB_H2_F_END_STREAM)) start_job(c, st);
            }
            // A GOAWAY from the client opens no more streams; the connection is read
            // on until the client closes it (h2srv_closes counts that close).
        }
        cb_buf_consume(&in, at);
        bool dead = c->dead;
        pthread_mutex_unlock(&c->wmu);
        if (dead) break;
    }
    pthread_mutex_lock(&c->wmu);
    c->dead = true;
    pthread_cond_broadcast(&c->wcv);
    pthread_mutex_unlock(&c->wmu);
    shutdown(c->fd, SHUT_RDWR);
    cb_hpack_dec_free(&dec);
    cb_buf_free(&in);
    cb_buf_free(&block);
    cb_buf_free(&out);
    unref(c);
    thread_done(s);
    return NULL;
}

static int alpn_select(SSL *ssl, const unsigned char **out, unsigned char *outlen, const unsigned char *in,
                       unsigned inlen, void *arg) {
    h2srv *s = arg;
    unsigned keep = inlen < sizeof s->alpn_offer ? inlen : (unsigned)sizeof s->alpn_offer;
    memcpy(s->alpn_offer, in, keep);
    atomic_store(&s->alpn_offer_len, (int)keep);
    const char *want = s->o.alpn_http11_only ? "http/1.1" : "h2";
    size_t wl = strlen(want);
    for (unsigned i = 0; i < inlen;) {
        unsigned l = in[i];
        if (l == wl && i + 1 + l <= inlen && memcmp(in + i + 1, want, wl) == 0) {
            *out = in + i + 1;
            *outlen = (unsigned char)l;
            return SSL_TLSEXT_ERR_OK;
        }
        i += 1 + l;
    }
    return SSL_TLSEXT_ERR_NOACK;
}

static void *acceptor(void *arg) {
    h2srv *s = arg;
    while (!atomic_load(&s->stop)) {
        struct pollfd pf = { s->lfd, POLLIN, 0 };
        if (poll(&pf, 1, 50) != 1) continue;
        int fd = accept(s->lfd, NULL, NULL);
        if (fd < 0) continue;
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
#ifdef SO_NOSIGPIPE
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
        SSL *ssl = NULL;
        if (s->o.tls) {
            ssl = SSL_new(s->ctx);
            SSL_set_fd(ssl, fd);
            if (SSL_accept(ssl) != 1) {
                ERR_clear_error();
                SSL_free(ssl);
                close(fd);
                continue;
            }
        }
        atomic_fetch_add(&s->accepts, 1);
        sconn *c = calloc(1, sizeof *c);
        c->s = s;
        c->fd = fd;
        c->ssl = ssl;
        pthread_mutex_init(&c->wmu, NULL);
        pthread_cond_init(&c->wcv, NULL);
        c->conn_window = CB_H2_DEFAULT_WINDOW;
        c->peer_initial = CB_H2_DEFAULT_WINDOW;
        for (int i = 0; i < MAX_STREAMS; i++) {
            cb_buf_init(&c->st[i].path);
            cb_buf_init(&c->st[i].api_key);
            cb_buf_init(&c->st[i].timeout);
            cb_buf_init(&c->st[i].body);
        }
        c->refs = 1;
        pthread_mutex_lock(&s->mu);
        c->next = s->conns;
        s->conns = c;
        s->threads++;
        pthread_mutex_unlock(&s->mu);
        pthread_t th;
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        pthread_create(&th, &at, reader, c);
        pthread_attr_destroy(&at);
    }
    return NULL;
}

h2srv *h2srv_start(const h2srv_opts *o) {
    h2srv *s = calloc(1, sizeof *s);
    if (o) s->o = *o;
    pthread_mutex_init(&s->mu, NULL);
    pthread_cond_init(&s->cv, NULL);
    s->lfd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(s->lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = { 0 };
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;   // a free port
    socklen_t al = sizeof a;
    if (bind(s->lfd, (struct sockaddr *)&a, sizeof a) != 0 || listen(s->lfd, 64) != 0 ||
        getsockname(s->lfd, (struct sockaddr *)&a, &al) != 0) {
        close(s->lfd);
        free(s);
        return NULL;
    }
    s->port = ntohs(a.sin_port);
    if (s->o.tls) {
        s->ctx = SSL_CTX_new(TLS_server_method());
        BIO *cb = BIO_new_mem_buf(tls_test_cert_pem, -1), *kb = BIO_new_mem_buf(tls_test_key_pem, -1);
        X509 *x = PEM_read_bio_X509(cb, NULL, NULL, NULL);
        EVP_PKEY *k = PEM_read_bio_PrivateKey(kb, NULL, NULL, NULL);
        BIO_free(cb);
        BIO_free(kb);
        if (!s->ctx || !x || !k || SSL_CTX_use_certificate(s->ctx, x) != 1 || SSL_CTX_use_PrivateKey(s->ctx, k) != 1) {
            close(s->lfd);
            free(s);
            return NULL;
        }
        X509_free(x);
        EVP_PKEY_free(k);
        SSL_CTX_set_alpn_select_cb(s->ctx, alpn_select, s);
        const char *tmp = getenv("TMPDIR");
        snprintf(s->ca_file, sizeof s->ca_file, "%s/cbase_h2_ca_%d_%p.pem", tmp && *tmp ? tmp : "/tmp", (int)getpid(),
                 (void *)s);
        FILE *f = fopen(s->ca_file, "w");
        if (!f) { close(s->lfd); free(s); return NULL; }
        fputs(tls_test_cert_pem, f);
        fclose(f);
    }
    pthread_create(&s->acceptor, NULL, acceptor, s);
    return s;
}

void h2srv_stop(h2srv *s) {
    atomic_store(&s->stop, 1);
    pthread_join(s->acceptor, NULL);
    close(s->lfd);
    pthread_mutex_lock(&s->mu);
    for (sconn *c = s->conns; c; c = c->next) shutdown(c->fd, SHUT_RDWR);
    while (s->threads) pthread_cond_wait(&s->cv, &s->mu);
    pthread_mutex_unlock(&s->mu);
    if (s->ctx) SSL_CTX_free(s->ctx);
    if (s->ca_file[0]) unlink(s->ca_file);
    pthread_mutex_destroy(&s->mu);
    pthread_cond_destroy(&s->cv);
    free(s);
}

int h2srv_port(const h2srv *s) { return s->port; }
const char *h2srv_ca_file(const h2srv *s) { return s->ca_file; }
int h2srv_accepts(h2srv *s) { return atomic_load(&s->accepts); }
int h2srv_max_open(h2srv *s) { return atomic_load(&s->max_open); }
int h2srv_requests(h2srv *s) { return atomic_load(&s->requests); }
int h2srv_resets(h2srv *s) { return atomic_load(&s->resets); }
int h2srv_closes(h2srv *s) { return atomic_load(&s->closes); }
int h2srv_alpn_offer(h2srv *s, const unsigned char **p) {
    *p = s->alpn_offer;
    return atomic_load(&s->alpn_offer_len);
}
