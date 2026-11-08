// grpc.h - gRPC unary calls over the HTTP/2 client (h2.h): bytes in, bytes out.
//
// No protobuf: the caller writes its request message and reads the reply's
// itself. This layer does the rest of the gRPC wire (the HTTP/2 mapping of
// grpc/grpc's PROTOCOL-HTTP2.md): a POST to /<service>/<method>, content-type
// application/grpc, te: trailers, grpc-timeout from the call's deadline, the
// caller's metadata, the request as one length-prefixed message, and the
// reply's message, grpc-status, grpc-message (percent-decoded) and
// grpc-status-details-bin (base64-decoded: a protobuf Status message, for the caller
// to read) from the trailers, or from the headers of a trailers-only answer.
//
// An answer without a grpc-status is not gRPC (a proxy's error page, a 503
// from a load balancer): it comes back with code -1 and its HTTP status, so the
// caller can tell "the service said no" from "something in front of it did".
//
// Compressed messages are refused: this side never sends grpc-accept-encoding,
// so a server may not compress.
#ifndef CB_GRPC_H
#define CB_GRPC_H

#include "net/h2.h"

#include <stddef.h>
#include <stdint.h>

// The status codes the callers name.
enum {
    CB_GRPC_OK = 0, CB_GRPC_CANCELLED = 1, CB_GRPC_UNKNOWN = 2, CB_GRPC_INVALID_ARGUMENT = 3,
    CB_GRPC_DEADLINE_EXCEEDED = 4, CB_GRPC_NOT_FOUND = 5, CB_GRPC_PERMISSION_DENIED = 7,
    CB_GRPC_RESOURCE_EXHAUSTED = 8, CB_GRPC_FAILED_PRECONDITION = 9, CB_GRPC_INTERNAL = 13,
    CB_GRPC_UNAVAILABLE = 14, CB_GRPC_UNAUTHENTICATED = 16,
};

typedef struct {
    int            code;         // grpc-status, or -1 when the answer is not gRPC
    int            http_status;  // :status
    cb_buf         message;      // grpc-message, percent-decoded (NUL-terminated)
    cb_buf         details;      // grpc-status-details-bin, decoded (empty when absent)
    // The reply's message, pointing into resp.body: valid until the reply is
    // freed. Present (has_msg) on CB_GRPC_OK; an error carries none.
    const uint8_t *msg;
    size_t         msg_len;
    bool           has_msg;
    cb_h2_response resp;         // the HTTP/2 response underneath, and why on CB_E_IO
} cb_grpc_reply;

void cb_grpc_reply_init(cb_grpc_reply *r);
void cb_grpc_reply_free(cb_grpc_reply *r);   // and re-inits it

// One unary call. path: "/package.Service/Method". meta: the request's
// metadata (lowercase names; a "-bin" name's value must already be base64, as
// gRPC sends it). timeout_ms: the whole call; it also goes to the server as
// grpc-timeout. max_reply: the most reply body taken (0: 32 MB).
// CB_OK: an answer came (any code: read r->code). CB_E_IO: none (r->resp.why,
// r->resp.err). CB_E_INVALID: a field HTTP/2 refuses, or an answer that is
// gRPC but malformed (a cut message, a compressed one, more than one message
// for a unary call, a status that does not parse); r->resp.err says which.
int cb_grpc_unary(cb_h2_channel *ch, const char *path, const cb_h2_field *meta, int n_meta, const void *req,
                  size_t req_len, int timeout_ms, size_t max_reply, cb_grpc_reply *r);

// ---- the pieces, exposed for the tests and the fuzzer --------------------------
// The grpc-timeout value for ms ("<= 8 digits><unit>"): at most 8 digits in
// milliseconds, else in seconds, else in hours. Writes it to out (cap >= 10).
void cb_grpc_timeout(int64_t ms, char *out, size_t cap);
// The messages in a reply body: CB_OK with the one message (or none) in
// *msg/*len/*has, or CB_E_INVALID with none.
int cb_grpc_unframe(const uint8_t *p, size_t n, const uint8_t **msg, size_t *len, bool *has);
// grpc-message's percent-encoding, decoded into out (appended). Bytes that are
// not a valid %XX escape are kept as they are, as the spec asks.
void cb_grpc_percent_decode(const char *p, size_t n, cb_buf *out);

#endif
