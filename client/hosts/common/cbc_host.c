// cbc_host.c - the shared host (cbc_host.h says the contract it keeps).
#include "cbc_host.h"

#include <string.h>

cbc *cbc_host_client(cbc_host *h) { return &h->c; }
int64_t cbc_host_now(const cbc_host *h) { return h->env->now(h->ud); }

// ---- events ------------------------------------------------------------------------------------
static void session_save(cbc_host *h) {
    if (!h->env->save) return;
    int n = cbc_session_save(&h->c, h->sess, sizeof h->sess);
    h->env->save(h->ud, h->sess, n > 0 ? (size_t)n : 0);   // signed out (0): forget it
}

// The app's callback, counted (cbc_host_in_app). It may stop the host (cbc_host_stop): the rest of
// the batch is then neither saved nor told.
static void tell(cbc_host *h, const cbc_event *e, const uint8_t *bytes) {
    if (!h->fn) return;
    h->depth++;
    h->fn(h->ctx, &h->c, e, bytes);
    h->depth--;
}

static void events(cbc_host *h, const uint8_t *bytes) {
    int n = cbc_n_events(&h->c), saved = 0;
    for (int i = 0; i < n && !h->stopped; i++) {
        const cbc_event *e = cbc_event_at(&h->c, i);
        if (e->kind == CBC_EV_SESSION && !saved) { session_save(h); saved = 1; }
        tell(h, e, bytes);
    }
}

// ---- HTTP --------------------------------------------------------------------------------------
void cbc_host_http_answered(void *tag, int status, const uint8_t *body, size_t len) {
    cbc_host_pend *q = tag;
    cbc_host *h = q->h;
    q->used = 0;
    cbc_on_response(&h->c, q->id, status, body, len, cbc_host_now(h));
    events(h, body);
    cbc_host_pump(h);
}

static void send_request(cbc_host *h) {
    cbc_req_hdr r;
    memcpy(&r, h->req, sizeof r);
    cbc_host_pend *q = NULL;
    for (int i = 0; i < CBC_HOST_PEND_MAX && !q; i++)
        if (!h->pend[i].used) q = &h->pend[i];
    if (!q) {   // more out than the client can have: it never arrived
        cbc_on_response(&h->c, r.id, 0, NULL, 0, cbc_host_now(h));
        events(h, NULL);
        return;
    }
    q->h = h;
    q->id = r.id;
    q->used = 1;
    cbc_host_req rq;
    rq.method = r.method == CBC_POST ? "POST" : "GET";
    rq.path = (const char *)h->req + sizeof r;
    rq.path_len = r.path_len;
    rq.auth = r.auth_len ? rq.path + r.path_len : NULL;
    rq.auth_len = r.auth_len;
    // A borrowed body (cbc_call_ref) is the caller's, sent from where it is: never copied here.
    rq.body = r.flags & CBC_RQ_REF ? cbc_req_body(&h->c, r.id) : (const uint8_t *)rq.path + r.path_len + r.auth_len;
    rq.body_len = r.body_len;
    rq.wait_ms = r.wait_ms;
    h->env->http(h->ud, &rq, q);
}

// ---- the socket and the timers -----------------------------------------------------------------
static void on_reopen(void *ctx) {
    cbc_host *h = ctx;
    h->reopen_timer = 0;
    cbc_host_pump(h);
}

static void on_tick(void *ctx) {
    cbc_host *h = ctx;
    h->tick_timer = 0;
    cbc_tick(&h->c, cbc_host_now(h));
    events(h, NULL);
    cbc_host_pump(h);
}

// The socket is gone, opened or not: cbc_closed (which counts every try, so a socket that never
// opens backs off as one that keeps dropping does), and the reopen armed BEFORE the app hears the
// events: an app that re-issues a lost call and pumps from inside its callback must not open the
// next socket at once. A failure (why: a CBC_SOCK_*) is told the app first; 0: a close.
static void gone(cbc_host *h, int why) {
    h->has_sock = 0;
    h->open = 0;
    uint32_t wait = cbc_closed(&h->c, cbc_host_now(h));
    if (h->reopen_timer) h->env->timer_stop(h->ud, h->reopen_timer);
    h->reopen_timer = h->env->timer(h->ud, wait, on_reopen, h);
    if (why) {
        cbc_event e;
        memset(&e, 0, sizeof e);
        e.kind = CBC_EV_SOCKET_FAILED;
        e.why = (uint8_t)why;
        tell(h, &e, NULL);
    }
    events(h, NULL);
}

static void ws_open(cbc_host *h) {
    h->open = 0;
    int why = h->env->ws_open(h->ud, cbc_stream_path(&h->c), &h->sock);
    h->has_sock = why == 0;
    if (why) gone(h, why);
}

void cbc_host_ws_opened(cbc_host *h, uint32_t id) {
    if (!h->has_sock || id != h->sock) return;
    h->open = 1;
    cbc_opened(&h->c, cbc_host_now(h));
    cbc_host_pump(h);
}

void cbc_host_ws_message(cbc_host *h, uint32_t id, const uint8_t *frame, size_t len) {
    if (!h->has_sock || id != h->sock || !h->open) return;
    cbc_in(&h->c, frame, len, cbc_host_now(h));
    events(h, frame);
    cbc_host_pump(h);
}

void cbc_host_ws_closed(cbc_host *h, uint32_t id) {
    if (!h->has_sock || id != h->sock) return;
    gone(h, h->open ? 0 : CBC_SOCK_FAILED);
    cbc_host_pump(h);
}

void cbc_host_ws_failed(cbc_host *h, uint32_t id, int why) {
    if (!h->has_sock || id != h->sock) return;
    gone(h, h->open ? 0 : why);
    cbc_host_pump(h);
}

void cbc_host_pump(cbc_host *h) {
    if (h->held || h->stopped) return;
    int64_t now = cbc_host_now(h);
    // The app may stop the host from an event the pump itself raises: a socket that cannot be made
    // (below), or a request with no slot, answered at once (which the core's bound on the calls out
    // keeps from happening). After each, a stopped host goes no further.
    for (int n; (n = cbc_next_request(&h->c, h->req, sizeof h->req, now)) > 0;) {
        send_request(h);
        if (h->stopped) return;
    }
    // An open socket's close is the core's to ask for (cbc_next_out's -1, asked once: it answers 0
    // after it until cbc_closed, and cbc_due_ms counts nothing of the socket meanwhile).
    if (h->has_sock && h->open) {
        for (int n; (n = cbc_next_out(&h->c, h->frame, sizeof h->frame, now)) != 0;) {
            if (n < 0) {   // -1: close it now (CBC_E_CAP cannot happen at CBC_HOST_FRAME_CAP)
                h->env->ws_close(h->ud, h->sock);
                break;
            }
            h->env->ws_send(h->ud, h->sock, h->frame, (size_t)n);
        }
    }
    // One still opening, which the core has not heard of, is closed and forgotten here when nobody
    // wants it any more (its close, when the platform reports it, is not the client's).
    if (h->has_sock && !h->open && !cbc_stream_wanted(&h->c)) {
        h->env->ws_close(h->ud, h->sock);
        h->has_sock = 0;
    }
    if (!h->has_sock && !h->reopen_timer && cbc_stream_wanted(&h->c)) ws_open(h);
    if (h->stopped) return;
    if (h->tick_timer) { h->env->timer_stop(h->ud, h->tick_timer); h->tick_timer = 0; }
    int due = cbc_due_ms(&h->c, now);
    if (due >= 0) h->tick_timer = h->env->timer(h->ud, (uint32_t)due, on_tick, h);
}

void cbc_host_hold(cbc_host *h, int on) { h->held = on != 0; }

// A stopped host is held in two places, which between them cover every way out of it: the pump
// (nothing sent, opened or armed: the trailing pump of every answer, and the pump's own steps when
// the app stops it from an event the pump raised) and events (nothing saved or told). The socket
// reports need nothing more (the stop drops has_sock, so they ignore every id), nor does
// cbc_host_http_answered (it feeds the core, whose events and pump are those two).
void cbc_host_stop(cbc_host *h) {
    if (h->stopped) return;
    h->stopped = 1;
    if (h->tick_timer) h->env->timer_stop(h->ud, h->tick_timer);
    if (h->reopen_timer) h->env->timer_stop(h->ud, h->reopen_timer);
    h->tick_timer = h->reopen_timer = 0;
    if (h->has_sock) h->env->ws_close(h->ud, h->sock);   // its close, when the platform reports it, is ignored
    h->has_sock = 0;
    h->open = 0;
}

int cbc_host_in_app(const cbc_host *h) { return h->depth != 0; }

void cbc_host_init(cbc_host *h, const cbc_host_env *env, void *ud, const cbc_cfg *cfg, cbc_host_event_fn fn, void *ctx) {
    memset(h, 0, sizeof *h);
    h->env = env;
    h->ud = ud;
    h->fn = fn;
    h->ctx = ctx;
    cbc_init(&h->c, cfg);
}
