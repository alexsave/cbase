// host_suite.c - the host suite (host_suite.h): the world a host runs on, the scripted server
// behind it, and the cases.
#include "host_suite.h"

#include "t.h"

#include "stream_frame.h"   // lib/: the server's frames are written and read with the wire's own code
#include "le_bytes.h"
#include "rng.h"            // lib/: the large body's bytes, from one named seed

#include <stdio.h>
#include <string.h>

#define W_SOCKS   8
#define W_FRAMES  32
#define W_FRAME   4096
#define W_REQS    32
#define W_BODY    (64 * 1024 + 1024)   // a borrowed 64 KB body, whole
#define W_TIMERS  16
#define W_EVENTS  64
#define W_DATA    256
#define W_STORE   4096
#define W_TIMERS_ARMED_MAX 2   // the tick's and the reopen's (cbase/client.h: one timer on cbc_due_ms)

// The suite's clock starts at a real epoch ms (2023-11-14), so expiries read as they do in use.
#define T0 INT64_C(1700000000000)

enum { S_FREE, S_OPENING, S_OPEN, S_CLOSING };   // CLOSING: the host closed it; the loop has not said so

typedef struct {
    int      kind;
    uint32_t ticket;
    int      status, why, push;
    char     room[CBC_ROOM_MAX + 1];
    size_t   len;               // the span's length (DONE, PUSH)
    uint8_t  data[W_DATA];      // the span's bytes, as the host handed them
} w_event;

typedef struct {
    char     method[8];
    char     path[CBC_PATH_MAX + 1];
    char     auth[2200];        // "" when none
    size_t   body_len;
    uint8_t  body[W_BODY];
    cbhs_done_fn done;
    void    *ctx;
    int      answered;
} w_req;

struct cbhs_world {
    const cbhs_host *h;
    void   *host;
    int64_t now;
    // sockets, by id - 1
    struct { uint8_t state; char path[32]; } sock[W_SOCKS];
    uint32_t n_socks;
    // the frames the server received, in order, each with its socket
    struct { uint32_t sock; size_t len; uint8_t b[W_FRAME]; } frame[W_FRAMES];
    int n_frames, frames_read;
    w_req req[W_REQS];
    int n_reqs, reqs_read;
    struct { uint8_t armed; uint32_t id; int64_t at; cbhs_fire_fn fire; void *ctx; } timer[W_TIMERS];
    uint32_t next_timer;        // ids are never reused, so a stale stop stops nothing
    uint64_t entropy;
    size_t   store_len;
    uint8_t  store[W_STORE];
    w_event  ev[W_EVENTS];
    int      n_ev, ev_read;
    int      broken;            // the host broke a rule of the world (named on stderr)
    // The script's app, when a case sets one: called with every event the host hands over, after it
    // is recorded, from inside the host's own callback (so it may call into the client and pump,
    // as an app is allowed to).
    void   (*app)(cbhs_world *w, cbc *c, const w_event *e);
    uint32_t app_ticket;        // what the app's last call answered
};

static void broke(cbhs_world *w, const char *why) {
    w->broken++;
    fprintf(stderr, "host suite: %s: %s\n", w->h->name, why);
}

// ---- the world's calls -----------------------------------------------------------------------------
int64_t cbhs_now(cbhs_world *w) { return w->now; }

uint32_t cbhs_ws_open(cbhs_world *w, const char *path) {
    if (w->n_socks == W_SOCKS) { broke(w, "more sockets opened than the suite has room for"); return 0; }
    for (uint32_t i = 0; i < w->n_socks; i++)
        if (w->sock[i].state == S_OPENING || w->sock[i].state == S_OPEN) broke(w, "a second socket opened while one is up");
    uint32_t i = w->n_socks++;
    w->sock[i].state = S_OPENING;
    snprintf(w->sock[i].path, sizeof w->sock[i].path, "%s", path);
    return i + 1;
}

void cbhs_ws_send(cbhs_world *w, uint32_t id, const uint8_t *frame, size_t len) {
    if (id == 0 || id > w->n_socks || w->sock[id - 1].state != S_OPEN) { broke(w, "a frame sent on a socket that is not open"); return; }
    if (w->n_frames == W_FRAMES || len > W_FRAME) { broke(w, "more frames than the suite has room for"); return; }
    w->frame[w->n_frames].sock = id;
    w->frame[w->n_frames].len = len;
    memcpy(w->frame[w->n_frames].b, frame, len);
    w->n_frames++;
}

void cbhs_ws_close(cbhs_world *w, uint32_t id) {
    if (id == 0 || id > w->n_socks || (w->sock[id - 1].state != S_OPEN && w->sock[id - 1].state != S_OPENING)) {
        broke(w, "a close of a socket that is not up");
        return;
    }
    w->sock[id - 1].state = S_CLOSING;
}

void cbhs_http(cbhs_world *w, const char *method, const char *path, size_t path_len, const char *auth, size_t auth_len,
               const uint8_t *body, size_t body_len, cbhs_done_fn done, void *ctx) {
    if (w->n_reqs == W_REQS || path_len > CBC_PATH_MAX || auth_len >= sizeof w->req[0].auth || body_len > W_BODY) {
        broke(w, "a request past the suite's room");
        return;
    }
    w_req *r = &w->req[w->n_reqs++];
    memset(r, 0, sizeof *r);
    snprintf(r->method, sizeof r->method, "%s", method);
    memcpy(r->path, path, path_len);
    if (auth) memcpy(r->auth, auth, auth_len);
    memcpy(r->body, body, body_len);
    r->body_len = body_len;
    r->done = done;
    r->ctx = ctx;
}

uint32_t cbhs_timer(cbhs_world *w, uint32_t ms, cbhs_fire_fn fire, void *ctx) {
    int armed = 0, slot = -1;
    for (int i = 0; i < W_TIMERS; i++) {
        if (w->timer[i].armed) armed++;
        else if (slot < 0) slot = i;
    }
    if (armed >= W_TIMERS_ARMED_MAX) broke(w, "a third timer armed (one for the ticks, one for the reopen)");
    if (slot < 0) return 0;
    w->timer[slot].armed = 1;
    w->timer[slot].id = ++w->next_timer;
    w->timer[slot].at = w->now + ms;
    w->timer[slot].fire = fire;
    w->timer[slot].ctx = ctx;
    return w->timer[slot].id;
}

// A stop of one that fired already does nothing, as clearTimeout does.
void cbhs_timer_stop(cbhs_world *w, uint32_t id) {
    if (id == 0 || id > w->next_timer) { broke(w, "a stop of a timer that never was"); return; }
    for (int i = 0; i < W_TIMERS; i++)
        if (w->timer[i].armed && w->timer[i].id == id) w->timer[i].armed = 0;
}

size_t cbhs_load(cbhs_world *w, uint8_t *out, size_t cap) {
    if (w->store_len > cap) return 0;
    memcpy(out, w->store, w->store_len);
    return w->store_len;
}

void cbhs_save(cbhs_world *w, const uint8_t *p, size_t len) {
    if (len > W_STORE) { broke(w, "a session past the suite's room"); return; }
    memcpy(w->store, p, len);
    w->store_len = len;
}

int cbhs_entropy(cbhs_world *w, void *out, size_t len) {
    uint8_t *o = out;
    for (size_t i = 0; i < len; i++) { w->entropy = w->entropy * 6364136223846793005u + 1442695040888963407u; o[i] = (uint8_t)(w->entropy >> 56); }
    return 0;
}

void cbhs_event(cbhs_world *w, cbc *c, const cbc_event *e, const uint8_t *bytes) {
    (void)c;
    if (w->n_ev == W_EVENTS) { broke(w, "more events than the suite has room for"); return; }
    w_event *v = &w->ev[w->n_ev++];
    memset(v, 0, sizeof *v);
    v->kind = e->kind;
    v->ticket = e->ticket;
    v->status = e->status;
    v->why = e->why;
    v->push = e->push;
    memcpy(v->room, e->room, e->room_len < CBC_ROOM_MAX ? e->room_len : CBC_ROOM_MAX);
    if (e->kind == CBC_EV_DONE || e->kind == CBC_EV_PUSH) {
        v->len = e->len;
        if (e->len && !bytes) broke(w, "an event with a span and no bytes to read it from");
        else if (e->len <= W_DATA && bytes) memcpy(v->data, bytes + e->off, e->len);
    }
    if (w->app) w->app(w, c, v);
}

// ---- the script's side: the server, the clock, the app ---------------------------------------------
static cbhs_world g_w;
static cbc_cfg g_cfg;

static cbhs_world *world(void) {
    memset(&g_w, 0, sizeof g_w);
    g_w.now = T0;
    g_w.entropy = 42;
    cbc_cfg_default(&g_cfg);
    return &g_w;
}

static cbc *start(cbhs_world *w, const cbhs_host *h) {
    w->h = h;
    w->host = h->start(w, &g_cfg);
    return h->client(w->host);
}

static void pump(cbhs_world *w) { w->h->pump(w->host); }

// Moves the clock ms ahead, firing every timer that falls due on the way, each at its own time. A
// host that keeps re-arming a timer at 0 ms spins its loop (a busy wait, burning a core until
// something else happens): more than W_SPIN fires at one instant is a broken rule, and the clock
// moves on.
#define W_SPIN 64
static void advance(cbhs_world *w, int64_t ms) {
    int64_t end = w->now + ms, at = w->now;
    int same = 0;
    for (;;) {
        int k = -1;
        for (int i = 0; i < W_TIMERS; i++)
            if (w->timer[i].armed && w->timer[i].at <= end && (k < 0 || w->timer[i].at < w->timer[k].at)) k = i;
        if (k < 0) break;
        if (w->timer[k].at > w->now) w->now = w->timer[k].at;
        if (w->now == at && ++same > W_SPIN) { broke(w, "a timer re-armed at 0 ms over and over: the loop spins"); break; }
        if (w->now != at) { at = w->now; same = 0; }
        w->timer[k].armed = 0;
        w->timer[k].fire(w->timer[k].ctx);
    }
    w->now = end;
}

// The socket the host is opening (0: none), accepted.
static uint32_t opening(cbhs_world *w) {
    for (uint32_t i = 0; i < w->n_socks; i++)
        if (w->sock[i].state == S_OPENING) return i + 1;
    return 0;
}
static uint32_t up_sock(cbhs_world *w) {
    for (uint32_t i = 0; i < w->n_socks; i++)
        if (w->sock[i].state == S_OPEN) return i + 1;
    return 0;
}
static void accept_sock(cbhs_world *w, uint32_t id) {
    w->sock[id - 1].state = S_OPEN;
    w->h->ws_opened(w->host, id);
}
// The loop reports every close the host asked for.
static void settle(cbhs_world *w) {
    for (uint32_t i = 0; i < w->n_socks; i++)
        if (w->sock[i].state == S_CLOSING) { w->sock[i].state = S_FREE; w->h->ws_closed(w->host, i + 1); }
}
// The server drops the socket.
static void drop(cbhs_world *w) {
    uint32_t id = up_sock(w);
    T_CHECK(id != 0);
    if (!id) return;
    w->sock[id - 1].state = S_FREE;
    w->h->ws_closed(w->host, id);
}
static int closing(cbhs_world *w) {
    for (uint32_t i = 0; i < w->n_socks; i++)
        if (w->sock[i].state == S_CLOSING) return 1;
    return 0;
}

// The next frame the server received, parsed (0: none).
static sf_frame g_f;
static int frame(cbhs_world *w) {
    if (w->frames_read == w->n_frames) return 0;
    int i = w->frames_read++;
    T_EQ_INT(sf_parse(w->frame[i].b, w->frame[i].len, &g_f), 0);
    return g_f.kind;
}
static void to_host(cbhs_world *w, const uint8_t *p, size_t n) {
    uint32_t id = up_sock(w);
    T_CHECK(id != 0);
    static uint8_t b[W_FRAME];   // the loop's buffer: the host reads its spans out of it
    memcpy(b, p, n);
    if (id) w->h->ws_message(w->host, id, b, n);
}
static void bare(cbhs_world *w, uint8_t kind) {
    uint8_t b[2];
    sf_put_bare(b, sizeof b, kind);
    to_host(w, b, 2);
}
static void reply(cbhs_world *w, uint32_t id, int status, const void *body, size_t n) {
    uint8_t b[512];
    sf_put_reply_head(b, sizeof b, id, (uint16_t)status);
    if (n) memcpy(b + 8, body, n);
    to_host(w, b, 8 + n);
}

// The next request the server received (NULL: none).
static w_req *request(cbhs_world *w) { return w->reqs_read == w->n_reqs ? NULL : &w->req[w->reqs_read++]; }
static void answer(w_req *r, int status, const void *body, size_t n) {
    T_CHECK(r && !r->answered);
    if (!r || r->answered) return;
    r->answered = 1;
    static uint8_t b[W_BODY];   // the HTTP client's buffer
    if (n) memcpy(b, body, n);
    r->done(r->ctx, status, n ? b : NULL, n);
}

// The next event the app was told (NULL: none).
static const w_event *event(cbhs_world *w) { return w->ev_read == w->n_ev ? NULL : &w->ev[w->ev_read++]; }

static size_t session_body(uint8_t *o, const char *access, const char *refresh, int64_t exp) {
    size_t n = 0, a = strlen(access), r = strlen(refresh);
    o[n++] = CBW_V; o[n++] = CBW_SESSION;
    le_put_u16(o + n, (uint16_t)a); n += 2; memcpy(o + n, access, a); n += a;
    o[n++] = (uint8_t)r; memcpy(o + n, refresh, r); n += r;
    le_put_u48(o + n, (uint64_t)exp); n += 6;
    o[n++] = 3; memcpy(o + n, "u-1", 3); n += 3;
    o[n++] = 3; memcpy(o + n, "ANN", 3); n += 3;
    return n;
}

// Every case ends with the world's rules kept.
static void kept(cbhs_world *w) { T_EQ_INT(w->broken, 0); }

// Signed up over HTTP, then the socket to READY: the steps every signed-in case starts from.
static cbc *signed_up(cbhs_world *w, const cbhs_host *h, int64_t exp) {
    cbc *c = start(w, h);
    T_EQ_INT(w->n_socks, 0);                       // nobody signed in: no socket
    T_CHECK(request(w) == NULL);
    uint32_t t = cbc_sign_in(c, CBW_SIGNUP, "ANN", 3, "secret!!", 8, cbhs_now(w));
    T_CHECK(t != 0);
    pump(w);
    w_req *r = request(w);
    T_CHECK(r != NULL);
    if (!r) return c;
    T_EQ_STR(r->method, "POST");
    T_EQ_STR(r->path, "/v1/auth/signup");
    T_EQ_STR(r->auth, "");
    T_EQ_MEM(r->body, r->body_len, "\x01\x01\x03" "ANN" "\x08" "secret!!", 15);   // [CBW_V][CBW_SIGNUP]
    uint8_t b[128];
    size_t n = session_body(b, "TOK", "R1", exp);
    answer(r, 200, b, n);
    const w_event *e = event(w);
    T_CHECK(e && e->kind == CBC_EV_SESSION);
    e = event(w);
    T_CHECK(e && e->kind == CBC_EV_DONE && e->ticket == t && e->status == 200 && e->why == 0);
    uint8_t s[256];
    int sn = cbc_session_save(c, s, sizeof s);
    T_EQ_MEM(w->store, w->store_len, s, (size_t)sn);   // kept on the SESSION event
    uint32_t id = opening(w);
    T_CHECK(id != 0);
    if (!id) return c;
    T_EQ_STR(w->sock[id - 1].path, "/ws");
    accept_sock(w, id);
    T_EQ_INT(frame(w), SF_HELLO);
    T_EQ_MEM(g_f.tok, g_f.tok_len, "TOK", 3);
    bare(w, SF_READY);
    e = event(w);
    T_CHECK(e && e->kind == CBC_EV_READY);
    return c;
}

#define FAR (T0 + INT64_C(86400000))   // an expiry a day off

// ---- the cases -------------------------------------------------------------------------------------
static const cbhs_host *g_h;

static void signs_up_then_says_hello(void) {
    cbhs_world *w = world();
    signed_up(w, g_h, FAR);
    T_EQ_INT(frame(w), 0);
    kept(w);
}

static void a_call_rides_the_socket(void) {
    cbhs_world *w = world();
    cbc *c = signed_up(w, g_h, FAR);
    uint32_t t = cbc_call(c, CBC_POST, 0, "/v1/echo", 8, (const uint8_t *)"hi", 2, cbhs_now(w));
    pump(w);
    T_CHECK(request(w) == NULL);                   // not over HTTP
    T_EQ_INT(frame(w), SF_REQ);
    T_EQ_INT(g_f.meth, SF_METH_POST);
    T_EQ_MEM(g_f.path, g_f.path_len, "/v1/echo", 8);
    T_EQ_MEM(g_f.body, g_f.body_len, "hi", 2);
    reply(w, g_f.id, 200, "okay", 4);
    const w_event *e = event(w);
    T_CHECK(e && e->kind == CBC_EV_DONE && e->ticket == t && e->status == 200);
    if (e) T_EQ_MEM(e->data, e->len, "okay", 4);   // the span, read out of the bytes the host passed
    kept(w);
}

static void an_http_call_carries_the_bearer_and_is_retried_by_the_timer(void) {
    cbhs_world *w = world();
    cbc *c = signed_up(w, g_h, FAR);
    uint32_t t = cbc_call(c, CBC_GET, CBC_C_HTTP | CBC_C_READ, "/v1/list", 8, NULL, 0, cbhs_now(w));
    pump(w);
    w_req *r = request(w);
    T_CHECK(r != NULL);
    if (!r) return;
    T_EQ_STR(r->method, "GET");
    T_EQ_STR(r->path, "/v1/list");
    T_EQ_STR(r->auth, "Bearer TOK");
    answer(r, 0, NULL, 0);                      // it never arrived: a read goes again, later
    T_CHECK(event(w) == NULL);
    T_CHECK(request(w) == NULL);
    advance(w, g_cfg.retry_first_ms - 1);
    T_CHECK(request(w) == NULL);
    advance(w, 1);                                 // the host's timer sends it again
    r = request(w);
    T_CHECK(r != NULL);
    if (!r) return;
    T_EQ_STR(r->path, "/v1/list");
    answer(r, 200, "[1,2]", 5);
    const w_event *e = event(w);
    T_CHECK(e && e->kind == CBC_EV_DONE && e->ticket == t && e->status == 200);
    if (e) T_EQ_MEM(e->data, e->len, "[1,2]", 5);
    kept(w);
}

static void the_timer_pings_and_a_silent_socket_is_closed(void) {
    cbhs_world *w = world();
    signed_up(w, g_h, FAR);
    advance(w, g_cfg.ping_ms - 1);
    T_EQ_INT(frame(w), 0);
    advance(w, 1);
    T_EQ_INT(frame(w), SF_PING);
    bare(w, SF_PONG);
    advance(w, g_cfg.pong_ms);
    T_EQ_INT(closing(w), 0);                       // answered
    advance(w, g_cfg.ping_ms - g_cfg.pong_ms);     // ping_ms after the last frame out
    T_EQ_INT(frame(w), SF_PING);
    advance(w, g_cfg.pong_ms - 1);
    T_EQ_INT(closing(w), 0);
    advance(w, 1);                                 // no frame of any kind for pong_ms: half-open
    T_EQ_INT(closing(w), 1);
    kept(w);
}

// The host closed a half-open socket and the platform has not reported the close (a browser can
// take a minute on a dead link): the client's other timers run on meanwhile. A lost HTTP read is
// sent again on time, retry_first_ms after the loss, with no spin, and the socket reopens after
// the close is heard.
static void an_http_retry_goes_on_time_while_a_close_is_pending(void) {
    cbhs_world *w = world();
    cbc *c = signed_up(w, g_h, FAR);
    advance(w, g_cfg.ping_ms);
    T_EQ_INT(frame(w), SF_PING);
    advance(w, g_cfg.pong_ms);                     // half-open: the host closes it
    T_EQ_INT(closing(w), 1);
    uint32_t t = cbc_call(c, CBC_GET, CBC_C_HTTP | CBC_C_READ, "/v1/list", 8, NULL, 0, cbhs_now(w));
    pump(w);
    w_req *r = request(w);
    T_CHECK(r != NULL);
    if (!r) return;
    answer(r, 0, NULL, 0);                         // lost: a read goes again after retry_first_ms
    advance(w, g_cfg.retry_first_ms - 1);
    T_CHECK(request(w) == NULL);
    advance(w, 1);
    r = request(w);
    T_CHECK(r != NULL);                            // on time, the close still out
    if (!r) return;
    T_EQ_STR(r->path, "/v1/list");
    answer(r, 200, "[]", 2);
    const w_event *e = event(w);
    T_CHECK(e && e->kind == CBC_EV_DONE && e->ticket == t && e->status == 200);
    T_EQ_INT(closing(w), 1);
    advance(w, 30000);                             // nothing else is due: no spin, no frame
    T_EQ_INT(frame(w), 0);
    T_EQ_INT(opening(w), 0);
    settle(w);                                     // the close is heard: the next socket after the wait
    advance(w, g_cfg.retry_max_ms);
    uint32_t id = opening(w);
    T_CHECK(id != 0);
    if (!id) return;
    accept_sock(w, id);
    T_EQ_INT(frame(w), SF_HELLO);
    kept(w);
}

static void a_dropped_socket_is_reopened_after_the_wait(void) {
    cbhs_world *w = world();
    signed_up(w, g_h, FAR);
    drop(w);
    T_EQ_INT(opening(w), 0);                       // not at once: the reconnect floor
    advance(w, g_cfg.retry_first_ms - 1);
    T_EQ_INT(opening(w), 0);
    advance(w, g_cfg.retry_max_ms);
    uint32_t id = opening(w);
    T_CHECK(id != 0);
    if (!id) return;
    accept_sock(w, id);
    T_EQ_INT(frame(w), SF_HELLO);
    T_EQ_MEM(g_f.tok, g_f.tok_len, "TOK", 3);
    bare(w, SF_READY);
    const w_event *e = event(w);
    T_CHECK(e && e->kind == CBC_EV_READY);
    kept(w);
}

// The app re-issues a lost write and pumps from inside its own DONE callback: the reopen's wait
// is already armed when the host hands the close's events over, so the re-entrant pump opens no
// socket at once, and the call goes on the next one, after the backoff.
static void retry_lost(cbhs_world *w, cbc *c, const w_event *e) {
    if (e->kind != CBC_EV_DONE || e->why != CBC_WHY_LOST) return;
    w->app_ticket = cbc_call(c, CBC_POST, 0, "/v1/echo", 8, (const uint8_t *)"hi", 2, cbhs_now(w));
    w->h->pump(w->host);
}
static void a_call_retried_from_inside_done_waits_for_the_reopen(void) {
    cbhs_world *w = world();
    cbc *c = signed_up(w, g_h, FAR);
    uint32_t t = cbc_call(c, CBC_POST, 0, "/v1/echo", 8, (const uint8_t *)"hi", 2, cbhs_now(w));
    pump(w);
    T_EQ_INT(frame(w), SF_REQ);
    w->app = retry_lost;
    drop(w);                                       // the write is lost: DONE LOST, and the app retries
    w->app = NULL;
    const w_event *e = event(w);
    T_CHECK(e && e->kind == CBC_EV_DONE && e->ticket == t && e->why == CBC_WHY_LOST);
    T_CHECK(w->app_ticket != 0);
    T_EQ_INT(opening(w), 0);                       // not at once: the reconnect floor
    advance(w, g_cfg.retry_first_ms - 1);
    T_EQ_INT(opening(w), 0);
    advance(w, g_cfg.retry_max_ms);
    uint32_t id = opening(w);
    T_CHECK(id != 0);
    if (!id) return;
    accept_sock(w, id);
    T_EQ_INT(frame(w), SF_HELLO);
    bare(w, SF_READY);
    T_EQ_INT(frame(w), SF_REQ);                    // the retried call, on the new socket
    T_EQ_MEM(g_f.path, g_f.path_len, "/v1/echo", 8);
    reply(w, g_f.id, 200, NULL, 0);
    e = event(w);
    T_CHECK(e && e->kind == CBC_EV_READY);
    e = event(w);
    T_CHECK(e && e->kind == CBC_EV_DONE && e->ticket == w->app_ticket && e->status == 200);
    kept(w);
}

static void the_kept_session_is_used_at_start(void) {
    cbhs_world *w = world();
    uint8_t b[128];
    cbc probe;
    cbc_init(&probe, &g_cfg);
    size_t n = session_body(b, "KEPT", "R1", FAR);
    T_EQ_INT(cbc_session_load(&probe, b, n), 0);
    w->store_len = (size_t)cbc_session_save(&probe, w->store, sizeof w->store);
    cbc *c = start(w, g_h);
    T_EQ_INT(cbc_user_of(c)->signed_in, 1);
    T_CHECK(request(w) == NULL);                   // no sign-in
    uint32_t id = opening(w);
    T_CHECK(id != 0);
    if (!id) return;
    accept_sock(w, id);
    T_EQ_INT(frame(w), SF_HELLO);
    T_EQ_MEM(g_f.tok, g_f.tok_len, "KEPT", 4);
    kept(w);
}

static void a_room_goes_live_and_pushes_reach_the_app(void) {
    cbhs_world *w = world();
    cbc *c = signed_up(w, g_h, FAR);
    T_EQ_INT(cbc_sub(c, CBC_ROOM, "s/doc", 5), 0);
    pump(w);
    T_EQ_INT(frame(w), SF_SUB);
    T_EQ_MEM(g_f.gid, g_f.gid_len, "s/doc", 5);
    reply(w, g_f.id, 200, NULL, 0);
    const w_event *e = event(w);
    T_CHECK(e && e->kind == CBC_EV_SUB_LIVE);
    if (e) T_EQ_STR(e->room, "s/doc");
    uint8_t p[5] = { SF_V, 5, 1, 2, 3 };
    to_host(w, p, sizeof p);
    e = event(w);
    T_CHECK(e && e->kind == CBC_EV_PUSH && e->push == 5);
    if (e) T_EQ_MEM(e->data, e->len, p, sizeof p);
    kept(w);
}

static void a_refresh_rolls_the_socket_onto_the_new_token(void) {
    cbhs_world *w = world();
    int64_t exp = T0 + g_cfg.refresh_early_ms + 5000;
    signed_up(w, g_h, exp);
    advance(w, 4999);
    T_CHECK(request(w) == NULL);
    advance(w, 1);                                 // refresh_early_ms before the expiry
    w_req *r = request(w);
    T_CHECK(r != NULL);
    if (!r) return;
    T_EQ_STR(r->path, "/v1/auth/refresh");
    uint8_t b[128];
    size_t n = session_body(b, "TOK2", "R2", FAR);
    answer(r, 200, b, n);
    const w_event *e = event(w);
    T_CHECK(e && e->kind == CBC_EV_SESSION);
    uint8_t s[256];
    int sn = cbc_session_save(w->h->client(w->host), s, sizeof s);
    T_EQ_MEM(w->store, w->store_len, s, (size_t)sn);
    T_EQ_INT(closing(w), 1);                       // the socket rolls onto the new token
    settle(w);
    advance(w, 0);                                 // the roll's wait is 0
    uint32_t id = opening(w);
    T_CHECK(id != 0);
    if (!id) return;
    accept_sock(w, id);
    T_EQ_INT(frame(w), SF_HELLO);
    T_EQ_MEM(g_f.tok, g_f.tok_len, "TOK2", 4);
    kept(w);
}

static void a_sign_out_closes_and_forgets(void) {
    cbhs_world *w = world();
    cbc *c = signed_up(w, g_h, FAR);
    uint32_t t = cbc_sign_out(c, 0, cbhs_now(w));
    pump(w);
    T_EQ_INT(closing(w), 1);
    w_req *r = request(w);
    T_CHECK(r != NULL);
    if (!r) return;
    T_EQ_STR(r->path, "/v1/auth/signout");
    T_EQ_STR(r->auth, "Bearer TOK");
    answer(r, 200, NULL, 0);
    int done = 0, session = 0;
    for (const w_event *e; (e = event(w));) {
        if (e->kind == CBC_EV_DONE && e->ticket == t) done = 1;
        if (e->kind == CBC_EV_SESSION) session = 1;
    }
    T_CHECK(done && session);
    T_EQ_INT(w->store_len, 0);                     // forgotten
    settle(w);
    advance(w, 60000);
    T_EQ_INT(opening(w), 0);                       // nobody to open a socket for
    T_CHECK(request(w) == NULL);
    kept(w);
}

// ---- a large body, borrowed (cbc_call_ref) ------------------------------------------------------
#define BIG_N    (64 * 1024)
#define BIG_SEED UINT64_C(0x5eed0b0d1e5)   // the body's bytes: change this one line to draw others
static uint8_t g_big[BIG_N];
static void big_fill(void) {
    uint64_t s = BIG_SEED;
    for (size_t i = 0; i < BIG_N; i += 8) {
        uint64_t v = rng_splitmix64(&s);
        for (int j = 0; j < 8; j++) g_big[i + (size_t)j] = (uint8_t)(v >> (8 * j));
    }
}
// What the server received is the caller's 64 KB, byte for byte, with the head the core wrote.
static void big_arrived(const w_req *r, const char *path) {
    T_CHECK(r != NULL);
    if (!r) return;
    T_EQ_STR(r->method, "POST");
    T_EQ_STR(r->path, path);
    T_EQ_STR(r->auth, "Bearer TOK");
    T_EQ_INT(r->body_len, BIG_N);
    T_CHECK(r->body_len == BIG_N && memcmp(r->body, g_big, BIG_N) == 0);
}

static void a_64k_body_arrives_byte_for_byte_and_done_comes_once_after(void) {
    cbhs_world *w = world();
    big_fill();
    cbc *c = signed_up(w, g_h, FAR);
    uint32_t t = cbc_call_ref(c, CBC_POST, CBC_C_AUTH, "/notes/bulk", 11, g_big, BIG_N, cbhs_now(w));
    T_CHECK(t != 0);
    T_CHECK(event(w) == NULL);
    pump(w);
    T_EQ_INT(frame(w), 0);                         // over HTTP, never the socket
    w_req *r = request(w);
    big_arrived(r, "/notes/bulk");
    T_CHECK(event(w) == NULL);                     // the bytes went out; no DONE before the answer
    advance(w, g_cfg.call_wait_ms * 2);
    T_CHECK(event(w) == NULL);
    T_CHECK(request(w) == NULL);
    if (r) answer(r, 200, "ok", 2);
    const w_event *e = event(w);
    T_CHECK(e && e->kind == CBC_EV_DONE && e->ticket == t && e->status == 200 && e->why == 0);
    if (e) T_EQ_MEM(e->data, e->len, "ok", 2);
    T_CHECK(event(w) == NULL);                     // exactly once
    advance(w, 60000);
    T_CHECK(event(w) == NULL);
    T_CHECK(request(w) == NULL);
    kept(w);
}

static void a_retried_borrowed_body_resends_the_same_bytes(void) {
    cbhs_world *w = world();
    big_fill();
    cbc *c = signed_up(w, g_h, FAR);
    uint32_t t = cbc_call_ref(c, CBC_POST, CBC_C_AUTH | CBC_C_READ, "/notes/bulk", 11, g_big, BIG_N, cbhs_now(w));
    pump(w);
    w_req *r = request(w);
    big_arrived(r, "/notes/bulk");
    if (r) answer(r, 0, NULL, 0);                  // lost: a read goes again after the wait
    T_CHECK(event(w) == NULL);
    T_CHECK(request(w) == NULL);
    advance(w, g_cfg.retry_first_ms);
    r = request(w);
    big_arrived(r, "/notes/bulk");
    if (r) answer(r, 503, NULL, 0);                // busy: once more after 250 ms
    T_CHECK(event(w) == NULL);
    advance(w, 250);
    r = request(w);
    big_arrived(r, "/notes/bulk");
    T_CHECK(event(w) == NULL);
    if (r) answer(r, 200, NULL, 0);
    const w_event *e = event(w);
    T_CHECK(e && e->kind == CBC_EV_DONE && e->ticket == t && e->status == 200);
    T_CHECK(event(w) == NULL);
    T_CHECK(request(w) == NULL);
    kept(w);
}

// An HTTP call that needs the user, made while the refresh is out, waits for it without spinning
// the loop (cbc_due_ms and cbc_next_request read one rule, call_sendable): while the refresh is
// out, while it backs off after a loss, and it goes out on the new token once the refresh lands.
static void an_auth_http_call_waits_for_the_refresh_without_spinning(void) {
    cbhs_world *w = world();
    int64_t exp = T0 + g_cfg.refresh_early_ms + 5000;
    cbc *c = signed_up(w, g_h, exp);
    advance(w, 5000);
    w_req *r = request(w);
    T_CHECK(r != NULL);
    if (!r) return;
    T_EQ_STR(r->path, "/v1/auth/refresh");         // out, not answered yet
    uint32_t t = cbc_call(c, CBC_GET, CBC_C_HTTP | CBC_C_AUTH, "/v1/list", 8, NULL, 0, cbhs_now(w));
    T_CHECK(t != 0);
    pump(w);
    T_CHECK(request(w) == NULL);                   // it waits for the refresh
    advance(w, 1000);                              // the refresh still out: no spin
    T_CHECK(request(w) == NULL);
    answer(r, 0, NULL, 0);                         // lost: the refresh backs off retry_first_ms
    T_CHECK(request(w) == NULL);
    advance(w, g_cfg.retry_first_ms - 1);          // backing off: no spin, nothing sent
    T_CHECK(request(w) == NULL);
    advance(w, 1);
    r = request(w);
    T_CHECK(r != NULL);
    if (!r) return;
    T_EQ_STR(r->path, "/v1/auth/refresh");
    T_CHECK(request(w) == NULL);
    uint8_t b[128];
    size_t n = session_body(b, "TOK2", "R2", FAR);
    answer(r, 200, b, n);                          // the refresh lands: the call goes at once
    const w_event *e = event(w);
    T_CHECK(e && e->kind == CBC_EV_SESSION);
    w_req *q = request(w);
    T_CHECK(q != NULL);
    if (!q) return;
    T_EQ_STR(q->path, "/v1/list");
    T_EQ_STR(q->auth, "Bearer TOK2");
    answer(q, 200, "[]", 2);
    e = event(w);
    T_CHECK(e && e->kind == CBC_EV_DONE && e->ticket == t && e->status == 200);
    kept(w);
}

static void an_anonymous_listener_opens_the_public_socket(void) {
    cbhs_world *w = world();
    g_cfg.anonymous = 1;
    cbc *c = start(w, g_h);
    T_EQ_INT(w->n_socks, 0);
    T_EQ_INT(cbc_sub(c, CBC_ROOM, "feed", 4), 0);
    pump(w);
    uint32_t id = opening(w);
    T_CHECK(id != 0);
    if (!id) return;
    T_EQ_STR(w->sock[id - 1].path, "/ws/public");
    accept_sock(w, id);
    T_EQ_INT(frame(w), SF_HELLO);
    T_EQ_INT(g_f.tok_len, 0);
    bare(w, SF_READY);
    T_EQ_INT(frame(w), SF_SUB);
    reply(w, g_f.id, 200, NULL, 0);
    T_EQ_INT(cbc_unsub(c, CBC_ROOM, "feed", 4), 0);   // the last hold: the socket closes
    pump(w);
    T_EQ_INT(closing(w), 1);
    kept(w);
}

// The last room goes while the socket is still opening (the client has not heard of it): the host
// closes it then and there, and the next hold opens a new one.
static void a_socket_nobody_wants_is_closed_while_it_opens(void) {
    cbhs_world *w = world();
    g_cfg.anonymous = 1;
    cbc *c = start(w, g_h);
    T_EQ_INT(cbc_sub(c, CBC_ROOM, "feed", 4), 0);
    pump(w);
    uint32_t id = opening(w);
    T_CHECK(id != 0);
    T_EQ_INT(cbc_unsub(c, CBC_ROOM, "feed", 4), 0);
    pump(w);
    T_EQ_INT(opening(w), 0);
    T_EQ_INT(closing(w), 1);                       // closed before it opened
    settle(w);
    advance(w, 60000);
    T_EQ_INT(w->n_socks, 1);                       // nobody wants one: none opened
    T_EQ_INT(cbc_sub(c, CBC_ROOM, "feed", 4), 0);
    pump(w);
    uint32_t id2 = opening(w);
    T_CHECK(id2 != 0 && id2 != id);
    if (!id2) return;
    accept_sock(w, id2);
    T_EQ_INT(frame(w), SF_HELLO);
    kept(w);
}

int cbhs_run(const cbhs_host *h) {
    g_h = h;
    T_RUN(signs_up_then_says_hello);
    T_RUN(a_call_rides_the_socket);
    T_RUN(an_http_call_carries_the_bearer_and_is_retried_by_the_timer);
    T_RUN(the_timer_pings_and_a_silent_socket_is_closed);
    T_RUN(a_dropped_socket_is_reopened_after_the_wait);
    T_RUN(an_http_retry_goes_on_time_while_a_close_is_pending);
    T_RUN(a_call_retried_from_inside_done_waits_for_the_reopen);
    T_RUN(the_kept_session_is_used_at_start);
    T_RUN(a_room_goes_live_and_pushes_reach_the_app);
    T_RUN(a_refresh_rolls_the_socket_onto_the_new_token);
    T_RUN(a_sign_out_closes_and_forgets);
    T_RUN(an_anonymous_listener_opens_the_public_socket);
    T_RUN(a_socket_nobody_wants_is_closed_while_it_opens);
    T_RUN(a_64k_body_arrives_byte_for_byte_and_done_comes_once_after);
    T_RUN(a_retried_borrowed_body_resends_the_same_bytes);
    T_RUN(an_auth_http_call_waits_for_the_refresh_without_spinning);
    printf("host suite: %s\n", h->name);
    return T_DONE();
}
