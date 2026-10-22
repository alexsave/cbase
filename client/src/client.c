// client.c - the c_base client (cbase/client.h): the socket's state machine, the calls and their
// retries, the session and its refresh. Every decision about time, retries and tokens is here,
// once, for every host; the host moves bytes.
#include "cbase/client.h"

#include "stream_frame.h"   // shared/c
#include "rng.h"            // shared/c
#include "le_bytes.h"       // shared/c

#include <limits.h>
#include <string.h>

_Static_assert(CBC_PATH_MAX == SF_PATH_MAX, "a call's path is an SF_REQ's");
_Static_assert(CBC_ROOM_MAX == SF_GID_MAX, "a room is an SF_SUB's gid");
_Static_assert(CBC_GET == SF_METH_GET && CBC_POST == SF_METH_POST, "a call's method is an SF_REQ's");
_Static_assert(CBW_ACCESS_MAX <= SF_TOKEN_MAX, "a HELLO carries the whole access token");

enum { SOCK_CLOSED, SOCK_OPEN, SOCK_HELLO, SOCK_READY };
enum { SUB_FREE, SUB_WANT, SUB_SENT, SUB_LIVE, SUB_REFUSED, SUB_DROP };
enum { CALL_FREE, CALL_QUEUED, CALL_WAIT, CALL_HTTP, CALL_SOCK };
enum { K_APP, K_SIGN, K_SIGNOUT };

#define CBC_TRIES        4     // a read is sent at most this often in all after losses
#define CBC_BUSY_TRIES   3     // and sent again at most this often after a 503 busy
#define CBC_BUSY_FIRST_MS 250  // 250, 500, 1000
#define TOP_BIT          0x80000000u   // a SUB's request id: its REPLY is told from a call's by it

// ---- small helpers -----------------------------------------------------------------------------
static int ok_status(int status) { return status >= 200 && status < 300; }

static uint32_t next_id31(uint32_t *n) {
    *n = (*n + 1) & 0x7fffffffu;
    if (*n == 0) *n = 1;
    return *n;
}

// A doubling wait: retry_first_ms for the first loss, doubling, capped at retry_max_ms.
static uint32_t backoff_ms(const cbc *c, int n) {
    uint64_t w = c->cfg.retry_first_ms;
    for (int i = 1; i < n && w < c->cfg.retry_max_ms; i++) w *= 2;
    return (uint32_t)(w < c->cfg.retry_max_ms ? w : c->cfg.retry_max_ms);
}

// A refusal body (auth_wire.h): 1 and the fields, or 0 for bytes that are not one.
static int refusal_read(const uint8_t *p, size_t n, int *why, int32_t *code, int32_t *detail) {
    if (!p || n != CBW_REFUSAL_BYTES || p[0] != CBW_REFUSAL_FORMAT || p[1] == 0) return 0;
    *why = p[1];
    *code = (int32_t)le_get_u32(p + 2);
    *detail = (int32_t)le_get_u32(p + 6);
    return 1;
}

// What a non-2xx answer means: the refusal's why, or one read from the status when the body is not
// a refusal (a proxy's 502, a body cut short, a body not handed in).
static void why_of(int status, const uint8_t *p, size_t n, int *why, int32_t *code, int32_t *detail) {
    *code = *detail = 0;
    if (refusal_read(p, n, why, code, detail)) return;
    *why = status == 0 ? CBC_WHY_LOST : status == 401 ? CBW_WHY_AUTH : status == 404 ? CBW_WHY_NOT_FOUND
         : status == 403 ? CBW_WHY_FORBIDDEN : status == 503 ? CBW_WHY_BUSY : CBW_WHY_INTERNAL;
}

// ---- events ------------------------------------------------------------------------------------
static void ev_reset(cbc *c) { c->n_events = 0; }

static cbc_event *ev_add(cbc *c, int kind) {
    if (c->n_events >= CBC_EVENTS_MAX) return NULL;   // cannot happen: no call raises more (client.h)
    cbc_event *e = &c->events[c->n_events++];
    memset(e, 0, sizeof *e);
    e->kind = (uint8_t)kind;
    return e;
}

int cbc_n_events(const cbc *c) { return c->n_events; }
const cbc_event *cbc_event_at(const cbc *c, int i) { return i >= 0 && i < c->n_events ? &c->events[i] : NULL; }

// ---- the arena: each waiting call's path, then its body ----------------------------------------
static int arena_take(cbc *c, size_t n) {
    if (n > (size_t)CBC_ARENA - c->arena_used) return -1;
    int at = c->arena_used;
    c->arena_used = (uint16_t)(c->arena_used + n);
    return at;
}

// Frees a call's bytes and moves every later call's down over them, so the arena never fragments.
static void arena_give(cbc *c, cbc_call_slot *k) {
    size_t n = (size_t)k->path_len + k->body_len, at = k->at_arena;
    if (!n) return;
    memmove(c->arena + at, c->arena + at + n, c->arena_used - at - n);
    c->arena_used = (uint16_t)(c->arena_used - n);
    for (int i = 0; i < CBC_CALLS_MAX; i++)
        if (c->calls[i].state != CALL_FREE && &c->calls[i] != k && c->calls[i].at_arena > at)
            c->calls[i].at_arena = (uint16_t)(c->calls[i].at_arena - n);
    k->path_len = k->body_len = 0;
}

static const char *call_path(const cbc *c, const cbc_call_slot *k) { return (const char *)c->arena + k->at_arena; }
static const uint8_t *call_body(const cbc *c, const cbc_call_slot *k) { return c->arena + k->at_arena + k->path_len; }

// ---- init --------------------------------------------------------------------------------------
void cbc_cfg_default(cbc_cfg *cfg) {
    memset(cfg, 0, sizeof *cfg);
    cfg->ping_ms = 30000;
    cfg->pong_ms = 10000;
    cfg->ack_wait_ms = 10000;
    cfg->retry_first_ms = 500;
    cfg->retry_max_ms = 10000;
    cfg->refresh_early_ms = 60000;
    cfg->force_refresh_ms = 600000;
    cfg->call_wait_ms = 15000;
}

void cbc_init(cbc *c, const cbc_cfg *cfg) {
    memset(c, 0, sizeof *c);
    c->cfg = *cfg;
    if (c->cfg.retry_first_ms == 0) c->cfg.retry_first_ms = 1;
    if (c->cfg.retry_max_ms < c->cfg.retry_first_ms) c->cfg.retry_max_ms = c->cfg.retry_first_ms;
    c->rng = cfg->seed;
}

int cbc_error(const cbc *c) { return c->err; }

// ---- the session -------------------------------------------------------------------------------
// A CBW_SESSION body into the client: 0, or -1 for bytes that are not one (nothing changes).
static int session_take(cbc *c, const uint8_t *p, size_t n) {
    if (!p || n < 4 || p[0] != CBW_V || p[1] != CBW_SESSION) return -1;
    size_t at = 2;
    size_t alen = le_get_u16(p + at); at += 2;
    if (alen == 0 || alen > CBW_ACCESS_MAX || n - at < alen + 1) return -1;
    const uint8_t *a = p + at; at += alen;
    size_t rlen = p[at++];
    if (rlen == 0 || rlen > CBW_REFRESH_MAX || n - at < rlen + 6 + 1) return -1;
    const uint8_t *r = p + at; at += rlen;
    uint64_t exp = le_get_u48(p + at); at += 6;
    size_t ilen = p[at++];
    if (ilen == 0 || ilen > CBW_ID_MAX || n - at < ilen + 1) return -1;
    const uint8_t *id = p + at; at += ilen;
    size_t nlen = p[at++];
    if (nlen > CBW_NAME_MAX || n - at != nlen) return -1;
    const uint8_t *name = p + at;
    memcpy(c->access, a, alen); c->access_len = (uint16_t)alen;
    memcpy(c->refresh, r, rlen); c->refresh_len = (uint8_t)rlen;
    memset(&c->user, 0, sizeof c->user);
    c->user.signed_in = 1;
    c->user.expires_at = (int64_t)exp;
    memcpy(c->user.user_id, id, ilen); c->user.user_id_len = (uint8_t)ilen;
    memcpy(c->user.name, name, nlen); c->user.name_len = (uint8_t)nlen;
    c->refresh_forced = 0;
    c->refresh_tries = 0;
    c->refresh_at = 0;
    return 0;
}

static void session_clear(cbc *c) {
    memset(&c->user, 0, sizeof c->user);
    memset(c->access, 0, sizeof c->access);
    memset(c->refresh, 0, sizeof c->refresh);
    c->access_len = 0;
    c->refresh_len = 0;
    c->ending = 0;
    c->refresh_out = 0;
    c->refresh_forced = 0;
    c->refresh_tries = 0;
    c->refresh_at = 0;
}

int cbc_session_save(const cbc *c, uint8_t *out, size_t cap) {
    if (!c->user.signed_in) return 0;
    size_t n = 2 + 2 + c->access_len + 1 + c->refresh_len + 6 + 1 + c->user.user_id_len + 1 + c->user.name_len;
    if (cap < n) return CBC_E_CAP;
    size_t at = 0;
    out[at++] = CBW_V; out[at++] = CBW_SESSION;
    le_put_u16(out + at, c->access_len); at += 2;
    memcpy(out + at, c->access, c->access_len); at += c->access_len;
    out[at++] = c->refresh_len;
    memcpy(out + at, c->refresh, c->refresh_len); at += c->refresh_len;
    le_put_u48(out + at, (uint64_t)c->user.expires_at); at += 6;
    out[at++] = c->user.user_id_len;
    memcpy(out + at, c->user.user_id, c->user.user_id_len); at += c->user.user_id_len;
    out[at++] = c->user.name_len;
    memcpy(out + at, c->user.name, c->user.name_len); at += c->user.name_len;
    return (int)at;
}

int cbc_session_load(cbc *c, const uint8_t *p, size_t n) {
    if (session_take(c, p, n) == 0) return 0;
    session_clear(c);
    return -1;
}

const cbc_user *cbc_user_of(const cbc *c) { return &c->user; }

// The access token is to be refreshed before it is used again.
static int token_due(const cbc *c, int64_t now) {
    return c->user.signed_in && (c->refresh_forced || now >= c->user.expires_at - (int64_t)c->cfg.refresh_early_ms);
}

// ---- calls -------------------------------------------------------------------------------------
static cbc_call_slot *call_by_id(cbc *c, int state, uint32_t id) {
    for (int i = 0; i < CBC_CALLS_MAX; i++)
        if (c->calls[i].state == state && c->calls[i].id == id) return &c->calls[i];
    return NULL;
}

// A call goes over HTTP (else it rides the socket): a session's own calls, a call flagged so, and
// every call of a client with no signed-in user (an anonymous socket takes no REQ).
static int over_http(const cbc *c, const cbc_call_slot *k) {
    return k->kind != K_APP || (k->flags & CBC_C_HTTP) || c->cfg.anonymous || !c->user.signed_in;
}

static int call_ready(const cbc_call_slot *k, int64_t now) {
    return k->state == CALL_QUEUED || (k->state == CALL_WAIT && k->at <= now);
}

static void call_free(cbc *c, cbc_call_slot *k) {
    arena_give(c, k);
    memset(k, 0, sizeof *k);
}

// The call is over: its DONE, with the span of its answer's body, then its slot freed.
static void call_done(cbc *c, cbc_call_slot *k, int status, int why, int32_t code, int32_t detail, uint32_t off, uint32_t len) {
    cbc_event *e = ev_add(c, CBC_EV_DONE);
    if (e) {
        e->ticket = k->ticket;
        e->status = (uint16_t)(status < 0 ? 0 : status > 65535 ? 65535 : status);
        e->why = (uint8_t)why;
        e->code = code;
        e->detail = detail;
        e->off = off;
        e->len = len;
    }
    call_free(c, k);
}

// A call that never got its answer (the socket closed, or no REPLY came, or HTTP status 0): a read
// goes again, at once on the socket's next READY or after a doubling wait over HTTP, CBC_TRIES
// times in all; a write is over (it may have landed).
static void call_lost(cbc *c, cbc_call_slot *k, int64_t now, int http) {
    if ((k->flags & CBC_C_READ) && k->tries + 1 < CBC_TRIES) {
        k->tries++;
        k->state = http ? CALL_WAIT : CALL_QUEUED;
        k->at = http ? now + backoff_ms(c, k->tries) : 0;
        return;
    }
    call_done(c, k, 0, CBC_WHY_LOST, 0, 0, 0, 0);
}

static uint32_t call_add(cbc *c, int kind, int method, int flags, const char *path, size_t pn, const uint8_t *body, size_t bn) {
    c->err = 0;
    if ((method != SF_METH_GET && method != SF_METH_POST) || pn == 0 || pn > CBC_PATH_MAX || path[0] != '/' ||
        bn > 0xffff || (bn && !body)) { c->err = CBC_E_ARG; return 0; }
    cbc_call_slot *k = NULL;
    for (int i = 0; i < CBC_CALLS_MAX && !k; i++)
        if (c->calls[i].state == CALL_FREE) k = &c->calls[i];
    if (!k) { c->err = CBC_E_FULL; return 0; }
    int at = arena_take(c, pn + bn);
    if (at < 0) { c->err = CBC_E_FULL; return 0; }
    memcpy(c->arena + at, path, pn);
    if (bn) memcpy(c->arena + at + pn, body, bn);
    memset(k, 0, sizeof *k);
    k->state = CALL_QUEUED;
    k->kind = (uint8_t)kind;
    k->method = (uint8_t)method;
    k->flags = (uint8_t)flags;
    k->at_arena = (uint16_t)at;
    k->path_len = (uint16_t)pn;
    k->body_len = (uint16_t)bn;
    k->ticket = next_id31(&c->next_ticket);
    return k->ticket;
}

uint32_t cbc_call(cbc *c, int method, int flags, const char *path, size_t path_len,
                  const uint8_t *body, size_t body_len, int64_t now) {
    (void)now;
    if (c->ending || ((flags & CBC_C_AUTH) && !c->user.signed_in)) { c->err = CBC_E_AUTH; return 0; }
    return call_add(c, K_APP, method, flags & (CBC_C_READ | CBC_C_AUTH | CBC_C_HTTP), path, path_len, body, body_len);
}

uint32_t cbc_sign_in(cbc *c, int op, const char *name, size_t name_len, const char *pw, size_t pw_len, int64_t now) {
    (void)now;
    if ((op != CBW_SIGNUP && op != CBW_SIGNIN) || name_len == 0 || name_len > 255 || pw_len > 255) { c->err = CBC_E_ARG; return 0; }
    if (c->ending) { c->err = CBC_E_AUTH; return 0; }
    uint8_t b[2 + 1 + 255 + 1 + 255];
    size_t n = 0;
    b[n++] = CBW_V; b[n++] = (uint8_t)op;
    b[n++] = (uint8_t)name_len; memcpy(b + n, name, name_len); n += name_len;
    b[n++] = (uint8_t)pw_len; if (pw_len) memcpy(b + n, pw, pw_len); n += pw_len;
    const char *path = op == CBW_SIGNUP ? "/v1/auth/signup" : "/v1/auth/signin";
    return call_add(c, K_SIGN, SF_METH_POST, 0, path, strlen(path), b, n);
}

uint32_t cbc_sign_out(cbc *c, int global, int64_t now) {
    (void)now;
    if (!c->user.signed_in || c->ending) { c->err = CBC_E_AUTH; return 0; }
    uint8_t b[3] = { CBW_V, CBW_SIGNOUT, (uint8_t)(global ? CBW_SCOPE_GLOBAL : CBW_SCOPE_LOCAL) };
    uint32_t t = call_add(c, K_SIGNOUT, SF_METH_POST, CBC_C_AUTH, "/v1/auth/signout", 16, b, sizeof b);
    if (t) c->ending = 1;
    return t;
}

// The session is over (signed out, or its refresh refused): every call waiting for the user is
// finished with CBW_WHY_AUTH, and the socket closes (cbc_next_out).
static void session_end(cbc *c) {
    session_clear(c);
    ev_add(c, CBC_EV_SESSION);
    for (int i = 0; i < CBC_CALLS_MAX; i++) {
        cbc_call_slot *k = &c->calls[i];
        if ((k->state == CALL_QUEUED || k->state == CALL_WAIT) && k->kind == K_APP && (k->flags & CBC_C_AUTH))
            call_done(c, k, 401, CBW_WHY_AUTH, 0, 0, 0, 0);
    }
}

// ---- HTTP --------------------------------------------------------------------------------------
static int refresh_sendable(const cbc *c, int64_t now) {
    return c->user.signed_in && !c->ending && !c->refresh_out && token_due(c, now) && now >= c->refresh_at;
}

// Writes one request: the head, the path, "Bearer <token>" when auth, the body.
static int put_request(cbc *c, uint8_t *out, size_t cap, uint32_t id, int method, int flags,
                       const char *path, size_t pn, int auth, const uint8_t *body, size_t bn) {
    size_t an = auth ? 7 + (size_t)c->access_len : 0;
    size_t n = sizeof(cbc_req_hdr) + pn + an + bn;
    if (cap < n) { c->err = CBC_E_CAP; return CBC_E_CAP; }
    cbc_req_hdr h;
    memset(&h, 0, sizeof h);
    h.id = id;
    h.method = (uint8_t)method;
    h.flags = (uint8_t)flags;
    h.path_len = (uint16_t)pn;
    h.auth_len = (uint16_t)an;
    h.body_len = (uint32_t)bn;
    memcpy(out, &h, sizeof h);
    size_t at = sizeof h;
    memcpy(out + at, path, pn); at += pn;
    if (auth) { memcpy(out + at, "Bearer ", 7); memcpy(out + at + 7, c->access, c->access_len); at += an; }
    if (bn) memcpy(out + at, body, bn);
    return (int)n;
}

int cbc_next_request(cbc *c, uint8_t *out, size_t cap, int64_t now) {
    if (refresh_sendable(c, now)) {
        uint8_t b[3 + CBW_REFRESH_MAX];
        b[0] = CBW_V; b[1] = CBW_REFRESH; b[2] = c->refresh_len;
        memcpy(b + 3, c->refresh, c->refresh_len);
        uint32_t id = next_id31(&c->next_http_id);
        int n = put_request(c, out, cap, id, SF_METH_POST, CBC_RQ_BODY, "/v1/auth/refresh", 16, 0, b, 3u + c->refresh_len);
        if (n < 0) return n;
        c->refresh_id = id;
        c->refresh_out = 1;
        return n;
    }
    // The oldest call that is due and goes over HTTP; one that needs the user waits for a refresh.
    cbc_call_slot *best = NULL;
    for (int i = 0; i < CBC_CALLS_MAX; i++) {
        cbc_call_slot *k = &c->calls[i];
        if (!call_ready(k, now) || !over_http(c, k)) continue;
        if (k->kind != K_SIGN && (k->flags & CBC_C_AUTH) && token_due(c, now)) continue;
        if (!best || (int32_t)(k->ticket - best->ticket) < 0) best = k;
    }
    if (!best) return 0;
    uint32_t id = next_id31(&c->next_http_id);
    int auth = c->user.signed_in && best->kind != K_SIGN;
    int n = put_request(c, out, cap, id, best->method, best->kind == K_SIGN ? CBC_RQ_BODY : 0,
                        call_path(c, best), best->path_len, auth, call_body(c, best), best->body_len);
    if (n < 0) return n;
    best->id = id;
    best->state = CALL_HTTP;
    best->at = now;
    return n;
}

static void refresh_answered(cbc *c, int status, const uint8_t *p, size_t n, int64_t now) {
    c->refresh_out = 0;
    if (ok_status(status) && session_take(c, p, n) == 0) {
        ev_add(c, CBC_EV_SESSION);
        return;
    }
    int why; int32_t code, detail;
    why_of(status, p, n, &why, &code, &detail);
    if (why == CBW_WHY_ACCOUNT && (code == CBW_ACCOUNT_E_BAD_REFRESH || code == CBW_ACCOUNT_E_REFRESH_USED)) {
        session_end(c);
        return;
    }
    // Not answered, or answered badly: try again after a doubling wait; after CBC_TRIES, finish
    // the calls waiting for the user (they would wait forever) and start the count again.
    c->refresh_tries++;
    c->refresh_at = now + backoff_ms(c, c->refresh_tries);
    if (c->refresh_tries >= CBC_TRIES) {
        c->refresh_tries = 0;
        for (int i = 0; i < CBC_CALLS_MAX; i++) {
            cbc_call_slot *k = &c->calls[i];
            if ((k->state == CALL_QUEUED || k->state == CALL_WAIT) && (k->flags & CBC_C_AUTH))
                call_done(c, k, status, CBW_WHY_AUTH, 0, 0, 0, 0);
        }
    }
}

int cbc_on_response(cbc *c, uint32_t id, int status, const uint8_t *body, size_t len, int64_t now) {
    ev_reset(c);
    if (c->refresh_out && id == c->refresh_id) {
        refresh_answered(c, status, body, len, now);
        return c->n_events;
    }
    cbc_call_slot *k = call_by_id(c, CALL_HTTP, id);
    if (!k) return 0;
    if (k->kind == K_SIGNOUT) {   // whatever the answer, the session is over
        call_done(c, k, status, 0, 0, 0, 0, 0);
        session_end(c);
        return c->n_events;
    }
    if (status == 0) { call_lost(c, k, now, 1); return c->n_events; }
    if (ok_status(status)) {
        if (k->kind == K_SIGN) {
            if (session_take(c, body, len) != 0) { call_done(c, k, status, CBW_WHY_MALFORMED, 0, 0, 0, 0); return c->n_events; }
            ev_add(c, CBC_EV_SESSION);
        }
        call_done(c, k, status, 0, 0, 0, 0, (uint32_t)len);
        return c->n_events;
    }
    int why; int32_t code, detail;
    why_of(status, body, len, &why, &code, &detail);
    if (status == 503 && why == CBW_WHY_BUSY && (k->flags & CBC_C_READ) && k->busy < CBC_BUSY_TRIES) {
        k->state = CALL_WAIT;
        k->at = now + ((int64_t)CBC_BUSY_FIRST_MS << k->busy);
        k->busy++;
        return c->n_events;
    }
    if (status == 401 && k->kind == K_APP && c->user.signed_in && !k->reauthed) {
        k->reauthed = 1;
        c->refresh_forced = 1;
        k->state = CALL_QUEUED;
        return c->n_events;
    }
    call_done(c, k, status, why, code, detail, 0, (uint32_t)len);
    return c->n_events;
}

// ---- the socket --------------------------------------------------------------------------------
int cbc_stream_wanted(const cbc *c) {
    if (c->cfg.anonymous) {
        for (int i = 0; i < CBC_SUBS_MAX; i++)
            if (c->subs[i].state != SUB_FREE && c->subs[i].state != SUB_DROP) return 1;
        return 0;
    }
    return c->user.signed_in && !c->ending;
}

const char *cbc_stream_path(const cbc *c) { return c->cfg.anonymous ? "/ws/public" : "/ws"; }

static cbc_sub_slot *sub_find(cbc *c, int what, const char *room, size_t n) {
    for (int i = 0; i < CBC_SUBS_MAX; i++) {
        cbc_sub_slot *s = &c->subs[i];
        if (s->state != SUB_FREE && s->what == what && s->room_len == n && memcmp(s->room, room, n) == 0) return s;
    }
    return NULL;
}

int cbc_sub(cbc *c, int what, const char *room, size_t n) {
    if (what < 0 || what > 255 || n == 0 || n > CBC_ROOM_MAX) return CBC_E_ARG;
    cbc_sub_slot *s = sub_find(c, what, room, n);
    if (s) {
        if (s->state == SUB_DROP) { s->state = SUB_LIVE; s->holds = 1; return 0; }   // its UNSUB never went
        if (s->holds == 255) return CBC_E_FULL;
        s->holds++;
        return 0;
    }
    for (int i = 0; i < CBC_SUBS_MAX; i++) {
        s = &c->subs[i];
        if (s->state != SUB_FREE) continue;
        memset(s, 0, sizeof *s);
        s->state = SUB_WANT;
        s->what = (uint8_t)what;
        s->room_len = (uint8_t)n;
        memcpy(s->room, room, n);
        s->holds = 1;
        return 0;
    }
    return CBC_E_FULL;
}

int cbc_unsub(cbc *c, int what, const char *room, size_t n) {
    if (what < 0 || what > 255 || n == 0 || n > CBC_ROOM_MAX) return CBC_E_ARG;
    cbc_sub_slot *s = sub_find(c, what, room, n);
    if (!s || s->state == SUB_DROP) return CBC_E_ARG;
    if (--s->holds) return 0;
    // The server knows it only once it was sent on the socket that is open now.
    if ((s->state == SUB_SENT || s->state == SUB_LIVE) && c->sock == SOCK_READY) s->state = SUB_DROP;
    else memset(s, 0, sizeof *s);
    return 0;
}

void cbc_opened(cbc *c, int64_t now) {
    c->sock = SOCK_OPEN;
    c->roll = 0;
    c->ping_at = 0;
    c->last_out = now;
}

// The decorrelated jitter (CLIENT_DESIGN.md 6.2): uniform in [floor, min(cap, 3 x the last wait)].
static uint32_t reconnect_wait(cbc *c) {
    uint32_t lo = c->cfg.retry_first_ms;
    uint64_t prev = c->reconnect_prev ? c->reconnect_prev : lo;
    uint64_t hi = prev * 3;
    if (hi > c->cfg.retry_max_ms) hi = c->cfg.retry_max_ms;
    if (hi < lo) hi = lo;
    uint32_t w = lo + (uint32_t)(rng_splitmix64(&c->rng) % (hi - lo + 1));
    c->reconnect_prev = w;
    return w;
}

uint32_t cbc_closed(cbc *c, int64_t now) {
    ev_reset(c);
    int was = c->sock;
    c->sock = SOCK_CLOSED;
    c->ping_at = 0;
    for (int i = 0; i < CBC_CALLS_MAX; i++)
        if (c->calls[i].state == CALL_SOCK) call_lost(c, &c->calls[i], now, 0);
    for (int i = 0; i < CBC_SUBS_MAX; i++) {
        cbc_sub_slot *s = &c->subs[i];
        if (s->state == SUB_DROP) memset(s, 0, sizeof *s);
        else if (s->state == SUB_SENT || s->state == SUB_LIVE) s->state = SUB_WANT;
    }
    if (c->roll) { c->roll = 0; return 0; }
    // A socket closed after its HELLO and before READY was refused: its token, most likely. Refresh
    // before the next HELLO, at most once every force_refresh_ms.
    if (was == SOCK_HELLO && !c->cfg.anonymous && c->user.signed_in &&
        (c->forced_last == 0 || now - c->forced_last >= (int64_t)c->cfg.force_refresh_ms)) {
        c->refresh_forced = 1;
        c->forced_last = now;
    }
    return reconnect_wait(c);
}

static void take_reply(cbc *c, const sf_frame *f, int64_t now) {
    if (f->id & TOP_BIT) {
        for (int i = 0; i < CBC_SUBS_MAX; i++) {
            cbc_sub_slot *s = &c->subs[i];
            if (s->state != SUB_SENT || s->id != f->id) continue;
            cbc_event *e;
            if (f->status == 200) {
                s->state = SUB_LIVE;
                s->backoff = 0;
                e = ev_add(c, CBC_EV_SUB_LIVE);
            } else {
                s->state = SUB_REFUSED;
                s->backoff = s->backoff ? (s->backoff * 2 < c->cfg.retry_max_ms ? s->backoff * 2 : c->cfg.retry_max_ms)
                                        : c->cfg.retry_first_ms;
                s->at = now + s->backoff;
                e = ev_add(c, CBC_EV_SUB_REFUSED);
            }
            if (e) {
                e->what = s->what;
                e->status = f->status;
                e->room_len = s->room_len;
                memcpy(e->room, s->room, s->room_len);
            }
            return;
        }
        return;   // an UNSUB's, or a SUB's that was given up on
    }
    cbc_call_slot *k = call_by_id(c, CALL_SOCK, f->id);
    if (!k) return;
    uint32_t off = 8, len = (uint32_t)f->body_len;
    if (ok_status(f->status)) { call_done(c, k, f->status, 0, 0, 0, off, len); return; }
    int why; int32_t code, detail;
    why_of(f->status, f->body, f->body_len, &why, &code, &detail);
    if (f->status == 503 && why == CBW_WHY_BUSY && (k->flags & CBC_C_READ) && k->busy < CBC_BUSY_TRIES) {
        k->state = CALL_WAIT;
        k->at = now + ((int64_t)CBC_BUSY_FIRST_MS << k->busy);
        k->busy++;
        return;
    }
    call_done(c, k, f->status, why, code, detail, off, len);
}

int cbc_in(cbc *c, const uint8_t *p, size_t n, int64_t now) {
    ev_reset(c);
    c->ping_at = 0;   // any frame answers the probe
    if (sf_is_push(p, n)) {
        cbc_event *e = ev_add(c, CBC_EV_PUSH);
        if (e) { e->push = p[1]; e->off = 0; e->len = (uint32_t)n; }
        return c->n_events;
    }
    sf_frame f;
    if (sf_parse(p, n, &f) != 0) { ev_add(c, CBC_EV_BAD); return c->n_events; }
    switch (f.kind) {
    case SF_READY:
        if (c->sock != SOCK_HELLO) { ev_add(c, CBC_EV_BAD); break; }
        c->sock = SOCK_READY;
        c->reconnect_prev = 0;
        ev_add(c, CBC_EV_READY);
        break;
    case SF_REPLY:
        take_reply(c, &f, now);
        break;
    case SF_PONG:
        break;
    default:   // a client's frame, from the server
        ev_add(c, CBC_EV_BAD);
        break;
    }
    return c->n_events;
}

static int sock_calls_out(const cbc *c) {
    for (int i = 0; i < CBC_CALLS_MAX; i++)
        if (c->calls[i].state == CALL_SOCK) return 1;
    return 0;
}

static int sub_due(const cbc *c, const cbc_sub_slot *s, int64_t now) {
    return s->state == SUB_WANT || (s->state == SUB_REFUSED && s->at <= now) ||
           (s->state == SUB_SENT && s->at + (int64_t)c->cfg.ack_wait_ms <= now);
}

// The token's roll: a refresh moved the expiry since this socket's HELLO, and nothing rides it.
static int roll_due(const cbc *c, int64_t now) {
    return !c->cfg.anonymous && c->sock == SOCK_READY && c->user.signed_in && c->hello_exp != c->user.expires_at &&
           !token_due(c, now) && !sock_calls_out(c);
}

static int sent(cbc *c, int n, int64_t now) {
    if (n < 0) { c->err = CBC_E_CAP; return CBC_E_CAP; }
    c->last_out = now;
    return n;
}

int cbc_next_out(cbc *c, uint8_t *out, size_t cap, int64_t now) {
    if (c->sock == SOCK_CLOSED) return 0;
    if (!cbc_stream_wanted(c)) return -1;
    if (c->sock == SOCK_OPEN) {
        if (c->cfg.anonymous) {
            int n = sf_put_hello(out, cap, "", 0);
            if (n < 0) return sent(c, n, now);
            c->sock = SOCK_HELLO;
            c->hello_at = now;
            c->hello_exp = 0;
            return sent(c, n, now);
        }
        if (token_due(c, now)) return 0;   // the refresh first (cbc_next_request)
        int n = sf_put_hello(out, cap, c->access, c->access_len);
        if (n < 0) return sent(c, n, now);
        c->sock = SOCK_HELLO;
        c->hello_at = now;
        c->hello_exp = c->user.expires_at;
        return sent(c, n, now);
    }
    if (c->sock == SOCK_HELLO) return now - c->hello_at >= (int64_t)c->cfg.ack_wait_ms ? -1 : 0;
    // READY.
    if (c->ping_at && now - c->ping_at >= (int64_t)c->cfg.pong_ms) return -1;   // half-open
    if (roll_due(c, now)) { c->roll = 1; return -1; }
    for (int i = 0; i < CBC_SUBS_MAX; i++) {
        cbc_sub_slot *s = &c->subs[i];
        if (s->state != SUB_DROP) continue;
        int n = sf_put_sub(out, cap, SF_UNSUB, next_id31(&c->next_sub_id) | TOP_BIT, s->what, s->room, s->room_len);
        if (n < 0) return sent(c, n, now);
        memset(s, 0, sizeof *s);
        return sent(c, n, now);
    }
    for (int i = 0; i < CBC_SUBS_MAX; i++) {
        cbc_sub_slot *s = &c->subs[i];
        if (!sub_due(c, s, now)) continue;
        uint32_t id = next_id31(&c->next_sub_id) | TOP_BIT;
        int n = sf_put_sub(out, cap, SF_SUB, id, s->what, s->room, s->room_len);
        if (n < 0) return sent(c, n, now);
        s->state = SUB_SENT;
        s->id = id;
        s->at = now;
        return sent(c, n, now);
    }
    cbc_call_slot *best = NULL;
    for (int i = 0; i < CBC_CALLS_MAX; i++) {
        cbc_call_slot *k = &c->calls[i];
        if (call_ready(k, now) && !over_http(c, k) && (!best || (int32_t)(k->ticket - best->ticket) < 0)) best = k;
    }
    if (best) {
        uint32_t id = next_id31(&c->next_sock_id);
        int n = sf_put_req(out, cap, id, best->method, call_path(c, best), best->path_len, call_body(c, best), best->body_len);
        if (n < 0) return sent(c, n, now);
        best->id = id;
        best->state = CALL_SOCK;
        best->at = now;
        return sent(c, n, now);
    }
    if (!c->ping_at && now - c->last_out >= (int64_t)c->cfg.ping_ms) {
        int n = sf_put_bare(out, cap, SF_PING);
        if (n < 0) return sent(c, n, now);
        c->ping_at = now;
        return sent(c, n, now);
    }
    return 0;
}

// ---- time --------------------------------------------------------------------------------------
int cbc_tick(cbc *c, int64_t now) {
    ev_reset(c);
    for (int i = 0; i < CBC_CALLS_MAX; i++) {
        cbc_call_slot *k = &c->calls[i];
        if (k->state == CALL_SOCK && now - k->at >= (int64_t)c->cfg.call_wait_ms) call_lost(c, k, now, 0);
    }
    return c->n_events;
}

static void earliest(int64_t *t, int64_t at) { if (at < *t) *t = at; }

int cbc_due_ms(const cbc *c, int64_t now) {
    int64_t t = INT64_MAX;
    if (c->user.signed_in && !c->ending && !c->refresh_out) {
        int64_t r = c->refresh_forced ? now : c->user.expires_at - (int64_t)c->cfg.refresh_early_ms;
        earliest(&t, r > c->refresh_at ? r : c->refresh_at);
    }
    int ready = c->sock == SOCK_READY;
    for (int i = 0; i < CBC_CALLS_MAX; i++) {
        const cbc_call_slot *k = &c->calls[i];
        if (k->state == CALL_QUEUED && (over_http(c, k) || ready)) earliest(&t, now);
        else if (k->state == CALL_WAIT && (over_http(c, k) || ready)) earliest(&t, k->at);
        else if (k->state == CALL_SOCK) earliest(&t, k->at + (int64_t)c->cfg.call_wait_ms);
    }
    if (c->sock != SOCK_CLOSED && !cbc_stream_wanted(c)) earliest(&t, now);
    if (c->sock == SOCK_OPEN && (c->cfg.anonymous || !token_due(c, now))) earliest(&t, now);
    if (c->sock == SOCK_HELLO) earliest(&t, c->hello_at + (int64_t)c->cfg.ack_wait_ms);
    if (ready) {
        earliest(&t, c->ping_at ? c->ping_at + (int64_t)c->cfg.pong_ms : c->last_out + (int64_t)c->cfg.ping_ms);
        if (roll_due(c, now)) earliest(&t, now);
        for (int i = 0; i < CBC_SUBS_MAX; i++) {
            const cbc_sub_slot *s = &c->subs[i];
            if (s->state == SUB_WANT || s->state == SUB_DROP) earliest(&t, now);
            else if (s->state == SUB_REFUSED) earliest(&t, s->at);
            else if (s->state == SUB_SENT) earliest(&t, s->at + (int64_t)c->cfg.ack_wait_ms);
        }
    }
    if (t == INT64_MAX) return -1;
    if (t <= now) return 0;
    return t - now > INT_MAX ? INT_MAX : (int)(t - now);
}
