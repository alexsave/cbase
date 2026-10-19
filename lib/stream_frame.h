// stream_frame.h - the frames of the push socket that are not a push: what a
// client says to the server and what the server answers. One header, written
// once, read by both ends (the server's hub and the client's socket).
//
// The socket is a plain binary WebSocket (no subprotocol): ONE WebSocket
// MESSAGE IS ONE FRAME. Little-endian. Every frame starts [SF_V][kind].
//
//   client to server
//     SF_HELLO  u16 n, n bytes of access token   first frame, once
//     SF_SUB    u32 id, u8 what, u8 n, n gid     hear `what` (netwire.h NW_SUB_*)
//     SF_UNSUB  u32 id, u8 what, u8 n, n gid
//     SF_REQ    u32 id, u8 meth, u16 n, n path, body to the end
//                                                the request the HTTP route of
//                                                that path answers (POST /v1/action)
//     SF_PING                                    the client's own dead-socket probe
//   server to client
//     SF_READY                                   the HELLO was taken
//     SF_REPLY  u32 id, u16 status, body to the end   the answer to SF_SUB, SF_UNSUB
//                                                (empty body) or SF_REQ (the HTTP body)
//     SF_PONG                                    answers SF_PING
//     anything else is a push: the product's own frame (netwire.h NW_WS_*)
//
// A frame that does not parse ends the connection. Liveness of the peer is the
// WebSocket's own ping and pong frames, which the server sends (net/server.c);
// SF_PING exists only because a browser cannot see them.
#ifndef CB_STREAM_FRAME_H
#define CB_STREAM_FRAME_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define SF_V 1

#define SF_HELLO 60
#define SF_READY 61
#define SF_SUB   62
#define SF_UNSUB 63
#define SF_REQ   64
#define SF_REPLY 65
#define SF_PING  66
#define SF_PONG  67

#define SF_METH_GET  1
#define SF_METH_POST 2

#define SF_TOKEN_MAX 2048
#define SF_GID_MAX   64
#define SF_PATH_MAX  256

typedef struct {
    uint8_t        kind;
    uint32_t       id;
    uint8_t        what;
    uint8_t        meth;
    uint16_t       status;
    const char    *gid;      uint8_t  gid_len;
    const char    *path;     uint16_t path_len;
    const uint8_t *tok;      uint16_t tok_len;
    const uint8_t *body;     size_t   body_len;
} sf_frame;

static inline uint32_t sf_u32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static inline uint16_t sf_u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static inline void sf_w32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static inline void sf_w16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

// 0 and *f filled (its pointers into p), or -1 for a frame of this file that is
// malformed, a bound passed, or a kind that is not this file's (a push: use
// sf_is_push). Exact: a byte short or over is -1 for the kinds with no tail.
static inline int sf_parse(const uint8_t *p, size_t n, sf_frame *f) {
    memset(f, 0, sizeof *f);
    if (n < 2 || p[0] != SF_V) return -1;
    f->kind = p[1];
    switch (f->kind) {
    case SF_READY: case SF_PING: case SF_PONG:
        return n == 2 ? 0 : -1;
    case SF_HELLO:
        if (n < 4) return -1;
        f->tok_len = sf_u16(p + 2);
        if (f->tok_len > SF_TOKEN_MAX || n != 4u + f->tok_len) return -1;
        f->tok = p + 4;
        return 0;
    case SF_SUB: case SF_UNSUB:
        if (n < 8) return -1;
        f->id = sf_u32(p + 2);
        f->what = p[6];
        f->gid_len = p[7];
        if (f->gid_len > SF_GID_MAX || n != 8u + f->gid_len) return -1;
        f->gid = (const char *)p + 8;
        return 0;
    case SF_REQ:
        if (n < 9) return -1;
        f->id = sf_u32(p + 2);
        f->meth = p[6];
        f->path_len = sf_u16(p + 7);
        if (f->path_len == 0 || f->path_len > SF_PATH_MAX || n < 9u + f->path_len) return -1;
        if (f->meth != SF_METH_GET && f->meth != SF_METH_POST) return -1;
        f->path = (const char *)p + 9;
        f->body = p + 9 + f->path_len;
        f->body_len = n - 9 - f->path_len;
        return 0;
    case SF_REPLY:
        if (n < 8) return -1;
        f->id = sf_u32(p + 2);
        f->status = sf_u16(p + 6);
        f->body = p + 8;
        f->body_len = n - 8;
        return 0;
    default:
        return -1;
    }
}

// A push, as opposed to a frame of this file: the product's own kinds.
static inline int sf_is_push(const uint8_t *p, size_t n) { return n >= 2 && p[0] == SF_V && (p[1] < SF_HELLO || p[1] > SF_PONG); }

// The writers return the frame's length, or -1 when it does not fit cap.
static inline int sf_put_bare(uint8_t *o, size_t cap, uint8_t kind) {
    if (cap < 2) return -1;
    o[0] = SF_V; o[1] = kind;
    return 2;
}

static inline int sf_put_hello(uint8_t *o, size_t cap, const char *tok, size_t n) {
    if (n > SF_TOKEN_MAX || cap < 4 + n) return -1;
    o[0] = SF_V; o[1] = SF_HELLO;
    sf_w16(o + 2, (uint16_t)n);
    if (n) memcpy(o + 4, tok, n);
    return (int)(4 + n);
}

static inline int sf_put_sub(uint8_t *o, size_t cap, uint8_t kind, uint32_t id, uint8_t what, const char *gid, size_t n) {
    if (n > SF_GID_MAX || cap < 8 + n) return -1;
    o[0] = SF_V; o[1] = kind;
    sf_w32(o + 2, id);
    o[6] = what;
    o[7] = (uint8_t)n;
    if (n) memcpy(o + 8, gid, n);
    return (int)(8 + n);
}

static inline int sf_put_req(uint8_t *o, size_t cap, uint32_t id, uint8_t meth, const char *path, size_t pn, const uint8_t *body, size_t bn) {
    if (pn == 0 || pn > SF_PATH_MAX || cap < 9 + pn || cap - 9 - pn < bn) return -1;
    o[0] = SF_V; o[1] = SF_REQ;
    sf_w32(o + 2, id);
    o[6] = meth;
    sf_w16(o + 7, (uint16_t)pn);
    memcpy(o + 9, path, pn);
    if (bn) memcpy(o + 9 + pn, body, bn);
    return (int)(9 + pn + bn);
}

// The head of a reply (8 bytes); the caller appends the body after it.
static inline int sf_put_reply_head(uint8_t *o, size_t cap, uint32_t id, uint16_t status) {
    if (cap < 8) return -1;
    o[0] = SF_V; o[1] = SF_REPLY;
    sf_w32(o + 2, id);
    sf_w16(o + 6, status);
    return 8;
}

#endif
