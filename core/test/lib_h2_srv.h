// lib_h2_srv.h - a local HTTP/2 server for the client's tests (unit_h2, unit_grpc).
//
// A thread on 127.0.0.1 (a free port) accepting connections, each read on its
// own thread, each request answered on a thread of its own, so many streams are
// in flight on one connection at once. Cleartext with prior knowledge, or TLS
// with lib_tls.c's "localhost" certificate and ALPN h2. It keeps to the
// client's flow-control windows when it sends, and gives back credit for every
// DATA frame it reads.
//
// What a request gets depends on its :path:
//   /echo                 200, the request body back, trailer x-trailer: done
//   /big/<n>              200, n bytes (byte i is i % 251)
//   /slow/<ms>            200 "slow" after ms
//   /status/<code>        that status, body "status", no gRPC fields
//   /reset                RST_STREAM (INTERNAL_ERROR)
//   /refuse               RST_STREAM (REFUSED_STREAM), every time
//   /refuse-once          the first request on the server: RST_STREAM (REFUSED_STREAM); later ones 200 "again"
//   /goaway               GOAWAY below its stream, then the connection closes, every time
//   /goaway-once          the first: GOAWAY below its stream, then the connection closes; later ones 200 "again"
//   /close                the connection is closed under it
//   /test.Echo/Echo       gRPC: the request message back, grpc-status 0
//   /test.Echo/Fail       gRPC trailers-only: grpc-status 8, a percent-encoded grpc-message, status details
//   /test.Echo/Meta       gRPC: a message "<x-goog-api-key>|<grpc-timeout>|<never or indexed>" (how the key came)
//   /test.Echo/Two        gRPC: two messages (malformed for unary)
//   /test.Echo/Zip        gRPC: one message flagged compressed
//   /test.Echo/Raw/<http>/<grpc-status>/<details>/<msg>
//                         :status <http>, grpc-status and grpc-status-details-bin as given ("-": none);
//                         msg "-": trailers-only; "!<bytes>": the bytes unframed; else one framed message
//                         with the gRPC fields in the trailers
// Every other /test.Echo/ route first checks the request is one framed message with a
// true length prefix, and answers grpc-status 13 trailers-only when it is not.
//   anything else         404
#ifndef CB_TEST_H2_SRV_H
#define CB_TEST_H2_SRV_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool     tls;
    bool     alpn_http11_only;   // TLS offering only http/1.1: the client must refuse it
    uint32_t max_concurrent;     // SETTINGS_MAX_CONCURRENT_STREAMS sent; 0: none
    uint32_t initial_window;     // SETTINGS_INITIAL_WINDOW_SIZE sent; 0: none (65535)
} h2srv_opts;

typedef struct h2srv h2srv;

// NULL on failure.
h2srv *h2srv_start(const h2srv_opts *o);
void   h2srv_stop(h2srv *s);
int    h2srv_port(const h2srv *s);
const char *h2srv_ca_file(const h2srv *s);   // the certificate as a PEM file (TLS)
int    h2srv_accepts(h2srv *s);              // connections taken
int    h2srv_max_open(h2srv *s);             // the most streams open at once on one connection
int    h2srv_requests(h2srv *s);
int    h2srv_resets(h2srv *s);               // RST_STREAMs the client sent

#endif
