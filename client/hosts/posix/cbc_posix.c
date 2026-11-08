// cbc_posix.c - the C SDK's native host (cbc_posix.h says the contract it keeps).
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE   // glibc: getentropy and clock_gettime under a strict -std
#endif
#include "cbc_posix.h"

#include <string.h>
#include <time.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/random.h>   // getentropy
#endif

int64_t cbc_posix_now(const cbc_posix *h) {
    if (h->io.now) return h->io.now(h->io.ud);
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

cbc *cbc_posix_client(cbc_posix *h) { return &h->c; }

// ---- the session -------------------------------------------------------------------------------
static void session_save(cbc_posix *h) {
    if (!h->io.save) return;
    int n = cbc_session_save(&h->c, h->sess, sizeof h->sess);
    h->io.save(h->io.ud, h->sess, n > 0 ? (size_t)n : 0);   // signed out (0): forget it
}

static void session_load(cbc_posix *h) {
    if (!h->io.load) return;
    size_t n = h->io.load(h->io.ud, h->sess, sizeof h->sess);
    if (n > 0 && n <= sizeof h->sess) cbc_session_load(&h->c, h->sess, n);
}

// ---- events ------------------------------------------------------------------------------------
static void events(cbc_posix *h, const uint8_t *bytes) {
    int n = cbc_n_events(&h->c), saved = 0;
    for (int i = 0; i < n; i++) {
        const cbc_event *e = cbc_event_at(&h->c, i);
        if (e->kind == CBC_EV_SESSION && !saved) { session_save(h); saved = 1; }
        if (h->fn) h->fn(h->ctx, &h->c, e, bytes);
    }
}

// ---- HTTP --------------------------------------------------------------------------------------
static void on_http(void *ctx, int status, const uint8_t *body, size_t len) {
    cbc_posix_pend *q = ctx;
    cbc_posix *h = q->h;
    q->used = 0;
    cbc_on_response(&h->c, q->id, status, body, len, cbc_posix_now(h));
    events(h, body);
    cbc_posix_pump(h);
}

static void send_request(cbc_posix *h) {
    cbc_req_hdr r;
    memcpy(&r, h->req, sizeof r);
    cbc_posix_pend *q = NULL;
    for (int i = 0; i < CBC_POSIX_PEND_MAX && !q; i++)
        if (!h->pend[i].used) q = &h->pend[i];
    if (!q) {   // more out than the client can have: it never arrived
        cbc_on_response(&h->c, r.id, 0, NULL, 0, cbc_posix_now(h));
        events(h, NULL);
        return;
    }
    q->h = h;
    q->id = r.id;
    q->used = 1;
    const char *path = (const char *)h->req + sizeof r;
    const char *auth = path + r.path_len;
    const uint8_t *body = (const uint8_t *)auth + r.auth_len;
    h->io.http(h->io.ud, r.method == CBC_POST ? "POST" : "GET", path, r.path_len, r.auth_len ? auth : NULL, r.auth_len,
               body, r.body_len, on_http, q);
}

// ---- the socket and the timers -----------------------------------------------------------------
static void on_reopen(void *ctx) {
    cbc_posix *h = ctx;
    h->reopen_timer = 0;
    cbc_posix_pump(h);
}

static void on_tick(void *ctx) {
    cbc_posix *h = ctx;
    h->tick_timer = 0;
    cbc_tick(&h->c, cbc_posix_now(h));
    events(h, NULL);
    cbc_posix_pump(h);
}

static void ws_open(cbc_posix *h) {
    h->open = h->closing = 0;
    h->sock = h->io.ws_open(h->io.ud, cbc_stream_path(&h->c));
    if (!h->sock) h->reopen_timer = h->io.timer(h->io.ud, h->c.cfg.retry_first_ms, on_reopen, h);
}

void cbc_posix_ws_opened(cbc_posix *h, uint32_t id) {
    if (!h->sock || id != h->sock) return;
    h->open = 1;
    cbc_opened(&h->c, cbc_posix_now(h));
    cbc_posix_pump(h);
}

void cbc_posix_ws_message(cbc_posix *h, uint32_t id, const uint8_t *frame, size_t len) {
    if (!h->sock || id != h->sock || !h->open) return;
    cbc_in(&h->c, frame, len, cbc_posix_now(h));
    events(h, frame);
    cbc_posix_pump(h);
}

void cbc_posix_ws_closed(cbc_posix *h, uint32_t id) {
    if (!h->sock || id != h->sock) return;
    h->sock = 0;
    h->open = h->closing = 0;
    uint32_t wait = cbc_closed(&h->c, cbc_posix_now(h));
    events(h, NULL);
    if (h->reopen_timer) h->io.timer_stop(h->io.ud, h->reopen_timer);
    h->reopen_timer = h->io.timer(h->io.ud, wait, on_reopen, h);
    cbc_posix_pump(h);
}

void cbc_posix_pump(cbc_posix *h) {
    int64_t now = cbc_posix_now(h);
    for (int n; (n = cbc_next_request(&h->c, h->req, sizeof h->req, now)) > 0;) send_request(h);
    if (h->sock && h->open && !h->closing) {
        for (int n; (n = cbc_next_out(&h->c, h->frame, sizeof h->frame, now)) != 0;) {
            if (n < 0) {   // -1: close it now (CBC_E_CAP cannot happen at CBC_POSIX_FRAME_CAP)
                h->io.ws_close(h->io.ud, h->sock);
                h->closing = 1;
                break;
            }
            h->io.ws_send(h->io.ud, h->sock, h->frame, (size_t)n);
        }
    }
    if (!h->sock && !h->reopen_timer && cbc_stream_wanted(&h->c)) ws_open(h);
    if (h->sock && !h->closing && !cbc_stream_wanted(&h->c)) {
        h->io.ws_close(h->io.ud, h->sock);
        h->closing = 1;
    }
    if (h->tick_timer) { h->io.timer_stop(h->io.ud, h->tick_timer); h->tick_timer = 0; }
    // While the socket is closing the client still has it, and answers 0 for what it wants done
    // with it (the close, the HELLO of the next): after this pump nothing else can be due at once,
    // so a 0 then waits for the close's report, which pumps, instead of spinning the loop.
    int due = cbc_due_ms(&h->c, now);
    if (due > 0 || (due == 0 && !h->closing)) h->tick_timer = h->io.timer(h->io.ud, (uint32_t)due, on_tick, h);
}

int cbc_posix_start(cbc_posix *h, const cbc_posix_io *io, const cbc_cfg *cfg, cbc_posix_event_fn fn, void *ctx) {
    memset(h, 0, sizeof *h);
    h->io = *io;
    h->fn = fn;
    h->ctx = ctx;
    cbc_cfg k;
    if (cfg) k = *cfg;
    else cbc_cfg_default(&k);
    int got = io->entropy ? io->entropy(io->ud, &k.seed, sizeof k.seed) : getentropy(&k.seed, sizeof k.seed);
    if (got != 0) return -1;
    cbc_init(&h->c, &k);
    session_load(h);
    cbc_posix_pump(h);
    return 0;
}
