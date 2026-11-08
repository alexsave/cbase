// tls.c - the shared dial and TLS client context: see tls.h.
#include "net/tls.h"

#include "util/clock.h"
#include "util/err.h"
#include "util/log.h"

#include <openssl/err.h>
#include <openssl/x509v3.h>

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

static bool file_exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

SSL_CTX *cb_tls_client_ctx(const char *ca_file, const unsigned char *alpn, unsigned alpn_len) {
    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) return NULL;
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
#ifdef SSL_OP_IGNORE_UNEXPECTED_EOF
    // Some front ends close without close_notify; the parser decides completeness.
    SSL_CTX_set_options(ctx, SSL_OP_IGNORE_UNEXPECTED_EOF);
#endif
    const char *ca = ca_file && *ca_file ? ca_file : getenv("CBASE_CA_FILE");
    static const char *const KNOWN[] = {
        "/etc/pki/tls/certs/ca-bundle.crt",     // Fedora, RHEL and their kin (a one-shot provider's runtime)
        "/etc/ssl/certs/ca-certificates.crt",   // Debian, Ubuntu, Alpine
        "/etc/ssl/cert.pem",                    // Alpine, macOS's system copy
    };
    int ok = 0;
    if (ca && *ca) {
        ok = SSL_CTX_load_verify_locations(ctx, ca, NULL);
        if (!ok) CB_ERROR("tls", "cannot load CA bundle %s", ca);
    } else {
        // OpenSSL's own bundle first ($SSL_CERT_FILE, else the file its build was configured
        // with: Homebrew's maintained one on a Mac, the distribution's on Linux), when it
        // exists. Only then the fixed list: macOS also has /etc/ssl/cert.pem, Apple's copy,
        // which lags the roots servers now chain to (CI's macOS runner could not verify
        // example.com against it), and the static Alpine build's configured file is absent
        // on the runtime it ships to.
        const char *own = getenv(X509_get_default_cert_file_env());
        if (!own || !*own) own = X509_get_default_cert_file();
        if (own && *own && file_exists(own)) ok = SSL_CTX_load_verify_locations(ctx, own, NULL);
        for (size_t i = 0; i < sizeof KNOWN / sizeof KNOWN[0] && !ok; i++)
            if (file_exists(KNOWN[i])) ok = SSL_CTX_load_verify_locations(ctx, KNOWN[i], NULL);
        if (!ok) ok = SSL_CTX_set_default_verify_paths(ctx);
    }
    if (ok && alpn && SSL_CTX_set_alpn_protos(ctx, alpn, alpn_len) != 0) ok = 0;   // 0 is success here
    if (!ok) {
        SSL_CTX_free(ctx);
        ERR_clear_error();
        return NULL;
    }
    return ctx;
}

int cb_tls_bind_host(SSL *ssl, const char *host) {
    SSL_set_tlsext_host_name(ssl, host);   // SNI
    if (SSL_set1_host(ssl, host) != 1) {
        ERR_clear_error();
        return CB_E_IO;
    }
    return CB_OK;
}

static int64_t left(int64_t deadline) {
    int64_t l = deadline - cb_mono_ms();
    return l > 0 ? l : 0;
}

int cb_net_dial(const char *host, int port, int64_t deadline_ms, bool nonblocking) {
    struct addrinfo hints, *ai = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_socktype = SOCK_STREAM;
    char ps[16];
    snprintf(ps, sizeof ps, "%d", port);
    if (getaddrinfo(host, ps, &hints, &ai) != 0) return -1;
    int fd = -1;
    for (struct addrinfo *a = ai; a && fd < 0 && left(deadline_ms) > 0; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) continue;
#ifdef SO_NOSIGPIPE
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
        int fl = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, fl | O_NONBLOCK);
        int rc = connect(fd, a->ai_addr, a->ai_addrlen);
        if (rc != 0 && errno == EINPROGRESS) {
            struct pollfd pf = { fd, POLLOUT, 0 };
            int err = 0;
            socklen_t el = sizeof err;
            if (poll(&pf, 1, (int)left(deadline_ms)) == 1 && getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) == 0 &&
                err == 0)
                rc = 0;
        }
        if (rc != 0) { close(fd); fd = -1; continue; }
        if (!nonblocking) fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
    }
    freeaddrinfo(ai);
    return fd;
}
