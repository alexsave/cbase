// lib_aws_tls.c - the local TLS stand-in for AWS endpoints (lib_aws_tls.h).
#include "lib_aws_tls.h"

#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

// openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -days 36500
//   -nodes -subj /CN=localhost -addext subjectAltName=DNS:localhost
static const char CERT[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIBlDCCATugAwIBAgIUS5Gjqj5aKgf2FZ2KDLrlqY4XqdAwCgYIKoZIzj0EAwIw\n"
    "FDESMBAGA1UEAwwJbG9jYWxob3N0MCAXDTI2MTAxMzE3MzUxM1oYDzIxMjYwOTE5\n"
    "MTczNTEzWjAUMRIwEAYDVQQDDAlsb2NhbGhvc3QwWTATBgcqhkjOPQIBBggqhkjO\n"
    "PQMBBwNCAATGjOlro7rpkBRboPjMTR2qg8zyJKFhPgnCoaZ3yETblLnraeozLrGK\n"
    "xijp+jt62f5gBBN2Gs8rCblBd+N7c3cSo2kwZzAdBgNVHQ4EFgQUU+AQ6/3wHRdW\n"
    "+MTbMoXbwdoX6GQwHwYDVR0jBBgwFoAUU+AQ6/3wHRdW+MTbMoXbwdoX6GQwDwYD\n"
    "VR0TAQH/BAUwAwEB/zAUBgNVHREEDTALgglsb2NhbGhvc3QwCgYIKoZIzj0EAwID\n"
    "RwAwRAIgKWdZaw0nAThN5MlcyRCwacHevaIM7ZGWHntM3DFJpeMCIClTdJVtgzyB\n"
    "K/gfdPMBHMgoJBRcx5yP7p7vlhTBbwUp\n"
    "-----END CERTIFICATE-----\n";
static const char KEY[] =
    "-----BEGIN PRIVATE KEY-----\n"
    "MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgr/b0XIoorD205G/Z\n"
    "EgXwfGYSxrpzAywOxlAert7X66KhRANCAATGjOlro7rpkBRboPjMTR2qg8zyJKFh\n"
    "PgnCoaZ3yETblLnraeozLrGKxijp+jt62f5gBBN2Gs8rCblBd+N7c3cS\n"
    "-----END PRIVATE KEY-----\n";

// Reads one request: the head, then Content-Length bytes. 1, 0 at a clean
// close before any byte, -1 on error.
static int read_request(SSL *ssl, cb_buf *in) {
    cb_buf_reset(in);
    size_t need = 0;
    for (;;) {
        if (need && in->len >= need) return 1;
        if (!cb_buf_reserve(in, 4096)) return -1;
        int r = SSL_read(ssl, in->p + in->len, 4096);
        if (r <= 0) return in->len == 0 ? 0 : -1;
        in->len += (size_t)r;
        in->p[in->len] = 0;
        if (!need) {
            const char *he = strstr((const char *)in->p, "\r\n\r\n");
            if (!he) continue;
            size_t hl = (size_t)(he - (const char *)in->p) + 4;
            size_t cl = 0;
            for (const char *l = strstr((const char *)in->p, "\r\n"); l && l < he; l = strstr(l + 2, "\r\n"))
                if (strncasecmp(l + 2, "content-length:", 15) == 0) cl = (size_t)strtoul(l + 17, NULL, 10);
            need = hl + cl;
        }
    }
}

static void serve_conn(tls_srv *s, SSL_CTX *ctx, int fd) {
    SSL *ssl = SSL_new(ctx);
    SSL_set_fd(ssl, fd);
    if (SSL_accept(ssl) != 1) { ERR_clear_error(); SSL_free(ssl); close(fd); return; }
    atomic_fetch_add(&s->accepts, 1);
    cb_buf in, out;
    cb_buf_init(&in);
    cb_buf_init(&out);
    bool drop_next = false;
    while (!atomic_load(&s->stop)) {
        // Wait for the next request without blocking past a stop.
        struct pollfd pf = { fd, POLLIN, 0 };
        if (SSL_pending(ssl) == 0 && poll(&pf, 1, 50) == 0) continue;
        if (drop_next) break;
        int r = read_request(ssl, &in);
        if (r <= 0) break;
        pthread_mutex_lock(&s->lock);
        cb_buf_reset(&s->last);
        cb_buf_append(&s->last, in.p, in.len);
        pthread_mutex_unlock(&s->lock);
        atomic_fetch_add(&s->requests, 1);
        cb_buf_reset(&out);
        tls_action a = s->fn(&in, &out, s->ud);
        if (a == TLS_HANG) {
            // Hold the connection, answering nothing, until the client closes it.
            while (!atomic_load(&s->stop)) {
                struct pollfd hp = { fd, POLLIN, 0 };
                char b[64];
                if (poll(&hp, 1, 50) == 1 && SSL_read(ssl, b, sizeof b) <= 0) break;
            }
            break;
        }
        if (SSL_write(ssl, out.p, (int)out.len) <= 0) break;
        if (a == TLS_CLOSE) { SSL_shutdown(ssl); break; }
        if (a == TLS_DROP) drop_next = true;
    }
    ERR_clear_error();
    SSL_free(ssl);
    close(fd);
    cb_buf_free(&in);
    cb_buf_free(&out);
}

static void *run(void *arg) {
    tls_srv *s = arg;
    while (!atomic_load(&s->stop)) {
        struct pollfd pf = { s->lfd, POLLIN, 0 };
        if (poll(&pf, 1, 50) != 1) continue;
        int fd = accept(s->lfd, NULL, NULL);
        if (fd < 0) continue;
#ifdef SO_NOSIGPIPE
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
        serve_conn(s, s->ctx, fd);
    }
    return NULL;
}

int tls_srv_start(tls_srv *s, tls_handler fn, void *ud) {
    memset(s, 0, sizeof *s);
    pthread_mutex_init(&s->lock, NULL);
    cb_buf_init(&s->last);
    s->fn = fn;
    s->ud = ud;
    SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
    BIO *cb = BIO_new_mem_buf(CERT, -1), *kb = BIO_new_mem_buf(KEY, -1);
    X509 *x = PEM_read_bio_X509(cb, NULL, NULL, NULL);
    EVP_PKEY *k = PEM_read_bio_PrivateKey(kb, NULL, NULL, NULL);
    BIO_free(cb);
    BIO_free(kb);
    if (!ctx || !x || !k || SSL_CTX_use_certificate(ctx, x) != 1 || SSL_CTX_use_PrivateKey(ctx, k) != 1) return -1;
    X509_free(x);
    EVP_PKEY_free(k);
    s->ctx = ctx;
    const char *tmp = getenv("TMPDIR");
    snprintf(s->ca_file, sizeof s->ca_file, "%s/cbase_test_ca_%d_%p.pem", tmp && *tmp ? tmp : "/tmp", (int)getpid(), (void *)s);
    FILE *f = fopen(s->ca_file, "w");
    if (!f) return -1;
    fputs(CERT, f);
    fclose(f);
    s->lfd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(s->lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t al = sizeof a;
    if (bind(s->lfd, (struct sockaddr *)&a, sizeof a) != 0 || listen(s->lfd, 8) != 0 ||
        getsockname(s->lfd, (struct sockaddr *)&a, &al) != 0) return -1;
    s->port = ntohs(a.sin_port);
    return pthread_create(&s->th, NULL, run, s) == 0 ? 0 : -1;
}

void tls_srv_stop(tls_srv *s) {
    atomic_store(&s->stop, 1);
    pthread_join(s->th, NULL);
    close(s->lfd);
    SSL_CTX_free(s->ctx);
    unlink(s->ca_file);
    cb_buf_free(&s->last);
    pthread_mutex_destroy(&s->lock);
}

char *tls_srv_last(tls_srv *s) {
    pthread_mutex_lock(&s->lock);
    char *c = strdup(cb_buf_cstr(&s->last));
    pthread_mutex_unlock(&s->lock);
    return c;
}
