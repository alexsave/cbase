#include "lib_client.h"

#include "net/ws.h"
#include "util/uuid.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

void tc_response_free(tc_response *r) {
    cb_buf_free(&r->head);
    cb_buf_free(&r->body);
}

const char *tc_header(const tc_response *r, const char *name, char *out, size_t cap) {
    const char *h = cb_buf_cstr(&r->head);
    size_t nl = strlen(name);
    for (const char *line = strstr(h, "\r\n"); line && line[2]; line = strstr(line + 2, "\r\n")) {
        const char *l = line + 2;
        if (strncasecmp(l, name, nl) == 0 && l[nl] == ':') {
            const char *v = l + nl + 1;
            while (*v == ' ') v++;
            const char *e = strstr(v, "\r\n");
            size_t n = e ? (size_t)(e - v) : strlen(v);
            if (n >= cap) n = cap - 1;
            memcpy(out, v, n);
            out[n] = 0;
            return out;
        }
    }
    return NULL;
}

int tc_connect(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) { close(fd); return -1; }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
#ifdef SO_NOSIGPIPE
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    return fd;
}

static int write_all(int fd, const void *p, size_t n) {
    const uint8_t *b = p;
    while (n) {
        ssize_t w = send(fd, b, n, 0);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        b += w;
        n -= (size_t)w;
    }
    return 0;
}

// Reads at least one more byte into `in` within timeout_ms. 1, 0 on close, -1.
static int read_more(int fd, cb_buf *in, int timeout_ms) {
    struct pollfd p = { fd, POLLIN, 0 };
    int r = poll(&p, 1, timeout_ms);
    if (r <= 0) return -1;
    if (!cb_buf_reserve(in, 8192)) return -1;
    ssize_t n = recv(fd, in->p + in->len, in->cap - in->len - 1, 0);
    if (n < 0) return errno == EINTR ? read_more(fd, in, timeout_ms) : -1;
    if (n == 0) return 0;
    in->len += (size_t)n;
    in->p[in->len] = 0;
    return 1;
}

int tc_request(int fd, const char *method, const char *path, const char *headers,
               const void *body, size_t body_len, int timeout_ms, tc_response *out) {
    memset(out, 0, sizeof *out);
    cb_buf req;
    cb_buf_init(&req);
    cb_buf_printf(&req, "%s %s HTTP/1.1\r\nHost: 127.0.0.1\r\n%sContent-Length: %zu\r\n\r\n",
                  method, path, headers ? headers : "", body_len);
    cb_buf_append(&req, body, body_len);
    int rc = write_all(fd, req.p, req.len);
    cb_buf_free(&req);
    if (rc) return -1;
    cb_buf in;
    cb_buf_init(&in);
    char *he = NULL;
    while (!(he = strstr((char *)cb_buf_cstr(&in), "\r\n\r\n"))) {
        if (read_more(fd, &in, timeout_ms) != 1) { cb_buf_free(&in); return -1; }
    }
    size_t head_len = (size_t)(he - (char *)in.p) + 4;
    cb_buf_append(&out->head, in.p, head_len);
    if (sscanf((char *)in.p, "HTTP/1.%*d %d", &out->status) != 1) { cb_buf_free(&in); return -1; }
    char cl[32];
    size_t want = tc_header(out, "content-length", cl, sizeof cl) ? (size_t)strtoul(cl, NULL, 10) : 0;
    if (out->status == 101) want = 0;
    // A HEAD answer has GET's Content-Length and no body. Whatever arrived
    // behind the head anyway is kept as the body, so a test sees the bug.
    if (strcmp(method, "HEAD") == 0) want = in.len - head_len;
    while (in.len - head_len < want) {
        if (read_more(fd, &in, timeout_ms) != 1) { cb_buf_free(&in); return -1; }
    }
    cb_buf_append(&out->body, in.p + head_len, want);
    cb_buf_free(&in);
    return 0;
}

int tc_http(int port, const char *method, const char *path, const char *headers,
            const void *body, size_t body_len, tc_response *out) {
    int fd = tc_connect(port);
    if (fd < 0) { memset(out, 0, sizeof *out); return -1; }
    int rc = tc_request(fd, method, path, headers, body, body_len, 10000, out);
    close(fd);
    return rc;
}

int tc_ws_open(int port, const char *path, const char *headers) {
    int fd = tc_connect(port);
    if (fd < 0) return -1;
    char req[2048];
    int n = snprintf(req, sizeof req,
                     "GET %s HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                     "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n%s\r\n",
                     path, headers ? headers : "");
    if (n <= 0 || (size_t)n >= sizeof req || write_all(fd, req, (size_t)n)) { close(fd); return -1; }
    // Byte by byte up to the blank line, so a frame the server sends right
    // behind the 101 stays in the socket for tc_ws_recv.
    cb_buf head;
    cb_buf_init(&head);
    while (!strstr(cb_buf_cstr(&head), "\r\n\r\n")) {
        struct pollfd p = { fd, POLLIN, 0 };
        uint8_t b;
        if (poll(&p, 1, 5000) <= 0 || recv(fd, &b, 1, 0) != 1) { cb_buf_free(&head); close(fd); return -1; }
        cb_buf_append(&head, &b, 1);
    }
    tc_response r;
    memset(&r, 0, sizeof r);
    r.head = head;
    char accept[64];
    int ok = sscanf(cb_buf_cstr(&head), "HTTP/1.1 %d", &r.status) == 1 && r.status == 101 &&
             tc_header(&r, "sec-websocket-accept", accept, sizeof accept) &&
             strcmp(accept, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") == 0;
    tc_response_free(&r);
    if (!ok) { close(fd); return -1; }
    return fd;
}

int tc_ws_send(int fd, int opcode, const void *data, size_t len) {
    uint8_t mask[4];
    cb_random_bytes(mask, 4);
    uint8_t hdr[14];
    size_t hn = cb_ws_frame_header(hdr, true, opcode, len, mask);
    uint8_t *frame = malloc(hn + len + 1);
    if (!frame) return -1;
    memcpy(frame, hdr, hn);
    memcpy(frame + hn, data, len);
    cb_ws_unmask(frame + hn, len, mask);
    int rc = write_all(fd, frame, hn + len);
    free(frame);
    return rc;
}

int tc_ws_recv(int fd, int timeout_ms, cb_buf *out) {
    // Byte-at-a-time framing is fine for tests: read until one frame parses.
    cb_buf in;
    cb_buf_init(&in);
    for (;;) {
        cb_ws_frame f;
        int n = in.len ? cb_ws_frame_parse(in.p, in.len, 1u << 24, &f) : 0;
        if (n < 0) { cb_buf_free(&in); return -1; }
        if (n > 0) {
            cb_buf_reset(out);
            cb_buf_append(out, in.p + f.header_len, (size_t)f.len);
            cb_buf_free(&in);
            return f.opcode;
        }
        struct pollfd p = { fd, POLLIN, 0 };
        if (poll(&p, 1, timeout_ms) <= 0) { cb_buf_free(&in); return -1; }
        uint8_t b;
        ssize_t r = recv(fd, &b, 1, 0);
        if (r <= 0) { cb_buf_free(&in); return -1; }
        cb_buf_append(&in, &b, 1);
    }
}
