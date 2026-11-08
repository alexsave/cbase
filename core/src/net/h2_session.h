// h2_session.h - the client side of one HTTP/2 connection (RFC 9113), sans I/O.
//
// Bytes the peer sent go in (cb_h2_sess_recv), bytes to send come out
// (cb_h2_sess_flush), and each stream's response comes back through
// callbacks. No socket, no clock, no thread and no lock: h2.c wraps it in TLS
// and threads, and the tests and the fuzzer drive it directly.
//
// WHAT IT DOES: the client preface and SETTINGS; HPACK (hpack.h) both ways;
// HEADERS and CONTINUATION; request bodies sent within the peer's flow-control
// windows (connection and stream) and its frame size; response bodies taken
// within ours, with WINDOW_UPDATEs as they are consumed; the peer's
// MAX_CONCURRENT_STREAMS (streams past it wait their turn, in order); SETTINGS
// and PING acknowledged; RST_STREAM and GOAWAY in both directions; trailers.
//
// WHAT IT REFUSES: server push (ENABLE_PUSH is 0, so a PUSH_PROMISE is a
// connection error), and priority, which it ignores as RFC 9113 allows.
//
// A CONNECTION ERROR (a protocol violation by the peer, a COMPRESSION_ERROR)
// queues a GOAWAY, ends every stream with that error, and leaves the session
// dead: recv answers CB_E_IO from then on, and flush still yields the GOAWAY.
// A STREAM ERROR resets that stream alone.
#ifndef CB_H2_SESSION_H
#define CB_H2_SESSION_H

#include "util/str.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ---- the wire's numbers (RFC 9113 6, 7, 11) ---------------------------------
enum {
    CB_H2_DATA = 0x0, CB_H2_HEADERS = 0x1, CB_H2_PRIORITY = 0x2, CB_H2_RST_STREAM = 0x3, CB_H2_SETTINGS = 0x4,
    CB_H2_PUSH_PROMISE = 0x5, CB_H2_PING = 0x6, CB_H2_GOAWAY = 0x7, CB_H2_WINDOW_UPDATE = 0x8,
    CB_H2_CONTINUATION = 0x9,
};
enum { CB_H2_F_END_STREAM = 0x1, CB_H2_F_ACK = 0x1, CB_H2_F_END_HEADERS = 0x4, CB_H2_F_PADDED = 0x8,
       CB_H2_F_PRIORITY = 0x20 };
enum {
    CB_H2_NO_ERROR = 0x0, CB_H2_PROTOCOL_ERROR = 0x1, CB_H2_INTERNAL_ERROR = 0x2, CB_H2_FLOW_CONTROL_ERROR = 0x3,
    CB_H2_SETTINGS_TIMEOUT = 0x4, CB_H2_STREAM_CLOSED = 0x5, CB_H2_FRAME_SIZE_ERROR = 0x6,
    CB_H2_REFUSED_STREAM = 0x7, CB_H2_CANCEL = 0x8, CB_H2_COMPRESSION_ERROR = 0x9, CB_H2_CONNECT_ERROR = 0xa,
    CB_H2_ENHANCE_YOUR_CALM = 0xb, CB_H2_INADEQUATE_SECURITY = 0xc, CB_H2_HTTP_1_1_REQUIRED = 0xd,
};
enum {
    CB_H2_S_HEADER_TABLE_SIZE = 0x1, CB_H2_S_ENABLE_PUSH = 0x2, CB_H2_S_MAX_CONCURRENT_STREAMS = 0x3,
    CB_H2_S_INITIAL_WINDOW_SIZE = 0x4, CB_H2_S_MAX_FRAME_SIZE = 0x5, CB_H2_S_MAX_HEADER_LIST_SIZE = 0x6,
};
#define CB_H2_PREFACE     "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n"
#define CB_H2_PREFACE_LEN 24
#define CB_H2_FRAME_HEAD  9
#define CB_H2_DEFAULT_WINDOW 65535
#define CB_H2_DEFAULT_FRAME  16384
#define CB_H2_MAX_WINDOW     0x7fffffff
#define CB_H2_MAX_FRAME      0xffffff

// What this side advertises and keeps to. The defaults (cb_h2_limits_default)
// suit unary calls with replies of a few MB.
typedef struct {
    uint32_t stream_window;     // SETTINGS_INITIAL_WINDOW_SIZE: per stream, at most CB_H2_MAX_WINDOW
    uint32_t conn_window;       // the connection's, raised by a WINDOW_UPDATE after the preface
    uint32_t max_header_list;   // SETTINGS_MAX_HEADER_LIST_SIZE: over it, the stream is reset
    uint32_t max_header_block;  // one block, HEADERS and CONTINUATIONs joined: over it, a connection error
    uint32_t max_streams;       // streams this side keeps at once, open and waiting
} cb_h2_limits;
void cb_h2_limits_default(cb_h2_limits *l);

// One header field of a request. never: never-indexed (an API key).
typedef struct {
    const char *name, *value;
    size_t      name_len, value_len;
    bool        never;
} cb_h2_field;

// A piece of a request body. The bytes must stay valid until the stream ends
// or is cancelled: the session sends them from where they are.
typedef struct {
    const void *p;
    size_t      n;
} cb_h2_iov;

// How a stream ended.
typedef enum {
    CB_H2_END_OK = 0,
    CB_H2_END_PEER_RESET,    // the peer sent RST_STREAM
    CB_H2_END_GOAWAY,        // the peer went away before processing it
    CB_H2_END_STREAM_ERROR,  // this side reset it: a malformed response, or a callback refused it
    CB_H2_END_CONN_ERROR,    // the peer broke the protocol, and the connection is dead
    CB_H2_END_FAILED,        // this side ended the session (cb_h2_sess_fail, a graceful close, out of ids)
} cb_h2_end_kind;

typedef struct {
    cb_h2_end_kind kind;
    bool     ok;          // the response arrived whole (END_STREAM after its headers)
    uint32_t h2_code;     // the RST_STREAM or GOAWAY code, or the one this side used
    bool     by_peer;     // the peer reset it, or went away under it
    // The peer never processed the request (REFUSED_STREAM, a GOAWAY whose last
    // stream id is below it, or it never left this side): safe to send again,
    // on another connection.
    bool     unprocessed;
    const char *why;      // a constant sentence for the logs, NULL when ok
} cb_h2_end;

typedef struct cb_h2_stream cb_h2_stream;

// Each callback gets the user pointer given to submit. on_header and on_data
// answer CB_OK to go on; anything else resets the stream (CANCEL), and its
// on_end follows with ok false. A stream's on_end is its last call, and
// after it the stream handle is gone.
typedef struct {
    // trailer false: the response's header block (each 1xx block's too);
    // true: the trailers after the body.
    int  (*on_header)(void *user, bool trailer, const char *name, size_t name_len, const char *value,
                      size_t value_len);
    int  (*on_data)(void *user, const uint8_t *p, size_t n);
    void (*on_end)(void *user, const cb_h2_end *end);
} cb_h2_callbacks;

typedef struct cb_h2_sess cb_h2_sess;

// A session with the preface and SETTINGS queued. NULL on CB_E_NOMEM.
cb_h2_sess *cb_h2_sess_new(const cb_h2_limits *limits, const cb_h2_callbacks *cb);
// Frees it; no callback runs (end the streams first if their callers wait).
void cb_h2_sess_free(cb_h2_sess *s);

// Queues a request: HEADERS (:method, :scheme, :authority, :path, then
// `fields`) and the body, END_STREAM on the last frame. The header block is
// encoded now; the body is sent later, from its iovs. CB_OK and *out, or
// CB_E_STATE (dead, going away, or out of stream ids: open another
// connection), CB_E_FULL (max_streams in hand), CB_E_INVALID (a field name
// with an uppercase letter, or a connection-specific field: RFC 9113 8.2.2).
int cb_h2_sess_submit(cb_h2_sess *s, const char *method, const char *scheme, const char *authority, const char *path,
                      const cb_h2_field *fields, int n_fields, const cb_h2_iov *body, int n_body, void *user,
                      cb_h2_stream **out);

// Drops a stream: a RST_STREAM (CANCEL) when it had opened. No callback runs
// for it again.
void cb_h2_sess_cancel(cb_h2_sess *s, cb_h2_stream *st);

// Takes the peer's bytes, any amount, frames split anywhere. CB_OK, or CB_E_IO
// when the connection is now dead (a protocol error by the peer, or a GOAWAY
// earlier); the callbacks have run either way.
int cb_h2_sess_recv(cb_h2_sess *s, const uint8_t *p, size_t n);

// Appends what is ready to send to out: frames queued by recv (ACKs, window
// updates, resets), then new streams' HEADERS and as much body as the windows
// allow. Answers the bytes appended.
size_t cb_h2_sess_flush(cb_h2_sess *s, cb_buf *out);
bool   cb_h2_sess_want_flush(const cb_h2_sess *s);

// Ends the session from this side (a timeout, a closed socket): every stream
// ends with `why` (unprocessed when it never opened), and a GOAWAY with `code`
// is queued for flush. Idempotent.
void cb_h2_sess_fail(cb_h2_sess *s, uint32_t code, const char *why);
// A graceful close: a GOAWAY (NO_ERROR) queued, no new streams accepted.
void cb_h2_sess_goaway(cb_h2_sess *s);

bool cb_h2_sess_dead(const cb_h2_sess *s);        // a connection error, or failed: close the socket
bool cb_h2_sess_accepting(const cb_h2_sess *s);   // submit would take a stream
int  cb_h2_sess_streams(const cb_h2_sess *s);     // streams in hand, open and waiting
// The peer's settings, for the tests.
uint32_t cb_h2_sess_peer_setting(const cb_h2_sess *s, int id);

// ---- frames, for the session, the test server and the fuzzer -----------------
// A frame's header at p, when all 9 + len bytes are there: answers 9 + len, 0
// when more bytes are needed, CB_E_INVALID when len > max_frame.
typedef struct {
    uint32_t len, stream;
    uint8_t  type, flags;
} cb_h2_frame;
int  cb_h2_frame_parse(const uint8_t *p, size_t n, uint32_t max_frame, cb_h2_frame *f);
void cb_h2_frame_head(cb_buf *out, uint32_t len, uint8_t type, uint8_t flags, uint32_t stream);
// A header block as HEADERS then CONTINUATIONs of at most max_frame each.
void cb_h2_frame_headers(cb_buf *out, uint32_t stream, const uint8_t *block, size_t n, bool end_stream,
                         uint32_t max_frame);
void cb_h2_frame_settings(cb_buf *out, const uint16_t *ids, const uint32_t *values, int n, bool ack);
void cb_h2_frame_u32(cb_buf *out, uint8_t type, uint32_t stream, uint32_t v);   // RST_STREAM, WINDOW_UPDATE
void cb_h2_frame_goaway(cb_buf *out, uint32_t last_stream, uint32_t code);
void cb_h2_frame_ping(cb_buf *out, const uint8_t opaque[8], bool ack);
// A frame's payload without its padding (DATA, HEADERS: PADDED) and, for
// HEADERS, its priority fields. CB_OK, or CB_E_INVALID (a pad longer than the
// frame: PROTOCOL_ERROR).
int cb_h2_frame_unpad(const cb_h2_frame *f, const uint8_t *payload, const uint8_t **out, size_t *out_len);

#endif
