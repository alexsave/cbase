// smoke_s3_post.c - a browser's upload to the object store, end to end against the fake S3.
//
//   build/test/smoke_s3_post build/test/fake_aws
//
// The fake (tools/cbase.d/test/fake_aws.c, built by cbase.mk's rule for build/test/fake_aws, which is
// this smoke's prerequisite: this program builds nothing) is started on loopback with --secret,
// --cors-origin and --data. The platform's side is the real one: cb_media_configure opens the S3 blob
// with AWS_ENDPOINT_URL_S3 at the fake and dummy keys, and cb_media_post makes the form. The browser's
// side is written here: a multipart body, POSTed with no signature of its own through
// cb_http_plain_request, and the object read back with no signature (as the CDN's /m/* is read).
//
// The fake checks the form as S3 does, with its own SHA-256 and HMAC (not the server's libcrypto), so
// a form the server signs wrongly, or a policy the fake reads differently, fails here:
//   - the upload is taken (204), and read back with its bytes, its Content-Type and the _media headers;
//   - each of these is refused with S3's status and code: another Content-Type, a file over the
//     range, an empty file, an expired form, one byte of the policy changed, the key changed, a
//     signature made with another secret, a field the policy does not cover;
//   - CORS: a listed origin is echoed on the POST and the GET and its preflight is 200, another gets
//     no header and its preflight 403;
//   - the fake stopped and started again on the same --data directory still answers the object.
#include "t.h"

#include "aws/s3_post.h"
#include "aws/sigv4.h"
#include "config.h"
#include "lib_proc.h"
#include "media.h"
#include "net/https.h"
#include "util/err.h"
#include "util/log.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define SECRET "smoke-dummy-secret-key-0123456789"
#define ORIGIN "http://127.0.0.1:5173"
#define OTHER  "http://127.0.0.1:6666"
#define BUCKET "smokebkt"
#define NOW_S  1800000000LL           // the fake's clock (the now knob) and the form's, both fixed
#define NOW_MS (NOW_S * 1000)
#define MAX    1000                   // the size range's top, so "over it" is a small body

static const char *g_bin;
static tp_child g_fake;
static int g_port;
static char g_dir[256];     // port file, log and --data: outlives a fake

static void rm_dir(const char *d) {
    char cmd[300];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", d);
    if (system(cmd) != 0) fprintf(stderr, "could not remove %s\n", d);
}

static bool start_fake(const char *bin) {
    char portf[300], logf[300], data[300];
    snprintf(portf, sizeof portf, "%s/port", g_dir);
    snprintf(logf, sizeof logf, "%s/log", g_dir);
    snprintf(data, sizeof data, "%s/data", g_dir);
    unlink(portf);
    char *argv[] = { (char *)bin, "--port-file", portf, "--log", logf, "--bucket", BUCKET ":us-west-1", "--secret", SECRET,
                     "--cors-origin", ORIGIN, "--data", data, NULL };
    if (tp_start(&g_fake, argv, (char *[]){ NULL }) != 0) return false;
    for (int i = 0; i < 200; i++) {
        FILE *f = fopen(portf, "r");
        if (f) {
            int p = 0;
            if (fscanf(f, "%d", &p) == 1 && p > 0) g_port = p;
            fclose(f);
            if (g_port) return true;
        }
        usleep(25000);
    }
    return false;
}

static void stop_fake(void) {
    tp_stop(&g_fake, SIGTERM, 3000);
    g_port = 0;
}

static int http(const char *method, const char *path, const char *hdrs, const void *body, size_t n, cb_https_response *res) {
    memset(res, 0, sizeof *res);
    return cb_http_plain_request(CB_HTTPS_OTHER, "127.0.0.1", g_port, method, path, hdrs, body, n, 10000, res);
}

// A header of the response head, case-insensitively: copied into out ("" when absent).
static bool head_get(const cb_https_response *r, const char *name, char *out, size_t cap) {
    out[0] = 0;
    char pat[64];
    snprintf(pat, sizeof pat, "\r\n%s:", name);
    const char *p = cb_buf_cstr(&r->headers), *end = p + r->headers.len;
    for (; p && p < end; p++) {
        if (strncasecmp(p, pat, strlen(pat)) != 0) continue;
        p += strlen(pat);
        while (*p == ' ') p++;
        size_t n = strcspn(p, "\r\n");
        if (n >= cap) n = cap - 1;
        memcpy(out, p, n);
        out[n] = 0;
        return true;
    }
    return false;
}

static void fake_set(const char *query) {
    cb_https_response res;
    char path[200];
    snprintf(path, sizeof path, "/_fake/set?%s", query);
    T_EQ_INT(http("POST", path, "", "", 0, &res), CB_OK);
    T_EQ_INT(res.status, 200);
    cb_https_response_free(&res);
}

// A multipart/form-data body: the form's fields in order, then the file part, then a part after the
// file (S3 ignores what follows the file).
static char *body_of(const cb_media_form *f, const void *file, size_t n, size_t *len) {
    cb_buf b;
    cb_buf_init(&b);
    for (int i = 0; i < f->n; i++)
        cb_buf_printf(&b, "--XsmokeX\r\nContent-Disposition: form-data; name=\"%s\"\r\n\r\n%s\r\n", f->name[i], f->value[i]);
    cb_buf_puts(&b, "--XsmokeX\r\nContent-Disposition: form-data; name=\"file\"; filename=\"p\"\r\n"
                    "Content-Type: application/octet-stream\r\n\r\n");
    cb_buf_append(&b, file, n);
    cb_buf_puts(&b, "\r\n--XsmokeX\r\nContent-Disposition: form-data; name=\"after\"\r\n\r\nignored\r\n--XsmokeX--\r\n");
    *len = b.len;
    char *out = malloc(b.len + 1);
    memcpy(out, b.p, b.len);
    out[b.len] = 0;
    cb_buf_free(&b);
    return out;
}

// The path of a form's URL (http://127.0.0.1:<port>/<bucket>).
static const char *url_path(const cb_media_form *f) {
    const char *p = strstr(f->url, "://");
    p = p ? strchr(p + 3, '/') : NULL;
    return p ? p : "/";
}

// POSTs the form with this file and Origin ("" none); status in *status, the head and body in res.
static void upload(const cb_media_form *f, const void *file, size_t n, const char *origin, cb_https_response *res) {
    size_t len;
    char *body = body_of(f, file, n, &len);
    char hdrs[300];
    snprintf(hdrs, sizeof hdrs, "Content-Type: multipart/form-data; boundary=XsmokeX\r\n%s%s%s", origin[0] ? "Origin: " : "",
             origin, origin[0] ? "\r\n" : "");
    T_EQ_INT(http("POST", url_path(f), hdrs, body, len, res), CB_OK);
    free(body);
}

static bool body_has(const cb_https_response *r, const char *s) { return r->body.len && strstr(cb_buf_cstr(&r->body), s) != NULL; }

// The form posted and S3's refusal checked: its status and the code (or message text) in its body.
static void refused(const cb_media_form *f, const void *file, size_t n, int status, const char *code, const char *what) {
    cb_https_response res;
    upload(f, file, n, "", &res);
    if (res.status != status || !body_has(&res, code))
        fprintf(stderr, "  case [%s]: got %d %.200s\n", what, res.status, cb_buf_cstr(&res.body));
    T_EQ_INT(res.status, status);
    T_CHECK(body_has(&res, code));
    cb_https_response_free(&res);
}

// The form posted and taken (204): a bound itself is not a refusal.
static void taken(const cb_media_form *f, const void *file, size_t n, const char *what) {
    cb_https_response res;
    upload(f, file, n, "", &res);
    if (res.status != 204) fprintf(stderr, "  case [%s]: got %d %.200s\n", what, res.status, cb_buf_cstr(&res.body));
    T_EQ_INT(res.status, 204);
    cb_https_response_free(&res);
}

static void set_value(cb_media_form *f, const char *name, const char *value) {
    for (int i = 0; i < f->n; i++)
        if (strcmp(f->name[i], name) == 0) {
            free(f->value[i]);
            f->value[i] = strdup(value);
        }
}

static const char *get_value(const cb_media_form *f, const char *name) {
    for (int i = 0; i < f->n; i++)
        if (strcmp(f->name[i], name) == 0) return f->value[i];
    return NULL;
}

static void add_field(cb_media_form *f, const char *name, const char *value) {
    snprintf(f->name[f->n], sizeof f->name[0], "%s", name);
    f->value[f->n++] = strdup(value);
}

static const uint8_t IMG[] = "RIFF....WEBPVP8 pretend image bytes";

static void run(void) {
    char err[256];
    cb_config cfg;
    char endpoint[64];
    snprintf(endpoint, sizeof endpoint, "http://127.0.0.1:%d", g_port);
    setenv("AWS_ENDPOINT_URL_S3", endpoint, 1);
    setenv("AWS_ACCESS_KEY_ID", "AKIACBASEFAKE0000000", 1);
    setenv("AWS_SECRET_ACCESS_KEY", SECRET, 1);
    setenv("AWS_REGION", "us-west-1", 1);
    unsetenv("AWS_SESSION_TOKEN");
    setenv("CBASE_MEDIA_BUCKET", BUCKET, 1);
    setenv("CBASE_MEDIA_REGION", "us-west-1", 1);
    setenv("CBASE_MEDIA_PREFIX", "_media/smoke/m/", 1);
    T_EQ_INT(cb_config_from_env(&cfg, err, sizeof err), CB_OK);
    T_EQ_INT(cb_media_configure(&cfg, err, sizeof err), CB_OK);
    fake_set("now=1800000000");

    // THE UPLOAD: taken, and read back.
    cb_media_form f;
    T_EQ_INT(cb_media_post("image/webp", MAX, NOW_MS, &f), CB_OK);
    char key[200];
    snprintf(key, sizeof key, "%s", get_value(&f, "key"));
    T_CHECK(strncmp(key, "_media/smoke/m/", 15) == 0);
    cb_https_response res;
    upload(&f, IMG, sizeof IMG, ORIGIN, &res);
    T_EQ_INT(res.status, 204);
    char v[300];
    T_CHECK(head_get(&res, "Access-Control-Allow-Origin", v, sizeof v) && strcmp(v, ORIGIN) == 0);   // CORS: listed, on the POST
    T_CHECK(head_get(&res, "Vary", v, sizeof v) && strcmp(v, "Origin") == 0);
    cb_https_response_free(&res);

    char path[300];
    snprintf(path, sizeof path, "/%s/%s", BUCKET, key);
    T_EQ_INT(http("GET", path, "", "", 0, &res), CB_OK);
    T_EQ_INT(res.status, 200);
    T_EQ_MEM(res.body.p, res.body.len, IMG, sizeof IMG);
    T_CHECK(head_get(&res, "Content-Type", v, sizeof v) && strcmp(v, "image/webp") == 0);   // proof (b): GetObject serves the stored type
    T_CHECK(head_get(&res, "Cache-Control", v, sizeof v) && strcmp(v, "public, max-age=31536000, immutable") == 0);
    T_CHECK(head_get(&res, "X-Content-Type-Options", v, sizeof v) && strcmp(v, "nosniff") == 0);
    T_CHECK(head_get(&res, "Content-Security-Policy", v, sizeof v) && strcmp(v, "default-src 'none'; sandbox") == 0);
    T_CHECK(!head_get(&res, "Access-Control-Allow-Origin", v, sizeof v));   // no Origin sent
    cb_https_response_free(&res);

    // CORS on the GET: the listed origin is echoed, another gets nothing.
    T_EQ_INT(http("GET", path, "Origin: " ORIGIN "\r\n", "", 0, &res), CB_OK);
    T_CHECK(head_get(&res, "Access-Control-Allow-Origin", v, sizeof v) && strcmp(v, ORIGIN) == 0);
    cb_https_response_free(&res);
    T_EQ_INT(http("GET", path, "Origin: " OTHER "\r\n", "", 0, &res), CB_OK);
    T_EQ_INT(res.status, 200);
    T_CHECK(!head_get(&res, "Access-Control-Allow-Origin", v, sizeof v));
    cb_https_response_free(&res);

    // CORS preflight.
    const char *pre_ok = "Origin: " ORIGIN "\r\nAccess-Control-Request-Method: POST\r\nAccess-Control-Request-Headers: content-type\r\n";
    T_EQ_INT(http("OPTIONS", url_path(&f), pre_ok, "", 0, &res), CB_OK);
    T_EQ_INT(res.status, 200);
    T_CHECK(head_get(&res, "Access-Control-Allow-Origin", v, sizeof v) && strcmp(v, ORIGIN) == 0);
    T_CHECK(head_get(&res, "Access-Control-Allow-Methods", v, sizeof v) && strcmp(v, "POST, GET, HEAD") == 0);
    cb_https_response_free(&res);
    T_EQ_INT(http("OPTIONS", url_path(&f), "Origin: " OTHER "\r\nAccess-Control-Request-Method: POST\r\n", "", 0, &res), CB_OK);
    T_EQ_INT(res.status, 403);
    T_CHECK(body_has(&res, "CORSResponse"));
    T_CHECK(!head_get(&res, "Access-Control-Allow-Origin", v, sizeof v));
    cb_https_response_free(&res);
    cb_media_form_free(&f);

    // THE REFUSALS, each from a fresh form.
    T_EQ_INT(cb_media_post("image/webp", MAX, NOW_MS, &f), CB_OK);
    set_value(&f, "Content-Type", "image/png");   // another type than the policy's
    refused(&f, IMG, sizeof IMG, 403, "AccessDenied", "another Content-Type");
    cb_media_form_free(&f);

    T_EQ_INT(cb_media_post("image/webp", MAX, NOW_MS, &f), CB_OK);
    char big[MAX + 1];
    memset(big, 'x', sizeof big);
    refused(&f, big, sizeof big, 400, "EntityTooLarge", "a file over the range");
    taken(&f, big, MAX, "a file at the range's top");   // the bound itself is taken
    cb_media_form_free(&f);

    T_EQ_INT(cb_media_post("image/webp", MAX, NOW_MS, &f), CB_OK);
    refused(&f, "", 0, 400, "EntityTooSmall", "an empty file");
    cb_media_form_free(&f);

    T_EQ_INT(cb_media_post("image/webp", MAX, NOW_MS, &f), CB_OK);
    fake_set("now=1800000301");   // 301 s later: the form lives 300
    refused(&f, IMG, sizeof IMG, 403, "Policy expired", "an expired form");
    fake_set("now=1800000299");
    taken(&f, IMG, sizeof IMG, "a form at 299 s");
    fake_set("now=1800000000");
    cb_media_form_free(&f);

    T_EQ_INT(cb_media_post("image/webp", MAX, NOW_MS, &f), CB_OK);
    {
        char pol[8192];
        snprintf(pol, sizeof pol, "%s", get_value(&f, "policy"));
        pol[20] = pol[20] == 'A' ? 'B' : 'A';   // one byte of the policy
        set_value(&f, "policy", pol);
    }
    refused(&f, IMG, sizeof IMG, 403, "SignatureDoesNotMatch", "one byte of the policy changed");
    cb_media_form_free(&f);

    T_EQ_INT(cb_media_post("image/webp", MAX, NOW_MS, &f), CB_OK);
    set_value(&f, "key", "_media/smoke/m/00000000000000000000000000000000.webp");   // another key, under the same prefix
    refused(&f, IMG, sizeof IMG, 403, "AccessDenied", "the key changed");
    set_value(&f, "key", "other/place.webp");
    refused(&f, IMG, sizeof IMG, 403, "AccessDenied", "the key moved out of the folder");
    cb_media_form_free(&f);

    {   // a form signed with another secret: the server's own form maker, other credentials
        cb_aws_creds c;
        memset(&c, 0, sizeof c);
        snprintf(c.access_key, sizeof c.access_key, "AKIACBASEFAKE0000000");
        snprintf(c.secret_key, sizeof c.secret_key, "another-secret-another-secret-0123");
        snprintf(c.region, sizeof c.region, "us-west-1");
        char url[100];
        snprintf(url, sizeof url, "http://127.0.0.1:%d/%s", g_port, BUCKET);
        T_EQ_INT(cb_s3_post_form_make(&c, url, BUCKET, "_media/smoke/m/ffffffffffffffffffffffffffffffff.webp", "image/webp", MAX,
                                      NOW_MS, 300, &f), CB_OK);
        refused(&f, IMG, sizeof IMG, 403, "SignatureDoesNotMatch", "a signature made with another secret");
        cb_media_form_free(&f);
    }

    T_EQ_INT(cb_media_post("image/webp", MAX, NOW_MS, &f), CB_OK);
    add_field(&f, "x-amz-meta-uninvited", "1");   // after the signature: the policy does not name it
    refused(&f, IMG, sizeof IMG, 403, "Extra input fields", "a field the policy does not cover");
    cb_media_form_free(&f);

    // --data: the fake stopped and started again on the same directory still answers the object, with
    // its Content-Type.
    stop_fake();
    T_CHECK(start_fake(g_bin));
    T_EQ_INT(http("GET", path, "", "", 0, &res), CB_OK);
    T_EQ_INT(res.status, 200);
    T_EQ_MEM(res.body.p, res.body.len, IMG, sizeof IMG);
    T_CHECK(head_get(&res, "Content-Type", v, sizeof v) && strcmp(v, "image/webp") == 0);
    T_CHECK(head_get(&res, "Content-Security-Policy", v, sizeof v));
    cb_https_response_free(&res);
    // And an object that was refused is not there.
    snprintf(path, sizeof path, "/%s/other/place.webp", BUCKET);
    T_EQ_INT(http("GET", path, "", "", 0, &res), CB_OK);
    T_CHECK(res.status != 200);
    cb_https_response_free(&res);
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: smoke_s3_post build/test/fake_aws\n");
        return 2;
    }
    g_bin = argv[1];
    cb_log_init(CB_LOG_WARN, 0);
    signal(SIGPIPE, SIG_IGN);
    T_EQ_INT(tp_tempdir(g_dir, sizeof g_dir), 0);
    char data[300];
    snprintf(data, sizeof data, "%s/data", g_dir);
    T_EQ_INT(mkdir(data, 0700), 0);
    if (start_fake(g_bin)) {
        T_RUN(run);
        stop_fake();
    } else {
        T_CHECK(!"the fake started");
    }
    rm_dir(g_dir);
    return T_DONE();
}
