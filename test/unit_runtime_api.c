// unit_runtime_api.c - the Lambda Runtime API client against a raw socket
// that answers /next the ways Lambda does.
//
// Lambda's Runtime API answers GET /next with Transfer-Encoding: chunked once
// the event is large enough (a CloudFront request's event, about 2 KB, was;
// the bare Function URL event of the same request was not). The client once
// kept the chunk framing in the event ("8aa\r\n{...}\r\n0\r\n\r\n"), so every
// POST through CloudFront failed to decode and answered 502.
#include "t.h"

#include "aws/event.h"
#include "aws/runtime_api.h"
#include "fixture_cloudfront_event.h"
#include "util/err.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct {
    int         listen_fd;
    const char *response;   // written whole, then the connection closes
    size_t      response_len;
    char        request[1024];
} fake;

static void *serve_one(void *ud) {
    fake *f = ud;
    int c = accept(f->listen_fd, NULL, NULL);
    if (c < 0) return NULL;
    size_t n = 0;
    while (n + 1 < sizeof f->request) {
        ssize_t r = recv(c, f->request + n, sizeof f->request - 1 - n, 0);
        if (r <= 0) break;
        n += (size_t)r;
        f->request[n] = 0;
        if (strstr(f->request, "\r\n\r\n")) break;
    }
    for (size_t off = 0; off < f->response_len;) {
        ssize_t w = send(c, f->response + off, f->response_len - off, 0);
        if (w <= 0) break;
        off += (size_t)w;
    }
    close(c);
    return NULL;
}

static int listen_any(int *port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t al = sizeof a;
    if (fd < 0 || bind(fd, (struct sockaddr *)&a, sizeof a) != 0 || listen(fd, 4) != 0 ||
        getsockname(fd, (struct sockaddr *)&a, &al) != 0)
        return -1;
    *port = ntohs(a.sin_port);
    return fd;
}

static const char HEAD[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: application/json\r\n"
    "Lambda-Runtime-Aws-Request-Id: f0ec636b-0228-4ef9-9038-35ec931c3354\r\n"
    "Lambda-Runtime-Deadline-Ms: 1791998921003\r\n"
    "Lambda-Runtime-Invoked-Function-Arn: arn:aws:lambda:us-east-1:000000000000:function:cbase\r\n"
    "Lambda-Runtime-Trace-Id: Root=1-6acfbbc5-70866caf694799b013c7d38d;Lineage=1\r\n"
    "Date: Wed, 14 Oct 2026 17:28:38 GMT\r\n";

// One /next against `response`; the invocation it produced.
static int next_from(const char *response, size_t len, cb_invocation *inv) {
    fake f = { .response = response, .response_len = len };
    int port = 0;
    f.listen_fd = listen_any(&port);
    T_CHECK(f.listen_fd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, serve_one, &f);
    int rc = cb_runtime_next("127.0.0.1", port, inv);
    pthread_join(th, NULL);
    close(f.listen_fd);
    T_CHECK(strncmp(f.request, "GET /2018-06-01/runtime/invocation/next HTTP/1.1\r\n", 50) == 0);
    return rc;
}

static void check_event(const cb_invocation *inv) {
    T_EQ_STR(inv->request_id, "f0ec636b-0228-4ef9-9038-35ec931c3354");
    T_EQ_INT(inv->deadline_ms, 1791998921003LL);
    T_EQ_INT((int)inv->event.len, (int)strlen(CLOUDFRONT_SIGNUP_EVENT));
    T_CHECK(inv->event.len == strlen(CLOUDFRONT_SIGNUP_EVENT) &&
            memcmp(inv->event.p, CLOUDFRONT_SIGNUP_EVENT, inv->event.len) == 0);
    cb_event ev;
    T_EQ_INT(cb_event_decode((const char *)inv->event.p, inv->event.len, &ev), CB_OK);
    T_EQ_INT(ev.kind, CB_EVENT_HTTP);
    T_EQ_STR(ev.http.method, "POST");
    T_EQ_STR(ev.http.path, "/auth/v1/signup");
    cb_event_free(&ev);
}

// The response that broke production: the event in chunks, as Lambda sent it.
static void next_chunked(void) {
    const char *e = CLOUDFRONT_SIGNUP_EVENT;
    size_t n = strlen(e), half = n / 2;
    cb_buf r;
    cb_buf_init(&r);
    cb_buf_puts(&r, HEAD);
    cb_buf_puts(&r, "Transfer-Encoding: chunked\r\n\r\n");
    cb_buf_printf(&r, "%zx\r\n", half);
    cb_buf_append(&r, e, half);
    cb_buf_printf(&r, "\r\n%zx\r\n", n - half);
    cb_buf_append(&r, e + half, n - half);
    cb_buf_puts(&r, "\r\n0\r\n\r\n");
    cb_invocation inv;
    T_EQ_INT(next_from((const char *)r.p, r.len, &inv), CB_OK);
    check_event(&inv);
    cb_invocation_free(&inv);
    cb_buf_free(&r);
}

// The small-event answer: Content-Length.
static void next_content_length(void) {
    cb_buf r;
    cb_buf_init(&r);
    cb_buf_puts(&r, HEAD);
    cb_buf_printf(&r, "Content-Length: %zu\r\n\r\n", strlen(CLOUDFRONT_SIGNUP_EVENT));
    cb_buf_puts(&r, CLOUDFRONT_SIGNUP_EVENT);
    cb_invocation inv;
    T_EQ_INT(next_from((const char *)r.p, r.len, &inv), CB_OK);
    check_event(&inv);
    cb_invocation_free(&inv);
    cb_buf_free(&r);
}

// A /next that is not a whole response is the runtime going away, not an event.
static void next_truncated(void) {
    cb_buf r;
    cb_buf_init(&r);
    cb_buf_puts(&r, HEAD);
    cb_buf_puts(&r, "Transfer-Encoding: chunked\r\n\r\n10\r\nshort");
    cb_invocation inv;
    T_EQ_INT(next_from((const char *)r.p, r.len, &inv), CB_E_IO);
    cb_invocation_free(&inv);
    cb_buf_free(&r);
}

int main(void) {
    T_RUN(next_chunked);
    T_RUN(next_content_length);
    T_RUN(next_truncated);
    return T_DONE();
}
