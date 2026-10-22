// auth_diff.c - the differential proof of a change to the session routes (/v1/auth/*): two
// `cbase serve` binaries, each with its own fresh store and the same settings, are sent the same
// requests, one to each in turn, and every answer is compared after the parts that are random by
// design are named instead of compared (a token's bytes, a user id, an expiry's instant). What is
// compared: the status, every header but Date, and the body's bytes (a refusal's ten exactly; a
// session's layout, its lengths, its name, its user by order of appearance, and its expiry against
// the TTL). c_base/tools/auth_route_diff.sh builds the base binary and runs this.
//
//   build/test/auth_diff <base cbase> <new cbase>
//
// Exit 0 when every answer agreed (the transcript is printed either way), 1 when one did not.
// The request bodies are written here byte by byte, in the layout cbase/auth_wire.h states, so the
// proof does not lean on the codec it checks.
#include "lib_client.h"
#include "lib_proc.h"

#include "cbase/auth_wire.h"
#include "util/str.h"

#include "../../../shared/c/le_bytes.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define ORIGIN_SECRET "auth-diff-origin-secret-0123456789"
#define TTL_S 3600

typedef struct {
    const char *bin;
    tp_child    child;
    int         port;
    char        access[CBW_ACCESS_MAX + 1];   // the last session's
    char        refresh[CBW_REFRESH_MAX + 1];
    char        access_b[CBW_ACCESS_MAX + 1]; // a second account's
    char        ids[16][CBW_ID_MAX + 1];      // users by order of appearance
    int         n_ids;
} server;

static server g_s[2];
static int g_diffs, g_steps;
// The viewer's address the edge names (config.h CB_VIEWER_HEADER, honoured with the origin secret):
// each phase below speaks from its own, so one phase's rate limits do not decide the next's.
static int g_addr = 1;

static int start(server *s, const char *dbdir_tag) {
    static char envdb[2][512];
    int i = (int)(s - g_s);
    char dir[400];
    if (tp_tempdir(dir, sizeof dir) != 0) return -1;
    snprintf(envdb[i], sizeof envdb[i], "CBASE_DB=%s/%s.sqlite", dir, dbdir_tag);
    char *env[] = { "CBASE_DEV=1", "CBASE_LISTEN=127.0.0.1:0", envdb[i], "CBASE_JWT_SECRET=auth-diff-secret-auth-diff-secret-0123",
                    "CBASE_JWT_TTL=3600", "CBASE_ORIGIN_SECRET=" ORIGIN_SECRET, "CBASE_ALLOWED_ORIGINS=https://app.example",
                    "CBASE_ADMIN_TOKEN=auth-diff-admin-token-0123456789", NULL };
    char *argv[] = { (char *)s->bin, "serve", NULL };
    if (tp_start(&s->child, argv, env) != 0) return -1;
    char line[512];
    if (tp_wait_line(&s->child, "listening on", 30000, line, sizeof line) != 0) return -1;
    const char *colon = strrchr(line, ':');
    s->port = colon ? atoi(colon + 1) : 0;
    return s->port > 0 ? 0 : -1;
}

// ---- bodies, in cbase/auth_wire.h's layout ------------------------------------------------

typedef struct {
    uint8_t b[512];
    size_t  n;
} body;

static body raw(const uint8_t *p, size_t n) {
    body b;
    memcpy(b.b, p, n);
    b.n = n;
    return b;
}
static void put8(body *b, uint8_t v) { b->b[b->n++] = v; }
static void put_str8(body *b, const char *s, size_t n) {
    put8(b, (uint8_t)n);
    memcpy(b->b + b->n, s, n);
    b->n += n;
}
static body creds(uint8_t kind, const char *name, size_t nn, const char *pw, size_t pn) {
    body b = { .n = 0 };
    put8(&b, CBW_V);
    put8(&b, kind);
    put_str8(&b, name, nn);
    put_str8(&b, pw, pn);
    return b;
}
static body cred(uint8_t kind, const char *name, const char *pw) { return creds(kind, name, strlen(name), pw, strlen(pw)); }
static body refresh_body(const char *tok) {
    body b = { .n = 0 };
    put8(&b, CBW_V);
    put8(&b, CBW_REFRESH);
    put_str8(&b, tok, strlen(tok));
    return b;
}
static body signout_body(uint8_t scope) {
    body b = { .n = 0 };
    put8(&b, CBW_V);
    put8(&b, CBW_SIGNOUT);
    put8(&b, scope);
    return b;
}
static body password_body(const char *cur, const char *pw) {
    body b = { .n = 0 };
    put8(&b, CBW_V);
    put8(&b, CBW_PASSWORD);
    put_str8(&b, cur, strlen(cur));
    put_str8(&b, pw, strlen(pw));
    return b;
}

// ---- one request to each ------------------------------------------------------------------

typedef enum { TOK_NONE, TOK_LAST, TOK_SECOND, TOK_GARBAGE } tok;
typedef enum { REF_GIVEN, REF_LAST } ref;

// The answer, normalized: what is compared.
static void normalize(server *s, const tc_response *r, cb_buf *out, int keep) {
    char line[4096];
    snprintf(line, sizeof line, "status %d\n", r->status);
    cb_buf_append(out, line, strlen(line));
    // Every header but Date, in order (the head's lines after the status line).
    const char *h = (const char *)r->head.p;
    size_t hn = r->head.len;
    const char *eol = memchr(h, '\n', hn);
    const char *p = eol ? eol + 1 : h + hn;
    while (p < h + hn) {
        const char *e = memchr(p, '\n', (size_t)(h + hn - p));
        size_t len = e ? (size_t)(e - p) : (size_t)(h + hn - p);
        if (len && p[len - 1] == '\r') len--;
        if (len && strncasecmp(p, "Date:", 5) != 0) {
            cb_buf_append(out, "  ", 2);
            cb_buf_append(out, p, len);
            cb_buf_append(out, "\n", 1);
        }
        if (!e) break;
        p = e + 1;
    }
    const uint8_t *b = r->body.p;
    size_t n = r->body.len;
    if (r->status == 200 && n >= 4 && b[0] == CBW_V && b[1] == CBW_SESSION) {
        // [V][SESSION] str16 access, str8 refresh, u48 expires_at, str8 user id, str8 name
        size_t at = 2, an = le_get_u16(b + at);
        at += 2;
        const uint8_t *acc = b + at;
        at += an;
        size_t rn = at < n ? b[at] : 0;
        const uint8_t *rf = b + at + 1;
        at += 1 + rn;
        uint64_t exp = at + 6 <= n ? le_get_u48(b + at) : 0;
        at += 6;
        size_t un = at < n ? b[at] : 0;
        const uint8_t *uid = b + at + 1;
        at += 1 + un;
        size_t nn = at < n ? b[at] : 0;
        const uint8_t *nm = b + at + 1;
        at += 1 + nn;
        int64_t left = (int64_t)(exp / 1000) - (int64_t)time(NULL);
        char id[CBW_ID_MAX + 1] = "";
        if (un <= CBW_ID_MAX) { memcpy(id, uid, un); id[un] = 0; }
        int label = -1;
        for (int i = 0; i < s->n_ids; i++)
            if (strcmp(s->ids[i], id) == 0) label = i;
        if (label < 0 && s->n_ids < 16) { label = s->n_ids; cb_strlcpy(s->ids[s->n_ids++], id, sizeof s->ids[0]); }
        snprintf(line, sizeof line, "  session access_len=%zu refresh_len=%zu expiry=%s user=u%d name=%.*s whole=%s\n", an, rn,
                 left > TTL_S - 30 && left <= TTL_S + 1 ? "ttl" : "OFF", label, (int)nn, (const char *)nm, at == n ? "yes" : "NO");
        cb_buf_append(out, line, strlen(line));
        if (keep == 1 && an <= CBW_ACCESS_MAX && rn <= CBW_REFRESH_MAX) {
            memcpy(s->access, acc, an);
            s->access[an] = 0;
            memcpy(s->refresh, rf, rn);
            s->refresh[rn] = 0;
        } else if (keep == 2 && an <= CBW_ACCESS_MAX) {
            memcpy(s->access_b, acc, an);
            s->access_b[an] = 0;
        }
        return;
    }
    cb_buf_append(out, "  body ", 7);
    for (size_t i = 0; i < n && i < 256; i++) {
        snprintf(line, sizeof line, "%02x", b[i]);
        cb_buf_append(out, line, 2);
    }
    snprintf(line, sizeof line, " (%zu bytes)\n", n);
    cb_buf_append(out, line, strlen(line));
}

// One step: the same request to both servers (base first), compared. keep: 1 holds the session as
// the last, 2 as the second account's.
static void step(const char *what, const char *method, const char *path, tok t, const body *bd, int keep) {
    cb_buf got[2];
    for (int i = 0; i < 2; i++) {
        server *s = &g_s[i];
        cb_buf_init(&got[i]);
        char headers[CBW_ACCESS_MAX + 512];
        int h = snprintf(headers, sizeof headers,
                         "X-Cbase-Origin: %s\r\nOrigin: https://app.example\r\nCloudFront-Viewer-Address: 203.0.113.%d:443\r\n",
                         ORIGIN_SECRET, g_addr);
        const char *bearer = t == TOK_LAST ? s->access : t == TOK_SECOND ? s->access_b : t == TOK_GARBAGE ? "not-a-token" : NULL;
        if (bearer) h += snprintf(headers + h, sizeof headers - (size_t)h, "Authorization: Bearer %s\r\n", bearer);
        if (strcmp(method, "POST") == 0) h += snprintf(headers + h, sizeof headers - (size_t)h, "Content-Type: application/octet-stream\r\n");
        if (strcmp(method, "OPTIONS") == 0)
            h += snprintf(headers + h, sizeof headers - (size_t)h,
                          "Access-Control-Request-Method: POST\r\nAccess-Control-Request-Headers: authorization, content-type\r\n");
        tc_response r;
        if (tc_http(s->port, method, path, headers, bd ? bd->b : NULL, bd ? bd->n : 0, &r) != 0) {
            cb_buf_append(&got[i], "no answer\n", 10);
            continue;
        }
        normalize(s, &r, &got[i], keep);
        tc_response_free(&r);
    }
    g_steps++;
    bool same = got[0].len == got[1].len && memcmp(got[0].p, got[1].p, got[0].len) == 0;
    printf("%s %s %s [%s]\n%.*s", same ? "same" : "DIFF", method, path, what, (int)got[0].len, (const char *)got[0].p);
    if (!same) {
        g_diffs++;
        printf("  ---- the new binary answered:\n%.*s", (int)got[1].len, (const char *)got[1].p);
    }
    cb_buf_free(&got[0]);
    cb_buf_free(&got[1]);
}

static void post(const char *what, const char *path, tok t, body b, int keep) { step(what, "POST", path, t, &b, keep); }

// A refresh with each server's own last refresh token.
static void refresh_last(const char *what, int keep) {
    char tok0[CBW_REFRESH_MAX + 1], tok1[CBW_REFRESH_MAX + 1];
    cb_strlcpy(tok0, g_s[0].refresh, sizeof tok0);
    cb_strlcpy(tok1, g_s[1].refresh, sizeof tok1);
    // Each server is sent its own token (step sends one body to both).
    cb_buf got[2];
    for (int i = 0; i < 2; i++) {
        server *s = &g_s[i];
        body b = refresh_body(i == 0 ? tok0 : tok1);
        cb_buf_init(&got[i]);
        char headers[256];
        snprintf(headers, sizeof headers,
                 "X-Cbase-Origin: %s\r\nOrigin: https://app.example\r\nCloudFront-Viewer-Address: 203.0.113.%d:443\r\n"
                 "Content-Type: application/octet-stream\r\n",
                 ORIGIN_SECRET, g_addr);
        tc_response r;
        if (tc_http(s->port, "POST", "/v1/auth/refresh", headers, b.b, b.n, &r) != 0) {
            cb_buf_append(&got[i], "no answer\n", 10);
            continue;
        }
        normalize(s, &r, &got[i], keep);
        tc_response_free(&r);
    }
    g_steps++;
    bool same = got[0].len == got[1].len && memcmp(got[0].p, got[1].p, got[0].len) == 0;
    printf("%s POST /v1/auth/refresh [%s]\n%.*s", same ? "same" : "DIFF", what, (int)got[0].len, (const char *)got[0].p);
    if (!same) {
        g_diffs++;
        printf("  ---- the new binary answered:\n%.*s", (int)got[1].len, (const char *)got[1].p);
    }
    cb_buf_free(&got[0]);
    cb_buf_free(&got[1]);
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: auth_diff <base cbase> <new cbase>\n");
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);
    g_s[0].bin = argv[1];
    g_s[1].bin = argv[2];
    if (start(&g_s[0], "base") != 0 || start(&g_s[1], "new") != 0) {
        fprintf(stderr, "auth_diff: a server did not start\n");
        return 2;
    }
    const char *SU = "/v1/auth/signup", *SI = "/v1/auth/signin", *RF = "/v1/auth/refresh", *SO = "/v1/auth/signout",
               *PW = "/v1/auth/password";

    // The router's own answers on these paths.
    step("preflight", "OPTIONS", SI, TOK_NONE, NULL, 0);
    step("preflight of an unrouted auth path", "OPTIONS", "/v1/auth/nothing", TOK_NONE, NULL, 0);
    step("GET of a POST route", "GET", SI, TOK_NONE, NULL, 0);
    step("unrouted auth path", "POST", "/v1/auth/nothing", TOK_NONE, NULL, 0);

    // Malformed bodies.
    post("empty body", SU, TOK_NONE, raw((const uint8_t *)"", 0), 0);
    post("version only", SU, TOK_NONE, raw((const uint8_t *)"\x01", 1), 0);
    post("wrong version", SU, TOK_NONE, raw((const uint8_t *)"\x02\x01\x01" "A" "\x01" "b", 7), 0);
    post("sign-in body at sign-up", SU, TOK_NONE, cred(CBW_SIGNIN, "ALICE", "correct-horse-1"), 0);
    post("sign-up body at sign-in", SI, TOK_NONE, cred(CBW_SIGNUP, "ALICE", "correct-horse-1"), 0);
    post("a refresh body at sign-in", SI, TOK_NONE, refresh_body("x"), 0);
    {
        body b = cred(CBW_SIGNUP, "ALICE", "correct-horse-1");
        b.b[b.n++] = 0;
        post("a byte over", SU, TOK_NONE, b, 0);
        b.n -= 2;
        post("a byte short", SU, TOK_NONE, b, 0);
    }
    {
        char longname[CBW_NAME_MAX + 2];
        memset(longname, 'A', sizeof longname);
        post("a name past its field", SU, TOK_NONE, creds(CBW_SIGNUP, longname, CBW_NAME_MAX + 1, "correct-horse-1", 15), 0);
        post("a name at its field (64 bytes, past the 32 characters)", SU, TOK_NONE,
             creds(CBW_SIGNUP, longname, CBW_NAME_MAX, "correct-horse-1", 15), 0);
        char longpw[CBW_PASSWORD_MAX + 2];
        memset(longpw, 'p', sizeof longpw);
        post("a password past its field", SU, TOK_NONE, creds(CBW_SIGNUP, "LONGPW", 6, longpw, CBW_PASSWORD_MAX + 1), 0);
    }
    post("a NUL in the name", SU, TOK_NONE, creds(CBW_SIGNUP, "AL\0CE", 5, "correct-horse-1", 15), 0);
    post("a NUL in the password", SU, TOK_NONE, creds(CBW_SIGNUP, "ALICE", 5, "correct\0horse-1", 15), 0);

    // Sign-up's rules.
    g_addr = 2;
    post("a bot's prefix", SU, TOK_NONE, cred(CBW_SIGNUP, "%EVE", "correct-horse-1"), 0);
    post("not UTF-8", SU, TOK_NONE, cred(CBW_SIGNUP, "\xff\xfe", "correct-horse-1"), 0);
    post("an empty name", SU, TOK_NONE, cred(CBW_SIGNUP, "", "correct-horse-1"), 0);
    post("mixed scripts", SU, TOK_NONE, cred(CBW_SIGNUP, "A\xd0\x90", "correct-horse-1"), 0);
    post("a short password", SU, TOK_NONE, cred(CBW_SIGNUP, "CAROL", "short"), 0);
    post("a common password", SU, TOK_NONE, cred(CBW_SIGNUP, "CAROL", "password123"), 0);
    post("the name as the password", SU, TOK_NONE, cred(CBW_SIGNUP, "CAROLINE1", "caroline1"), 0);

    // Accounts.
    g_addr = 3;
    post("sign up ALICE", SU, TOK_NONE, cred(CBW_SIGNUP, "ALICE", "correct-horse-1"), 1);
    post("sign up alice again (the case step)", SU, TOK_NONE, cred(CBW_SIGNUP, "alice", "correct-horse-2"), 0);
    post("sign up BOB", SU, TOK_NONE, cred(CBW_SIGNUP, "BOB", "battery-staple-2"), 2);
    post("sign in, wrong password", SI, TOK_NONE, cred(CBW_SIGNIN, "ALICE", "wrong-password-1"), 0);
    post("sign in, no such account", SI, TOK_NONE, cred(CBW_SIGNIN, "NOBODY", "wrong-password-1"), 0);
    post("sign in, not UTF-8", SI, TOK_NONE, cred(CBW_SIGNIN, "\xff", "wrong-password-1"), 0);
    post("sign in alice (lower case)", SI, TOK_NONE, cred(CBW_SIGNIN, "alice", "correct-horse-1"), 1);

    // Refresh.
    g_addr = 4;
    refresh_last("refresh", 0);
    refresh_last("the same token again, within the grace", 0);
    post("refresh, unknown token", RF, TOK_NONE, refresh_body("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"), 0);
    post("refresh, short token", RF, TOK_NONE, refresh_body("abc"), 0);
    post("refresh, empty token", RF, TOK_NONE, refresh_body(""), 0);
    post("refresh, NUL inside", RF, TOK_NONE, raw((const uint8_t *)"\x01\x03\x02" "a\0", 5), 0);

    // Bearer routes.
    g_addr = 5;
    post("password, no bearer", PW, TOK_NONE, password_body("correct-horse-1", "correct-horse-9"), 0);
    post("password, a garbage bearer", PW, TOK_GARBAGE, password_body("correct-horse-1", "correct-horse-9"), 0);
    post("password, malformed", PW, TOK_LAST, raw((const uint8_t *)"\x01\x05\x01", 3), 0);
    post("password, wrong current", PW, TOK_LAST, password_body("wrong-current-1", "correct-horse-9"), 0);
    post("password, the same", PW, TOK_LAST, password_body("correct-horse-1", "correct-horse-1"), 0);
    post("password, weak new", PW, TOK_LAST, password_body("correct-horse-1", "short"), 0);
    post("password, NUL in the new", PW, TOK_LAST, creds(CBW_PASSWORD, "correct-horse-1", 15, "correct\0horse-9", 15), 0);
    post("password, changed", PW, TOK_LAST, password_body("correct-horse-1", "correct-horse-9"), 0);
    post("sign in with the new password", SI, TOK_NONE, cred(CBW_SIGNIN, "ALICE", "correct-horse-9"), 1);
    post("sign-out, no bearer", SO, TOK_NONE, signout_body(CBW_SCOPE_LOCAL), 0);
    post("sign-out, a bad scope", SO, TOK_LAST, signout_body(7), 0);
    post("sign-out, a byte over", SO, TOK_LAST, raw((const uint8_t *)"\x01\x04\x00\x00", 4), 0);
    post("sign-out, local", SO, TOK_LAST, signout_body(CBW_SCOPE_LOCAL), 0);
    post("password with the signed-out token", PW, TOK_LAST, password_body("correct-horse-9", "correct-horse-8"), 0);
    refresh_last("the signed-out session's refresh", 0);
    post("sign-out, global (BOB)", SO, TOK_SECOND, signout_body(CBW_SCOPE_GLOBAL), 0);
    post("sign-out with that token again", SO, TOK_SECOND, signout_body(CBW_SCOPE_LOCAL), 0);

    g_addr = 6;
    // A reused refresh token, past the grace, ends the session (the grace is 10 s: wait it out).
    post("sign in BOB", SI, TOK_NONE, cred(CBW_SIGNIN, "BOB", "battery-staple-2"), 1);
    refresh_last("BOB's refresh", 0);
    struct timespec ts = { 11, 0 };
    nanosleep(&ts, NULL);
    refresh_last("BOB's token again, past the grace: reuse", 0);

    // The account lock: wrong passwords at one account until it locks.
    g_addr = 7;
    for (int i = 0; i < 8; i++) {
        char what[64];
        snprintf(what, sizeof what, "a guess at BOB, %d", i + 1);
        post(what, SI, TOK_NONE, cred(CBW_SIGNIN, "BOB", "guess-guess-guess"), 0);
    }
    // The address's sign-ups, until the limit.
    g_addr = 8;
    for (int i = 0; i < 12; i++) {
        char name[16], what[64];
        snprintf(name, sizeof name, "USER%d", i);
        snprintf(what, sizeof what, "sign up %s", name);
        post(what, SU, TOK_NONE, cred(CBW_SIGNUP, name, "correct-horse-1"), 0);
    }
    // The address's sign-ins, until the limit.
    g_addr = 9;
    for (int i = 0; i < 25; i++) {
        char what[64];
        snprintf(what, sizeof what, "sign in USER0, %d", i + 1);
        post(what, SI, TOK_NONE, cred(CBW_SIGNIN, "USER0", "correct-horse-1"), 0);
    }
    // The address's refreshes, until the limit.
    g_addr = 10;
    for (int i = 0; i < 65; i++) {
        char what[64];
        snprintf(what, sizeof what, "refresh unknown, %d", i + 1);
        post(what, RF, TOK_NONE, refresh_body("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"), 0);
    }

    tp_stop(&g_s[0].child, SIGTERM, 10000);
    tp_stop(&g_s[1].child, SIGTERM, 10000);
    printf("%s auth_diff: %d steps, %d differ\n", g_diffs ? "FAIL" : "ok  ", g_steps, g_diffs);
    return g_diffs ? 1 : 0;
}
