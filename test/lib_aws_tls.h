// lib_aws_tls.h - a local TLS server standing in for an AWS endpoint.
//
// For unit_https.c, unit_aws_calls.c: a thread on 127.0.0.1 serving TLS with a
// fixed self-signed certificate for "localhost" (lib_aws_tls.c embeds it; it
// is valid until 2126), one connection at a time. Each request (head and
// Content-Length body) is recorded and answered by the test's callback.
#ifndef CB_TEST_AWS_TLS_H
#define CB_TEST_AWS_TLS_H

#include "util/str.h"

#include <pthread.h>
#include <stdatomic.h>

typedef enum {
    TLS_KEEP = 0,   // write the response, read the next request on this connection
    TLS_CLOSE,      // write the response, close
    TLS_HANG,       // write nothing, hold the connection until the client goes
    TLS_DROP,       // write the response, then close silently before the NEXT request (an idle timeout)
} tls_action;

// `req` is the whole request (head and body); append the raw response to `resp`.
typedef tls_action (*tls_handler)(const cb_buf *req, cb_buf *resp, void *ud);

typedef struct {
    int         port;
    char        ca_file[256];   // the certificate as a PEM file, for cb_https_set_ca_file
    atomic_int  accepts;        // TLS connections completed
    atomic_int  requests;
    cb_buf      last;           // the last request (under lock)
    pthread_mutex_t lock;
    // private
    int         lfd;
    atomic_int  stop;
    pthread_t   th;
    tls_handler fn;
    void       *ud;
    void       *ctx;
    atomic_int  plain;          // tls_srv_start_plain: no TLS at all
} tls_srv;

// 0, or -1.
int  tls_srv_start(tls_srv *s, tls_handler fn, void *ud);
// The same server speaking plain HTTP (DynamoDB Local's way), for the
// client's http:// mode.
int  tls_srv_start_plain(tls_srv *s, tls_handler fn, void *ud);
void tls_srv_stop(tls_srv *s);
// A copy of the last request as a C string (free it).
char *tls_srv_last(tls_srv *s);

#endif
