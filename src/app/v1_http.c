// v1_http.c - the binary surface's HTTP routes (v1_http.h).
//
// Every body is netwire.h's, every refusal the kernel's AskRefusal, and every
// read is the same store call the PostgREST facade makes, without its query
// language: each route answers exactly the one question a client asks.
#include "v1_http.h"

#include "auth/auth.h"
#include "auth/names.h"
#include "limits.h"
#include "net/router.h"
#include "realtime/realtime.h"
#include "table_ops.h"
#include "util/clock.h"
#include "util/err.h"
#include "util/log.h"
#include "util/uuid.h"

#include "ask.h"
#include "netwire.h"
#include "../../../shared/c/sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OCTETS "application/octet-stream"
#define ACCOUNT_DOMAIN "foolish.cards"
#define CHAT_PAGE 100
#define BOARD_TOP 100
#define HISTORY_PAGE 50

void cb_v1_email_of(const char *name, size_t len, char out[CB_V1_EMAIL_BYTES]) {
    uint8_t d[SHA256_DIGEST_LEN];
    sha256(name, len, d);
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 8; i++) {
        out[2 * i] = hex[d[i] >> 4];
        out[2 * i + 1] = hex[d[i] & 15];
    }
    snprintf(out + 16, CB_V1_EMAIL_BYTES - 16, "@%s", ACCOUNT_DOMAIN);
}

// ---- replies ---------------------------------------------------------------------------

// Every answer's Cache-Control. Nothing is cached but the two public reads,
// whose bytes are the same for every caller: browsers ask again every time
// (max-age=0) and CloudFront keeps one copy for s-maxage, a safety net only,
// since the server invalidates the path when the answer changes (app/edge.h).
// A route that sends one of these may never read the caller (DESIGN.md "Edge
// cache").
#define CC_PRIVATE "no-store"
#define CC_BOTS    "public, max-age=0, s-maxage=86400"   // a day: bots change only when seeded
#define CC_BOARD   "public, max-age=0, s-maxage=3600"    // an hour: a rated game invalidates it

static void refuse(cb_http_res *res, int status, int why, int code) {
    AskRefusal r = { (uint8_t)why, code, 0 };
    uint8_t body[ASK_REFUSAL_BYTES];
    int n = ask_refusal_encode(&r, body, sizeof body);
    cb_res_header(res, "Cache-Control", CC_PRIVATE);
    cb_res_send(res, status, OCTETS, body, n > 0 ? (size_t)n : 0);
}

static void reply(cb_http_res *res, const char *cache, const void *p, size_t n) {
    cb_res_header(res, "Cache-Control", cache);
    // A public answer must not vary by Origin either: the edge keys it on the
    // path alone, so it says "*" whatever CBASE_ALLOWED_ORIGINS lists (the
    // router's CORS step leaves a header already set alone).
    if (strcmp(cache, CC_PRIVATE) != 0) cb_res_header(res, "Access-Control-Allow-Origin", "*");
    cb_res_send(res, 200, OCTETS, p, n);
}

static void octets(cb_http_res *res, const void *p, size_t n) { reply(res, CC_PRIVATE, p, n); }

void cb_v1_busy(cb_http_res *res) {
    cb_res_header(res, "Retry-After", "1");
    refuse(res, 503, ASK_WHY_BUSY, 0);
}

void cb_v1_limited(cb_http_res *res, int retry_s, bool account) {
    char ra[16];
    snprintf(ra, sizeof ra, "%d", retry_s > 0 ? retry_s : 1);
    cb_res_header(res, "Retry-After", ra);
    if (account) refuse(res, 429, ASK_WHY_ACCOUNT, NW_ACCOUNT_E_TOO_MANY);
    else refuse(res, 429, ASK_WHY_BUSY, 0);
}

// One from key's bucket of `kind`, or false with the 429 answered.
static bool within(cb_http_res *res, cb_limit_kind kind, const char *key, bool account) {
    int retry;
    if (cb_limit_take(kind, key, cb_now_ms(), &retry)) return true;
    CB_INFO("v1", "rate limit %s: %s", cb_limit_rules[kind].name, key);
    cb_v1_limited(res, retry, account);
    return false;
}

// A store or app failure: 404 for a game that is not there, 503 BUSY for a
// store throttled past its backoff (the client sends a read again), else the server's.
static void failed(cb_http_res *res, int rc) {
    if (rc == CB_E_NOT_FOUND) refuse(res, 404, ASK_WHY_NOT_FOUND, 0);
    else if (rc == CB_E_BUSY) cb_v1_busy(res);
    else if (rc == CB_E_STATE) refuse(res, 503, ASK_WHY_INTERNAL, 0);
    else {
        CB_ERROR("v1", "request failed: %s", cb_strerror(rc));
        refuse(res, 500, ASK_WHY_INTERNAL, 0);
    }
}

// The verified caller, or false with a 401 AUTH refusal answered.
static bool caller(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res, cb_user *u) {
    if (!ctx->auth || !ctx->store) { refuse(res, 503, ASK_WHY_INTERNAL, 0); return false; }
    memset(u, 0, sizeof *u);
    int rc = cb_auth_verify_bearer(ctx->auth, req, u);
    if (rc == CB_E_AUTH) { refuse(res, 401, ASK_WHY_AUTH, 0); return false; }
    if (rc != CB_OK) { failed(res, rc); return false; }   // the check could not run: the server's failure
    return true;
}

// The request body as an NwRequest of `kind`, or false with 400 MALFORMED.
static bool request_of(const cb_http_req *req, cb_http_res *res, uint8_t kind, NwRequest *q) {
    if (req->body_len > (size_t)INT32_MAX || nw_request_decode(req->body, (int)req->body_len, q) != NW_OK || q->kind != kind) {
        refuse(res, 400, ASK_WHY_MALFORMED, 0);
        return false;
    }
    return true;
}

// The id after `prefix` in the path, or false with 404.
static bool path_id(const cb_http_req *req, cb_http_res *res, const char *prefix, char *out, size_t cap) {
    const char *id = req->path + strlen(prefix);
    size_t n = strlen(id);
    bool ok = n > 0 && n < cap && n <= CB_GAME_ID_MAX;
    for (size_t i = 0; ok && i < n; i++) {
        char c = id[i];
        ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
    }
    if (!ok) { refuse(res, 404, ASK_WHY_NOT_FOUND, 0); return false; }
    memcpy(out, id, n + 1);
    return true;
}

// A NUL-terminated copy of a length-counted field; false when it holds a NUL
// (a C string cannot say it, and no password or name has one).
static bool cstr(const char *p, size_t n, char *out, size_t cap) {
    if (n >= cap || memchr(p, 0, n)) return false;
    memcpy(out, p, n);
    out[n] = 0;
    return true;
}

// ---- auth -------------------------------------------------------------------------------

static int account_code(int rc) {
    switch (rc) {
    case CB_AUTH_E_EXISTS: return NW_ACCOUNT_E_EXISTS;
    case CB_AUTH_E_BAD_LOGIN: return NW_ACCOUNT_E_BAD_LOGIN;
    case CB_AUTH_E_WEAK_PASSWORD: return NW_ACCOUNT_E_WEAK_PASSWORD;
    case CB_AUTH_E_LONG_PASSWORD: return NW_ACCOUNT_E_LONG_PASSWORD;
    case CB_AUTH_E_BAD_USERNAME: return NW_ACCOUNT_E_BAD_NAME;
    case CB_AUTH_E_BAD_EMAIL: return NW_ACCOUNT_E_BAD_NAME;   // the email is the name's
    case CB_AUTH_E_BAD_REFRESH: return NW_ACCOUNT_E_BAD_REFRESH;
    case CB_AUTH_E_REFRESH_USED: return NW_ACCOUNT_E_REFRESH_USED;
    case CB_AUTH_E_SAME_PASSWORD: return NW_ACCOUNT_E_SAME_PASSWORD;
    case CB_AUTH_E_TOO_MANY: return NW_ACCOUNT_E_TOO_MANY;
    default: return 0;
    }
}

static void auth_failed(cb_http_res *res, int rc) {
    int code = account_code(rc);
    if (code) refuse(res, code == NW_ACCOUNT_E_EXISTS ? 409 : code == NW_ACCOUNT_E_TOO_MANY ? 429 : 400, ASK_WHY_ACCOUNT, code);
    else if (rc == CB_E_NOT_FOUND) refuse(res, 401, ASK_WHY_AUTH, 0);   // the account behind the token is gone
    else failed(res, rc);
}

static void send_session(cb_http_res *res, const cb_session *s) {
    NwSession w;
    memset(&w, 0, sizeof w);
    size_t n = strlen(s->access_token);
    size_t rn = strlen(s->refresh_token), un = strlen(s->user.id), nn = strlen(s->user.username);
    if (n > NW_ACCESS_MAX || rn > NW_REFRESH_MAX || un > NW_ID_MAX || nn > NW_NAME_MAX) { failed(res, CB_E_NOMEM); return; }
    w.access_len = (uint16_t)n;
    memcpy(w.access, s->access_token, n);
    w.refresh_len = (uint8_t)rn;
    memcpy(w.refresh, s->refresh_token, rn);
    w.expires_at_ms = s->expires_at > 0 ? (uint64_t)s->expires_at * 1000 : 0;
    w.user_id_len = (uint8_t)un;
    memcpy(w.user_id, s->user.id, un);
    w.username_len = (uint8_t)nn;
    memcpy(w.username, s->user.username, nn);
    uint8_t body[NW_ACCESS_MAX + NW_REFRESH_MAX + NW_ID_MAX + NW_NAME_MAX + 32];
    int len = nw_session_encode(&w, body, sizeof body);
    if (len < 0) { failed(res, CB_E_NOMEM); return; }
    octets(res, body, (size_t)len);
}

static void h_credentials(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res, uint8_t kind) {
    if (!ctx->auth) { refuse(res, 503, ASK_WHY_INTERNAL, 0); return; }
    NwRequest q;
    if (!request_of(req, res, kind, &q)) return;
    char name[NW_NAME_MAX + 1], pw[NW_PASSWORD_MAX + 1], email[CB_V1_EMAIL_BYTES];
    if (!cstr(q.name, q.name_len, name, sizeof name) || !cstr(q.password, q.password_len, pw, sizeof pw)) {
        refuse(res, 400, ASK_WHY_MALFORMED, 0);
        return;
    }
    // The case step of the canonical name (auth/names.h) for both routes: the
    // account is the one the name's bytes derive, so "alice" is "ALICE" as
    // every client has always sent it. Not UTF-8: no account has that name.
    char canon[NW_NAME_MAX * 2 + 8];
    int cn = cb_name_case(q.name, q.name_len, canon, sizeof canon);
    if (cn < 0 || cn > NW_NAME_MAX) {
        memset(pw, 0, sizeof pw);
        auth_failed(res, kind == NW_SIGNUP ? CB_AUTH_E_BAD_USERNAME : CB_AUTH_E_BAD_LOGIN);
        return;
    }
    cb_v1_email_of(canon, (size_t)cn, email);
    // The limits before any work: an address's attempts, an account's
    // guesses, and every sign-up this container makes (limits.h).
    char ip[96];
    cb_viewer_key(ctx, req, ip, sizeof ip);
    if (kind == NW_SIGNUP ? !within(res, CB_LIMIT_SIGNUP_IP, ip, true) || !within(res, CB_LIMIT_SIGNUP_ALL, "*", true)
                          : !within(res, CB_LIMIT_SIGNIN_IP, ip, true) || !within(res, CB_LIMIT_SIGNIN_NAME, email, true)) {
        memset(pw, 0, sizeof pw);
        return;
    }
    cb_session s;
    int rc = kind == NW_SIGNUP ? cb_auth_signup(ctx->auth, email, pw, canon, &s) : cb_auth_signin(ctx->auth, email, pw, &s);
    memset(pw, 0, sizeof pw);
    if (rc != CB_OK) auth_failed(res, rc);
    else send_session(res, &s);
}

static void h_signup(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) { h_credentials(ctx, req, res, NW_SIGNUP); }
static void h_signin(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) { h_credentials(ctx, req, res, NW_SIGNIN); }

static void h_refresh(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    if (!ctx->auth) { refuse(res, 503, ASK_WHY_INTERNAL, 0); return; }
    NwRequest q;
    if (!request_of(req, res, NW_REFRESH, &q)) return;
    char tok[NW_REFRESH_MAX + 1];
    if (!cstr(q.refresh, q.refresh_len, tok, sizeof tok)) { refuse(res, 400, ASK_WHY_MALFORMED, 0); return; }
    char ip[96];
    cb_viewer_key(ctx, req, ip, sizeof ip);
    if (!within(res, CB_LIMIT_REFRESH_IP, ip, true)) return;
    cb_session s;
    int rc = cb_auth_refresh(ctx->auth, tok, &s);
    if (rc != CB_OK) auth_failed(res, rc);
    else send_session(res, &s);
}

static void h_signout(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    cb_user u;
    if (!caller(ctx, req, res, &u)) return;
    NwRequest q;
    if (!request_of(req, res, NW_SIGNOUT, &q)) return;
    int rc = cb_auth_logout(ctx->auth, &u, q.scope == NW_SCOPE_GLOBAL ? CB_LOGOUT_GLOBAL : CB_LOGOUT_LOCAL);
    if (rc != CB_OK) auth_failed(res, rc);
    else cb_res_status(res, 204);
}

static void h_password(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    cb_user u;
    if (!caller(ctx, req, res, &u)) return;
    NwRequest q;
    if (!request_of(req, res, NW_PASSWORD, &q)) return;
    if (!within(res, CB_LIMIT_PASSWORD_USER, u.id, true)) return;
    char cur[NW_PASSWORD_MAX + 1], pw[NW_PASSWORD_MAX + 1];
    if (!cstr(q.password, q.password_len, cur, sizeof cur) || !cstr(q.new_password, q.new_password_len, pw, sizeof pw)) {
        refuse(res, 400, ASK_WHY_MALFORMED, 0);
        return;
    }
    int rc = cb_auth_change_password(ctx->auth, &u, cur, pw);
    memset(cur, 0, sizeof cur);
    memset(pw, 0, sizeof pw);
    if (rc != CB_OK) auth_failed(res, rc);
    else cb_res_status(res, 204);
}

// ---- views ------------------------------------------------------------------------------

typedef struct {
    cb_buf body;
    int    n;
    bool   bad;
} list;

static void list_begin(list *l, size_t head) {
    cb_buf_init(&l->body);
    l->n = 0;
    l->bad = !cb_buf_reserve(&l->body, head);
    if (!l->bad) { memset(l->body.p, 0, head); l->body.len = head; }
}

// Appends one row through `put`, which writes into at most `cap` bytes.
#define LIST_PUT(l, cap, call)                                                   \
    do {                                                                         \
        if ((l)->bad) break;                                                     \
        size_t cap_ = (cap);                                                     \
        if (!cb_buf_reserve(&(l)->body, cap_)) { (l)->bad = true; break; }       \
        uint8_t *out = (l)->body.p + (l)->body.len;                              \
        int n_ = (call);                                                         \
        if (n_ < 0) { (l)->bad = true; break; }                                  \
        (l)->body.len += (size_t)n_;                                             \
        (l)->n++;                                                                \
    } while (0)

static void list_send_as(cb_http_res *res, list *l, uint8_t kind, const char *cache) {
    if (!l->bad && nw_list_head(kind, l->n, l->body.p, NW_LIST_HEAD_BYTES) == NW_LIST_HEAD_BYTES) reply(res, cache, l->body.p, l->body.len);
    else failed(res, CB_E_NOMEM);
    cb_buf_free(&l->body);
}

static void list_send(cb_http_res *res, list *l, uint8_t kind) { list_send_as(res, l, kind, CC_PRIVATE); }

static bool game_fn(const cb_view_row *v, void *ud) {
    list *l = ud;
    NwBlob env = { v->view, (uint32_t)v->view_len };
    LIST_PUT(l, 8 + v->view_len, nw_put_game((uint32_t)v->version, env, out, (int)cap_));
    return !l->bad;
}

static void h_games(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    cb_user u;
    if (!caller(ctx, req, res, &u)) return;
    list l;
    list_begin(&l, NW_LIST_HEAD_BYTES);
    int rc = cb_table_views_of_user(ctx, u.id, 0, game_fn, &l);
    if (rc < 0) { cb_buf_free(&l.body); failed(res, rc); return; }
    list_send(res, &l, NW_GAMES);
}

static void h_game(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    cb_user u;
    char gid[CB_GAME_ID_MAX + 1];
    if (!caller(ctx, req, res, &u) || !path_id(req, res, "/v1/games/", gid, sizeof gid)) return;
    cb_view_row v;
    memset(&v, 0, sizeof v);
    // The caller's own view when they sit at the game, else the spectators'.
    int rc = cb_table_view(ctx, gid, u.id, &v);
    if (rc == CB_E_NOT_FOUND) { cb_view_row_free(&v); memset(&v, 0, sizeof v); rc = cb_table_view(ctx, gid, NULL, &v); }
    if (rc != CB_OK) { cb_view_row_free(&v); failed(res, rc); return; }
    size_t cap = 16 + v.view_len;
    uint8_t *body = malloc(cap);
    NwBlob env = { v.view, (uint32_t)v.view_len };
    int n = body ? nw_view_encode((uint32_t)v.version, env, body, (int)cap) : -1;
    if (n < 0) failed(res, CB_E_NOMEM);
    else octets(res, body, (size_t)n);
    free(body);
    cb_view_row_free(&v);
}

// ---- chat -------------------------------------------------------------------------------

// The game in the path and whether the caller sits at it (*member), or false
// with the refusal answered.
static bool chat_of(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res, cb_user *u, char *gid, size_t cap, bool *member) {
    if (!caller(ctx, req, res, u) || !path_id(req, res, "/v1/chat/", gid, cap)) return false;
    int m = CB_STORE_CALL(ctx->store, is_member, gid, u->id);
    if (m < 0) { failed(res, m); return false; }
    *member = m == 1;
    return true;
}

static bool chat_fn(const cb_chat_row *c, void *ud) {
    list *l = ud;
    static _Thread_local NwChat w;
    cb_chat_to_nw(c, &w);
    LIST_PUT(l, 16 + 2 * NW_ID_MAX + (size_t)w.message_len, nw_put_chat(&w, out, (int)cap_));
    return !l->bad;
}

static void h_chat_get(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    cb_user u;
    char gid[CB_GAME_ID_MAX + 1];
    bool member;
    if (!chat_of(ctx, req, res, &u, gid, sizeof gid, &member)) return;
    list l;
    list_begin(&l, NW_LIST_HEAD_BYTES);
    // Someone who does not sit at the game sees no messages, as chat_messages'
    // row policy answered: an empty list, not a refusal (a spectator's page asks).
    int rc = member ? CB_STORE_CALL(ctx->store, chat_list, gid, CHAT_PAGE, chat_fn, &l) : 0;
    if (rc < 0) { cb_buf_free(&l.body); failed(res, rc); return; }
    list_send(res, &l, NW_CHATS);
}

typedef struct {
    int     n;
    cb_uuid ids[CB_MAX_SEATS];
} members;

static bool member_fn(const char *id, void *ud) {
    members *m = ud;
    if (m->n < CB_MAX_SEATS) cb_strlcpy(m->ids[m->n++], id, sizeof m->ids[0]);
    return m->n < CB_MAX_SEATS;
}

static void h_chat_post(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    cb_user u;
    char gid[CB_GAME_ID_MAX + 1];
    if (!caller(ctx, req, res, &u) || !path_id(req, res, "/v1/chat/", gid, sizeof gid)) return;
    // Who sits at the game: the caller must, and each of them hears the
    // message on their own topic (realtime.h). Read before anything is
    // written, so a failed read stores nothing.
    members m = { 0 };
    int mn = CB_STORE_CALL(ctx->store, game_members, gid, member_fn, &m);
    if (mn < 0) { failed(res, mn); return; }
    bool member = false;
    for (int i = 0; i < m.n; i++) member = member || strcmp(m.ids[i], u.id) == 0;
    if (!member) { refuse(res, 403, ASK_WHY_FORBIDDEN, 0); return; }
    NwRequest q;
    if (!request_of(req, res, NW_CHAT_SEND, &q)) return;
    if (!within(res, CB_LIMIT_CHAT_USER, u.id, false)) return;
    if (q.message_len == 0 || memchr(q.message, 0, q.message_len)) { refuse(res, 400, ASK_WHY_MALFORMED, 0); return; }
    cb_chat_row row;
    memset(&row, 0, sizeof row);
    cb_uuid_v4(row.id);
    cb_strlcpy(row.game_id, gid, sizeof row.game_id);
    cb_strlcpy(row.user_id, u.id, sizeof row.user_id);
    row.message = malloc((size_t)q.message_len + 1);
    if (!row.message) { failed(res, CB_E_NOMEM); return; }
    memcpy(row.message, q.message, q.message_len);
    row.message[q.message_len] = 0;
    row.created_at = cb_now_ms();
    int rc = CB_STORE_CALL(ctx->store, chat_put, &row);
    if (rc != CB_OK) failed(res, rc);
    else {
        rt_publish_chat(ctx->rt, gid, &row, m.ids, m.n);
        cb_res_status(res, 204);
    }
    free(row.message);
}

// ---- the bots, the leaderboard, history and replays ---------------------------------------

static bool bot_fn(const cb_bot_row *b, void *ud) {
    list *l = ud;
    NwBot w;
    memset(&w, 0, sizeof w);
    size_t n = strlen(b->id), nn = strlen(b->nickname), kn = strlen(b->strategy_key);
    if (n > NW_ID_MAX || nn > NW_NAME_MAX || kn > NW_KEY_MAX) { l->bad = true; return false; }
    w.id_len = (uint8_t)n;
    memcpy(w.id, b->id, n);
    w.nickname_len = (uint8_t)nn;
    memcpy(w.nickname, b->nickname, nn);
    w.key_len = (uint8_t)kn;
    memcpy(w.key, b->strategy_key, kn);
    LIST_PUT(l, 3 + n + nn + kn, nw_put_bot(&w, out, (int)cap_));
    return !l->bad;
}

// Public: the same bytes for everyone, so it never reads the caller.
static void h_bots(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    if (!ctx->store) { refuse(res, 503, ASK_WHY_INTERNAL, 0); return; }
    list l;
    list_begin(&l, NW_LIST_HEAD_BYTES);
    int rc = CB_STORE_CALL(ctx->store, bots_list, bot_fn, &l);
    if (rc < 0) { cb_buf_free(&l.body); failed(res, rc); return; }
    list_send_as(res, &l, NW_BOTS, CC_BOTS);
}

typedef struct {
    NwBoardRow *rows;
    int n, cap;
    bool oom;
} board;

static void board_add(board *b, bool is_bot, const char *id, const char *name, int32_t rating, int32_t games) {
    if (b->oom || games <= 0) return;
    if (b->n == b->cap) {
        int nc = b->cap ? b->cap * 2 : 64;
        NwBoardRow *nr = realloc(b->rows, (size_t)nc * sizeof *nr);
        if (!nr) { b->oom = true; return; }
        b->rows = nr;
        b->cap = nc;
    }
    NwBoardRow *r = &b->rows[b->n++];
    memset(r, 0, sizeof *r);
    r->is_bot = is_bot;
    size_t n = strlen(id), nn = strlen(name);
    r->id_len = (uint8_t)(n <= NW_ID_MAX ? n : 0);
    memcpy(r->id, id, r->id_len);
    r->name_len = (uint8_t)(nn <= NW_NAME_MAX ? nn : 0);
    memcpy(r->name, name, r->name_len);
    r->rating = rating > 0 ? (uint32_t)rating : 0;
    r->games = (uint32_t)games;
}

static bool board_human(const cb_rating_row *g, void *ud) {
    board_add(ud, false, g->user_id, g->username, g->elo_rating, g->games_played);
    return !((board *)ud)->oom;
}

static bool board_bot(const cb_bot_row *b, void *ud) {
    board_add(ud, true, b->id, b->nickname, b->elo_rating, b->games_played);
    return !((board *)ud)->oom;
}

static int by_rating(const void *a, const void *b) {
    const NwBoardRow *x = a, *y = b;
    return x->rating < y->rating ? 1 : x->rating > y->rating ? -1 : 0;
}

// Public, as h_bots. It reads only what it shows: the top BOARD_TOP played
// humans (ratings_top, one bounded Query) and the bots (a fixed few dozen).
typedef struct {
    cb_store *store;
    board     rows;
    int       rc;
} bots_read;

static void read_bots(void *ud) {
    bots_read *b = ud;
    b->rc = CB_STORE_CALL(b->store, bots_list, board_bot, &b->rows);
}

static void h_board(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    if (!ctx->store) { refuse(res, 503, ASK_WHY_INTERNAL, 0); return; }
    board humans = { 0 };
    // The two populations are independent reads: the bots' on a side thread.
    bots_read br = { .store = ctx->store };
    cb_store_side side;
    cb_store_side_start(&side, read_bots, &br);
    int rc = CB_STORE_CALL(ctx->store, ratings_top, BOARD_TOP, board_human, &humans);
    cb_store_side_join(&side);
    board bots = br.rows;
    if (rc >= 0) rc = br.rc;
    if (rc >= 0 && (humans.oom || bots.oom)) rc = CB_E_NOMEM;
    if (rc < 0) { free(humans.rows); free(bots.rows); failed(res, rc); return; }
    // Each population's top: the leaderboard merges them and cuts the whole again.
    if (humans.n) qsort(humans.rows, (size_t)humans.n, sizeof *humans.rows, by_rating);   // no rows: rows is NULL
    if (bots.n) qsort(bots.rows, (size_t)bots.n, sizeof *bots.rows, by_rating);
    list l;
    list_begin(&l, NW_LIST_HEAD_BYTES);
    for (int i = 0; i < humans.n && i < BOARD_TOP; i++) LIST_PUT(&l, 16 + 2 * NW_ID_MAX + NW_NAME_MAX, nw_put_board(&humans.rows[i], out, (int)cap_));
    for (int i = 0; i < bots.n && i < BOARD_TOP; i++) LIST_PUT(&l, 16 + 2 * NW_ID_MAX + NW_NAME_MAX, nw_put_board(&bots.rows[i], out, (int)cap_));
    free(humans.rows);
    free(bots.rows);
    list_send_as(res, &l, NW_BOARD, CC_BOARD);
}

static bool history_fn(const cb_snapshot_row *s, void *ud) {
    list *l = ud;
    static _Thread_local NwHistoryRow h;
    memset(&h, 0, sizeof h);
    size_t n = strlen(s->id);
    if (n > NW_ID_MAX || s->n_players > MAX_PLAYERS) return true;   // not a row a client can hold: skipped
    h.id_len = (uint8_t)n;
    memcpy(h.id, s->id, n);
    h.created_ms = s->created_at > 0 ? (uint64_t)s->created_at : 0;
    h.n_players = (uint8_t)s->n_players;
    for (int i = 0; i < s->n_players; i++) {
        size_t pn = strlen(s->player_ids[i]);
        h.player_len[i] = (uint8_t)(pn <= NW_ID_MAX ? pn : 0);
        memcpy(h.player[i], s->player_ids[i], h.player_len[i]);
    }
    h.moves.p = s->moves;
    h.moves.len = (uint32_t)s->moves_len;
    h.extras.p = s->extras;
    h.extras.len = (uint32_t)s->extras_len;
    LIST_PUT(l, 32 + (size_t)(MAX_PLAYERS + 1) * (NW_ID_MAX + 1) + s->moves_len + s->extras_len, nw_put_history(&h, out, (int)cap_));
    return !l->bad;
}

typedef struct {
    cb_store   *store;
    const char *id;
    int32_t     mine;
    int         rc;
} rating_read;

static void read_rating(void *ud) {
    rating_read *r = ud;
    const char *ids[1] = { r->id };
    r->rc = CB_STORE_CALL(r->store, ratings_for, ids, 1, &r->mine);
}

static void h_history(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    cb_user u;
    if (!caller(ctx, req, res, &u)) return;
    // The caller's own rating (one read by key, not the leaderboard) on a side
    // thread, while this one reads the replays: neither needs the other.
    rating_read rr = { .store = ctx->store, .id = u.id, .mine = TABLE_RATING_NONE };
    cb_store_side side;
    cb_store_side_start(&side, read_rating, &rr);
    list l;
    list_begin(&l, NW_HISTORY_HEAD_BYTES);
    cb_snapshot_query q = { u.id, NULL, NULL, HISTORY_PAGE, true };
    int rc = CB_STORE_CALL(ctx->store, snapshots_query, &q, history_fn, &l);
    cb_store_side_join(&side);
    if (rc >= 0 && rr.rc < 0) rc = rr.rc;
    if (rc < 0) { cb_buf_free(&l.body); failed(res, rc); return; }
    int32_t mine = rr.mine;
    bool found = mine != TABLE_RATING_NONE;
    uint32_t rating = found && mine > 0 ? (uint32_t)mine : 0;
    if (!l.bad && nw_history_head(found, rating, l.n, l.body.p, NW_HISTORY_HEAD_BYTES) == NW_HISTORY_HEAD_BYTES)
        octets(res, l.body.p, l.body.len);
    else failed(res, CB_E_NOMEM);
    cb_buf_free(&l.body);
}

typedef struct { cb_buf body; bool found, bad; } replay;

static bool replay_fn(const cb_snapshot_row *s, void *ud) {
    replay *r = ud;
    r->found = true;
    size_t cap = 16 + s->moves_len + s->extras_len;
    NwBlob mv = { s->moves, (uint32_t)s->moves_len }, ex = { s->extras, (uint32_t)s->extras_len };
    int n = cb_buf_reserve(&r->body, cap) ? nw_replay_encode(mv, ex, r->body.p, (int)cap) : -1;
    if (n < 0) r->bad = true;
    else r->body.len = (size_t)n;
    return false;   // the newest only
}

static void h_replay(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    cb_user u;
    char gid[CB_GAME_ID_MAX + 1];
    if (!caller(ctx, req, res, &u) || !path_id(req, res, "/v1/replay/", gid, sizeof gid)) return;
    replay r;
    cb_buf_init(&r.body);
    r.found = r.bad = false;
    // Only a replay of a game the caller played (game_snapshots' policy).
    cb_snapshot_query q = { u.id, gid, NULL, 1, true };
    int rc = CB_STORE_CALL(ctx->store, snapshots_query, &q, replay_fn, &r);
    if (rc < 0) failed(res, rc);
    else if (!r.found) refuse(res, 404, ASK_WHY_NOT_FOUND, 0);
    else if (r.bad) failed(res, CB_E_NOMEM);
    else octets(res, r.body.p, r.body.len);
    cb_buf_free(&r.body);
}

// ---- routes -----------------------------------------------------------------------------

int cb_v1_routes(cb_ctx *ctx) {
    static const struct { const char *method, *path; cb_handler fn; } routes[] = {
        { "POST", "/v1/auth/signup", h_signup },
        { "POST", "/v1/auth/signin", h_signin },
        { "POST", "/v1/auth/refresh", h_refresh },
        { "POST", "/v1/auth/signout", h_signout },
        { "POST", "/v1/auth/password", h_password },
        { "GET", "/v1/games", h_games },
        { "GET", "/v1/games/*", h_game },
        { "GET", "/v1/chat/*", h_chat_get },
        { "POST", "/v1/chat/*", h_chat_post },
        { "GET", "/v1/bots", h_bots },
        { "GET", "/v1/leaderboard", h_board },
        { "GET", "/v1/history", h_history },
        { "GET", "/v1/replay/*", h_replay },
    };
    for (size_t i = 0; i < sizeof routes / sizeof routes[0]; i++) {
        int rc = cb_router_add(ctx->router, routes[i].method, routes[i].path, CB_ROUTE_POOL, routes[i].fn);
        if (rc != CB_OK) return rc;
    }
    return CB_OK;
}
