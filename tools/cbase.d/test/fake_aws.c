// fake_aws.c - a recorded stand-in for the AWS APIs ./cbase calls, on loopback, for
// tools/cbase.d/test/cbase_test.sh. Test-only: never part of a product, never reached by anything
// but a test's AWS CLI through AWS_ENDPOINT_URL=http://127.0.0.1:<port>.
//
//   fake_aws --port-file F --log L --account 111122223333 [--bucket B[:REGION]]...
//            [--resource 'KEY VALUE TYPE ARN']... [--cert 'ARN STATUS NAME,NAME...']... [--zone 'ID NAME [private]']...
//
// A request is routed by the service its signature's credential scope names, as AWS's endpoints
// are told apart by host. What it answers, each as the real service answers it (the bodies are
// recorded from AWS's documented responses, trimmed to what the CLI reads):
//   * S3, path-style (/bucket/key): GetObject and HeadObject (200 with an ETag and the stored
//     Content-Type and Cache-Control, 404 NoSuchKey, 404 NoSuchBucket), PutObject with its
//     preconditions (If-Match on a missing key is 404 NoSuchKey, on another ETag 412
//     PreconditionFailed; If-None-Match: * on a present key is 412), DeleteObject,
//     GetBucketLocation and ListObjectsV2 (one page, by prefix). A body in the aws-chunked
//     encoding (the CLI's trailing checksums) is decoded first. Expect: 100-continue is answered.
//   * STS GetCallerIdentity (the Query protocol, an XML answer naming --account).
//   * Tagging GetResources (JSON 1.1): the --resource lines whose tag key, value and resource type
//     the request's filters name.
//   * CloudFormation (Query): DescribeStacks (a vm group's with its OriginIpv6 output), DescribeStackResources (an adopted stack's instance: a
//     --resource line tagged aws:cloudformation:stack-name), Create/Describe/Delete/ExecuteChangeSet, DeleteStack,
//     DetectStackDrift, DescribeStackDriftDetectionStatus, DescribeStackResourceDrifts,
//     DescribeStackEvents. Every wait ends at once (the change set and the stack are done when
//     first asked). A create's change set adds the template's resources; an update's is a Modify of
//     the distribution when the template changed, nothing when it did not, or what /_fake/set
//     scripts.
//   * ACM (JSON 1.1): ListCertificates (by status), DescribeCertificate (with each name's
//     validation record), RequestCertificate (pending; issued once Route 53 holds every one of its
//     validation records).
//   * Route 53 (REST): ListHostedZonesByName, ChangeResourceRecordSets, GetChange (INSYNC).
//   * CloudFront (REST): CreateInvalidation, GetInvalidation (Completed).
//   * S3's bucket calls group-init and group-destroy make: HeadBucket, CreateBucket (409
//     BucketAlreadyOwnedByYou for one that exists), PutPublicAccessBlock, PutBucketVersioning,
//     DeleteBucketTagging (the bucket's group tag dropped from the tagging answers).
//   * EC2 (Query, XML): DescribeInstances (an instance a --resource line or a group stack names:
//     running, its type the instance-type knob, a public DNS name), DescribeInstanceTypes (a small
//     recorded table: t4g.nano 512 MiB arm64 up to t4g.small, t3.nano x86_64),
//     DescribeManagedPrefixLists (CloudFront's IPv4 and IPv6 origin-facing lists, by name),
//     DescribeInstanceTypeOfferings (any type, in us-east-1b and us-east-1a).
//   * SSM (JSON 1.1): GetParameter (the Amazon Linux image), SendCommand (logged with its
//     commands, which the test decodes), GetCommandInvocation (InProgress at the first read, then
//     what the ssm-* knobs said when the command was sent).
//   * A group stack (cbase-group-vm-<g>, cbase-group-static-<g>) made by an executed change set
//     adds what group-init's would: the tagged instance i-0<g>, or the tagged bucket <g>-group-bucket.
//     Deleting it takes the instance away; the bucket stays (Retain), with its tag.
//   * Control, for the test alone: POST /_fake/gather?n=N holds the next N GetObject answers until
//     all N have been asked (so N clients read the same ETag before any of them writes: a race
//     made deterministic), or 20 s pass. POST /_fake/set?KNOB=VALUE scripts the next answers:
//     changes=auto|none|replace-bucket|conditional-bucket|remove-bucket|replace-distribution, drift=0|1,
//     secret=S and secret2=S (planted where a change set, a drift and an error would carry an
//     origin header's or a parameter's value), fail-execute=0|1, list-ignores-prefix=0|1 (a
//     listing that answers every key, whatever its prefix), acm-deny=0|1 (DescribeCertificate
//     refused, AccessDenied), changes=replace-instance (a group's instance replaced), stack-fail=0|1 (an
//     executed update fails and rolls back, its events naming the failed resources), cs-fail=REASON
//     (a change set ends FAILED for REASON), stack-status=NAME:STATUS (a stack's status set),
//     instance-type=T (every instance's type), ssm-status=S, ssm-code=N, ssm-out=TEXT (what the next
//     commands end with). GET /_fake/template?stack=NAME answers the template the stack holds.
//
// Every request is one line of the log: "<METHOD> <path> <status>" for S3 (a PutObject with its
// precondition and its headers), "CFN <Action> ...", "ACM ...", "R53 ...", "CF ...". One thread per
// connection; the state is under one lock. Plain C and libc only (tools/ stands apart from the
// platform: tools/repo_gate_test.sh).
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

#define MAX_OBJ 512
#define MAX_RES 64
#define MAX_BUCKETS 16
#define BODY_MAX (1 << 20)
#define MAX_STACKS 32
#define MAX_CMDS 64
#define MAX_CERTS 16
#define MAX_ZONES 8

typedef struct {
    char   path[1024];  // "/bucket/key"
    char  *body;
    size_t len;
    unsigned version;   // the ETag is "v<version>-<len>"
    char   ctype[128], cache[128];   // its Content-Type and Cache-Control, as put
} object;

typedef struct {
    char key[128], value[128], type[64], arn[256];
} resource;

typedef struct {
    char  name[128], id[256], status[64], dist_id[64], dist_domain[128];
    char *template;      // the template the stack holds (NULL until a change set ran)
    bool  has_cs;        // one change set at a time: the tool's
    char  cs_name[128], cs_type[16];
    char *cs_template;
} stack;

typedef struct {
    char arn[256], status[32], names[8][256];
    int  nnames;
    bool written[8];     // its names' validation records, in a zone
} cert;

typedef struct {
    char id[64], name[256];   // the name ends with '.'
    bool private_;
} zone;

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t g_log_mu = PTHREAD_MUTEX_INITIALIZER;
static object g_obj[MAX_OBJ];
static int g_nobj;
static unsigned g_version;
static resource g_res[MAX_RES];
static int g_nres;
static char g_buckets[MAX_BUCKETS][64];
static char g_bucket_region[MAX_BUCKETS][32];
static int g_nbuckets;
static stack g_stacks[MAX_STACKS];
static int g_nstacks;
static cert g_certs[MAX_CERTS];
static int g_ncerts;
static zone g_zones[MAX_ZONES];
static int g_nzones;
// The scripts (/_fake/set).
static char g_changes[32] = "auto";
static int g_drift, g_fail_execute, g_list_ignores_prefix, g_acm_deny;
// stack-fail=1: an executed change set's update fails and rolls back (the stack ends ROLLBACK_COMPLETE
// for a create, UPDATE_ROLLBACK_COMPLETE for an update) and DescribeStackEvents answers a failed
// resource whose reason holds secret=; cs-fail=REASON: a change set ends FAILED with that reason.
static int g_stack_fail;
static char g_cs_fail[256];
static char g_secret[256], g_secret2[256];
// The SSM SecureString /cbase/<app>/CBASE_ORIGIN_SECRET (origin-secret=V; empty: ParameterNotFound).
static char g_origin_secret[256] = "fake-origin-secret-0123456789abcdef";
static char g_account[16] = "111122223333";
static char g_instance_type[32] = "t4g.nano";
// The SSM commands sent, each with the answer it ends with.
typedef struct {
    char id[40], instance[64], status[32], *out;
    int  code, reads;
} command;
static command g_cmds[MAX_CMDS];
static int g_ncmds;
static char g_ssm_status[32] = "Success";
static int g_ssm_code;
static char g_ssm_out[2048] = "CBASE: done (the fake's answer)\n";
static FILE *g_log;
// The gather: the next g_gather_want GetObject answers wait until that many have been computed.
static int g_gather_want, g_gather_seen;
static unsigned g_gather_round;

static const char *xml_text(const char *p, const char *tag, char *out, size_t cap);

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
    pthread_mutex_lock(&g_log_mu);   // its own lock: a handler logs while it holds g_mu
    vfprintf(g_log, fmt, ap);
    fputc('\n', g_log);
    fflush(g_log);
    pthread_mutex_unlock(&g_log_mu);
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

// The parts end to end into dst[cap]; false when they do not fit.
static bool join(char *dst, size_t cap, const char *const *parts, int n) {
    size_t at = 0;
    for (int i = 0; i < n; i++) {
        size_t k = strlen(parts[i]);
        if (at + k >= cap) return false;
        memcpy(dst + at, parts[i], k);
        at += k;
    }
    dst[at] = 0;
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

// ---- answers ------------------------------------------------------------------------------------

// A growing text, for an answer of any length (no fixed buffer for a compiler to warn about).
typedef struct {
    char  *p;
    size_t n, cap;
} sb;

static void sb_add(sb *b, const char *fmt, ...) {
    for (;;) {
        va_list ap;
        va_start(ap, fmt);
        size_t room = b->cap - b->n;
        int k = vsnprintf(b->p ? b->p + b->n : NULL, b->p ? room : 0, fmt, ap);
        va_end(ap);
        if (k < 0) die("vsnprintf");
        if (b->p && (size_t)k < room) {
            b->n += (size_t)k;
            return;
        }
        b->cap = (b->cap + (size_t)k + 1) * 2;
        b->p = realloc(b->p, b->cap);
        if (!b->p) die("out of memory");
        if (b->n == 0) b->p[0] = 0;
    }
}

// Text made safe inside an XML element.
static void sb_xml(sb *b, const char *s) {
    for (; *s; s++) {
        if (*s == '<') sb_add(b, "&lt;");
        else if (*s == '>') sb_add(b, "&gt;");
        else if (*s == '&') sb_add(b, "&amp;");
        else if (*s == '"') sb_add(b, "&quot;");
        else sb_add(b, "%c", *s);
    }
}

static void respond(int fd, int status, const char *reason, const char *ctype, const char *extra, const char *body,
                    size_t n) {
    sb h = { 0 };
    sb_add(&h, "HTTP/1.1 %d %s\r\nx-amz-request-id: FAKE0000\r\nContent-Type: %s\r\nContent-Length: %zu\r\n%s\r\n", status,
           reason, ctype, n, extra ? extra : "");
    write_all(fd, h.p, h.n);
    write_all(fd, body, n);
    free(h.p);
}

static void respond_sb(int fd, int status, const char *ctype, sb *b) {
    respond(fd, status, status == 200 ? "OK" : status == 201 ? "Created" : status == 204 ? "No Content" : "Bad Request", ctype,
            NULL, b->p ? b->p : "", b->n);
    free(b->p);
}

static void s3_error(int fd, int status, const char *reason, const char *code, const char *message) {
    sb b = { 0 };
    sb_add(&b, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<Error><Code>%s</Code><Message>%s</Message>"
               "<RequestId>FAKE0000</RequestId></Error>",
           code, message);
    respond(fd, status, reason, "application/xml", NULL, b.p, b.n);
    free(b.p);
}

// %XX and (in a form) '+' decoded, from s[0..n), into a new string.
static char *urldec(const char *s, size_t n, bool form) {
    char *o = malloc(n + 1);
    if (!o) die("out of memory");
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '%' && i + 2 < n) {
            char h[3] = { s[i + 1], s[i + 2], 0 };
            o[k++] = (char)strtol(h, NULL, 16);
            i += 2;
        } else if (s[i] == '+' && form) o[k++] = ' ';
        else o[k++] = s[i];
    }
    o[k] = 0;
    return o;
}

// The value of NAME in a form (a=1&b=2), decoded, or NULL. The caller frees it.
static char *form_get(const char *form, const char *name) {
    size_t nl = strlen(name);
    for (const char *p = form; p && *p;) {
        const char *amp = strchr(p, '&');
        size_t len = amp ? (size_t)(amp - p) : strlen(p);
        if (len > nl && memcmp(p, name, nl) == 0 && p[nl] == '=') return urldec(p + nl + 1, len - nl - 1, true);
        p = amp ? amp + 1 : NULL;
    }
    return NULL;
}

// A JSON string member "name": "value" of a request body (the recorded fake's plain reading), or "".
static void json_str(const char *body, const char *name, char *out, size_t cap) {
    out[0] = 0;
    sb key = { 0 };
    sb_add(&key, "\"%s\"", name);
    const char *p = strstr(body, key.p);
    if (p) p += key.n;
    while (p && (*p == ' ' || *p == ':')) p++;
    if (p && *p == '"') {
        p++;
        const char *e = strchr(p, '"');
        if (e && (size_t)(e - p) < cap) {
            memcpy(out, p, (size_t)(e - p));
            out[e - p] = 0;
        }
    }
    free(key.p);
}

// ---- S3 ------------------------------------------------------------------------------------------

static object *find(const char *path) {
    for (int i = 0; i < g_nobj; i++)
        if (g_obj[i].path[0] && strcmp(g_obj[i].path, path) == 0) return &g_obj[i];
    return NULL;
}

// The bucket of a path, or -1.
static int bucket_of(const char *path) {
    const char *b = path + 1, *slash = strchr(b, '/');
    size_t n = slash ? (size_t)(slash - b) : strlen(b);
    for (int i = 0; i < g_nbuckets; i++)
        if (strlen(g_buckets[i]) == n && memcmp(g_buckets[i], b, n) == 0) return i;
    return -1;
}
static bool bucket_known(const char *path) { return bucket_of(path) >= 0; }

static void etag_of(const object *o, char *out, size_t cap) { snprintf(out, cap, "\"v%u-%zu\"", o->version, o->len); }

static void s3_get(int fd, const request *r, bool head) {
    pthread_mutex_lock(&g_mu);
    int status;
    char etag[64] = "", *body = NULL, ctype[128] = "text/plain", cache[128] = "";
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
        if (o->ctype[0]) copy(ctype, sizeof ctype, o->ctype);
        copy(cache, sizeof cache, o->cache);
    }
    // The gather: the answer is fixed now, and written once every gathered reader has its own.
    if (g_gather_want > 0 && !head) {
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
                    logf_("GATHER timed out");
                    g_gather_want = g_gather_seen = 0;
                    g_gather_round++;
                    break;
                }
        }
    }
    pthread_mutex_unlock(&g_mu);
    const char *m = head ? "HEAD" : "GET";
    if (status != 200 && head) {   // a HEAD answer has no body: the CLI reads the bare status
        logf_("HEAD %s 404%s", r->path, status == 4041 ? " NoSuchBucket" : "");
        respond(fd, 404, "Not Found", "application/xml", NULL, "", 0);
    } else if (status == 4041) {
        logf_("%s %s 404 NoSuchBucket", m, r->path);
        s3_error(fd, 404, "Not Found", "NoSuchBucket", "The specified bucket does not exist");
    } else if (status == 404) {
        logf_("%s %s 404", m, r->path);
        s3_error(fd, 404, "Not Found", "NoSuchKey", "The specified key does not exist.");
    } else {
        logf_("%s %s 200", m, r->path);
        sb extra = { 0 };
        sb_add(&extra, "ETag: %s\r\nLast-Modified: Mon, 09 Nov 2026 12:00:00 GMT\r\nAccept-Ranges: bytes\r\n", etag);
        if (cache[0]) sb_add(&extra, "Cache-Control: %s\r\n", cache);
        if (head) {
            sb h = { 0 };
            sb_add(&h, "HTTP/1.1 200 OK\r\nx-amz-request-id: FAKE0000\r\nContent-Type: %s\r\nContent-Length: %zu\r\n%s\r\n", ctype, n,
                   extra.p);
            write_all(fd, h.p, h.n);
            free(h.p);
        } else respond(fd, 200, "OK", ctype, extra.p, body, n);
        free(extra.p);
    }
    free(body);
}

static void s3_put(int fd, const request *r) {
    const char *im = header(r, "If-Match"), *inm = header(r, "If-None-Match");
    const char *cond = im ? "if-match" : inm ? "if-none-match" : "unconditional";
    const char *ct = header(r, "Content-Type"), *cc = header(r, "Cache-Control");
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
            for (int i = 0; i < g_nobj && !o; i++)
                if (!g_obj[i].path[0]) o = &g_obj[i];   // a deleted object's slot
            if (!o) {
                if (g_nobj == MAX_OBJ) die("too many objects");
                o = &g_obj[g_nobj++];
            }
            if (!copy(o->path, sizeof o->path, r->path)) die("a key too long for the recording");
        }
        free(o->body);
        o->body = malloc(r->len + 1);
        if (!o->body) die("out of memory");
        memcpy(o->body, r->body, r->len);
        o->len = r->len;
        o->version = ++g_version;
        if (!copy(o->ctype, sizeof o->ctype, ct ? ct : "")) o->ctype[0] = 0;
        if (!copy(o->cache, sizeof o->cache, cc ? cc : "")) o->cache[0] = 0;
        etag_of(o, etag, sizeof etag);
    }
    pthread_mutex_unlock(&g_mu);
    logf_("PUT %s %d %s ct=%s cc=%s", r->path, status == 4041 ? 404 : status, cond, ct ? ct : "-", cc ? cc : "-");
    if (status == 4041) s3_error(fd, 404, "Not Found", "NoSuchBucket", "The specified bucket does not exist");
    else if (status == 404) s3_error(fd, 404, "Not Found", "NoSuchKey", "The specified key does not exist.");
    else if (status == 412)
        s3_error(fd, 412, "Precondition Failed", "PreconditionFailed",
                 "At least one of the pre-conditions you specified did not hold");
    else {
        sb extra = { 0 };
        sb_add(&extra, "ETag: %s\r\n", etag);
        respond(fd, 200, "OK", "text/plain", extra.p, "", 0);
        free(extra.p);
    }
}

static void s3_delete(int fd, const request *r) {
    pthread_mutex_lock(&g_mu);
    object *o = find(r->path);
    if (o) {
        free(o->body);
        o->body = NULL;
        o->path[0] = 0;
    }
    pthread_mutex_unlock(&g_mu);
    logf_("DELETE %s 204", r->path);
    respond(fd, 204, "No Content", "text/plain", NULL, "", 0);
}

// GET /bucket?location and GET /bucket?list-type=2&prefix=P (one page: no recording holds more).
static void s3_bucket(int fd, const request *r) {
    int bi = bucket_of(r->path);
    if (bi < 0) {
        logf_("GET %s?%s 404 NoSuchBucket", r->path, r->query);
        s3_error(fd, 404, "Not Found", "NoSuchBucket", "The specified bucket does not exist");
        return;
    }
    sb b = { 0 };
    sb_add(&b, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
    if (strstr(r->query, "location")) {
        const char *region = g_bucket_region[bi];
        sb_add(&b, "<LocationConstraint xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\">%s</LocationConstraint>",
               strcmp(region, "us-east-1") == 0 ? "" : region);
        logf_("GET %s?location 200", r->path);
        respond_sb(fd, 200, "application/xml", &b);
        return;
    }
    char *prefix = form_get(r->query, "prefix");
    sb_add(&b, "<ListBucketResult xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\"><Name>%s</Name><Prefix>",
           g_buckets[bi]);
    sb_xml(&b, prefix ? prefix : "");
    sb_add(&b, "</Prefix><MaxKeys>1000</MaxKeys><IsTruncated>false</IsTruncated>");
    int count = 0;
    pthread_mutex_lock(&g_mu);
    size_t bl = strlen(g_buckets[bi]);
    for (int i = 0; i < g_nobj; i++) {
        const char *p = g_obj[i].path;
        if (!p[0] || strncmp(p + 1, g_buckets[bi], bl) != 0 || p[1 + bl] != '/') continue;
        const char *key = p + 2 + bl;
        // --list-ignores-prefix: a listing that answers every key, so the tool's own guard is tested.
        if (!g_list_ignores_prefix && prefix && strncmp(key, prefix, strlen(prefix)) != 0) continue;
        char etag[64];
        etag_of(&g_obj[i], etag, sizeof etag);
        sb_add(&b, "<Contents><Key>");
        sb_xml(&b, key);
        sb_add(&b, "</Key><LastModified>2026-11-09T12:00:00.000Z</LastModified><ETag>&quot;%.*s&quot;</ETag><Size>%zu</Size>"
                   "<StorageClass>STANDARD</StorageClass></Contents>",
               (int)strlen(etag) - 2, etag + 1, g_obj[i].len);
        count++;
    }
    pthread_mutex_unlock(&g_mu);
    sb_add(&b, "<KeyCount>%d</KeyCount></ListBucketResult>", count);
    logf_("GET %s LIST prefix=%s 200 %d", r->path, prefix ? prefix : "", count);
    free(prefix);
    respond_sb(fd, 200, "application/xml", &b);
}

static void bucket_add(const char *name, const char *region) {
    if (g_nbuckets == MAX_BUCKETS) die("too many buckets");
    copy(g_buckets[g_nbuckets], sizeof g_buckets[0], name);
    copy(g_bucket_region[g_nbuckets++], sizeof g_bucket_region[0], region);
}

// The tagging answers' resources: one added, or every one whose ARN is arn dropped.
static void resource_add(const char *key, const char *value, const char *type, const char *arn) {
    if (g_nres == MAX_RES) die("too many resources");
    resource *x = &g_res[g_nres++];
    copy(x->key, sizeof x->key, key);
    copy(x->value, sizeof x->value, value);
    copy(x->type, sizeof x->type, type);
    copy(x->arn, sizeof x->arn, arn);
}
static void resource_drop(const char *arn) {
    for (int i = 0; i < g_nres; i++)
        if (strcmp(g_res[i].arn, arn) == 0) g_res[i].arn[0] = g_res[i].key[0] = 0;
}

// HEAD /bucket, PUT /bucket (CreateBucket, ?publicAccessBlock, ?versioning), DELETE /bucket?tagging.
static void s3_bucket_call(int fd, const request *r) {
    pthread_mutex_lock(&g_mu);
    int bi = bucket_of(r->path);
    const char *name = r->path + 1;
    int status = 200;
    const char *what = "?";
    if (strcmp(r->method, "HEAD") == 0) {
        what = "HeadBucket";
        status = bi < 0 ? 404 : 200;
    } else if (strcmp(r->method, "PUT") == 0 && !r->query[0]) {
        what = "CreateBucket";
        if (bi >= 0) status = 409;
        else {
            char region[32] = "us-east-1";
            xml_text(r->body, "LocationConstraint", region, sizeof region);
            if (!region[0]) copy(region, sizeof region, "us-east-1");
            bucket_add(name, region);
        }
    } else if (strcmp(r->method, "PUT") == 0) {
        what = strstr(r->query, "publicAccessBlock") ? "PutPublicAccessBlock" : strstr(r->query, "versioning") ? "PutBucketVersioning" : "PutBucket?";
        status = bi < 0 ? 4041 : 200;
    } else if (strcmp(r->method, "DELETE") == 0 && strstr(r->query, "tagging")) {
        what = "DeleteBucketTagging";
        if (bi < 0) status = 4041;
        else {
            char arn[128];
            snprintf(arn, sizeof arn, "arn:aws:s3:::%.63s", name);
            resource_drop(arn);
            status = 204;
        }
    }
    pthread_mutex_unlock(&g_mu);
    // A PutBucketVersioning is logged with the status its body sets (Enabled, Suspended).
    char vstatus[32] = "";
    if (strcmp(what, "PutBucketVersioning") == 0) xml_text(r->body, "Status", vstatus, sizeof vstatus);
    logf_("S3 %s %s %d%s%s", what, name, status == 4041 ? 404 : status, vstatus[0] ? " status=" : "", vstatus);
    if (strcmp(r->method, "HEAD") == 0) respond(fd, status, status == 200 ? "OK" : "Not Found", "application/xml", NULL, "", 0);
    else if (status == 409) s3_error(fd, 409, "Conflict", "BucketAlreadyOwnedByYou", "Your previous request to create the named bucket succeeded and you already own it.");
    else if (status == 4041) s3_error(fd, 404, "Not Found", "NoSuchBucket", "The specified bucket does not exist");
    else respond(fd, status, status == 204 ? "No Content" : "OK", "application/xml", NULL, "", 0);
}

// ---- STS, Tagging --------------------------------------------------------------------------------

static void sts(int fd, const request *r) {
    if (!strstr(r->body, "Action=GetCallerIdentity")) {
        logf_("POST STS %.60s 400", r->body);
        respond(fd, 400, "Bad Request", "text/xml", NULL, "", 0);
        return;
    }
    sb b = { 0 };
    sb_add(&b,
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
    respond_sb(fd, 200, "text/xml", &b);
}

// The request's filters, read the plain way a recorded fake may: the body with its blanks dropped
// holds "Key":"<k>","Values":["<v>"] and "ResourceTypeFilters":["<t>"].
static void tagging(int fd, const request *r) {
    char flat[8192];
    size_t k = 0;
    for (size_t i = 0; i < r->len && k + 1 < sizeof flat; i++)
        if (r->body[i] != ' ' && r->body[i] != '\n') flat[k++] = r->body[i];
    flat[k] = 0;
    sb out = { 0 };
    sb_add(&out, "{\"PaginationToken\":\"\",\"ResourceTagMappingList\":[");
    int hits = 0;
    for (int i = 0; i < g_nres; i++) {
        if (!g_res[i].arn[0]) continue;   // dropped
        char want_tag[1024], want_type[1024];
        const char *tag[] = { "\"Key\":\"", g_res[i].key, "\",\"Values\":[\"", g_res[i].value, "\"]" };
        const char *type[] = { "\"ResourceTypeFilters\":[\"", g_res[i].type, "\"]" };
        if (!join(want_tag, sizeof want_tag, tag, 5) || !join(want_type, sizeof want_type, type, 3)) continue;
        if (!strstr(flat, want_tag) || !strstr(flat, want_type)) continue;
        sb_add(&out, "%s{\"ResourceARN\":\"%s\",\"Tags\":[{\"Key\":\"%s\",\"Value\":\"%s\"}]}", hits++ ? "," : "", g_res[i].arn,
               g_res[i].key, g_res[i].value);
    }
    sb_add(&out, "]}");
    logf_("POST Tagging GetResources 200 %d", hits);
    respond_sb(fd, 200, "application/x-amz-json-1.1", &out);
}

// ---- CloudFormation (the Query protocol: a form in, XML out) -------------------------------------

#define CFN_NS "http://cloudformation.amazonaws.com/doc/2010-05-15/"

static stack *stack_find(const char *name) {
    for (int i = 0; i < g_nstacks; i++)
        if (g_stacks[i].name[0] && strcmp(g_stacks[i].name, name) == 0) return &g_stacks[i];
    return NULL;
}

static void cfn_error(int fd, int status, const char *code, const char *msg) {
    sb b = { 0 };
    sb_add(&b, "<ErrorResponse xmlns=\"" CFN_NS "\"><Error><Type>Sender</Type><Code>%s</Code><Message>", code);
    sb_xml(&b, msg);
    sb_add(&b, "</Message></Error><RequestId>FAKE0000</RequestId></ErrorResponse>");
    respond_sb(fd, status, "text/xml", &b);
}

static void cfn_open(sb *b, const char *action) {
    sb_add(b, "<%sResponse xmlns=\"" CFN_NS "\"><%sResult>", action, action);
}
static void cfn_close(sb *b, const char *action) {
    sb_add(b, "</%sResult><ResponseMetadata><RequestId>FAKE0000</RequestId></ResponseMetadata></%sResponse>", action, action);
}

// One ResourceChange member.
static void cfn_change(sb *b, const char *action, const char *lid, const char *type, const char *repl, const char *details) {
    sb_add(b, "<member><Type>Resource</Type><ResourceChange><Action>%s</Action><LogicalResourceId>%s</LogicalResourceId>"
              "<ResourceType>%s</ResourceType>",
           action, lid, type);
    if (repl) sb_add(b, "<Replacement>%s</Replacement><Scope><member>Properties</member></Scope>", repl);
    sb_add(b, "<Details>%s</Details></ResourceChange></member>", details ? details : "");
}

// A change set's Details member: a Target with its path and values.
static void cfn_detail(sb *b, const char *path, const char *before, const char *after, const char *source, const char *cause) {
    sb_add(b, "<member><Target><Attribute>Properties</Attribute><Name>DistributionConfig</Name>"
              "<RequiresRecreation>Never</RequiresRecreation><Path>");
    sb_xml(b, path);
    sb_add(b, "</Path><BeforeValue>");
    sb_xml(b, before);
    sb_add(b, "</BeforeValue><AfterValue>");
    sb_xml(b, after);
    sb_add(b, "</AfterValue><AttributeChangeType>Modify</AttributeChangeType></Target><Evaluation>Static</Evaluation>"
              "<ChangeSource>%s</ChangeSource>",
           source);
    if (cause) sb_add(b, "<CausingEntity>%s</CausingEntity>", cause);
    sb_add(b, "</member>");
}

// The changes of a stack's pending change set, as CloudFormation would compute them here: a create
// adds every resource of the template; an update is what --changes scripts, or, unscripted, a
// Modify of the distribution when the template changed and nothing when it did not.
static int cfn_changes(const stack *st, sb *b) {
    int n = 0;
    if (strcmp(st->cs_type, "CREATE") == 0) {
        const char *t = st->cs_template, *res = strstr(t, "\nResources:\n");
        for (const char *p = res; p && *p;) {
            const char *nl = strchr(p + 1, '\n');
            if (!nl) break;
            p = nl;
            // "  Name:\n    Type: X"
            if (p[1] == ' ' && p[2] == ' ' && p[3] != ' ' && p[3] != '#') {
                const char *colon = strchr(p + 3, ':'), *eol = strchr(p + 3, '\n');
                if (!colon || !eol || colon > eol || colon[1] != '\n') continue;
                const char *ty = strstr(eol, "\n    Type: ");
                if (!ty) continue;
                const char *te = strchr(ty + 11, '\n');
                char lid[128], type[128];
                snprintf(lid, sizeof lid, "%.*s", (int)(colon - p - 3), p + 3);
                snprintf(type, sizeof type, "%.*s", te ? (int)(te - ty - 11) : 0, ty + 11);
                cfn_change(b, "Add", lid, type, NULL, NULL);
                n++;
            } else if (p[1] && p[1] != ' ' && p[1] != '\n' && p[1] != '#' && p != res) break;   // the next section
        }
        return n;
    }
    sb d = { 0 };
    cfn_detail(&d, "/Properties/DistributionConfig/Comment", "cbase app before", "cbase app after", "DirectModification", NULL);
    if (g_secret[0]) {
        // The planted secret: where a real change set would carry an origin header's value, and
        // a parameter's.
        cfn_detail(&d, "/Properties/DistributionConfig/Origins/1/OriginCustomHeaders/0/HeaderValue", g_secret, g_secret2,
                   "DirectModification", NULL);
        cfn_detail(&d, "/Properties/DistributionConfig/Origins/1", "{\"OriginCustomHeaders\":[]}", g_secret,
                   "ParameterReference", "OriginSecret");
    }
    if (strcmp(g_changes, "replace-bucket") == 0) {
        cfn_change(b, "Modify", "Bucket", "AWS::S3::Bucket", "True", NULL);
        cfn_change(b, "Modify", "Distribution", "AWS::CloudFront::Distribution", "False", d.p);
        n = 2;
    } else if (strcmp(g_changes, "conditional-bucket") == 0) {   // a replacement CloudFormation decides at run time
        cfn_change(b, "Modify", "Bucket", "AWS::S3::Bucket", "Conditional", NULL);
        n = 1;
    } else if (strcmp(g_changes, "remove-bucket") == 0) {
        cfn_change(b, "Remove", "Bucket", "AWS::S3::Bucket", NULL, NULL);
        n = 1;
    } else if (strcmp(g_changes, "replace-instance") == 0) {
        cfn_change(b, "Modify", "Instance", "AWS::EC2::Instance", "True", NULL);
        n = 1;
    } else if (strcmp(g_changes, "replace-distribution") == 0) {
        cfn_change(b, "Modify", "Distribution", "AWS::CloudFront::Distribution", "True", d.p);
        n = 1;
    } else if (strcmp(g_changes, "none") != 0 && (g_secret[0] || !st->template || strcmp(st->template, st->cs_template) != 0)) {
        cfn_change(b, "Modify", "Distribution", "AWS::CloudFront::Distribution", "False", d.p);
        n = 1;
    }
    free(d.p);
    return n;
}

// What a group stack's resources are to the other services, once its create ran: the instance (its
// tag, so the tagging answers find it) or the bucket and its tag.
static void group_made(const char *name) {
    char arn[256], bucket[160];
    if (strncmp(name, "cbase-group-vm-", 15) == 0) {
        snprintf(arn, sizeof arn, "arn:aws:ec2:us-east-1:%s:instance/i-0%.100s", g_account, name + 15);
        resource_add("cbase:vm-group", name + 15, "ec2:instance", arn);
    } else if (strncmp(name, "cbase-group-static-", 19) == 0) {
        snprintf(bucket, sizeof bucket, "%.100s-group-bucket", name + 19);
        snprintf(arn, sizeof arn, "arn:aws:s3:::%s", bucket);
        bucket_add(bucket, "us-east-1");
        resource_add("cbase:static-group", name + 19, "s3", arn);
    }
}

static void cfn_stack_drifts(sb *b, const stack *st) {
    sb_add(b, "<StackResourceDrifts>");
    if (g_drift) {
        sb_add(b, "<member><StackId>%s</StackId><LogicalResourceId>Distribution</LogicalResourceId>"
                  "<PhysicalResourceId>%s</PhysicalResourceId><ResourceType>AWS::CloudFront::Distribution</ResourceType>"
                  "<ExpectedProperties>",
               st->id, st->dist_id);
        sb_xml(b, "{\"DistributionConfig\":{\"Comment\":\"cbase app\",\"Origins\":[{\"OriginCustomHeaders\":[{\"HeaderName\":\"X-Origin-Secret\",\"HeaderValue\":\"");
        sb_xml(b, g_secret);
        sb_xml(b, "\"}]}]}}");
        sb_add(b, "</ExpectedProperties><ActualProperties>");
        sb_xml(b, "{\"DistributionConfig\":{\"Comment\":\"edited by hand\"}}");
        sb_add(b, "</ActualProperties><PropertyDifferences>"
                  "<member><PropertyPath>/DistributionConfig/Comment</PropertyPath><ExpectedValue>cbase app</ExpectedValue>"
                  "<ActualValue>edited by hand</ActualValue><DifferenceType>NOT_EQUAL</DifferenceType></member>");
        if (g_secret[0]) {
            sb_add(b, "<member><PropertyPath>/DistributionConfig/Origins/0/OriginCustomHeaders/0/HeaderValue</PropertyPath>"
                      "<ExpectedValue>");
            sb_xml(b, g_secret);
            sb_add(b, "</ExpectedValue><ActualValue>");
            sb_xml(b, g_secret2);
            sb_add(b, "</ActualValue><DifferenceType>NOT_EQUAL</DifferenceType></member>"
                      "<member><PropertyPath>/DistributionConfig/Origins/0</PropertyPath><ExpectedValue>");
            sb_xml(b, "{\"OriginCustomHeaders\":[{\"HeaderName\":\"X-Origin-Secret\",\"HeaderValue\":\"");
            sb_xml(b, g_secret);
            sb_xml(b, "\"}]}");
            sb_add(b, "</ExpectedValue><ActualValue>{}</ActualValue><DifferenceType>NOT_EQUAL</DifferenceType></member>");
        }
        sb_add(b, "</PropertyDifferences><StackResourceDriftStatus>MODIFIED</StackResourceDriftStatus>"
                  "<Timestamp>2026-11-09T12:00:00Z</Timestamp></member>");
    }
    sb_add(b, "</StackResourceDrifts>");
}

static void cloudformation(int fd, const request *r) {
    char *action = form_get(r->body, "Action"), *name = form_get(r->body, "StackName"),
         *csname = form_get(r->body, "ChangeSetName");
    const char *a = action ? action : "";
    sb b = { 0 };
    pthread_mutex_lock(&g_mu);
    stack *st = name ? stack_find(name) : NULL;
    int status = 200;
    const char *ecode = NULL;
    char emsg[1024] = "";
    if (strcmp(a, "DescribeStacks") == 0) {
        if (!st) {
            status = 400, ecode = "ValidationError";
            snprintf(emsg, sizeof emsg, "Stack with id %.200s does not exist", name ? name : "");
        } else {
            cfn_open(&b, a);
            sb_add(&b, "<Stacks><member><StackName>%s</StackName><StackId>%s</StackId><CreationTime>2026-11-09T12:00:00Z</CreationTime>"
                       "<StackStatus>%s</StackStatus><Parameters/>",
                   st->name, st->id, st->status);
            if (st->template) {
                sb_add(&b, "<Outputs><member><OutputKey>DistributionId</OutputKey><OutputValue>%s</OutputValue></member>"
                           "<member><OutputKey>DistributionDomain</OutputKey><OutputValue>%s</OutputValue></member>",
                       st->dist_id, st->dist_domain);
                // A vm group's: the subnet's ::10.
                if (strstr(st->template, "\n  OriginIpv6:\n"))
                    sb_add(&b, "<member><OutputKey>OriginIpv6</OutputKey><OutputValue>2600:1f18:4a3:6f00::10</OutputValue></member>");
                // A template with CBASE_API_HOST's distribution: its own cloudfront.net name.
                if (strstr(st->template, "\n  ApiDistribution:\n"))
                    sb_add(&b, "<member><OutputKey>ApiDistributionDomain</OutputKey><OutputValue>api-%s</OutputValue></member>"
                               "<member><OutputKey>ApiDistributionId</OutputKey><OutputValue>A%s</OutputValue></member>",
                           st->dist_domain, st->dist_id);
                sb_add(&b, "</Outputs>");
            }
            sb_add(&b, "</member></Stacks>");
            cfn_close(&b, a);
        }
        logf_("CFN DescribeStacks %s %s", name ? name : "-", st ? st->status : "none");
    } else if (strcmp(a, "CreateChangeSet") == 0) {
        char *type = form_get(r->body, "ChangeSetType"), *tpl = form_get(r->body, "TemplateBody");
        if (!st && type && strcmp(type, "CREATE") == 0) {
            if (g_nstacks == MAX_STACKS) die("too many stacks");
            st = &g_stacks[g_nstacks++];
            memset(st, 0, sizeof *st);
            copy(st->name, sizeof st->name, name ? name : "");
            snprintf(st->id, sizeof st->id, "arn:aws:cloudformation:us-east-1:%s:stack/%.100s/%d", g_account, name ? name : "", g_nstacks);
            snprintf(st->dist_id, sizeof st->dist_id, "E%dFAKEDIST", g_nstacks);
            snprintf(st->dist_domain, sizeof st->dist_domain, "d%dfake.cloudfront.net", g_nstacks);
            copy(st->status, sizeof st->status, "REVIEW_IN_PROGRESS");
        }
        if (!st) {
            status = 400, ecode = "ValidationError";
            snprintf(emsg, sizeof emsg, "Stack [%.200s] does not exist", name ? name : "");
        } else if (st->has_cs) {
            status = 400, ecode = "AlreadyExistsException";
            snprintf(emsg, sizeof emsg, "ChangeSet %.200s already exists", csname ? csname : "");
        } else {
            st->has_cs = true;
            copy(st->cs_name, sizeof st->cs_name, csname ? csname : "");
            copy(st->cs_type, sizeof st->cs_type, type ? type : "");
            free(st->cs_template);
            st->cs_template = tpl ? tpl : calloc(1, 1);
            tpl = NULL;
            cfn_open(&b, a);
            sb_add(&b, "<Id>arn:aws:cloudformation:us-east-1:%s:changeSet/%s/1</Id><StackId>%s</StackId>", g_account, st->cs_name, st->id);
            cfn_close(&b, a);
        }
        sb params = { 0 };
        for (int i = 1; i <= 16; i++) {
            char k[64];
            snprintf(k, sizeof k, "Parameters.member.%d.ParameterKey", i);
            char *pk = form_get(r->body, k);
            if (!pk) break;
            snprintf(k, sizeof k, "Parameters.member.%d.ParameterValue", i);
            char *pv = form_get(r->body, k);
            snprintf(k, sizeof k, "Parameters.member.%d.UsePreviousValue", i);
            char *pp = form_get(r->body, k);
            // The origin secret is never logged: whether it is the SSM parameter's value is.
            if (strcmp(pk, "OriginSecret") == 0)
                sb_add(&params, "%s%s=%s", params.n ? "," : "", pk, pv && strcmp(pv, g_origin_secret) == 0 ? "(the ssm value)" : "(MISMATCH)");
            else
                sb_add(&params, "%s%s=%s", params.n ? "," : "", pk, pv ? pv : pp ? "(previous)" : "");
            free(pk), free(pv), free(pp);
        }
        char *caps = form_get(r->body, "Capabilities.member.1");
        logf_("CFN CreateChangeSet %s %s %s params=%s caps=%s", name ? name : "-", type ? type : "-", ecode ? ecode : "ok",
              params.p ? params.p : "", caps ? caps : "");
        free(params.p);
        free(caps);
        free(type);
        free(tpl);
    } else if (strcmp(a, "DescribeChangeSet") == 0) {
        if (!st || !st->has_cs) {
            status = 404, ecode = "ChangeSetNotFound";
            snprintf(emsg, sizeof emsg, "ChangeSet [%.200s] does not exist", csname ? csname : "");
        } else {
            sb ch = { 0 };
            int n = cfn_changes(st, &ch);
            cfn_open(&b, a);
            sb_add(&b, "<ChangeSetName>%s</ChangeSetName><ChangeSetId>arn:aws:cloudformation:us-east-1:%s:changeSet/%s/1</ChangeSetId>"
                       "<StackId>%s</StackId><StackName>%s</StackName>",
                   st->cs_name, g_account, st->cs_name, st->id, st->name);
            if (g_secret[0])
                sb_add(&b, "<Parameters><member><ParameterKey>OriginSecret</ParameterKey><ParameterValue>%s</ParameterValue></member></Parameters>",
                       g_secret);
            if (g_cs_fail[0])
                sb_add(&b, "<Status>FAILED</Status><ExecutionStatus>UNAVAILABLE</ExecutionStatus><StatusReason>%s</StatusReason>", g_cs_fail);
            else if (n == 0)
                sb_add(&b, "<Status>FAILED</Status><ExecutionStatus>UNAVAILABLE</ExecutionStatus><StatusReason>The submitted "
                           "information didn't contain changes. Submit different information to create a change set.</StatusReason>");
            else sb_add(&b, "<Status>CREATE_COMPLETE</Status><ExecutionStatus>AVAILABLE</ExecutionStatus>");
            sb_add(&b, "<Changes>%s</Changes>", ch.p ? ch.p : "");
            cfn_close(&b, a);
            free(ch.p);
        }
        logf_("CFN DescribeChangeSet %s %s", name ? name : "-", ecode ? ecode : "ok");
    } else if (strcmp(a, "DeleteChangeSet") == 0) {
        if (!st || !st->has_cs) status = 404, ecode = "ChangeSetNotFound", copy(emsg, sizeof emsg, "ChangeSet does not exist");
        else {
            st->has_cs = false;
            cfn_open(&b, a);
            cfn_close(&b, a);
        }
        logf_("CFN DeleteChangeSet %s %s", name ? name : "-", ecode ? ecode : "ok");
    } else if (strcmp(a, "ExecuteChangeSet") == 0) {
        if (!st || !st->has_cs) status = 404, ecode = "ChangeSetNotFound", copy(emsg, sizeof emsg, "ChangeSet does not exist");
        else if (g_fail_execute == 2) {
            // The origin secret's bare value, on a line that names nothing a keyword could catch.
            status = 400, ecode = "ValidationError";
            snprintf(emsg, sizeof emsg, "Invalid value %.200s", g_origin_secret);
        } else if (g_fail_execute) {
            status = 400, ecode = "ValidationError";
            snprintf(emsg, sizeof emsg, "Invalid request: OriginCustomHeaders HeaderValue=%.200s is not valid", g_secret);
        } else if (g_stack_fail) {
            // The update fails and rolls back: the template is the one before, the change set used.
            copy(st->status, sizeof st->status, strcmp(st->cs_type, "CREATE") == 0 ? "ROLLBACK_COMPLETE" : "UPDATE_ROLLBACK_COMPLETE");
            st->has_cs = false;
            cfn_open(&b, a);
            cfn_close(&b, a);
        } else {
            if (strcmp(st->cs_type, "CREATE") == 0) group_made(st->name);
            copy(st->status, sizeof st->status, strcmp(st->cs_type, "CREATE") == 0 ? "CREATE_COMPLETE" : "UPDATE_COMPLETE");
            free(st->template);
            st->template = st->cs_template;
            st->cs_template = NULL;
            st->has_cs = false;
            cfn_open(&b, a);
            cfn_close(&b, a);
        }
        // --disable-rollback (a staged attach's update) is logged: rollback=off.
        char *dr = form_get(r->body, "DisableRollback");
        logf_("CFN ExecuteChangeSet %s %s%s", name ? name : "-", ecode ? ecode : "ok", dr && strcmp(dr, "true") == 0 ? " rollback=off" : "");
        free(dr);
    } else if (strcmp(a, "GetTemplate") == 0) {
        if (!st || !st->template) {
            status = 400, ecode = "ValidationError";
            snprintf(emsg, sizeof emsg, "Stack with id %.200s does not exist", name ? name : "");
        } else {
            cfn_open(&b, a);
            sb_add(&b, "<TemplateBody>");
            sb_xml(&b, st->template);
            sb_add(&b, "</TemplateBody><StagesAvailable><member>Original</member></StagesAvailable>");
            cfn_close(&b, a);
        }
        logf_("CFN GetTemplate %s %s", name ? name : "-", ecode ? ecode : "ok");
    } else if (strcmp(a, "DeleteStack") == 0) {
        if (st && strncmp(st->name, "cbase-group-vm-", 15) == 0) {   // the instance goes; a bucket stays (Retain)
            char arn[256];
            snprintf(arn, sizeof arn, "arn:aws:ec2:us-east-1:%s:instance/i-0%.100s", g_account, st->name + 15);
            resource_drop(arn);
        }
        if (st) {
            free(st->template);
            free(st->cs_template);
            memset(st, 0, sizeof *st);
        }
        cfn_open(&b, a);
        cfn_close(&b, a);
        logf_("CFN DeleteStack %s", name ? name : "-");
    } else if (strcmp(a, "DetectStackDrift") == 0) {
        cfn_open(&b, a);
        sb_add(&b, "<StackDriftDetectionId>drift-%s</StackDriftDetectionId>", st ? st->dist_id : "none");
        cfn_close(&b, a);
        logf_("CFN DetectStackDrift %s", name ? name : "-");
    } else if (strcmp(a, "DescribeStackDriftDetectionStatus") == 0) {
        cfn_open(&b, a);
        sb_add(&b, "<StackId>arn:stack</StackId><StackDriftDetectionId>drift</StackDriftDetectionId><StackDriftStatus>%s</StackDriftStatus>"
                   "<DetectionStatus>DETECTION_COMPLETE</DetectionStatus><DriftedStackResourceCount>%d</DriftedStackResourceCount>"
                   "<Timestamp>2026-11-09T12:00:00Z</Timestamp>",
               g_drift ? "DRIFTED" : "IN_SYNC", g_drift ? 1 : 0);
        cfn_close(&b, a);
        logf_("CFN DescribeStackDriftDetectionStatus %s", g_drift ? "DRIFTED" : "IN_SYNC");
    } else if (strcmp(a, "DescribeStackResourceDrifts") == 0) {
        if (!st) status = 400, ecode = "ValidationError", copy(emsg, sizeof emsg, "Stack does not exist");
        else {
            cfn_open(&b, a);
            cfn_stack_drifts(&b, st);
            cfn_close(&b, a);
        }
        logf_("CFN DescribeStackResourceDrifts %s", name ? name : "-");
    } else if (strcmp(a, "DescribeStackResources") == 0) {
        // An adopted stack (CBASE_VM_STACK): the instance a --resource line tags with
        // aws:cloudformation:stack-name, as CloudFormation tags what it makes.
        const char *inst = NULL;
        for (int i = 0; i < g_nres && name; i++)
            if (strcmp(g_res[i].key, "aws:cloudformation:stack-name") == 0 && strcmp(g_res[i].value, name) == 0 &&
                strcmp(g_res[i].type, "ec2:instance") == 0 && strrchr(g_res[i].arn, '/'))
                inst = strrchr(g_res[i].arn, '/') + 1;
        if (!inst) {
            status = 400, ecode = "ValidationError";
            snprintf(emsg, sizeof emsg, "Stack with id %.200s does not exist", name ? name : "");
        } else {
            cfn_open(&b, a);
            sb_add(&b, "<StackResources><member><StackName>%s</StackName><LogicalResourceId>Instance</LogicalResourceId>"
                       "<PhysicalResourceId>%s</PhysicalResourceId><ResourceType>AWS::EC2::Instance</ResourceType>"
                       "<ResourceStatus>CREATE_COMPLETE</ResourceStatus><Timestamp>2026-11-09T12:00:00Z</Timestamp></member></StackResources>",
                   name, inst);
            cfn_close(&b, a);
        }
        logf_("CFN DescribeStackResources %s %s", name ? name : "-", inst ? inst : "none");
    } else if (strcmp(a, "DescribeStackEvents") == 0) {
        cfn_open(&b, a);
        if (g_stack_fail)
            sb_add(&b, "<StackEvents><member><StackId>arn:stack</StackId><EventId>e1</EventId><StackName>%s</StackName>"
                       "<LogicalResourceId>Distribution</LogicalResourceId><ResourceType>AWS::CloudFront::Distribution</ResourceType>"
                       "<Timestamp>2026-11-09T12:00:00Z</Timestamp><ResourceStatus>UPDATE_FAILED</ResourceStatus>"
                       "<ResourceStatusReason>Invalid request: OriginCustomHeaders HeaderValue=%s is not valid</ResourceStatusReason></member>"
                       "<member><StackId>arn:stack</StackId><EventId>e2</EventId><StackName>%s</StackName>"
                       "<LogicalResourceId>Router</LogicalResourceId><ResourceType>AWS::CloudFront::Function</ResourceType>"
                       "<Timestamp>2026-11-09T12:00:00Z</Timestamp><ResourceStatus>CREATE_FAILED</ResourceStatus>"
                       "<ResourceStatusReason>The function code is too large</ResourceStatusReason></member></StackEvents>",
                   name ? name : "-", g_secret, name ? name : "-");
        else sb_add(&b, "<StackEvents/>");
        cfn_close(&b, a);
    } else {
        status = 400, ecode = "InvalidAction";
        snprintf(emsg, sizeof emsg, "not in the recording: %.100s", a);
        logf_("CFN %s 400", a);
    }
    pthread_mutex_unlock(&g_mu);
    if (ecode) {
        free(b.p);
        cfn_error(fd, status, ecode, emsg);
    } else respond_sb(fd, 200, "text/xml", &b);
    free(action);
    free(name);
    free(csname);
}

// ---- ACM (JSON 1.1) ------------------------------------------------------------------------------

static cert *cert_find(const char *arn) {
    for (int i = 0; i < g_ncerts; i++)
        if (strcmp(g_certs[i].arn, arn) == 0) return &g_certs[i];
    return NULL;
}

// A certificate is issued once every one of its validation records is in a zone.
static void cert_check_issued(cert *c) {
    if (strcmp(c->status, "PENDING_VALIDATION") != 0) return;
    for (int i = 0; i < c->nnames; i++)
        if (!c->written[i]) return;
    copy(c->status, sizeof c->status, "ISSUED");
    logf_("ACM issued %s", c->arn);
}

static void cert_add(const char *arn, const char *status, const char *names) {
    if (g_ncerts == MAX_CERTS) die("too many certificates");
    cert *c = &g_certs[g_ncerts++];
    memset(c, 0, sizeof *c);
    if (!copy(c->arn, sizeof c->arn, arn) || !copy(c->status, sizeof c->status, status)) die("a certificate too long");
    char tmp[1024];
    if (!copy(tmp, sizeof tmp, names)) die("names too long");
    for (char *save = NULL, *t = strtok_r(tmp, ",", &save); t && c->nnames < 8; t = strtok_r(NULL, ",", &save))
        if (!copy(c->names[c->nnames++], sizeof c->names[0], t)) die("a name too long");
}

static void acm(int fd, const request *r, const char *op) {
    sb b = { 0 };
    int status = 200;
    char arn[256];
    pthread_mutex_lock(&g_mu);
    if (strcmp(op, "ListCertificates") == 0) {
        sb_add(&b, "{\"CertificateSummaryList\":[");
        int k = 0;
        for (int i = 0; i < g_ncerts; i++) {
            sb q = { 0 };
            sb_add(&q, "\"%s\"", g_certs[i].status);
            if (strstr(r->body, q.p))
                sb_add(&b, "%s{\"CertificateArn\":\"%s\",\"DomainName\":\"%s\",\"Status\":\"%s\"}", k++ ? "," : "", g_certs[i].arn,
                       g_certs[i].names[0], g_certs[i].status);
            free(q.p);
        }
        sb_add(&b, "]}");
        logf_("ACM ListCertificates %d", k);
    } else if (strcmp(op, "DescribeCertificate") == 0) {
        json_str(r->body, "CertificateArn", arn, sizeof arn);
        cert *c = cert_find(arn);
        if (g_acm_deny) {
            status = 400;
            sb_add(&b, "{\"__type\":\"AccessDeniedException\",\"message\":\"User is not authorized to perform: acm:DescribeCertificate\"}");
        } else if (!c) {
            status = 400;
            sb_add(&b, "{\"__type\":\"ResourceNotFoundException\",\"message\":\"Could not find certificate %s.\"}", arn);
        } else {
            sb_add(&b, "{\"Certificate\":{\"CertificateArn\":\"%s\",\"DomainName\":\"%s\",\"Status\":\"%s\",\"Type\":\"AMAZON_ISSUED\","
                       "\"SubjectAlternativeNames\":[",
                   c->arn, c->names[0], c->status);
            for (int i = 0; i < c->nnames; i++) sb_add(&b, "%s\"%s\"", i ? "," : "", c->names[i]);
            sb_add(&b, "],\"DomainValidationOptions\":[");
            for (int i = 0; i < c->nnames; i++)
                sb_add(&b, "%s{\"DomainName\":\"%s\",\"ValidationDomain\":\"%s\",\"ValidationStatus\":\"%s\",\"ValidationMethod\":\"DNS\","
                           "\"ResourceRecord\":{\"Name\":\"_v%d%d.%s.\",\"Type\":\"CNAME\",\"Value\":\"_a%d%d.acm-validations.aws.\"}}",
                       i ? "," : "", c->names[i], c->names[i], strcmp(c->status, "ISSUED") == 0 ? "SUCCESS" : "PENDING_VALIDATION",
                       (int)(c - g_certs), i, c->names[i], (int)(c - g_certs), i);
            sb_add(&b, "]}}");
        }
        logf_("ACM DescribeCertificate %s %d", arn, status);
    } else if (strcmp(op, "RequestCertificate") == 0) {
        char domain[256], names[1024];
        json_str(r->body, "DomainName", domain, sizeof domain);
        copy(names, sizeof names, domain);
        const char *sans = strstr(r->body, "\"SubjectAlternativeNames\"");
        if (sans) {
            const char *p = strchr(sans + 25, '['), *end = p ? strchr(p, ']') : NULL;
            while (end && (p = strchr(p, '"')) && p < end) {
                const char *e = strchr(p + 1, '"');
                if (!e) break;
                char one[256];
                snprintf(one, sizeof one, "%.*s", (int)(e - p - 1), p + 1);
                if (strcmp(one, domain) != 0 && strlen(names) + strlen(one) + 2 < sizeof names) {
                    strcat(names, ",");
                    strcat(names, one);
                }
                p = e + 1;
            }
        }
        snprintf(arn, sizeof arn, "arn:aws:acm:us-east-1:%s:certificate/00000000-0000-0000-0000-%012d", g_account, g_ncerts + 1);
        cert_add(arn, "PENDING_VALIDATION", names);
        sb_add(&b, "{\"CertificateArn\":\"%s\"}", arn);
        char token[64];
        json_str(r->body, "IdempotencyToken", token, sizeof token);
        logf_("ACM RequestCertificate %s token=%s", names, token[0] ? token : "-");
    } else {
        status = 400;
        sb_add(&b, "{\"__type\":\"InvalidAction\",\"message\":\"not in the recording\"}");
        logf_("ACM %s 400", op);
    }
    pthread_mutex_unlock(&g_mu);
    respond_sb(fd, status, "application/x-amz-json-1.1", &b);
}

// ---- Route 53, CloudFront (REST, XML) ------------------------------------------------------------

#define R53_NS "https://route53.amazonaws.com/doc/2013-04-01/"

// The text of the first <tag>...</tag> at or after p, into out; the position after it, or NULL.
static const char *xml_text(const char *p, const char *tag, char *out, size_t cap) {
    sb open = { 0 }, close = { 0 };
    sb_add(&open, "<%s>", tag);
    sb_add(&close, "</%s>", tag);
    const char *s = p ? strstr(p, open.p) : NULL, *e = s ? strstr(s, close.p) : NULL, *after = NULL;
    out[0] = 0;
    if (s && e) {
        s += open.n;
        snprintf(out, cap, "%.*s", (int)(e - s), s);
        after = e + close.n;
    }
    free(open.p);
    free(close.p);
    return after;
}

static void route53(int fd, const request *r) {
    sb b = { 0 };
    sb_add(&b, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
    if (strcmp(r->method, "GET") == 0 && strcmp(r->path, "/2013-04-01/hostedzonesbyname") == 0) {
        char *dns = form_get(r->query, "dnsname");
        char want[300];
        snprintf(want, sizeof want, "%s.", dns ? dns : "");
        sb_add(&b, "<ListHostedZonesByNameResponse xmlns=\"" R53_NS "\"><HostedZones>");
        int k = 0;
        // The zones from the name on: the exact ones first (the order AWS's answer starts with).
        for (int pass = 0; pass < 2; pass++)
            for (int i = 0; i < g_nzones; i++) {
                bool exact = strcmp(g_zones[i].name, want) == 0;
                if (exact != (pass == 0)) continue;
                sb_add(&b, "<HostedZone><Id>/hostedzone/%s</Id><Name>%s</Name><CallerReference>fake</CallerReference>"
                           "<Config><PrivateZone>%s</PrivateZone></Config><ResourceRecordSetCount>2</ResourceRecordSetCount></HostedZone>",
                       g_zones[i].id, g_zones[i].name, g_zones[i].private_ ? "true" : "false");
                k++;
            }
        sb_add(&b, "</HostedZones><DNSName>%s</DNSName><IsTruncated>false</IsTruncated><MaxItems>10</MaxItems>"
                   "</ListHostedZonesByNameResponse>",
               dns ? dns : "");
        logf_("R53 ListHostedZonesByName %s %d", dns ? dns : "", k);
        free(dns);
        respond_sb(fd, 200, "text/xml", &b);
    } else if (strcmp(r->method, "POST") == 0 && strncmp(r->path, "/2013-04-01/hostedzone/", 23) == 0 && strstr(r->path, "/rrset")) {
        char zone[64];
        snprintf(zone, sizeof zone, "%.*s", (int)(strstr(r->path, "/rrset") - r->path - 23), r->path + 23);
        const char *p = r->body;
        pthread_mutex_lock(&g_mu);
        while (p && (p = strstr(p, "<Change>"))) {
            char action[32], name[300], type[16], value[300], alias[300];
            xml_text(p, "Action", action, sizeof action);
            xml_text(p, "Name", name, sizeof name);
            xml_text(p, "Type", type, sizeof type);
            const char *end = strstr(p, "</Change>");
            const char *v = xml_text(p, "Value", value, sizeof value);
            if (!v || (end && v > end)) value[0] = 0;
            const char *al = xml_text(p, "DNSName", alias, sizeof alias);
            if (!al || (end && al > end)) alias[0] = 0;
            logf_("R53 %s %s %s %s %s%s", action, zone, name, type, alias[0] ? "alias=" : "", alias[0] ? alias : value);
            // A validation record written: the certificate it proves is issued once all are.
            for (int c = 0; c < g_ncerts; c++)
                for (int i = 0; i < g_certs[c].nnames; i++) {
                    char rec[300];
                    snprintf(rec, sizeof rec, "_v%d%d.%s.", c, i, g_certs[c].names[i]);
                    if (strcmp(rec, name) == 0 || (strlen(name) + 1 == strlen(rec) && strncmp(rec, name, strlen(name)) == 0)) {
                        g_certs[c].written[i] = true;
                        cert_check_issued(&g_certs[c]);
                    }
                }
            p = end;
        }
        pthread_mutex_unlock(&g_mu);
        sb_add(&b, "<ChangeResourceRecordSetsResponse xmlns=\"" R53_NS "\"><ChangeInfo><Id>/change/C1FAKE</Id><Status>PENDING</Status>"
                   "<SubmittedAt>2026-11-09T12:00:00Z</SubmittedAt></ChangeInfo></ChangeResourceRecordSetsResponse>");
        respond_sb(fd, 200, "text/xml", &b);
    } else if (strcmp(r->method, "GET") == 0 && strncmp(r->path, "/2013-04-01/change/", 19) == 0) {
        sb_add(&b, "<GetChangeResponse xmlns=\"" R53_NS "\"><ChangeInfo><Id>/change/%s</Id><Status>INSYNC</Status>"
                   "<SubmittedAt>2026-11-09T12:00:00Z</SubmittedAt></ChangeInfo></GetChangeResponse>",
               r->path + 19);
        logf_("R53 GetChange %s INSYNC", r->path + 19);
        respond_sb(fd, 200, "text/xml", &b);
    } else {
        logf_("R53 %s %s 400", r->method, r->path);
        free(b.p);
        respond(fd, 400, "Bad Request", "text/xml", NULL, "", 0);
    }
}

// ListDistributions: each stack's distributions with their template's Aliases lines (the first is
// its Distribution, the second its ApiDistribution, id A<id>), and the foreign one (foreign-dist=
// ID:DOMAIN:HOST+HOST, a distribution no stack of the tool's made).
static char g_foreign_dist[512];
static void cf_dist_item(sb *b, const char *id, const char *domain, const char *aliases) {
    int n = 0;
    sb items = { 0 };
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s", aliases);
    for (char *save = NULL, *t = strtok_r(tmp, "+", &save); t; t = strtok_r(NULL, "+", &save), n++)
        sb_add(&items, "<CNAME>%s</CNAME>", t);
    sb_add(b, "<DistributionSummary><Id>%s</Id><DomainName>%s</DomainName><Aliases><Quantity>%d</Quantity>%s%s%s</Aliases></DistributionSummary>",
           id, domain, n, n ? "<Items>" : "", items.p ? items.p : "", n ? "</Items>" : "");
    free(items.p);
}
static void cf_list(sb *b) {
    int n = 0;
    sb list = { 0 };
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < g_nstacks; i++) {
        const char *t = g_stacks[i].template;
        for (int k = 0; t && (t = strstr(t, "        Aliases: [")); k++) {
            t += 18;
            const char *e = strchr(t, ']');
            if (!e) break;
            char al[512], id[80];
            snprintf(al, sizeof al, "%.*s", (int)(e - t), t);
            for (char *c = al; *c; c++) if (*c == ',') *c = '+';
            char *w = al, *o = al;
            for (; *w; w++) if (*w != ' ') *o++ = *w;
            *o = 0;
            snprintf(id, sizeof id, "%s%s", k ? "A" : "", g_stacks[i].dist_id);
            cf_dist_item(&list, id, g_stacks[i].dist_domain, al);
            n++;
        }
    }
    if (g_foreign_dist[0]) {
        char id[64] = "", dom[128] = "", hosts[300] = "";
        sscanf(g_foreign_dist, "%63[^:]:%127[^:]:%299s", id, dom, hosts);
        cf_dist_item(&list, id, dom, hosts);
        n++;
    }
    pthread_mutex_unlock(&g_mu);
    sb_add(b, "<DistributionList xmlns=\"http://cloudfront.amazonaws.com/doc/2020-05-31/\"><Marker></Marker><MaxItems>100</MaxItems>"
              "<IsTruncated>false</IsTruncated><Quantity>%d</Quantity>%s%s%s</DistributionList>",
           n, n ? "<Items>" : "", list.p ? list.p : "", n ? "</Items>" : "");
    free(list.p);
    logf_("CF ListDistributions %d", n);
}

static void cloudfront(int fd, const request *r) {
    sb b = { 0 };
    sb_add(&b, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
    if (strcmp(r->method, "GET") == 0 && strcmp(r->path, "/2020-05-31/distribution") == 0) {
        cf_list(&b);
        respond_sb(fd, 200, "text/xml", &b);
        return;
    }
    // AssociateAlias: the alias taken off the foreign distribution (the target's own template
    // names it once the stack update after the move runs).
    const char *aa = strstr(r->path, "/associate-alias");
    if (strcmp(r->method, "PUT") == 0 && strncmp(r->path, "/2020-05-31/distribution/", 25) == 0 && aa) {
        char *alias = form_get(r->query, "Alias");
        char id[64] = "", dom[128] = "", hosts[300] = "", kept[300] = "";
        pthread_mutex_lock(&g_mu);
        if (alias && g_foreign_dist[0]) {
            sscanf(g_foreign_dist, "%63[^:]:%127[^:]:%299s", id, dom, hosts);
            for (char *save = NULL, *t = strtok_r(hosts, "+", &save); t; t = strtok_r(NULL, "+", &save))
                if (strcmp(t, alias) != 0)
                    snprintf(kept + strlen(kept), sizeof kept - strlen(kept), "%s%s", kept[0] ? "+" : "", t);
            if (kept[0]) snprintf(g_foreign_dist, sizeof g_foreign_dist, "%s:%s:%s", id, dom, kept);
            else g_foreign_dist[0] = 0;
        }
        pthread_mutex_unlock(&g_mu);
        logf_("CF AssociateAlias %.*s %s", (int)(aa - r->path - 25), r->path + 25, alias ? alias : "-");
        free(alias);
        free(b.p);
        respond(fd, 200, "OK", "text/xml", NULL, "", 0);
        return;
    }
    const char *inv = strstr(r->path, "/invalidation");
    if (strncmp(r->path, "/2020-05-31/distribution/", 25) == 0 && inv) {
        char dist[64], paths[512];
        snprintf(dist, sizeof dist, "%.*s", (int)(inv - r->path - 25), r->path + 25);
        if (strcmp(r->method, "POST") == 0) {
            xml_text(r->body, "Items", paths, sizeof paths);
            logf_("CF CreateInvalidation %s %s", dist, paths);
            sb_add(&b, "<Invalidation xmlns=\"http://cloudfront.amazonaws.com/doc/2020-05-31/\"><Id>I1FAKE</Id><Status>InProgress</Status>"
                       "<CreateTime>2026-11-09T12:00:00Z</CreateTime><InvalidationBatch><Paths><Quantity>1</Quantity><Items>%s</Items>"
                       "</Paths><CallerReference>fake</CallerReference></InvalidationBatch></Invalidation>",
                   paths);
            respond_sb(fd, 201, "text/xml", &b);
        } else {
            logf_("CF GetInvalidation %s Completed", dist);
            sb_add(&b, "<Invalidation xmlns=\"http://cloudfront.amazonaws.com/doc/2020-05-31/\"><Id>I1FAKE</Id><Status>Completed</Status>"
                       "<CreateTime>2026-11-09T12:00:00Z</CreateTime><InvalidationBatch><Paths><Quantity>1</Quantity><Items><Path>/*</Path>"
                       "</Items></Paths><CallerReference>fake</CallerReference></InvalidationBatch></Invalidation>");
            respond_sb(fd, 200, "text/xml", &b);
        }
        return;
    }
    logf_("CF %s %s 400", r->method, r->path);
    free(b.p);
    respond(fd, 400, "Bad Request", "text/xml", NULL, "", 0);
}

// ---- EC2 (Query, XML) ------------------------------------------------------------------------------

#define EC2_NS "http://ec2.amazonaws.com/doc/2016-11-15/"

// The recorded instance types: name, MiB, architecture.
static const struct {
    const char *name;
    int         mib;
    const char *arch;
} TYPES[] = {
    { "t4g.nano", 512, "arm64" }, { "t4g.micro", 1024, "arm64" }, { "t4g.small", 2048, "arm64" }, { "t3.nano", 512, "x86_64" },
};

static void ec2_error(int fd, const char *code, const char *msg) {
    sb b = { 0 };
    sb_add(&b, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<Response><Errors><Error><Code>%s</Code><Message>%s</Message></Error></Errors>"
               "<RequestID>FAKE0000</RequestID></Response>",
           code, msg);
    respond(fd, 400, "Bad Request", "text/xml", NULL, b.p, b.n);
    free(b.p);
}

static void ec2(int fd, const request *r) {
    char *action = form_get(r->body, "Action");
    const char *a = action ? action : "";
    sb b = { 0 };
    sb_add(&b, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<%sResponse xmlns=\"" EC2_NS "\"><requestId>FAKE0000</requestId>", a);
    if (strcmp(a, "DescribeInstances") == 0) {
        char *id = form_get(r->body, "InstanceId.1");
        bool known = false;
        pthread_mutex_lock(&g_mu);
        for (int i = 0; i < g_nres && id; i++) {
            const char *slash = strrchr(g_res[i].arn, '/');
            if (g_res[i].arn[0] && strcmp(g_res[i].type, "ec2:instance") == 0 && slash && strcmp(slash + 1, id) == 0) known = true;
        }
        const char *arch = "arm64";
        for (size_t i = 0; i < sizeof TYPES / sizeof TYPES[0]; i++)
            if (strcmp(TYPES[i].name, g_instance_type) == 0) arch = TYPES[i].arch;
        char type[32];
        copy(type, sizeof type, g_instance_type);
        pthread_mutex_unlock(&g_mu);
        logf_("EC2 DescribeInstances %s %s", id ? id : "-", known ? type : "unknown");
        if (!known) {
            free(b.p);
            ec2_error(fd, "InvalidInstanceID.NotFound", "The instance ID does not exist");
            free(id), free(action);
            return;
        }
        sb_add(&b, "<reservationSet><item><reservationId>r-0fake</reservationId><ownerId>%s</ownerId><instancesSet><item>"
                   "<instanceId>%s</instanceId><instanceType>%s</instanceType><instanceState><code>16</code><name>running</name>"
                   "</instanceState><dnsName>ec2-%s.compute-1.amazonaws.com</dnsName><architecture>%s</architecture></item>"
                   "</instancesSet></item></reservationSet>",
               g_account, id, type, id, arch);
        free(id);
    } else if (strcmp(a, "DescribeInstanceTypes") == 0) {
        char *t = form_get(r->body, "InstanceType.1");
        int k = -1;
        for (size_t i = 0; t && i < sizeof TYPES / sizeof TYPES[0]; i++)
            if (strcmp(TYPES[i].name, t) == 0) k = (int)i;
        logf_("EC2 DescribeInstanceTypes %s %s", t ? t : "-", k < 0 ? "unknown" : "ok");
        free(t);
        if (k < 0) {
            free(b.p);
            ec2_error(fd, "InvalidInstanceType", "The following supplied instance types do not exist");
            free(action);
            return;
        }
        sb_add(&b, "<instanceTypeSet><item><instanceType>%s</instanceType><memoryInfo><sizeInMiB>%d</sizeInMiB></memoryInfo>"
                   "<processorInfo><supportedArchitectures><item>%s</item></supportedArchitectures></processorInfo></item></instanceTypeSet>",
               TYPES[k].name, TYPES[k].mib, TYPES[k].arch);
    } else if (strcmp(a, "DescribeManagedPrefixLists") == 0) {
        // CloudFront's two origin-facing lists, by the name the filter asks for.
        char *n = form_get(r->body, "Filter.1.Value.1");
        const char *id = !n ? NULL
                         : strcmp(n, "com.amazonaws.global.cloudfront.origin-facing") == 0      ? "pl-3b927c52"
                         : strcmp(n, "com.amazonaws.global.ipv6.cloudfront.origin-facing") == 0 ? "pl-0a6b6e1c"
                                                                                                : NULL;
        logf_("EC2 DescribeManagedPrefixLists %s %s", n ? n : "-", id ? id : "none");
        sb_add(&b, "<prefixListSet>");
        if (id) {
            sb_add(&b, "<item><prefixListId>%s</prefixListId><prefixListName>", id);
            sb_xml(&b, n);
            sb_add(&b, "</prefixListName></item>");
        }
        sb_add(&b, "</prefixListSet>");
        free(n);
    } else if (strcmp(a, "DescribeInstanceTypeOfferings") == 0) {
        // Every type in two zones, listed out of order (the tool sorts).
        char *t = form_get(r->body, "Filter.1.Value.1");
        logf_("EC2 DescribeInstanceTypeOfferings %s", t ? t : "-");
        sb_add(&b, "<instanceTypeOfferingSet>");
        for (int i = 0; t && i < 2; i++) {
            sb_add(&b, "<item><instanceType>");
            sb_xml(&b, t);
            sb_add(&b, "</instanceType><locationType>availability-zone</locationType><location>us-east-1%c</location></item>", i ? 'a' : 'b');
        }
        sb_add(&b, "</instanceTypeOfferingSet>");
        free(t);
    } else {
        logf_("EC2 %s 400", a);
        free(b.p);
        ec2_error(fd, "InvalidAction", "not in the recording");
        free(action);
        return;
    }
    sb_add(&b, "</%sResponse>", a);
    respond_sb(fd, 200, "text/xml", &b);
    free(action);
}

// ---- SSM (JSON 1.1) -------------------------------------------------------------------------------

// Text made safe inside a JSON string.
static void sb_json(sb *b, const char *s) {
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') sb_add(b, "\\%c", *s);
        else if (*s == '\n') sb_add(b, "\\n");
        else if ((unsigned char)*s < 0x20) sb_add(b, "\\u%04x", *s);
        else sb_add(b, "%c", *s);
    }
}

static void ssm(int fd, const request *r, const char *op) {
    sb b = { 0 };
    int status = 200;
    pthread_mutex_lock(&g_mu);
    if (strcmp(op, "GetParameter") == 0) {
        char name[256];
        json_str(r->body, "Name", name, sizeof name);
        size_t nl = strlen(name), sl = strlen("/CBASE_ORIGIN_SECRET");
        if (nl > sl && strcmp(name + nl - sl, "/CBASE_ORIGIN_SECRET") == 0) {
            if (!g_origin_secret[0]) {
                status = 400;
                sb_add(&b, "{\"__type\":\"ParameterNotFound\",\"message\":\"\"}");
            } else {
                sb_add(&b, "{\"Parameter\":{\"Name\":\"%s\",\"Type\":\"SecureString\",\"Value\":\"%s\",\"Version\":1}}", name, g_origin_secret);
            }
        } else {
            sb_add(&b, "{\"Parameter\":{\"Name\":\"%s\",\"Type\":\"String\",\"Value\":\"ami-0f4ce1234567890ab\",\"Version\":1}}", name);
        }
        logf_("SSM GetParameter %s %d", name, status);
    } else if (strcmp(op, "SendCommand") == 0) {
        // {"InstanceIds": ["i-..."], ..., "Parameters": {"commands": ["<one line>"]}}
        const char *ids = strstr(r->body, "\"InstanceIds\"");
        char instance[64] = "";
        if (ids) {
            const char *q = strchr(ids + 13, '"');
            const char *e = q ? strchr(q + 1, '"') : NULL;
            if (q && e && (size_t)(e - q - 1) < sizeof instance) snprintf(instance, sizeof instance, "%.*s", (int)(e - q - 1), q + 1);
        }
        bool known = false;
        for (int i = 0; i < g_nres; i++) {
            const char *slash = strrchr(g_res[i].arn, '/');
            if (g_res[i].arn[0] && strcmp(g_res[i].type, "ec2:instance") == 0 && slash && strcmp(slash + 1, instance) == 0) known = true;
        }
        const char *c = strstr(r->body, "\"commands\"");
        const char *q = c ? strstr(c, "[\"") : NULL;
        const char *e = q ? strstr(q + 2, "\"]") : NULL;
        char comment[256];
        json_str(r->body, "Comment", comment, sizeof comment);
        if (!known) {
            status = 400;
            sb_add(&b, "{\"__type\":\"InvalidInstanceId\",\"Message\":\"Instances not in a valid state for account\"}");
            logf_("SSM SendCommand %s InvalidInstanceId", instance);
        } else {
            if (g_ncmds == MAX_CMDS) die("too many commands");
            command *cm = &g_cmds[g_ncmds++];
            memset(cm, 0, sizeof *cm);
            snprintf(cm->id, sizeof cm->id, "c0ffee00-0000-4000-8000-%012d", g_ncmds);
            copy(cm->instance, sizeof cm->instance, instance);
            copy(cm->status, sizeof cm->status, g_ssm_status);
            cm->code = g_ssm_code;
            sb out = { 0 };
            sb_add(&out, "%s", g_ssm_out);
            if (g_secret[0]) sb_add(&out, "DB_PASSWORD=%s\nCBASE_ORIGIN_SECRET='%s'\n", g_secret, g_secret2);
            cm->out = out.p ? out.p : calloc(1, 1);
            sb_add(&b, "{\"Command\":{\"CommandId\":\"%s\",\"DocumentName\":\"AWS-RunShellScript\",\"Status\":\"Pending\"}}", cm->id);
            logf_("SSM SendCommand %s %s [%s] %.*s", instance, cm->id, comment, q && e ? (int)(e - q - 2) : 0, q && e ? q + 2 : "");
        }
    } else if (strcmp(op, "GetCommandInvocation") == 0) {
        char id[64];
        json_str(r->body, "CommandId", id, sizeof id);
        command *cm = NULL;
        for (int i = 0; i < g_ncmds; i++)
            if (strcmp(g_cmds[i].id, id) == 0) cm = &g_cmds[i];
        if (!cm) {
            status = 400;
            sb_add(&b, "{\"__type\":\"InvocationDoesNotExist\",\"Message\":\"\"}");
        } else {
            bool first = cm->reads++ == 0;   // the first read finds it running
            sb_add(&b, "{\"CommandId\":\"%s\",\"InstanceId\":\"%s\",\"DocumentName\":\"AWS-RunShellScript\",\"Status\":\"%s\","
                       "\"ResponseCode\":%d,\"StandardOutputContent\":\"",
                   cm->id, cm->instance, first ? "InProgress" : cm->status, first ? -1 : cm->code);
            if (!first) sb_json(&b, cm->out);
            sb_add(&b, "\",\"StandardErrorContent\":\"");
            if (!first && g_secret[0]) sb_json(&b, g_secret);
            sb_add(&b, "\"}");
            logf_("SSM GetCommandInvocation %s %s", cm->id, first ? "InProgress" : cm->status);
        }
    } else {
        status = 400;
        sb_add(&b, "{\"__type\":\"InvalidAction\",\"message\":\"not in the recording\"}");
        logf_("SSM %s 400", op);
    }
    pthread_mutex_unlock(&g_mu);
    respond_sb(fd, status, "application/x-amz-json-1.1", &b);
}

// ---- control, and the dispatch -------------------------------------------------------------------

// POST /_fake/gather?n=N, and POST /_fake/set?<knob>=<value> for the CloudFormation scripts.
static void control(int fd, const request *r) {
    if (strcmp(r->path, "/_fake/template") == 0) {   // GET /_fake/template?stack=NAME: the template it holds
        char *name = form_get(r->query, "stack");
        sb b = { 0 };
        pthread_mutex_lock(&g_mu);
        stack *st = name ? stack_find(name) : NULL;
        sb_add(&b, "%s", st && st->template ? st->template : "(none)\n");
        pthread_mutex_unlock(&g_mu);
        free(name);
        respond_sb(fd, 200, "text/plain", &b);
        return;
    }
    if (strcmp(r->path, "/_fake/set") == 0) {
        pthread_mutex_lock(&g_mu);
        char *v;
        if ((v = form_get(r->query, "changes"))) copy(g_changes, sizeof g_changes, v), free(v);
        if ((v = form_get(r->query, "drift"))) g_drift = atoi(v), free(v);
        if ((v = form_get(r->query, "stack-fail"))) g_stack_fail = atoi(v), free(v);
        if ((v = form_get(r->query, "cs-fail"))) copy(g_cs_fail, sizeof g_cs_fail, v), free(v);
        if ((v = form_get(r->query, "stack-status"))) {   // NAME:STATUS, that stack's status set as it is
            char *c = strchr(v, ':');
            stack *st = c ? (*c = 0, stack_find(v)) : NULL;
            if (st) copy(st->status, sizeof st->status, c + 1);
            free(v);
        }
        if ((v = form_get(r->query, "secret"))) copy(g_secret, sizeof g_secret, v), free(v);
        if ((v = form_get(r->query, "secret2"))) copy(g_secret2, sizeof g_secret2, v), free(v);
        if ((v = form_get(r->query, "origin-secret"))) copy(g_origin_secret, sizeof g_origin_secret, v), free(v);
        if ((v = form_get(r->query, "foreign-dist"))) copy(g_foreign_dist, sizeof g_foreign_dist, v), free(v);
        if ((v = form_get(r->query, "fail-execute"))) g_fail_execute = atoi(v), free(v);
        if ((v = form_get(r->query, "list-ignores-prefix"))) g_list_ignores_prefix = atoi(v), free(v);
        if ((v = form_get(r->query, "acm-deny"))) g_acm_deny = atoi(v), free(v);
        if ((v = form_get(r->query, "instance-type"))) copy(g_instance_type, sizeof g_instance_type, v), free(v);
        if ((v = form_get(r->query, "ssm-status"))) copy(g_ssm_status, sizeof g_ssm_status, v), free(v);
        if ((v = form_get(r->query, "ssm-code"))) g_ssm_code = atoi(v), free(v);
        if ((v = form_get(r->query, "ssm-out"))) copy(g_ssm_out, sizeof g_ssm_out, v), free(v);
        pthread_mutex_unlock(&g_mu);
        logf_("SET %s", r->query);
        respond(fd, 200, "OK", "text/plain", NULL, "ok\n", 3);
        return;
    }
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

// The service a request was signed for: the credential scope's fourth part
// ("Credential=<key>/<date>/<region>/<service>/aws4_request").
static void service_of(const char *auth, char *out, size_t cap) {
    out[0] = 0;
    const char *c = strstr(auth, "Credential=");
    if (!c) return;
    const char *p = c;
    for (int i = 0; i < 3 && p; i++) p = strchr(p + 1, '/');
    const char *e = p ? strchr(p + 1, '/') : NULL;
    if (p && e && (size_t)(e - p - 1) < cap) snprintf(out, cap, "%.*s", (int)(e - p - 1), p + 1);
}

static void handle(int fd, request *r) {
    const char *target = header(r, "X-Amz-Target");
    const char *auth = header(r, "Authorization");
    if (strncmp(r->path, "/_fake/", 7) == 0) {
        control(fd, r);
        return;
    }
    if (!auth || !strstr(auth, "AWS4-HMAC-SHA256 Credential=AKIACBASEFAKE0000000/")) {
        // Signed with the test's dummy credentials, or refused: a call that found other credentials
        // (a developer's real ones) must never get an answer.
        logf_("%s %s 403 unsigned", r->method, r->path);
        s3_error(fd, 403, "Forbidden", "InvalidAccessKeyId", "The AWS Access Key Id you provided does not exist in our records.");
        return;
    }
    char service[32];
    service_of(auth, service, sizeof service);
    if (strcmp(service, "tagging") == 0 && target && strcmp(target, "ResourceGroupsTaggingAPI_20170126.GetResources") == 0) {
        tagging(fd, r);
    } else if (strcmp(service, "sts") == 0) {
        sts(fd, r);
    } else if (strcmp(service, "cloudformation") == 0) {
        cloudformation(fd, r);
    } else if (strcmp(service, "acm") == 0 && target && strncmp(target, "CertificateManager.", 19) == 0) {
        acm(fd, r, target + 19);
    } else if (strcmp(service, "route53") == 0) {
        route53(fd, r);
    } else if (strcmp(service, "cloudfront") == 0) {
        cloudfront(fd, r);
    } else if (strcmp(service, "ec2") == 0) {
        ec2(fd, r);
    } else if (strcmp(service, "ssm") == 0 && target && strncmp(target, "AmazonSSM.", 10) == 0) {
        ssm(fd, r, target + 10);
    } else if (strcmp(service, "s3") == 0) {
        // The key as stored: the path decoded.
        char *dec = urldec(r->path, strlen(r->path), false);
        copy(r->path, sizeof r->path, dec);
        free(dec);
        bool bucket_only = !strchr(r->path + 1, '/');
        if (bucket_only && strcmp(r->method, "GET") != 0) s3_bucket_call(fd, r);
        else if (strcmp(r->method, "GET") == 0 && bucket_only) s3_bucket(fd, r);
        else if (strcmp(r->method, "GET") == 0) s3_get(fd, r, false);
        else if (strcmp(r->method, "HEAD") == 0) s3_get(fd, r, true);
        else if (strcmp(r->method, "PUT") == 0) s3_put(fd, r);
        else if (strcmp(r->method, "DELETE") == 0) s3_delete(fd, r);
        else {
            logf_("%s %s 405", r->method, r->path);
            s3_error(fd, 405, "Method Not Allowed", "MethodNotAllowed", "not in the recording");
        }
    } else {
        logf_("%s %s 400 service=%s", r->method, r->path, service);
        s3_error(fd, 400, "Bad Request", "UnknownService", "not in the recording");
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
            char name[64] = "", region[32] = "us-east-1";
            if (sscanf(v, "%63[^:]:%31s", name, region) < 1) die("--bucket is NAME or NAME:REGION");
            bucket_add(name, region);
        } else if (strcmp(a, "--cert") == 0) {
            char arn[256], status[32], names[1024];
            if (sscanf(v, "%255s %31s %1023s", arn, status, names) != 3) die("--cert is 'ARN STATUS NAME,NAME...'");
            cert_add(arn, status, names);
        } else if (strcmp(a, "--zone") == 0) {
            if (g_nzones == MAX_ZONES) die("too many zones");
            zone *z = &g_zones[g_nzones++];
            char id[64], name[250], priv[16] = "";
            if (sscanf(v, "%63s %249s %15s", id, name, priv) < 2) die("--zone is 'ID NAME [private]'");
            copy(z->id, sizeof z->id, id);
            snprintf(z->name, sizeof z->name, "%s.", name);
            z->private_ = strcmp(priv, "private") == 0;
        } else if (strcmp(a, "--resource") == 0) {
            if (g_nres == MAX_RES) die("too many resources");
            resource *x = &g_res[g_nres++];
            if (sscanf(v, "%127s %127s %63s %255s", x->key, x->value, x->type, x->arn) != 4)
                die("--resource is 'KEY VALUE TYPE ARN'");
        } else die("unknown argument %s", a);
    }
    if (!port_file || !log_path) die("usage: fake_aws --port-file F --log L [--account A] [--bucket B[:R]]... [--resource 'K V T ARN']... [--cert 'A S N,N']... [--zone 'ID NAME']...");
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
