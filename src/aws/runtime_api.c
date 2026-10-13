// runtime_api.c - the Lambda Runtime API client (runtime_api.h).
//
// One short-lived plain-HTTP connection per call to the loopback endpoint.
// GET .../next blocks for as long as Lambda keeps the container frozen, so
// there is no read timeout on it.
#include "runtime_api.h"

#include "util/err.h"
#include "util/json.h"
#include "util/log.h"

#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

int cb_runtime_init(char *host, size_t cap, int *port) {
    const char *api = getenv("AWS_LAMBDA_RUNTIME_API");
    if (!api || !*api) return CB_E_INVALID;
    const char *colon = strrchr(api, ':');
    if (!colon) { *port = 80; return cb_strlcpy(host, api, cap) < cap ? CB_OK : CB_E_INVALID; }
    if (!cb_strncpy0(host, cap, api, (size_t)(colon - api))) return CB_E_INVALID;
    *port = atoi(colon + 1);
    return *port > 0 ? CB_OK : CB_E_INVALID;
}

static int dial(const char *host, int port) {
    struct addrinfo hints, *ai = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_socktype = SOCK_STREAM;
    char ps[16];
    snprintf(ps, sizeof ps, "%d", port);
    if (getaddrinfo(host, ps, &hints, &ai) != 0) return -1;
    int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd >= 0 && connect(fd, ai->ai_addr, ai->ai_addrlen) != 0) { close(fd); fd = -1; }
    freeaddrinfo(ai);
    return fd;
}

static int send_all(int fd, const void *p, size_t n) {
    const char *b = p;
    while (n) {
        ssize_t w = send(fd, b, n, 0);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        b += w;
        n -= (size_t)w;
    }
    return 0;
}

// The value of header `name` in the head text, copied into out ("" if absent).
static void head_get(const char *head, const char *name, char *out, size_t cap) {
    out[0] = 0;
    size_t nl = strlen(name);
    for (const char *l = strstr(head, "\r\n"); l && l[2]; l = strstr(l + 2, "\r\n")) {
        const char *s = l + 2;
        if (strncasecmp(s, name, nl) == 0 && s[nl] == ':') {
            s += nl + 1;
            while (*s == ' ') s++;
            const char *e = strstr(s, "\r\n");
            cb_strncpy0(out, cap, s, e ? (size_t)(e - s) : strlen(s));
            return;
        }
    }
}

// One request; the response head into `head`, body into `body`. Returns the
// status, or CB_E_IO.
static int roundtrip(const char *host, int port, const char *method, const char *path, const char *extra,
                     const void *body, size_t len, cb_buf *head, cb_buf *resp) {
    int fd = dial(host, port);
    if (fd < 0) return CB_E_IO;
    cb_buf req;
    cb_buf_init(&req);
    cb_buf_printf(&req, "%s %s HTTP/1.1\r\nHost: %s:%d\r\nConnection: close\r\n%sContent-Length: %zu\r\n\r\n",
                  method, path, host, port, extra ? extra : "", len);
    cb_buf_append(&req, body, len);
    int rc = send_all(fd, req.p, req.len);
    cb_buf_free(&req);
    if (rc) { close(fd); return CB_E_IO; }
    cb_buf in;
    cb_buf_init(&in);
    for (;;) {
        if (!cb_buf_reserve(&in, 65536)) { close(fd); cb_buf_free(&in); return CB_E_NOMEM; }
        ssize_t r = recv(fd, in.p + in.len, in.cap - in.len - 1, 0);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        in.len += (size_t)r;
        in.p[in.len] = 0;
    }
    close(fd);
    const char *he = strstr(cb_buf_cstr(&in), "\r\n\r\n");
    int status = 0;
    if (!he || sscanf(cb_buf_cstr(&in), "HTTP/1.%*d %d", &status) != 1) { cb_buf_free(&in); return CB_E_IO; }
    size_t hl = (size_t)(he - (const char *)in.p) + 4;
    cb_buf_append(head, in.p, hl);
    char cl[32];
    head_get(cb_buf_cstr(head), "content-length", cl, sizeof cl);
    size_t blen = in.len - hl;
    if (cl[0] && (size_t)strtoull(cl, NULL, 10) < blen) blen = (size_t)strtoull(cl, NULL, 10);
    cb_buf_append(resp, in.p + hl, blen);
    cb_buf_free(&in);
    return status;
}

int cb_runtime_next(const char *host, int port, cb_invocation *out) {
    memset(out, 0, sizeof *out);
    cb_buf_init(&out->event);
    cb_buf head;
    cb_buf_init(&head);
    int st = roundtrip(host, port, "GET", "/2018-06-01/runtime/invocation/next", NULL, NULL, 0, &head, &out->event);
    if (st != 200) {
        CB_ERROR("runtime", "invocation/next answered %d", st);
        cb_buf_free(&head);
        return CB_E_IO;
    }
    const char *h = cb_buf_cstr(&head);
    char dl[32];
    head_get(h, "lambda-runtime-aws-request-id", out->request_id, sizeof out->request_id);
    head_get(h, "lambda-runtime-deadline-ms", dl, sizeof dl);
    head_get(h, "lambda-runtime-invoked-function-arn", out->function_arn, sizeof out->function_arn);
    head_get(h, "lambda-runtime-trace-id", out->trace_id, sizeof out->trace_id);
    out->deadline_ms = strtoll(dl, NULL, 10);
    cb_buf_free(&head);
    return out->request_id[0] ? CB_OK : CB_E_IO;
}

void cb_invocation_free(cb_invocation *inv) { cb_buf_free(&inv->event); }

static int post(const char *host, int port, const char *path, const char *extra, const void *body, size_t len) {
    cb_buf head, resp;
    cb_buf_init(&head);
    cb_buf_init(&resp);
    int st = roundtrip(host, port, "POST", path, extra, body, len, &head, &resp);
    cb_buf_free(&head);
    cb_buf_free(&resp);
    if (st != 202) {
        CB_ERROR("runtime", "POST %s answered %d", path, st);
        return CB_E_IO;
    }
    return CB_OK;
}

int cb_runtime_respond(const char *host, int port, const char *request_id, const void *body, size_t len) {
    char path[256];
    snprintf(path, sizeof path, "/2018-06-01/runtime/invocation/%s/response", request_id);
    return post(host, port, path, NULL, body, len);
}

// The error body Lambda documents: {"errorMessage": .., "errorType": ..}. JSON
// because that is Lambda's contract (DESIGN.md "Where JSON is tolerated").
static void error_body(cb_buf *b, const char *type, const char *message) {
    cb_jw w;
    cb_jw_init(&w, b);
    cb_jw_obj(&w);
    cb_jw_kstr(&w, "errorMessage", message);
    cb_jw_kstr(&w, "errorType", type);
    cb_jw_end(&w);
}

int cb_runtime_error(const char *host, int port, const char *request_id, const char *type, const char *message) {
    char path[256];
    snprintf(path, sizeof path, "/2018-06-01/runtime/invocation/%s/error", request_id);
    cb_buf b;
    cb_buf_init(&b);
    error_body(&b, type, message);
    int rc = post(host, port, path, "Lambda-Runtime-Function-Error-Type: Unhandled\r\n", b.p, b.len);
    cb_buf_free(&b);
    return rc;
}

int cb_runtime_init_error(const char *host, int port, const char *type, const char *message) {
    cb_buf b;
    cb_buf_init(&b);
    error_body(&b, type, message);
    int rc = post(host, port, "/2018-06-01/runtime/init/error", "Lambda-Runtime-Function-Error-Type: Unhandled\r\n", b.p, b.len);
    cb_buf_free(&b);
    return rc;
}
