// h2.h - an HTTP/2 client: one connection per origin, shared by every thread
// that calls through it, with many requests in flight on it at once.
//
// A CHANNEL names an origin: "https://host[:port]" (TLS 1.2+, ALPN h2, the
// peer's certificate and host name verified as tls.h does it) or
// "http://host:port" (cleartext with prior knowledge, for local stand-ins and
// tests). It opens a connection on first use, keeps it for the calls after,
// and replaces it after an error, a GOAWAY, or idle_close_ms without a call.
//
// A CALL blocks its thread until the response is whole, the deadline passes,
// or the connection fails. Each connection has one I/O thread, which alone
// touches its socket and TLS state; callers hand it requests through the
// session (h2_session.h) under the channel's lock and sleep on their own
// condition variable. A request the peer provably never processed (REFUSED_STREAM, a
// GOAWAY below it, or never sent) is sent once more, within the same deadline,
// on whichever connection takes new calls then (the same one after a
// REFUSED_STREAM, a fresh one after a GOAWAY); nothing else is retried.
//
// The request body is sent from the caller's buffers (no copy beyond one frame
// at a time) and the response body arrives into one buffer.
#ifndef CB_H2_H
#define CB_H2_H

#include "net/h2_session.h"
#include "util/str.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct cb_h2_channel cb_h2_channel;

typedef struct {
    const char  *ca_file;            // NULL: tls.h's search
    int          connect_timeout_ms; // 0: 10000; a call's own deadline bounds it too
    int          idle_close_ms;      // 0: 30000
    cb_h2_limits limits;             // all zero: cb_h2_limits_default
} cb_h2_opts;

// CB_OK and *out, CB_E_INVALID (the origin does not parse), CB_E_IO (the CA
// bundle does not load), CB_E_NOMEM. opts may be NULL.
int cb_h2_channel_new(const char *origin, const cb_h2_opts *opts, cb_h2_channel **out);
// Ends the channel's connections (a GOAWAY each) and waits for their threads.
// Calls still in flight end with CB_H2_WHY_CLOSED.
void cb_h2_channel_free(cb_h2_channel *ch);

// Why a call has no response.
typedef enum {
    CB_H2_WHY_NONE = 0,
    CB_H2_WHY_TIMEOUT,    // the deadline passed
    CB_H2_WHY_CONNECT,    // no TCP connection
    CB_H2_WHY_TLS,        // the handshake or the certificate failed, or the peer refused h2
    CB_H2_WHY_RESET,      // the peer reset the stream (h2_code)
    CB_H2_WHY_GOAWAY,     // the peer went away under it
    CB_H2_WHY_PROTOCOL,   // the peer broke the protocol, or the connection dropped
    CB_H2_WHY_TOO_BIG,    // the response body passed max_body
    CB_H2_WHY_CLOSED,     // the channel was freed
} cb_h2_why;

typedef struct {
    int       status;       // :status
    // Fields as "name\0value\0" pairs: the response's header block first
    // (n_headers of them), then its trailers (n_trailers).
    cb_buf    fields;
    int       n_headers, n_trailers;
    cb_buf    body;
    // When the call answered CB_E_IO:
    cb_h2_why why;
    uint32_t  h2_code;
    char      err[200];     // a sentence for the logs and for a person
} cb_h2_response;

void cb_h2_response_init(cb_h2_response *r);
void cb_h2_response_free(cb_h2_response *r);   // and re-inits it
// A header (trailer false) or trailer field's value, the first of that name.
bool cb_h2_response_get(const cb_h2_response *r, bool trailer, const char *name, cb_span *value);

// One request. fields: the request's own (lowercase names), after the
// pseudo-headers. timeout_ms: the whole call, a fresh connection included.
// max_body: the most response body taken (0: 32 MB).
// CB_OK with *out filled (any status), CB_E_IO with out->why and out->err,
// CB_E_INVALID (a field HTTP/2 refuses).
int cb_h2_request(cb_h2_channel *ch, const char *method, const char *path, const cb_h2_field *fields, int n_fields,
                  const cb_h2_iov *body, int n_body, int timeout_ms, size_t max_body, cb_h2_response *out);

// For the tests: connections opened over the channel's life.
int cb_h2_channel_dials(cb_h2_channel *ch);

#endif
