// The WebSocket frame codec - see ws.h. The connection half lives in server.c,
// beside the connection it is a state of.
//
// LIFTED from foolish/server/impls/native: the header writer is ws.c's
// ws_send_frame (0..125 / 126 + u16 / 127 + u64), the reader is shard.c's
// wsasync_feed with its per-field logic and unsigned length assembly. What
// changed: the reader is a pure function over a buffer instead of a state
// machine fed from a socket (the server keeps unread bytes in its buffer and
// calls again), it rejects reserved bits and the 64-bit top bit outright (the
// native one relied on the buffer cap to catch them), and SHA-1 is OpenSSL's
// rather than a second hand-written copy, since cbase links libcrypto anyway.
#include "ws.h"

#include "../../../shared/c/b64/b64.h"
#include "util/err.h"

#include <openssl/sha.h>
#include <string.h>
#include <strings.h>

int cb_ws_frame_parse(const uint8_t *buf, size_t n, uint64_t max_payload, cb_ws_frame *f) {
    if (n < 2) return 0;
    f->fin = buf[0] & 0x80;
    if (buf[0] & 0x70) return -1;   // RSV1-3: no extension was negotiated
    f->opcode = buf[0] & 0x0F;
    f->masked = buf[1] & 0x80;
    bool ctrl = f->opcode & 0x8;
    if (!(f->opcode <= CB_WS_OP_BIN || (f->opcode >= CB_WS_OP_CLOSE && f->opcode <= CB_WS_OP_PONG))) return -1;
    uint64_t len = buf[1] & 0x7F;
    size_t at = 2;
    if (len == 126) {
        if (n < 4) return 0;
        len = (uint64_t)buf[2] << 8 | buf[3];
        at = 4;
    } else if (len == 127) {
        if (n < 10) return 0;
        len = 0;
        for (int i = 0; i < 8; i++) len = len << 8 | buf[2 + i];
        if (len >> 63) return -1;
        at = 10;
    }
    if (ctrl && (len > 125 || !f->fin)) return -1;
    if (len > max_payload || len > (uint64_t)(0x7FFFFFFF - 14)) return -1;   // the return value is an int
    if (f->masked) {
        if (n < at + 4) return 0;
        memcpy(f->mask, buf + at, 4);
        at += 4;
    } else {
        memset(f->mask, 0, 4);
    }
    f->len = len;
    f->header_len = at;
    if ((uint64_t)(n - at) < len) return 0;
    return (int)(at + len);
}

void cb_ws_unmask(uint8_t *p, size_t n, const uint8_t mask[4]) {
    for (size_t i = 0; i < n; i++) p[i] ^= mask[i & 3];
}

size_t cb_ws_frame_header(uint8_t out[14], bool fin, int opcode, uint64_t len, const uint8_t *mask) {
    size_t n = 0;
    out[n++] = (uint8_t)((fin ? 0x80 : 0) | (opcode & 0x0F));
    uint8_t mbit = mask ? 0x80 : 0;
    if (len <= 125) {
        out[n++] = (uint8_t)(mbit | len);
    } else if (len <= 0xFFFF) {
        out[n++] = (uint8_t)(mbit | 126);
        out[n++] = (uint8_t)(len >> 8);
        out[n++] = (uint8_t)len;
    } else {
        out[n++] = (uint8_t)(mbit | 127);
        for (int i = 7; i >= 0; i--) out[n++] = (uint8_t)(len >> (8 * i));
    }
    if (mask) { memcpy(out + n, mask, 4); n += 4; }
    return n;
}

int cb_ws_accept_key(const char *client_key, char out[29]) {
    static const char GUID[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    size_t kn = strlen(client_key);
    uint8_t raw[18];
    if (kn != 24 || b64_decode(client_key, kn, raw, sizeof raw) != 16) return CB_E_INVALID;
    char buf[24 + sizeof GUID];
    memcpy(buf, client_key, 24);
    memcpy(buf + 24, GUID, sizeof GUID - 1);
    uint8_t digest[SHA_DIGEST_LENGTH];
    SHA1((const unsigned char *)buf, 24 + sizeof GUID - 1, digest);
    return b64_encode(digest, sizeof digest, out, 29) == 28 ? CB_OK : CB_E_INVALID;
}

static bool has_token(const char *v, const char *tok) {
    // A comma-separated header (Connection: keep-alive, Upgrade) holds tok, any case.
    size_t tn = strlen(tok);
    for (const char *p = v; p && *p;) {
        while (*p == ' ' || *p == ',') p++;
        const char *e = strchr(p, ',');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        while (n > 0 && p[n - 1] == ' ') n--;
        if (n == tn && strncasecmp(p, tok, tn) == 0) return true;
        p = e;
    }
    return false;
}

int cb_ws_upgrade(const cb_http_req *req, cb_http_res *res, const cb_ws_handlers *h, void *ud) {
    return cb_ws_upgrade_protocol(req, res, h, ud, NULL);
}

int cb_ws_upgrade_protocol(const cb_http_req *req, cb_http_res *res, const cb_ws_handlers *h, void *ud,
                           const char *protocol) {
    const char *up = cb_http_header_get(req, "upgrade");
    const char *conn = cb_http_header_get(req, "connection");
    const char *ver = cb_http_header_get(req, "sec-websocket-version");
    const char *key = cb_http_header_get(req, "sec-websocket-key");
    if (strcmp(req->method, "GET") != 0 || !up || !has_token(up, "websocket") || !conn || !has_token(conn, "upgrade")) {
        cb_res_text(res, 426, "this endpoint speaks WebSocket\n");
        cb_res_header(res, "Upgrade", "websocket");
        return CB_E_INVALID;
    }
    char accept[29];
    if (!ver || strcmp(ver, "13") != 0 || !key || cb_ws_accept_key(key, accept) != CB_OK) {
        cb_res_text(res, 400, "bad WebSocket handshake\n");
        cb_res_header(res, "Sec-WebSocket-Version", "13");
        return CB_E_INVALID;
    }
    // A subprotocol the endpoint speaks must be one the client offered (RFC
    // 6455 4.2.2): a browser that offered it fails a 101 that does not name
    // it, and a client that offered none is not speaking it.
    const char *offered = cb_http_header_get(req, "sec-websocket-protocol");
    if (protocol && (!offered || !has_token(offered, protocol))) {
        cb_res_text(res, 400, "this endpoint speaks a WebSocket subprotocol the request did not offer\n");
        return CB_E_INVALID;
    }
    res->status = 101;
    cb_buf_reset(&res->body);
    cb_res_header(res, "Upgrade", "websocket");
    cb_res_header(res, "Connection", "Upgrade");
    cb_res_header(res, "Sec-WebSocket-Accept", accept);
    if (protocol) cb_res_header(res, "Sec-WebSocket-Protocol", protocol);
    res->upgrade = true;
    res->ws_handlers = h;
    res->ws_ud = ud;
    return CB_OK;
}
