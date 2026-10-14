// lib_client.h - a blocking HTTP/1.1 and WebSocket client for the tests.
//
// Linked into every test program (the Makefile links every test/lib_*.c). It
// is deliberately plain: one request per connection unless a test keeps the
// socket, Content-Length bodies only, and client frames masked as RFC 6455
// requires. The smoke test plays a whole game through it.
#ifndef CB_TEST_CLIENT_H
#define CB_TEST_CLIENT_H

#include "util/str.h"

#include <stddef.h>
#include <stdint.h>

typedef struct {
    int    status;
    cb_buf head;   // the response head, as text (status line and headers)
    cb_buf body;
} tc_response;

void tc_response_free(tc_response *r);
// The value of a response header (case-insensitive) copied into out, or NULL.
const char *tc_header(const tc_response *r, const char *name, char *out, size_t cap);

// A TCP connection to 127.0.0.1:port, or -1.
int tc_connect(int port);
// Sends one request on `fd` and reads one response (for HEAD, the head, and as
// the body only bytes that came with it, which a correct server never sends). `headers` is extra header
// lines, each ending "\r\n" (NULL for none). 0 on success, -1 on I/O error or
// a response that does not parse within timeout_ms.
int tc_request(int fd, const char *method, const char *path, const char *headers,
               const void *body, size_t body_len, int timeout_ms, tc_response *out);
// tc_connect + tc_request + close.
int tc_http(int port, const char *method, const char *path, const char *headers,
            const void *body, size_t body_len, tc_response *out);

// Opens a WebSocket at `path` (with its query). The socket, or -1.
int tc_ws_open(int port, const char *path, const char *headers);
int tc_ws_send(int fd, int opcode, const void *data, size_t len);
// Reads one frame (answering nothing for it). Returns the opcode and fills
// out, or -1 on timeout / close / error.
int tc_ws_recv(int fd, int timeout_ms, cb_buf *out);

#endif
