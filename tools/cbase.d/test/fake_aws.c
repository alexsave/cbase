// fake_aws.c - a recorded stand-in for the three AWS APIs ./cbase calls, on loopback, for
// tools/cbase.d/test/cbase_test.sh. Test-only: never part of a product, never reached by anything
// but a test's AWS CLI through AWS_ENDPOINT_URL=http://127.0.0.1:<port>.
//
//   fake_aws --port-file F --log L --account 111122223333 [--bucket B]... [--resource 'KEY VALUE TYPE ARN']...
//
// What it answers, each as the real service answers it (the bodies are recorded from AWS's
// documented responses, trimmed to what the CLI reads):
//   * S3, path-style (/bucket/key): GetObject (200 with an ETag, 404 NoSuchKey, 404 NoSuchBucket)
//     and PutObject with its preconditions: If-Match on a missing key is 404 NoSuchKey, on another
//     ETag 412 PreconditionFailed; If-None-Match: * on a present key is 412. A body in the
//     aws-chunked encoding (the CLI's trailing checksums) is decoded first. Expect: 100-continue
//     is answered.
//   * STS GetCallerIdentity (the Query protocol, an XML answer naming --account).
//   * Tagging GetResources (JSON 1.1): the --resource lines whose tag key, value and resource type
//     the request's filters name.
//   * Control, for the test alone: POST /_fake/gather?n=N holds the next N GetObject answers until
//     all N have been asked (so N clients read the same ETag before any of them writes: a race
//     made deterministic), or 20 s pass.
//
// Every request is one line of the log: "<METHOD> <path> <status>", and for a PutObject its
// precondition ("if-match" / "if-none-match"). One thread per connection; the state is under one
// lock. Plain C and libc only (tools/ stands apart from the platform: tools/repo_gate_test.sh).
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define MAX_OBJ 64
#define MAX_RES 32
#define MAX_BUCKETS 8
#define BODY_MAX (1 << 20)

typedef struct {
    char   path[1024];  // "/bucket/key"
    char  *body;
    size_t len;
    unsigned version;   // the ETag is "v<version>-<len>"
} object;

typedef struct {
    char key[128], value[128], type[64], arn[256];
} resource;

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static object g_obj[MAX_OBJ];
static int g_nobj;
static unsigned g_version;
static resource g_res[MAX_RES];
static int g_nres;
static char g_buckets[MAX_BUCKETS][64];
static int g_nbuckets;
static char g_account[16] = "111122223333";
static FILE *g_log;
// The gather: the next g_gather_want GetObject answers wait until that many have been computed.
static int g_gather_want, g_gather_seen;
static unsigned g_gather_round;

static void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "fake_aws: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(2);
}

static void logf_(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    pthread_mutex_lock(&g_mu);
    vfprintf(g_log, fmt, ap);
    fputc('\n', g_log);
    fflush(g_log);
    pthread_mutex_unlock(&g_mu);
    va_end(ap);
}

// ---- a connection's bytes ---------------------------------------------------------------------

typedef struct {
    int    fd;
    char  *p;
    size_t len, cap;
} conn;

// Reads more into c; false at EOF or an error.
static bool fill(conn *c) {
    if (c->len + 4096 > c->cap) {
        c->cap = c->cap ? c->cap * 2 : 16384;
        if (c->cap > 4 * BODY_MAX) return false;
        c->p = realloc(c->p, c->cap);
        if (!c->p) die("out of memory");
    }
    ssize_t n = read(c->fd, c->p + c->len, c->cap - c->len - 1);
    if (n <= 0) return false;
    c->len += (size_t)n;
    c->p[c->len] = 0;
    return true;
}

static void consume(conn *c, size_t n) {
    memmove(c->p, c->p + n, c->len - n);
    c->len -= n;
    if (c->p) c->p[c->len] = 0;
}

// A line ending "\r\n" into out (without it); false at EOF.
static bool read_line(conn *c, char *out, size_t cap) {
    char *e;
    while (!c->p || !(e = memmem(c->p, c->len, "\r\n", 2)))
        if (!fill(c)) return false;
    size_t n = (size_t)(e - c->p);
    if (n >= cap) return false;
    memcpy(out, c->p, n);
    out[n] = 0;
    consume(c, n + 2);
    return true;
}

static bool read_n(conn *c, char *out, size_t n) {
    while (c->len < n)
        if (!fill(c)) return false;
    memcpy(out, c->p, n);
    consume(c, n);
    return true;
}

static void write_all(int fd, const char *p, size_t n) {
    while (n) {
        ssize_t k = write(fd, p, n);
        if (k <= 0) return;
        p += k;
        n -= (size_t)k;
    }
}

// Copies src into dst[cap]; false (dst untouched) when it does not fit. memcpy, not snprintf, so a
// compiler's truncation warnings have nothing to guess at.
static bool copy(char *dst, size_t cap, const char *src) {
    size_t n = strlen(src);
    if (n >= cap) return false;
    memcpy(dst, src, n + 1);
    return true;
}

// ---- requests -----------------------------------------------------------------------------------

typedef struct {
    char   method[16], target[1024], path[1024], query[1024];
    char   hdr[64][2][512];
    int    nhdr;
    char  *body;
    size_t len;
} request;

static const char *header(const request *r, const char *name) {
    for (int i = 0; i < r->nhdr; i++)
        if (strcasecmp(r->hdr[i][0], name) == 0) return r->hdr[i][1];
    return NULL;
}

// Decodes the chunk format in place (HTTP's chunked and S3's aws-chunked share it: a hex size,
// optional ";extensions", CRLF, the data, CRLF; a zero size ends it and trailers follow).
static size_t unchunk(char *b, size_t n) {
    size_t in = 0, out = 0;
    while (in < n) {
        char *e = memmem(b + in, n - in, "\r\n", 2);
        if (!e) break;
        size_t sz = strtoul(b + in, NULL, 16);   // test-only: the fake trusts its client's framing
        in = (size_t)(e - b) + 2;
        if (sz == 0 || in + sz > n) break;
        memmove(b + out, b + in, sz);
        out += sz;
        in += sz + 2;
    }
    return out;
}

static bool read_request(conn *c, request *r) {
    memset(r, 0, sizeof *r);
    char line[2048];
    do {
        if (!read_line(c, line, sizeof line)) return false;
    } while (!line[0]);
    if (sscanf(line, "%15s %1023s", r->method, r->target) != 2) return false;
    for (;;) {
        if (!read_line(c, line, sizeof line)) return false;
        if (!line[0]) break;
        char *colon = strchr(line, ':');
        if (!colon || r->nhdr == 64) continue;
        *colon = 0;
        char *v = colon + 1;
        while (*v == ' ') v++;
        if (copy(r->hdr[r->nhdr][0], sizeof r->hdr[0][0], line) && copy(r->hdr[r->nhdr][1], sizeof r->hdr[0][1], v))
            r->nhdr++;
    }
    char *q = strchr(r->target, '?');
    snprintf(r->path, sizeof r->path, "%.*s", q ? (int)(q - r->target) : (int)strlen(r->target), r->target);
    snprintf(r->query, sizeof r->query, "%s", q ? q + 1 : "");
    const char *expect = header(r, "Expect");
    if (expect && strcasecmp(expect, "100-continue") == 0) write_all(c->fd, "HTTP/1.1 100 Continue\r\n\r\n", 25);
    const char *te = header(r, "Transfer-Encoding");
    const char *cl = header(r, "Content-Length");
    if (te && strcasestr(te, "chunked")) {
        size_t cap = 0;
        for (;;) {
            if (!read_line(c, line, sizeof line)) return false;
            size_t sz = strtoul(line, NULL, 16);
            if (sz == 0) {
                while (read_line(c, line, sizeof line) && line[0]) {}
                break;
            }
            if (r->len + sz > BODY_MAX) return false;
            if (r->len + sz + 1 > cap) {
                cap = (r->len + sz + 1) * 2;
                r->body = realloc(r->body, cap);
                if (!r->body) die("out of memory");
            }
            if (!read_n(c, r->body + r->len, sz)) return false;
            r->len += sz;
            if (!read_line(c, line, sizeof line)) return false;
        }
    } else if (cl) {
        size_t n = strtoul(cl, NULL, 10);
        if (n > BODY_MAX) return false;
        r->body = malloc(n + 1);
        if (!r->body) die("out of memory");
        if (!read_n(c, r->body, n)) return false;
        r->len = n;
    }
    if (!r->body) r->body = calloc(1, 1);
    else r->body = realloc(r->body, r->len + 1);
    r->body[r->len] = 0;
    const char *ce = header(r, "Content-Encoding");
    if (ce && strstr(ce, "aws-chunked")) {
        r->len = unchunk(r->body, r->len);
        r->body[r->len] = 0;
    }
    return true;
}

static void respond(int fd, int status, const char *reason, const char *ctype, const char *extra, const char *body,
                    size_t n) {
    char head[1024];
    int k = snprintf(head, sizeof head,
                     "HTTP/1.1 %d %s\r\nx-amz-request-id: FAKE0000\r\nContent-Type: %s\r\nContent-Length: %zu\r\n%s\r\n",
                     status, reason, ctype, n, extra ? extra : "");
    write_all(fd, head, (size_t)k);
    write_all(fd, body, n);
}

static void s3_error(int fd, int status, const char *reason, const char *code, const char *message) {
    char b[512];
    int n = snprintf(b, sizeof b,
                     "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<Error><Code>%s</Code><Message>%s</Message>"
                     "<RequestId>FAKE0000</RequestId></Error>",
                     code, message);
    respond(fd, status, reason, "application/xml", NULL, b, (size_t)n);
}

static object *find(const char *path) {
    for (int i = 0; i < g_nobj; i++)
        if (strcmp(g_obj[i].path, path) == 0) return &g_obj[i];
    return NULL;
}

static bool bucket_known(const char *path) {
    const char *b = path + 1, *slash = strchr(b, '/');
    size_t n = slash ? (size_t)(slash - b) : strlen(b);
    for (int i = 0; i < g_nbuckets; i++)
        if (strlen(g_buckets[i]) == n && memcmp(g_buckets[i], b, n) == 0) return true;
    return false;
}

static void etag_of(const object *o, char *out, size_t cap) { snprintf(out, cap, "\"v%u-%zu\"", o->version, o->len); }

static void s3_get(int fd, const request *r) {
    pthread_mutex_lock(&g_mu);
    int status;
    char etag[64] = "", *body = NULL;
    size_t n = 0;
    object *o = find(r->path);
    if (!bucket_known(r->path)) status = 4041;
    else if (!o) status = 404;
    else {
        status = 200;
        etag_of(o, etag, sizeof etag);
        body = malloc(o->len + 1);
        if (!body) die("out of memory");
        memcpy(body, o->body, o->len);
        n = o->len;
    }
    // The gather: the answer is fixed now, and written once every gathered reader has its own.
    if (g_gather_want > 0) {
        unsigned round = g_gather_round;
        g_gather_seen++;
        if (g_gather_seen >= g_gather_want) {
            g_gather_want = g_gather_seen = 0;
            g_gather_round++;
            pthread_cond_broadcast(&g_cv);
        } else {
            struct timespec until;
            clock_gettime(CLOCK_REALTIME, &until);
            until.tv_sec += 20;
            while (g_gather_round == round)
                if (pthread_cond_timedwait(&g_cv, &g_mu, &until) == ETIMEDOUT) {
                    fprintf(g_log, "GATHER timed out\n");
                    g_gather_want = g_gather_seen = 0;
                    g_gather_round++;
                    break;
                }
        }
    }
    pthread_mutex_unlock(&g_mu);
    if (status == 4041) {
        logf_("GET %s 404 NoSuchBucket", r->path);
        s3_error(fd, 404, "Not Found", "NoSuchBucket", "The specified bucket does not exist");
    } else if (status == 404) {
        logf_("GET %s 404", r->path);
        s3_error(fd, 404, "Not Found", "NoSuchKey", "The specified key does not exist.");
    } else {
        logf_("GET %s 200", r->path);
        char extra[256];
        snprintf(extra, sizeof extra, "ETag: %s\r\nLast-Modified: Mon, 09 Nov 2026 12:00:00 GMT\r\nAccept-Ranges: bytes\r\n", etag);
        respond(fd, 200, "OK", "text/plain", extra, body, n);
    }
    free(body);
}

static void s3_put(int fd, const request *r) {
    const char *im = header(r, "If-Match"), *inm = header(r, "If-None-Match");
    const char *cond = im ? "if-match" : inm ? "if-none-match" : "unconditional";
    pthread_mutex_lock(&g_mu);
    object *o = find(r->path);
    int status = 200;
    char etag[64];
    if (!bucket_known(r->path)) status = 4041;
    else if (im && !o) status = 404;
    else if (im && (etag_of(o, etag, sizeof etag), strcmp(im, etag) != 0)) status = 412;
    else if (inm && strcmp(inm, "*") == 0 && o) status = 412;
    else {
        if (!o) {
            if (g_nobj == MAX_OBJ) die("too many objects");
            o = &g_obj[g_nobj++];
            if (!copy(o->path, sizeof o->path, r->path)) die("a key too long for the recording");
        }
        free(o->body);
        o->body = malloc(r->len + 1);
        if (!o->body) die("out of memory");
        memcpy(o->body, r->body, r->len);
        o->len = r->len;
        o->version = ++g_version;
        etag_of(o, etag, sizeof etag);
    }
    pthread_mutex_unlock(&g_mu);
    logf_("PUT %s %d %s", r->path, status == 4041 ? 404 : status, cond);
    if (status == 4041) s3_error(fd, 404, "Not Found", "NoSuchBucket", "The specified bucket does not exist");
    else if (status == 404) s3_error(fd, 404, "Not Found", "NoSuchKey", "The specified key does not exist.");
    else if (status == 412)
        s3_error(fd, 412, "Precondition Failed", "PreconditionFailed",
                 "At least one of the pre-conditions you specified did not hold");
    else {
        char extra[128];
        snprintf(extra, sizeof extra, "ETag: %s\r\n", etag);
        respond(fd, 200, "OK", "text/plain", extra, "", 0);
    }
}

static void sts(int fd, const request *r) {
    if (!strstr(r->body, "Action=GetCallerIdentity")) {
        logf_("POST STS %.60s 400", r->body);
        respond(fd, 400, "Bad Request", "text/xml", NULL, "", 0);
        return;
    }
    char b[1024];
    int n = snprintf(b, sizeof b,
                     "<GetCallerIdentityResponse xmlns=\"https://sts.amazonaws.com/doc/2011-06-15/\">\n"
                     "  <GetCallerIdentityResult>\n"
                     "    <Arn>arn:aws:iam::%s:user/cbase-test</Arn>\n"
                     "    <UserId>AIDACBASETESTUSER</UserId>\n"
                     "    <Account>%s</Account>\n"
                     "  </GetCallerIdentityResult>\n"
                     "  <ResponseMetadata>\n    <RequestId>01234567-89ab-cdef-0123-456789abcdef</RequestId>\n"
                     "  </ResponseMetadata>\n"
                     "</GetCallerIdentityResponse>\n",
                     g_account, g_account);
    logf_("POST STS GetCallerIdentity 200");
    respond(fd, 200, "OK", "text/xml", NULL, b, (size_t)n);
}

// The request's filters, read the plain way a recorded fake may: the body with its blanks dropped
// holds "Key":"<k>","Values":["<v>"] and "ResourceTypeFilters":["<t>"].
static void tagging(int fd, const request *r) {
    char flat[8192];
    size_t k = 0;
    for (size_t i = 0; i < r->len && k + 1 < sizeof flat; i++)
        if (r->body[i] != ' ' && r->body[i] != '\n') flat[k++] = r->body[i];
    flat[k] = 0;
    char out[8192];
    size_t n = (size_t)snprintf(out, sizeof out, "{\"PaginationToken\":\"\",\"ResourceTagMappingList\":[");
    int hits = 0;
    for (int i = 0; i < g_nres; i++) {
        char want_tag[1024], want_type[1024];
        snprintf(want_tag, sizeof want_tag, "\"Key\":\"%s\",\"Values\":[\"%s\"]", g_res[i].key, g_res[i].value);
        snprintf(want_type, sizeof want_type, "\"ResourceTypeFilters\":[\"%s\"]", g_res[i].type);
        if (!strstr(flat, want_tag) || !strstr(flat, want_type)) continue;
        n += (size_t)snprintf(out + n, sizeof out - n, "%s{\"ResourceARN\":\"%s\",\"Tags\":[{\"Key\":\"%s\",\"Value\":\"%s\"}]}",
                              hits++ ? "," : "", g_res[i].arn, g_res[i].key, g_res[i].value);
    }
    n += (size_t)snprintf(out + n, sizeof out - n, "]}");
    logf_("POST Tagging GetResources 200 %d", hits);
    respond(fd, 200, "OK", "application/x-amz-json-1.1", NULL, out, n);
}

static void control(int fd, const request *r) {
    int want = 0;
    if (sscanf(r->query, "n=%d", &want) != 1 || want < 1) {
        respond(fd, 400, "Bad Request", "text/plain", NULL, "n=", 2);
        return;
    }
    pthread_mutex_lock(&g_mu);
    g_gather_want = want;
    g_gather_seen = 0;
    pthread_mutex_unlock(&g_mu);
    respond(fd, 200, "OK", "text/plain", NULL, "ok\n", 3);
}

static void handle(int fd, const request *r) {
    const char *target = header(r, "X-Amz-Target");
    const char *auth = header(r, "Authorization");
    if (strncmp(r->path, "/_fake/", 7) == 0) {
        control(fd, r);
    } else if (!auth || !strstr(auth, "AWS4-HMAC-SHA256 Credential=AKIACBASEFAKE0000000/")) {
        // Signed with the test's dummy credentials, or refused: a call that found other credentials
        // (a developer's real ones) must never get an answer.
        logf_("%s %s 403 unsigned", r->method, r->path);
        s3_error(fd, 403, "Forbidden", "InvalidAccessKeyId", "The AWS Access Key Id you provided does not exist in our records.");
    } else if (target && strcmp(target, "ResourceGroupsTaggingAPI_20170126.GetResources") == 0) {
        tagging(fd, r);
    } else if (strcmp(r->method, "POST") == 0 && strcmp(r->path, "/") == 0) {
        sts(fd, r);
    } else if (strcmp(r->method, "GET") == 0) {
        s3_get(fd, r);
    } else if (strcmp(r->method, "PUT") == 0) {
        s3_put(fd, r);
    } else {
        logf_("%s %s 405", r->method, r->path);
        s3_error(fd, 405, "Method Not Allowed", "MethodNotAllowed", "not in the recording");
    }
}

static void *serve(void *arg) {
    conn c = { .fd = (int)(intptr_t)arg };
    request r;
    while (read_request(&c, &r)) {
        handle(c.fd, &r);
        free(r.body);
    }
    close(c.fd);
    free(c.p);
    return NULL;
}

int main(int argc, char **argv) {
    const char *port_file = NULL, *log_path = NULL;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!v) die("%s wants a value", a);
        i++;
        if (strcmp(a, "--port-file") == 0) port_file = v;
        else if (strcmp(a, "--log") == 0) log_path = v;
        else if (strcmp(a, "--account") == 0) snprintf(g_account, sizeof g_account, "%s", v);
        else if (strcmp(a, "--bucket") == 0) {
            if (g_nbuckets == MAX_BUCKETS) die("too many buckets");
            snprintf(g_buckets[g_nbuckets++], sizeof g_buckets[0], "%s", v);
        } else if (strcmp(a, "--resource") == 0) {
            if (g_nres == MAX_RES) die("too many resources");
            resource *x = &g_res[g_nres++];
            if (sscanf(v, "%127s %127s %63s %255s", x->key, x->value, x->type, x->arn) != 4)
                die("--resource is 'KEY VALUE TYPE ARN'");
        } else die("unknown argument %s", a);
    }
    if (!port_file || !log_path) die("usage: fake_aws --port-file F --log L [--account A] [--bucket B]... [--resource 'K V T ARN']...");
    g_log = fopen(log_path, "a");
    if (!g_log) die("cannot open %s: %s", log_path, strerror(errno));
    signal(SIGPIPE, SIG_IGN);
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    socklen_t sl = sizeof sa;
    if (lfd < 0 || bind(lfd, (struct sockaddr *)&sa, sizeof sa) != 0 || listen(lfd, 64) != 0 ||
        getsockname(lfd, (struct sockaddr *)&sa, &sl) != 0)
        die("cannot listen: %s", strerror(errno));
    char tmp[600];
    snprintf(tmp, sizeof tmp, "%s.new", port_file);
    FILE *pf = fopen(tmp, "w");
    if (!pf || fprintf(pf, "%d\n", ntohs(sa.sin_port)) < 0 || fclose(pf) != 0 || rename(tmp, port_file) != 0)
        die("cannot write %s", port_file);
    for (;;) {
        int fd = accept(lfd, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR) continue;
            die("accept: %s", strerror(errno));
        }
        struct timeval tv = { .tv_sec = 60 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        pthread_t th;
        if (pthread_create(&th, NULL, serve, (void *)(intptr_t)fd) != 0) die("pthread_create");
        pthread_detach(th);
    }
}
