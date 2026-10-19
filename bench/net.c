// net.c - the bench's HTTP and WebSocket client (net.h).
#include "net.h"

#include "aws/https.h"
#include "net/ws.h"
#include "netwire.h"
#include "mqtt/mqtt.h"
#include "../../shared/c/b64/b64.h"
#include "util/uuid.h"

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "../../shared/c/clock.h"

int64_t bn_now_us(void) {
    return host_mono_us();
}

bool bn_url_parse(const char *s, bn_url *out) {
    memset(out, 0, sizeof *out);
    const char *p;
    if (strncmp(s, "http://", 7) == 0) { p = s + 7; out->port = 80; }
    else if (strncmp(s, "https://", 8) == 0) { p = s + 8; out->tls = true; out->port = 443; }
    else return false;
    size_t hl = strcspn(p, ":/");
    if (hl == 0 || hl >= sizeof out->host) return false;
    memcpy(out->host, p, hl);
    p += hl;
    if (*p == ':') {
        char *e;
        long port = strtol(p + 1, &e, 10);
        if (e == p + 1 || port <= 0 || port > 65535) return false;
        out->port = (int)port;
        p = e;
    }
    if (*p == '/') p++;
    return *p == 0;
}

void bn_resp_init(bn_resp *r) {
    memset(r, 0, sizeof *r);
    cb_buf_init(&r->head);
    cb_buf_init(&r->body);
}

void bn_resp_free(bn_resp *r) {
    cb_buf_free(&r->head);
    cb_buf_free(&r->body);
}

// ---- sockets -------------------------------------------------------------------------

static int dial(const char *host, int port, int timeout_ms) {
    struct addrinfo hints, *ai = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_socktype = SOCK_STREAM;
    char ps[16];
    snprintf(ps, sizeof ps, "%d", port);
    if (getaddrinfo(host, ps, &hints, &ai) != 0) return -1;
    int fd = -1;
    for (struct addrinfo *a = ai; a && fd < 0; a = a->ai_next) {
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
            if (poll(&pf, 1, timeout_ms) == 1 && getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) == 0 && err == 0)
                rc = 0;
        }
        if (rc != 0) { close(fd); fd = -1; continue; }
        fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    }
    freeaddrinfo(ai);
    return fd;
}

static int write_all(int fd, const void *p, size_t n) {
    const uint8_t *b = p;
    while (n) {
        ssize_t w = send(fd, b, n, 0);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        b += w;
        n -= (size_t)w;
    }
    return 0;
}

// The request text as https.c writes it, so both schemes count the same bytes.
static void build_request(cb_buf *req, const bn_url *u, const char *method, const char *path, const char *headers,
                          const void *body, size_t body_len) {
    cb_buf_printf(req, "%s %s HTTP/1.1\r\n", method, path);
    if (u->port == (u->tls ? 443 : 80)) cb_buf_printf(req, "Host: %s\r\n", u->host);
    else cb_buf_printf(req, "Host: %s:%d\r\n", u->host, u->port);
    if (headers) cb_buf_puts(req, headers);
    if (body_len || strcmp(method, "POST") == 0 || strcmp(method, "PUT") == 0)
        cb_buf_printf(req, "Content-Length: %zu\r\n", body_len);
    cb_buf_puts(req, "\r\n");
    if (body_len) cb_buf_append(req, body, body_len);
}

// ---- HTTP ----------------------------------------------------------------------------

void bn_http_init(bn_http *h, const bn_url *u) {
    h->u = *u;
    h->fd = -1;
}

void bn_http_close(bn_http *h) {
    if (h->fd >= 0) close(h->fd);
    h->fd = -1;
}

// One exchange on h->fd. 0, -1 on failure, 1 for "failed before any byte".
static int plain_exchange(bn_http *h, const cb_buf *req, bool head_only, int timeout_ms, bn_resp *out, bool *ka) {
    if (write_all(h->fd, req->p, req->len) != 0) return 1;
    cb_buf in;
    cb_buf_init(&in);
    cb_https_response r;
    memset(&r, 0, sizeof r);
    cb_buf_init(&r.headers);
    cb_buf_init(&r.body);
    int rc = -1;
    int64_t deadline = bn_now_us() + (int64_t)timeout_ms * 1000;
    for (;;) {
        int64_t left = (deadline - bn_now_us()) / 1000;
        struct pollfd pf = { h->fd, POLLIN, 0 };
        if (left <= 0 || poll(&pf, 1, (int)left) != 1) break;
        if (!cb_buf_reserve(&in, 16384)) break;
        ssize_t n = recv(h->fd, in.p + in.len, in.cap - in.len - 1, 0);
        if (n < 0 && errno == EINTR) continue;
        bool eof = n <= 0;
        if (eof && in.len == 0) { rc = 1; break; }
        if (n > 0) in.len += (size_t)n;
        int k = cb_https_parse_response(in.p, in.len, head_only, eof, &r, ka);
        if (k > 0) {
            out->status = r.status;
            cb_buf_append(&out->head, r.headers.p, r.headers.len);
            cb_buf_append(&out->body, r.body.p, r.body.len);
            out->recv = (size_t)k;
            if ((size_t)k != in.len) *ka = false;
            rc = 0;
            break;
        }
        if (k < 0 || eof) break;
    }
    cb_https_response_free(&r);
    cb_buf_free(&in);
    return rc;
}

int bn_http_request(bn_http *h, const char *method, const char *path, const char *headers, const void *body,
                    size_t body_len, int timeout_ms, bn_resp *out) {
    cb_buf_reset(&out->head);
    cb_buf_reset(&out->body);
    out->status = 0;
    out->sent = out->recv = 0;
    cb_buf req;
    cb_buf_init(&req);
    build_request(&req, &h->u, method, path, headers, body, body_len);
    out->sent = req.len;
    int rc = -1;
    if (h->u.tls) {
        cb_https_response r;
        if (cb_https_request(h->u.host, h->u.port, method, path, headers, body, body_len, timeout_ms, &r) == 0) {
            out->status = r.status;
            cb_buf_append(&out->head, r.headers.p, r.headers.len);
            cb_buf_append(&out->body, r.body.p, r.body.len);
            out->recv = r.headers.len + r.body.len;
            rc = 0;
        }
        cb_https_response_free(&r);
    } else {
        for (int attempt = 0; attempt < 2; attempt++) {
            bool reused = h->fd >= 0;
            if (!reused && (h->fd = dial(h->u.host, h->u.port, timeout_ms)) < 0) break;
            bool ka = false;
            rc = plain_exchange(h, &req, strcmp(method, "HEAD") == 0, timeout_ms, out, &ka);
            if (rc != 0 || !ka) bn_http_close(h);
            if (rc == 1 && reused) continue;
            if (rc == 1) rc = -1;
            break;
        }
    }
    cb_buf_free(&req);
    return rc;
}

// ---- TLS for the WebSocket -------------------------------------------------------------

static SSL_CTX *g_ctx;
static pthread_once_t g_ctx_once = PTHREAD_ONCE_INIT;

static bool file_exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

// The CA search https.c makes: $CBASE_CA_FILE, the known bundles, OpenSSL's default.
static void ctx_build(void) {
    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) return;
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
    static const char *const KNOWN[] = { "/etc/pki/tls/certs/ca-bundle.crt", "/etc/ssl/certs/ca-certificates.crt",
                                         "/etc/ssl/cert.pem" };
    const char *ca = getenv("CBASE_CA_FILE");
    int ok = 0;
    if (ca && *ca) ok = SSL_CTX_load_verify_locations(ctx, ca, NULL);
    for (size_t i = 0; i < sizeof KNOWN / sizeof KNOWN[0] && !ok; i++)
        if (file_exists(KNOWN[i])) ok = SSL_CTX_load_verify_locations(ctx, KNOWN[i], NULL);
    if (!ok) ok = SSL_CTX_set_default_verify_paths(ctx);
    if (!ok) { SSL_CTX_free(ctx); ERR_clear_error(); return; }
    g_ctx = ctx;
}

struct bn_ws {
    int    fd;
    SSL   *ssl;
    cb_buf in;
};

static int ws_write(bn_ws *w, const void *p, size_t n) {
    if (!w->ssl) return write_all(w->fd, p, n);
    const char *b = p;
    while (n) {
        int k = SSL_write(w->ssl, b, (int)n);
        if (k <= 0) { ERR_clear_error(); return -1; }
        b += k;
        n -= (size_t)k;
    }
    return 0;
}

// Reads more into w->in within timeout_ms: 1, -1 on a timeout, -2 closed or broken.
static int ws_fill(bn_ws *w, int timeout_ms) {
    if (!(w->ssl && SSL_pending(w->ssl) > 0)) {
        struct pollfd pf = { w->fd, POLLIN, 0 };
        int pr = poll(&pf, 1, timeout_ms);
        if (pr == 0) return -1;
        if (pr < 0) return errno == EINTR ? -1 : -2;
    }
    if (!cb_buf_reserve(&w->in, 16384)) return -2;
    for (;;) {
        ssize_t n;
        if (w->ssl) {
            int k = SSL_read(w->ssl, w->in.p + w->in.len, (int)(w->in.cap - w->in.len - 1));
            if (k <= 0) {
                int e = SSL_get_error(w->ssl, k);
                ERR_clear_error();
                if (e == SSL_ERROR_WANT_READ) return -1;   // a TLS record not yet whole
                return -2;
            }
            n = k;
        } else {
            n = recv(w->fd, w->in.p + w->in.len, w->in.cap - w->in.len - 1, 0);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return -2;
        }
        w->in.len += (size_t)n;
        w->in.p[w->in.len] = 0;
        return 1;
    }
}

bn_ws *bn_ws_open(const bn_url *u, const char *path, int timeout_ms) { return bn_ws_open_with(u, path, NULL, timeout_ms); }

bn_ws *bn_ws_open_with(const bn_url *u, const char *path, const char *headers, int timeout_ms) {
    bn_ws *w = calloc(1, sizeof *w);
    if (!w) return NULL;
    cb_buf_init(&w->in);
    w->fd = dial(u->host, u->port, timeout_ms);
    if (w->fd < 0) { free(w); return NULL; }
    if (u->tls) {
        pthread_once(&g_ctx_once, ctx_build);
        if (!g_ctx || !(w->ssl = SSL_new(g_ctx))) { bn_ws_close(w); return NULL; }
        SSL_set_fd(w->ssl, w->fd);
        SSL_set_tlsext_host_name(w->ssl, u->host);
        SSL_set1_host(w->ssl, u->host);
        if (SSL_connect(w->ssl) != 1) {
            fprintf(stderr, "bench: TLS to %s:%d failed\n", u->host, u->port);
            ERR_clear_error();
            bn_ws_close(w);
            return NULL;
        }
    }
    uint8_t nonce[16];
    char key[32];
    cb_random_bytes(nonce, sizeof nonce);
    b64_encode(nonce, sizeof nonce, key, sizeof key);
    cb_buf req;
    cb_buf_init(&req);
    cb_buf_printf(&req,
                  "GET %s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                  "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: %s\r\n%s\r\n",
                  path, u->host, key, headers ? headers : "");
    int rc = ws_write(w, req.p, req.len);
    cb_buf_free(&req);
    if (rc != 0) { bn_ws_close(w); return NULL; }
    char *he;
    while (!(he = strstr((char *)cb_buf_cstr(&w->in), "\r\n\r\n")))
        if (ws_fill(w, timeout_ms) != 1) { bn_ws_close(w); return NULL; }
    int status = 0;
    if (sscanf((const char *)w->in.p, "HTTP/1.1 %d", &status) != 1 || status != 101) {
        fprintf(stderr, "bench: WebSocket upgrade at %s answered %d\n", path, status);
        bn_ws_close(w);
        return NULL;
    }
    // A frame the server sent right behind the 101 stays in w->in.
    cb_buf_consume(&w->in, (size_t)(he + 4 - (char *)w->in.p));
    return w;
}

void bn_ws_close(bn_ws *w) {
    if (!w) return;
    if (w->ssl) SSL_free(w->ssl);
    if (w->fd >= 0) close(w->fd);
    cb_buf_free(&w->in);
    free(w);
}

static int ws_send(bn_ws *w, int opcode, const void *data, size_t len) {
    uint8_t mask[4], hdr[14];
    cb_random_bytes(mask, 4);
    size_t hn = cb_ws_frame_header(hdr, true, opcode, len, mask);
    uint8_t *frame = malloc(hn + len + 1);
    if (!frame) return -1;
    memcpy(frame, hdr, hn);
    if (len) memcpy(frame + hn, data, len);
    cb_ws_unmask(frame + hn, len, mask);
    int rc = ws_write(w, frame, hn + len);
    free(frame);
    return rc;
}

int bn_ws_send_text(bn_ws *w, const char *text, size_t len) { return ws_send(w, CB_WS_OP_TEXT, text, len); }
int bn_ws_send_bin(bn_ws *w, const void *data, size_t len) { return ws_send(w, CB_WS_OP_BIN, data, len); }

int bn_ws_recv(bn_ws *w, int timeout_ms, cb_buf *out) {
    int64_t deadline = bn_now_us() + (int64_t)timeout_ms * 1000;
    for (;;) {
        cb_ws_frame f;
        int n = w->in.len ? cb_ws_frame_parse(w->in.p, w->in.len, 1u << 24, &f) : 0;
        if (n < 0) return -2;
        if (n > 0) {
            int op = f.opcode;
            if (op == CB_WS_OP_PING) {
                ws_send(w, CB_WS_OP_PONG, w->in.p + f.header_len, (size_t)f.len);
                cb_buf_consume(&w->in, (size_t)n);
                continue;
            }
            if (op == CB_WS_OP_CLOSE) return -2;
            cb_buf_reset(out);
            cb_buf_append(out, w->in.p + f.header_len, (size_t)f.len);
            cb_buf_consume(&w->in, (size_t)n);
            return op;
        }
        int64_t left = (deadline - bn_now_us()) / 1000;
        if (left < 0) left = 0;
        int r = ws_fill(w, (int)left);
        if (r == -2) return -2;
        if (r == -1 && bn_now_us() >= deadline) return -1;
    }
}

// ---- one MQTT connection ------------------------------------------------------------

bn_ws *bn_mq_open(const bn_url *u, const char *query_token, int timeout_ms) {
    char path[NW_ACCESS_MAX + 128];
    if (query_token) snprintf(path, sizeof path, "/mqtt?token=%s", query_token);
    else snprintf(path, sizeof path, "/mqtt");
    return bn_ws_open_with(u, path, "Sec-WebSocket-Protocol: mqtt\r\n", timeout_ms);
}

int bn_mq_next(bn_mq *c, int timeout_ms, void *pkt, cb_buf *scratch) {
    MqttPacket *p = pkt;
    for (;;) {
        int k = c->in.len ? mqtt_parse(c->in.p, (int)c->in.len, 1 << 20, MQTT_FROM_SERVER, p) : 0;
        if (k > 0) {
            // The packet's spans move to scratch, then the buffer drops it.
            cb_buf_reset(scratch);
            cb_buf_append(scratch, c->in.p, (size_t)k);
            mqtt_parse(scratch->p, k, 1 << 20, MQTT_FROM_SERVER, p);
            cb_buf_consume(&c->in, (size_t)k);
            return p->type;
        }
        if (k < 0) return -1;
        cb_buf msg;
        cb_buf_init(&msg);
        int op = bn_ws_recv(c->ws, timeout_ms, &msg);
        if (op == CB_WS_OP_BIN) cb_buf_append(&c->in, msg.p, msg.len);
        cb_buf_free(&msg);
        if (op == -1) return 0;
        if (op < 0 || op == CB_WS_OP_CLOSE) return -1;
    }
}

int bn_mq_connect(bn_mq *c, const char *uid, const char *token, int timeout_ms) {
    uint8_t b[NW_ACCESS_MAX + 256];
    char id[96];
    uint8_t rnd[4];
    cb_random_bytes(rnd, sizeof rnd);
    snprintf(id, sizeof id, "%s-bench%02x%02x%02x%02x", uid, rnd[0], rnd[1], rnd[2], rnd[3]);
    int n = mqtt_write_connect(b, sizeof b, id, (int)strlen(id), 60, token ? "token" : NULL, 5, (const uint8_t *)token,
                               token ? (int)strlen(token) : 0);
    if (n < 0 || bn_ws_send_bin(c->ws, b, (size_t)n) != 0) return -1;
    MqttPacket p;
    cb_buf s;
    cb_buf_init(&s);
    int t = bn_mq_next(c, timeout_ms, &p, &s);
    cb_buf_free(&s);
    return t == MQTT_CONNACK && p.return_code == 0 ? 1 : t;
}

int bn_mq_subscribe(bn_mq *c, uint16_t id, const char *filter, int timeout_ms) {
    uint8_t b[512];
    MqttTopic t = { filter, (int)strlen(filter), 0 };
    int n = mqtt_write_subscribe(b, sizeof b, id, &t, 1);
    if (n < 0 || bn_ws_send_bin(c->ws, b, (size_t)n) != 0) return -1;
    MqttPacket p;
    cb_buf s;
    cb_buf_init(&s);
    int r = bn_mq_next(c, timeout_ms, &p, &s);
    int ok = r == MQTT_SUBACK && p.packet_id == id && p.n_items == 1 && p.items[0] == 0;
    cb_buf_free(&s);
    return ok ? 1 : r;
}

int bn_mq_ping(bn_mq *c) {
    uint8_t b[2];
    int n = mqtt_write_bare(b, sizeof b, MQTT_PINGREQ);
    return n > 0 ? bn_ws_send_bin(c->ws, b, (size_t)n) : -1;
}

void bn_mq_close(bn_mq *c) {
    if (c->ws) bn_ws_close(c->ws);
    cb_buf_free(&c->in);
    memset(c, 0, sizeof *c);
}
