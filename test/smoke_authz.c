// smoke_authz.c - the authorization matrix, end to end against a real
// `cbase serve` over HTTP and WebSocket.
//
//   build/test/smoke_authz build/cbase
//
// c_base replaced what Supabase secured (auth, row policies, realtime
// authorization, rate limits), so every route is asked by every kind of
// caller and the answer's status is pinned here, one row each:
//   EDGELESS  no X-Cbase-Origin: the Function URL hit directly, walking
//             around CloudFront (config.h CBASE_ORIGIN_SECRET)
//   ANON      through the edge, no token
//   FORGED    a token signed with another key
//   OWNER     A, who created the game and sits at it
//   STRANGER  B, signed in, who does not sit at A's game
// A row is added with every authorization change (security findings H1..M9);
// a route added without rows here is a route nobody checked.
//
// The server runs with CBASE_ORIGIN_SECRET set, as production does.
#include "t.h"

#include "lib_client.h"
#include "lib_proc.h"

#include "ask.h"
#include "client_table.h"
#include "netwire.h"
#include "util/clock.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ORIGIN_SECRET "authz-origin-0123456789abcdef0123456789"

static const char *g_bin;
static tp_child g_child;
static int g_port;
static char g_dbdir[400];

typedef enum { EDGELESS, ANON, FORGED, OWNER, STRANGER, N_CALLERS } caller;
static const char *const CALLER_NAME[N_CALLERS] = { "edgeless", "anon", "forged", "owner", "stranger" };

static char g_token[N_CALLERS][NW_ACCESS_MAX + 1];
static char g_gid[32];

static void start_server(void) {
    char line[512];
    static char envdb[512];
    snprintf(envdb, sizeof envdb, "CBASE_DB=%s/cbase.sqlite", g_dbdir);
    char *env[] = { "CBASE_LISTEN=127.0.0.1:0", envdb, "CBASE_JWT_SECRET=authz-secret-authz-secret-authz-secret",
                    "CBASE_ORIGIN_SECRET=" ORIGIN_SECRET, "CBASE_ADMIN_TOKEN=authz-admin-token-0123456789abcdef", NULL };
    char *argv[] = { (char *)g_bin, "serve", NULL };
    T_EQ_INT(tp_start(&g_child, argv, env), 0);
    T_EQ_INT(tp_wait_line(&g_child, "listening on", 20000, line, sizeof line), 0);
    const char *colon = strrchr(line, ':');
    g_port = colon ? atoi(colon + 1) : 0;
    T_CHECK(g_port > 0);
}

// One request as `who`. The status, the body into *body when given.
static int req(caller who, const char *method, const char *path, const void *body, size_t n, cb_buf *out) {
    char headers[NW_ACCESS_MAX + 256];
    int h = 0;
    if (who != EDGELESS) h += snprintf(headers + h, sizeof headers - (size_t)h, "X-Cbase-Origin: " ORIGIN_SECRET "\r\n");
    if (who == FORGED || who == OWNER || who == STRANGER)
        h += snprintf(headers + h, sizeof headers - (size_t)h, "Authorization: Bearer %s\r\n", g_token[who]);
    if (strcmp(method, "POST") == 0) h += snprintf(headers + h, sizeof headers - (size_t)h, "Content-Type: application/octet-stream\r\n");
    headers[h] = 0;
    tc_response r;
    if (tc_http(g_port, method, path, headers, body, n, &r) != 0) return -1;
    int st = r.status;
    if (out) { cb_buf_reset(out); cb_buf_append(out, r.body.p, r.body.len); }
    tc_response_free(&r);
    return st;
}

static int request_body(const NwRequest *q, uint8_t *out, int cap) { return nw_request_encode(q, out, cap); }

// Signs `name` up through the edge; its access token into tok.
static bool signup(const char *name, char *tok) {
    NwRequest q;
    memset(&q, 0, sizeof q);
    q.kind = NW_SIGNUP;
    q.name_len = (uint8_t)strlen(name);
    memcpy(q.name, name, q.name_len);
    q.password_len = 14;
    memcpy(q.password, "authz-pass-123", 14);
    uint8_t b[256];
    int n = request_body(&q, b, sizeof b);
    cb_buf out;
    cb_buf_init(&out);
    int st = req(ANON, "POST", "/v1/auth/signup", b, (size_t)n, &out);
    static NwSession s;
    bool ok = st == 200 && nw_session_decode(out.p, (int)out.len, &s) == NW_OK;
    if (ok) snprintf(tok, NW_ACCESS_MAX + 1, "%.*s", (int)s.access_len, s.access);
    cb_buf_free(&out);
    return ok;
}

static void setup_world(void) {
    if (tp_tempdir(g_dbdir, sizeof g_dbdir) != 0) { T_CHECK(0); return; }
    start_server();
    T_CHECK(signup("AUTHZ_A", g_token[OWNER]));
    T_CHECK(signup("AUTHZ_B", g_token[STRANGER]));
    // A token of the right shape signed with another key.
    snprintf(g_token[FORGED], sizeof g_token[FORGED], "%s", g_token[OWNER]);
    char *sig = strrchr(g_token[FORGED], '.');
    if (sig && sig[1]) sig[1] = sig[1] == 'A' ? 'B' : 'A';
    // A creates; the envelope names the game.
    cb_buf env;
    cb_buf_init(&env);
    T_EQ_INT(req(OWNER, "POST", "/v1/create", NULL, 0, &env), 200);
    static ClientTable c;
    static ClientSlot slot;
    client_init(&c, &slot);
    T_CHECK(client_adopt_envelope(&c, env.p, (int)env.len) >= 0);
    snprintf(g_gid, sizeof g_gid, "%.*s", (int)c.gid_len, c.gid);
    T_EQ_INT(strlen(g_gid), 6);
    cb_buf_free(&env);
}

// ---- the matrix ----------------------------------------------------------------------

typedef enum { B_NONE, B_SIGNIN, B_SIGNUP, B_REFRESH, B_SIGNOUT, B_PASSWORD, B_CHAT, B_ASK_JOIN, B_ASK_BUMP } body_kind;

static int build(body_kind k, uint8_t *b, int cap) {
    NwRequest q;
    memset(&q, 0, sizeof q);
    switch (k) {
    case B_NONE: return 0;
    case B_SIGNIN:
    case B_SIGNUP:
        q.kind = k == B_SIGNIN ? NW_SIGNIN : NW_SIGNUP;
        q.name_len = 7;
        memcpy(q.name, k == B_SIGNIN ? "AUTHZ_A" : "AUTHZ_Z", 7);
        q.password_len = 14;
        memcpy(q.password, "wrong-pass-123", 14);
        return request_body(&q, b, cap);
    case B_REFRESH:
        q.kind = NW_REFRESH;
        q.refresh_len = 43;
        memset(q.refresh, 'x', 43);
        return request_body(&q, b, cap);
    case B_SIGNOUT:
        q.kind = NW_SIGNOUT;
        return request_body(&q, b, cap);
    case B_PASSWORD:
        q.kind = NW_PASSWORD;
        q.password_len = 14;
        memcpy(q.password, "other-pass-123", 14);
        return request_body(&q, b, cap);
    case B_CHAT:
        q.kind = NW_CHAT_SEND;
        q.message_len = 2;
        memcpy(q.message, "hi", 2);
        return request_body(&q, b, cap);
    case B_ASK_JOIN:
    case B_ASK_BUMP: {
        TableAsk a;
        memset(&a, 0, sizeof a);
        a.kind = k == B_ASK_JOIN ? ASK_JOIN : ASK_BUMP;
        a.gid_len = (uint8_t)strlen(g_gid);
        memcpy(a.gid, g_gid, a.gid_len);
        return ask_encode(&a, b, cap);
    }
    }
    return -1;
}

typedef struct {
    const char *method;
    const char *path;   // "%s" is the game's id
    body_kind   body;
    int         want[N_CALLERS];   // 0: not asked (a caller whose answer another row pins)
} row;

// The statuses. Read across: who may do what.
static const row ROWS[] = {
    //                                                  edgeless anon forged owner stranger
    { "GET",  "/health",            B_NONE,       { 403, 200, 200, 200, 200 } },
    { "GET",  "/v1/bots",           B_NONE,       { 403, 200, 200, 200, 200 } },
    { "GET",  "/v1/leaderboard",    B_NONE,       { 403, 200, 200, 200, 200 } },
    { "POST", "/v1/auth/signin",    B_SIGNIN,     { 403, 400, 0, 0, 0 } },    // wrong password: ACCOUNT refusal
    { "POST", "/v1/auth/refresh",   B_REFRESH,    { 403, 400, 0, 0, 0 } },    // unknown token
    { "POST", "/v1/auth/signout",   B_SIGNOUT,    { 403, 401, 401, 0, 0 } },
    { "POST", "/v1/auth/password",  B_PASSWORD,   { 403, 401, 401, 0, 0 } },
    { "GET",  "/v1/games",          B_NONE,       { 403, 401, 401, 200, 200 } },
    { "GET",  "/v1/games/%s",       B_NONE,       { 403, 401, 401, 200, 200 } },  // a stranger spectates (by design)
    { "GET",  "/v1/chat/%s",        B_NONE,       { 403, 401, 401, 200, 200 } },  // a stranger reads an empty list
    { "POST", "/v1/chat/%s",        B_CHAT,       { 403, 401, 401, 204, 403 } },
    { "GET",  "/v1/history",        B_NONE,       { 403, 401, 401, 200, 200 } },
    { "GET",  "/v1/replay/%s",      B_NONE,       { 403, 401, 401, 404, 404 } },  // nobody's replay yet; never a stranger's
    // The functions answer an unverified caller 400 AUTH (wrap400, the edge functions' rule).
    { "POST", "/v1/create",         B_NONE,       { 403, 400, 400, 0, 0 } },
    { "POST", "/v1/meta",           B_ASK_BUMP,   { 403, 400, 400, 200, 400 } },  // a bump is a seated player's only (M8)
    { "POST", "/v1/action",         B_NONE,       { 403, 400, 400, 0, 0 } },
    { "POST", "/v1/account/delete", B_NONE,       { 403, 401, 401, 0, 0 } },
    { "GET",  "/admin/api/overview", B_NONE,      { 403, 401, 401, 401, 401 } },  // a user's token is not the admin's
    { "GET",  "/m/x",               B_NONE,       { 403, 0, 0, 0, 0 } },
};

static void matrix(void) {
    cb_buf out;
    cb_buf_init(&out);
    for (size_t i = 0; i < sizeof ROWS / sizeof ROWS[0]; i++) {
        const row *r = &ROWS[i];
        char path[128];
        snprintf(path, sizeof path, r->path, g_gid);
        uint8_t b[512];
        int n = build(r->body, b, sizeof b);
        T_CHECK(n >= 0);
        for (int who = 0; who < N_CALLERS; who++) {
            if (!r->want[who]) continue;
            int st = req((caller)who, r->method, path, n > 0 ? b : NULL, n > 0 ? (size_t)n : 0, &out);
            if (st != r->want[who]) fprintf(stderr, "  %s %s as %s: %d, want %d\n", r->method, path, CALLER_NAME[who], st, r->want[who]);
            T_EQ_INT(st, r->want[who]);
        }
    }
    cb_buf_free(&out);
}

// The stream's upgrade is a route like the rest: around the edge it is refused.
static void stream_only_through_the_edge(void) {
    int fd = tc_ws_open(g_port, "/v1/stream?proto=nw1", NULL);
    T_CHECK(fd < 0);
    if (fd >= 0) close(fd);
    fd = tc_ws_open(g_port, "/v1/stream?proto=nw1", "X-Cbase-Origin: " ORIGIN_SECRET "\r\n");
    T_CHECK(fd >= 0);
    if (fd >= 0) close(fd);
}

// limits.h and auth.h over real HTTP (H1): an address's sign-ups run out, and
// guessing one account's password locks it, after which even the right
// password is answered 429 TOO_MANY.
static void limits_hold(void) {
    char name[16];
    int refused_at = -1;
    for (int i = 0; i < 10 && refused_at < 0; i++) {
        snprintf(name, sizeof name, "AUTHZ_L%d", i);
        NwRequest q;
        memset(&q, 0, sizeof q);
        q.kind = NW_SIGNUP;
        q.name_len = (uint8_t)strlen(name);
        memcpy(q.name, name, q.name_len);
        q.password_len = 14;
        memcpy(q.password, "authz-pass-123", 14);
        uint8_t b[256];
        int n = request_body(&q, b, sizeof b);
        if (req(ANON, "POST", "/v1/auth/signup", b, (size_t)n, NULL) == 429) refused_at = i;
    }
    // Two sign-ups were the world's; the address's burst is 5.
    T_EQ_INT(refused_at, 3);
    uint8_t b[256];
    int n = build(B_SIGNIN, b, sizeof b);   // AUTHZ_A, a wrong password; the matrix spent one
    int st = 0;
    for (int i = 0; i < 5; i++) st = req(ANON, "POST", "/v1/auth/signin", b, (size_t)n, NULL);
    T_EQ_INT(st, 400);   // the sixth failure: still "wrong", and now locked
    NwRequest q;
    memset(&q, 0, sizeof q);
    q.kind = NW_SIGNIN;
    q.name_len = 7;
    memcpy(q.name, "AUTHZ_A", 7);
    q.password_len = 14;
    memcpy(q.password, "authz-pass-123", 14);
    n = request_body(&q, b, sizeof b);
    cb_buf out;
    cb_buf_init(&out);
    T_EQ_INT(req(ANON, "POST", "/v1/auth/signin", b, (size_t)n, &out), 429);
    AskRefusal r;
    T_CHECK(ask_refusal_decode(out.p, (int)out.len, &r) == ASK_OK && r.why == ASK_WHY_ACCOUNT && r.code == NW_ACCOUNT_E_TOO_MANY);
    cb_buf_free(&out);
}

static void stops(void) {
    T_EQ_INT(kill(g_child.pid, SIGTERM), 0);
    T_EQ_INT(tp_stop(&g_child, 0, 15000), 0);
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s build/cbase\n", argv[0]); return 2; }
    g_bin = argv[1];
    T_RUN(setup_world);
    T_RUN(matrix);
    T_RUN(stream_only_through_the_edge);
    T_RUN(limits_hold);
    T_RUN(stops);
    return T_DONE();
}
