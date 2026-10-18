// ws.h - RFC 6455 WebSocket: the frame codec (pure) and the server side of a
// connection (loop thread).
//
// The codec is lifted from the native server (ws.c's frame writer and
// shard.c's resumable reader) and made a pure function over a buffer: it never
// touches a socket, so the hostile-length tests run without one. Realtime
// (src/realtime) is the only user of a connection; it speaks MQTT
// (realtime/stream_mqtt.h) on top.
#ifndef CB_WS_H
#define CB_WS_H

#include "http.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CB_WS_OP_CONT  0x0
#define CB_WS_OP_TEXT  0x1
#define CB_WS_OP_BIN   0x2
#define CB_WS_OP_CLOSE 0x8
#define CB_WS_OP_PING  0x9
#define CB_WS_OP_PONG  0xA

#define CB_WS_MAX_MESSAGE (1u << 20)   // an assembled inbound message, all fragments
#define CB_WS_MAX_QUEUED  (4u << 20)   // outbound bytes a slow reader may owe before it is dropped

// ---- the codec (pure) ----------------------------------------------------------

typedef struct {
    bool     fin;
    int      opcode;        // CB_WS_OP_*
    bool     masked;
    uint8_t  mask[4];
    uint64_t len;           // payload bytes
    size_t   header_len;    // 2..14
} cb_ws_frame;

// Reads one frame header from buf[0..n). Returns:
//   > 0  the whole frame (header + payload) is in buf: that many bytes; the
//        payload is at buf + f->header_len, still masked (cb_ws_unmask)
//   0    not all of it yet
//   -1   a protocol error: a reserved bit set, an unknown opcode, a control
//        frame longer than 125 or fragmented, an 8-byte length with the top
//        bit set (RFC 6455 5.2), or a payload longer than max_payload
// The 8-byte length is assembled UNSIGNED: assembling attacker bytes into a
// signed int64 is undefined behaviour that licenses the compiler to delete the
// very check meant to catch it (native ws.c, commit 22efa79).
int cb_ws_frame_parse(const uint8_t *buf, size_t n, uint64_t max_payload, cb_ws_frame *f);
// XORs the payload with the mask in place (the mask's phase starts at 0).
void cb_ws_unmask(uint8_t *p, size_t n, const uint8_t mask[4]);
// Writes a frame header for a `len`-byte payload into out (14 bytes is always
// enough) and returns its length. `mask` NULL for a server frame (RFC 6455 5.1:
// server frames are never masked); a client passes 4 bytes and masks the
// payload itself with cb_ws_unmask.
size_t cb_ws_frame_header(uint8_t out[14], bool fin, int opcode, uint64_t len, const uint8_t *mask);
// Sec-WebSocket-Accept for a Sec-WebSocket-Key: base64(SHA-1(key + GUID)), 28
// characters and a NUL. CB_OK, or CB_E_INVALID for a key that is not 24
// base64 characters (16 bytes).
int cb_ws_accept_key(const char *client_key, char out[29]);

// ---- a server connection (loop thread only) -----------------------------------

typedef struct cb_ws cb_ws;

typedef struct cb_ws_handlers {
    // The upgrade has been written; the socket is now this cb_ws.
    void (*on_open)(cb_ws *ws, void *ud);
    // One complete TEXT or BIN message, fragments assembled, unmasked. The
    // bytes live until the callback returns.
    void (*on_message)(cb_ws *ws, int opcode, const uint8_t *data, size_t len, void *ud);
    // Called exactly once, whatever closed it (peer, error, cb_ws_close,
    // shutdown). After it returns the cb_ws is freed: drop every pointer to it.
    void (*on_close)(cb_ws *ws, void *ud);
} cb_ws_handlers;

// Inside a CB_ROUTE_LOOP handler: validates the upgrade request (GET, Upgrade:
// websocket, Connection: upgrade, version 13, a 16-byte key) and turns `res`
// into the 101 response; the connection becomes a WebSocket once it is
// written, and on_open runs. CB_OK, or CB_E_INVALID with `res` already set to
// the 400 / 426 answer.
int cb_ws_upgrade(const cb_http_req *req, cb_http_res *res, const cb_ws_handlers *h, void *ud);
// The same for an endpoint that speaks `protocol` (Sec-WebSocket-Protocol,
// "mqtt"): the request must offer it, and the 101 names it. NULL: none, as
// cb_ws_upgrade.
int cb_ws_upgrade_protocol(const cb_http_req *req, cb_http_res *res, const cb_ws_handlers *h, void *ud,
                           const char *protocol);

// Queues one unfragmented frame. CB_OK; CB_E_STATE when the socket is closing;
// CB_E_FULL when the peer owes more than CB_WS_MAX_QUEUED (the connection is
// then closed with 1008: a reader that slow is not keeping up with pushes).
int  cb_ws_send(cb_ws *ws, int opcode, const void *data, size_t len);
int  cb_ws_send_text(cb_ws *ws, const char *text, size_t len);
// Sends a CLOSE with `code` and closes once it is written. on_close follows.
void cb_ws_close(cb_ws *ws, uint16_t code);
// A process-unique id (never reused), for finding the socket again from
// another thread's post (rt_local.c).
uint64_t cb_ws_id(const cb_ws *ws);
// The upgrade request's raw query, kept ("" for none): an MQTT connection's
// token may ride in it (realtime/stream_mqtt.h).
const char *cb_ws_query(const cb_ws *ws);

#endif
