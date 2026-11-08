// files.c - answering with a file (files.h).
#include "files.h"

#include "router.h"
#include "cbase/app.h"
#include "util/err.h"
#include "util/str.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

// The platform's table: what a web app's shell and its media are served as.
static const cb_mime PLATFORM_MIME[] = {
    { "html", "text/html; charset=utf-8" },
    { "htm", "text/html; charset=utf-8" },
    { "css", "text/css; charset=utf-8" },
    { "js", "text/javascript; charset=utf-8" },
    { "mjs", "text/javascript; charset=utf-8" },
    { "wasm", "application/wasm" },
    { "svg", "image/svg+xml" },
    { "png", "image/png" },
    { "jpg", "image/jpeg" },
    { "jpeg", "image/jpeg" },
    { "gif", "image/gif" },
    { "webp", "image/webp" },
    { "ico", "image/x-icon" },
    { "txt", "text/plain; charset=utf-8" },
    { "json", "application/json" },
    { "map", "application/json" },
    { "woff", "font/woff" },
    { "woff2", "font/woff2" },
    { "mp3", "audio/mpeg" },
    { "wav", "audio/wav" },
    { "ogg", "audio/ogg" },
    { "m4a", "audio/mp4" },
};

static const char *lookup(const cb_mime *t, int n, const char *ext) {
    for (int i = 0; t && i < n; i++)
        if (t[i].ext && t[i].type && strcasecmp(t[i].ext, ext) == 0) return t[i].type;
    return NULL;
}

const char *cb_mime_type(const cb_app *app, const char *path) {
    const char *slash = strrchr(path, '/');
    const char *dot = strrchr(slash ? slash + 1 : path, '.');
    if (!dot || !dot[1]) return "application/octet-stream";
    const char *t = app ? lookup(app->mime_types, app->n_mime_types, dot + 1) : NULL;
    if (!t) t = lookup(PLATFORM_MIME, (int)(sizeof PLATFORM_MIME / sizeof PLATFORM_MIME[0]), dot + 1);
    return t ? t : "application/octet-stream";
}

// One segment's shape: not empty, not "." or "..", no '/', '\' or control byte.
static bool name_ok(const char *s, size_t n) {
    if (n == 0 || (n == 1 && s[0] == '.') || (n == 2 && s[0] == '.' && s[1] == '.')) return false;
    for (size_t i = 0; i < n; i++) {
        const unsigned char c = (unsigned char)s[i];
        if (c == '/' || c == '\\' || c < 0x20 || c == 0x7f) return false;
    }
    return true;
}

// `joined` resolved, and held inside base's real path: the real path into out.
static int contain(const char *base, const char *joined, char *out, size_t cap) {
    char *rb = realpath(base, NULL);
    if (!rb) return errno == ENOMEM ? CB_E_NOMEM : CB_E_NOT_FOUND;
    char *rj = realpath(joined, NULL);
    if (!rj) {
        free(rb);
        return errno == ENOMEM ? CB_E_NOMEM : errno == EACCES ? CB_E_FORBIDDEN : CB_E_NOT_FOUND;
    }
    const size_t bl = strlen(rb);
    // Inside: base's real path, then a '/' (base "/" itself holds everything).
    const bool inside = strncmp(rj, rb, bl) == 0 && (rj[bl] == '/' || (bl == 1 && rb[0] == '/'));
    int rc = !inside ? CB_E_FORBIDDEN : cb_strlcpy(out, rj, cap) < cap ? CB_OK : CB_E_FULL;
    free(rb);
    free(rj);
    return rc;
}

int cb_path_join(const char *base, const char *name, char *out, size_t cap) {
    if (!base || !name || !name_ok(name, strlen(name))) return CB_E_INVALID;
    char j[PATH_MAX];
    if (cb_strlcpy(j, base, sizeof j) >= sizeof j || cb_strlcpy(j + strlen(j), "/", sizeof j - strlen(j)) >= sizeof j - strlen(j) ||
        cb_strlcpy(j + strlen(j), name, sizeof j - strlen(j)) >= sizeof j - strlen(j))
        return CB_E_FULL;
    return contain(base, j, out, cap);
}

static int meth(const cb_http_req *req) { return req->meth ? req->meth : cb_http_method_id(req->method); }

int cb_send_file(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res, const char *path, const char *content_type,
                 const char *cache_control) {
    // O_NONBLOCK: open() on a FIFO with no writer (or on some devices) waits for one, which would
    // hold this request's worker forever before the regular-file check below could refuse it. On a
    // regular file it changes nothing: a read of one never answers EAGAIN, so the pread below and
    // serve's (net/server.c) read it as they would without the flag.
    // The last component is followed if it is a link: cb_path_join and cb_serve_dir hand over the
    // resolved real path (a link inside the directory is served, one out of it refused there), and
    // a path the caller built itself is the caller's to trust (files.h).
    const int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        const int st = errno == EACCES ? 403 : errno == ENOENT || errno == ENOTDIR ? 404 : 500;
        cb_router_error(res, st, st == 404 ? "not found" : st == 403 ? "forbidden" : "could not read the file");
        return st;
    }
    struct stat sb;
    if (fstat(fd, &sb) != 0 || !S_ISREG(sb.st_mode)) {
        close(fd);
        cb_router_error(res, 404, "not found");
        return 404;
    }
    const uint64_t size = (uint64_t)sb.st_size;
    if (req->stream_ok) {
        // serve's HTTP/1.1 writes it from the descriptor; a HEAD needs only its size.
        res->file = true;
        res->file_len = size;
        if (res->head) close(fd);
        else res->file_fd = fd;
    } else {
        cb_buf_reset(&res->body);
        if (!cb_buf_reserve(&res->body, (size_t)size)) {
            close(fd);
            cb_router_error(res, 500, "out of memory");
            return 500;
        }
        size_t got = 0;
        while (got < size) {
            const ssize_t r = pread(fd, res->body.p + got, (size_t)(size - got), (off_t)got);
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) break;
            got += (size_t)r;
        }
        close(fd);
        if (got != size) {
            cb_router_error(res, 500, "could not read the file");
            return 500;
        }
        res->body.len = got;
        res->body.p[got] = 0;
    }
    res->status = 200;
    cb_res_header(res, "Content-Type", content_type ? content_type : cb_mime_type(ctx ? ctx->app : NULL, path));
    if (cache_control) cb_res_header(res, "Cache-Control", cache_control);
    return 200;
}

int cb_serve_dir(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res, const char *dir, const char *rel,
                 const char *cache_control) {
    const int m = meth(req);
    if (m != CB_M_GET && m != CB_M_HEAD) {
        cb_router_not_allowed(res, "GET, HEAD");
        return 405;
    }
    // dir, then each segment of rel decoded and checked, '/' between.
    char j[PATH_MAX], seg[NAME_MAX + 1];
    size_t jl = cb_strlcpy(j, dir, sizeof j);
    bool bad = jl >= sizeof j || !rel || !*rel;
    for (const char *p = rel; !bad && *p;) {
        const char *e = strchr(p, '/');
        const size_t n = e ? (size_t)(e - p) : strlen(p);
        const int dn = cb_url_decode(p, n, seg, sizeof seg, false);
        // Over all dn decoded bytes: a "%00" is a control byte to name_ok, so it is refused too.
        bad = dn < 0 || !name_ok(seg, (size_t)dn) || jl + 1 + (size_t)dn >= sizeof j;
        if (bad) break;
        j[jl++] = '/';
        memcpy(j + jl, seg, (size_t)dn + 1);
        jl += (size_t)dn;
        p = e ? e + 1 : p + n;
        if (e && !*p) bad = true;   // a trailing '/': a directory, never listed
    }
    char real[PATH_MAX];
    const int rc = bad ? CB_E_INVALID : contain(dir, j, real, sizeof real);
    if (rc != CB_OK) {
        const int st = rc == CB_E_FORBIDDEN ? 403 : 404;
        cb_router_error(res, st, st == 403 ? "forbidden" : "not found");
        return st;
    }
    // By the request's own name: a symlink inside dir to "x.bin" still answers as what was asked for.
    return cb_send_file(ctx, req, res, real, cb_mime_type(ctx ? ctx->app : NULL, j), cache_control);
}
