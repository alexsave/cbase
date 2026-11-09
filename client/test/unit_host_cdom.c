// unit_host_cdom.c - the host suite (test/host/host_suite.h) on the cdom adapter
// (hosts/cdom/cbc_cdom.c), built natively: this file is cdom, as far as the host reaches it. It
// defines the cdom calls the host makes, each mapped onto the suite's world, and hands the world's
// deliveries to the callbacks the host gave cdom. Then the adapter's own cases, which the shared suite
// cannot make, on the same fake cdom with no world behind it (solo).
//
// Where cdom differs from the world, the mapping is cdom's:
//   - the page: cd_page_origin is http://app.test (the suite runs again on https:); the socket's URL must be ws:// or wss:// as the page is, on its host, with the path
//     the world gets
//   - socket ids are cdom's, as its shim hands them out: from 0, the last freed id first, an id
//     freed when its close is delivered (before the callback hears it); the world's ids are mapped
//     onto them. cdom has no error event (a browser's says nothing), so the world's failure before
//     the open reaches the host as its close alone (cbhs_host no_sock_why). A socket the browser
//     refuses to make (its constructor throws) is cdom's CD_WS_REFUSED then its close, from the loop;
//     the world refuses at the open, which cdom never does, so that is the adapter's own case
//   - the request: Authorization travels as a header line (the world gets its value), and a body
//     without "Content-Type: application/octet-stream" (or a type without a body) is a broken rule;
//     its deadline must be the core's rule, http_wait_ms + body_len / (CBC_HTTP_MIN_BPS / 1000); an
//     answer the world gives at that deadline is CD_HTTP_TIMEOUT, any other status 0 CD_HTTP_FAILED
//   - the store holds base64 text and answers later (IndexedDB): the world holds the session's bytes,
//     so a put is decoded and a get encoded here, by this file's own codec (standard alphabet, '='
//     padding, as btoa writes), not the host's; each answer is delivered after the call that asked,
//     before the world's next delivery, and the host must open and send nothing while its load is out
//   - a page outlives each case's host: the next case stops it and starts the handle again, and
//     whatever cdom still owed the last one (its requests, its load, its socket's close) is delivered
//     after that start, where it must change nothing
// Exit 0 when every case passed and no rule of this file was broken.
#include "host_suite.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "cbc_cdom.h"
#include "le_bytes.h"   // lib/: the server's session, written as the server writes it
#include "cdom.h"
#include "cdom_net.h"
#include "cdom_random.h"
#include "cdom_store.h"

static cbhs_world *g_w;   // NULL: solo (the host's own cases), or between two cases
static cbc_cdom g_host;
static int g_broken;
static void broke(const char *why) { g_broken++; fprintf(stderr, "cdom adapter: %s\n", why); }

// ---- solo: a clock and a timer list of this file's own, for the cases with no world ----
static int64_t g_solo_now = INT64_C(1700000000000);
#define SOLO_TIMERS 64
static struct { uint32_t token; uint32_t ms; cd_timer_fn fn; void *ctx; int armed; } g_st[SOLO_TIMERS];
static uint32_t g_n_st;
static int solo_armed(void) {
    int n = 0;
    for (uint32_t i = 0; i < g_n_st; i++) n += g_st[i].armed;
    return n;
}
// Fires the armed solo timers due at the earliest time, as the page's loop would; 0 when none is.
static int solo_fire(void) {
    int k = -1;
    for (uint32_t i = 0; i < g_n_st; i++)
        if (g_st[i].armed && (k < 0 || g_st[i].ms < g_st[k].ms)) k = (int)i;
    if (k < 0) return 0;
    g_st[k].armed = 0;
    g_st[k].fn(g_st[k].ctx);
    return 1;
}

// ---- the page: its origin, location.protocol + "//" + location.host ----
static const char *g_proto = "http:";
static const char *g_page_host = "app.test";   // location.host ("" on a file: page)
// cdom's: location.protocol + "//" + location.host, at most cap bytes written, its whole length answered.
uint32_t cd_page_origin(char *dst, uint32_t cap) {
    char v[300];
    int n = snprintf(v, sizeof v, "%s//%s", g_proto, strcmp(g_proto, "file:") == 0 ? "" : g_page_host);
    uint32_t len = n > 0 ? (uint32_t)n : 0;
    if (len) memcpy(dst, v, len < cap ? len : cap);
    return len;
}

// ---- time and entropy ----
double cd_epoch_ms(void) { return (double)(g_w ? cbhs_now(g_w) : g_solo_now); }
static int g_no_crypto;
uint32_t cd_random_bytes(void *buf, uint32_t len) {
    if (g_no_crypto) return CD_RANDOM_UNAVAILABLE;
    if (g_w) cbhs_entropy(g_w, buf, len);
    else memset(buf, 7, len);
    return CD_RANDOM_OK;
}

// ---- the socket: cdom's ids over the world's ----
#define SOCKS 16
static struct { int live, refused; uint32_t wid; cd_ws_fn fn; void *ctx; } g_sock[SOCKS];
static int g_refuse;   // solo: the next cd_ws_open_cb is a socket the browser refuses to make
static uint32_t g_n_ids, g_free[SOCKS], g_n_free;   // the shim's: the ids made, the freed ones (a stack)
static int g_store_out;   // the host's load is not answered yet
static uint32_t g_n_opens;
static char g_url[512];
static void quiet(const char *what) { if (g_store_out) broke(what); }

uint32_t cd_ws_open_cb(cd_ws_fn fn, void *ctx, const char *url, uint32_t len) {
    quiet("a socket opened while the session's load is out");
    g_n_opens++;
    char origin[300];
    snprintf(origin, sizeof origin, "%s%s", strcmp(g_proto, "https:") == 0 ? "wss://" : "ws://", g_page_host);
    uint32_t o = (uint32_t)strlen(origin);
    char path[64];
    if (len < o || memcmp(url, origin, o) != 0 || len - o >= sizeof path) { broke("a socket URL not on the page's scheme and host"); o = len = 0; }
    memcpy(path, url + o, len - o);
    path[len - o] = 0;
    memcpy(g_url, url, len < sizeof g_url - 1 ? len : sizeof g_url - 1);
    g_url[len < sizeof g_url - 1 ? len : sizeof g_url - 1] = 0;
    uint32_t wid = 0;
    if (g_w && cbhs_ws_open(g_w, path, &wid) != 0) broke("the world refused a socket at its open, which cdom reports later");
    uint32_t id = g_n_free ? g_free[--g_n_free] : g_n_ids++;
    if (id >= SOCKS) { broke("more sockets than the adapter has room for"); return 0; }
    g_sock[id].live = 1;
    g_sock[id].refused = g_refuse;   // its CD_WS_REFUSED and close come from the loop, never in this call
    g_refuse = 0;
    g_sock[id].wid = wid;
    g_sock[id].fn = fn;
    g_sock[id].ctx = ctx;
    return id;
}
static int g_n_sends, g_n_closes;
void cd_ws_send(uint32_t id, const void *p, uint32_t len) {
    if (id >= SOCKS || !g_sock[id].live) { broke("a send on an id cdom has not handed out"); return; }
    if (g_sock[id].refused) broke("a send on a socket that never opened");
    g_n_sends++;
    if (g_w) cbhs_ws_send(g_w, g_sock[id].wid, p, len);
}
void cd_ws_close(uint32_t id) {
    if (id >= SOCKS || !g_sock[id].live) { broke("a close of an id cdom has not handed out"); return; }
    g_n_closes++;
    if (g_w) cbhs_ws_close(g_w, g_sock[id].wid);
}
// The cdom id of the world's socket wid (SOCKS: none).
static uint32_t cid(uint32_t wid) {
    for (uint32_t i = 0; i < g_n_ids && i < SOCKS; i++)
        if (g_sock[i].live && g_sock[i].wid == wid) return i;
    return SOCKS;
}
static void deliver(uint32_t id, uint32_t ev, const uint8_t *p, uint32_t len) {
    if (id >= SOCKS || !g_sock[id].live) return;
    cd_ws_fn fn = g_sock[id].fn;
    void *ctx = g_sock[id].ctx;
    if (ev == CD_WS_CLOSE) { g_sock[id].live = 0; g_free[g_n_free++] = id; }   // freed before fn hears it
    fn(ctx, id, ev, p, len);
}

// ---- HTTP ----
#define REQS 128
typedef struct { cd_http_fn fn; void *ctx; int64_t sent; uint32_t deadline; int answered; } fake_req;
static fake_req g_req[REQS];
static uint32_t g_n_req;
static void http_done(void *ctx, int status, const uint8_t *body, size_t len) {
    fake_req *q = &g_req[(uint32_t)(uintptr_t)ctx];
    q->answered = 1;
    uint32_t s = (uint32_t)status;
    // The world answers status 0 at the deadline as an aborted fetch: cdom says CD_HTTP_TIMEOUT.
    if (status == 0) s = q->deadline && cbhs_now(g_w) >= q->sent + q->deadline ? CD_HTTP_TIMEOUT : CD_HTTP_FAILED;
    q->fn(q->ctx, s, body, (uint32_t)len);
}
static const char *header(const char *hd, uint32_t hl, const char *name, uint32_t *vl) {
    uint32_t nl = (uint32_t)strlen(name);
    for (uint32_t i = 0; i < hl;) {
        uint32_t e = i;
        while (e < hl && hd[e] != '\n') e++;
        if (e - i > nl && memcmp(hd + i, name, nl) == 0) { *vl = e - i - nl; return hd + i + nl; }
        i = e + 1;
    }
    return NULL;
}
void cd_http_cb(cd_http_fn fn, void *ctx, const char *method, uint32_t mlen, const char *url, uint32_t ulen,
                const char *headers, uint32_t hlen, const void *body, uint32_t blen, uint32_t deadline_ms) {
    quiet("a request sent while the session's load is out");
    const cbc_cfg *cfg = &cbc_cdom_client(&g_host)->cfg;
    uint32_t want = cfg->http_wait_ms ? cfg->http_wait_ms + blen / (CBC_HTTP_MIN_BPS / 1000) : 0;
    if (deadline_ms != want) broke("a request whose deadline is not http_wait_ms + body_len / (CBC_HTTP_MIN_BPS / 1000)");
    char m[8] = {0};
    memcpy(m, method, mlen < 7 ? mlen : 7);
    uint32_t al = 0, cl = 0;
    const char *auth = header(headers, hlen, "Authorization: ", &al);
    const char *ct = header(headers, hlen, "Content-Type: ", &cl);
    int typed = ct && cl == 24 && memcmp(ct, "application/octet-stream", 24) == 0;
    if (blen ? !typed : ct != NULL) broke("a body without its octet-stream type, or a type without a body");
    if (ulen == 0 || url[0] != '/') broke("a request URL that is not a path on the page's origin");
    if (g_n_req == REQS) { broke("more requests than the adapter has room for"); return; }
    uint32_t i = g_n_req++;
    g_req[i] = (fake_req){ fn, ctx, g_w ? cbhs_now(g_w) : g_solo_now, deadline_ms, 0 };
    if (g_w) cbhs_http(g_w, m, url, ulen, auth, al, body, blen, deadline_ms, http_done, (void *)(uintptr_t)i);
}

// ---- timers: cdom's tokens are the world's ids (solo: this file's own, in cdom's range) ----
uint32_t cd_timer_cb(uint32_t ms, cd_timer_fn fn, void *ctx) {
    if (g_w) return cbhs_timer(g_w, ms, fn, ctx);
    if (g_n_st == SOLO_TIMERS) { broke("more solo timers than the adapter has room for"); return CD_TOKEN_CDOM; }
    uint32_t i = g_n_st++;
    g_st[i].token = CD_TOKEN_CDOM | (i + 1);
    g_st[i].ms = ms;
    g_st[i].fn = fn;
    g_st[i].ctx = ctx;
    g_st[i].armed = 1;
    return g_st[i].token;
}
void cd_timer_stop(uint32_t token) {
    if (g_w) { cbhs_timer_stop(g_w, token); return; }
    for (uint32_t i = 0; i < g_n_st; i++)
        if (g_st[i].token == token) g_st[i].armed = 0;
}

// ---- the store: base64 text over the world's session bytes, answered later ----
static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static size_t enc(const uint8_t *p, size_t n, char *o) {
    size_t k = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)p[i] << 16 | (i + 1 < n ? (uint32_t)p[i + 1] << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
        o[k++] = B64[v >> 18 & 63];
        o[k++] = B64[v >> 12 & 63];
        o[k++] = i + 1 < n ? B64[v >> 6 & 63] : '=';
        o[k++] = i + 2 < n ? B64[v & 63] : '=';
    }
    return k;
}
static long dec(const char *s, size_t n, uint8_t *o) {   // -1 unless canonical padded base64
    if (n % 4) return -1;
    size_t k = 0;
    for (size_t i = 0; i < n; i += 4) {
        uint32_t v = 0;
        int pad = 0;
        for (int j = 0; j < 4; j++) {
            const char *c = strchr(B64, s[i + j]);
            if (s[i + j] == '=' && i + 4 == n && j >= 2) { pad++; v <<= 6; continue; }
            if (!c || !s[i + j] || pad) return -1;
            v = v << 6 | (uint32_t)(c - B64);
        }
        o[k++] = (uint8_t)(v >> 16);
        if (pad < 2) o[k++] = (uint8_t)(v >> 8);
        if (pad < 1) o[k++] = (uint8_t)v;
    }
    return (long)k;
}

#define ANSWERS 8
typedef struct { int get; cd_store_fn got; cd_store_done_fn done; void *ctx; } fake_ans;
static fake_ans g_ans[ANSWERS];
static int g_n_ans;
static char g_key[CBC_CDOM_KEY_MAX + 8] = "app.session";   // the key the store expects
static uint8_t g_solo_store[CBC_HOST_SESS_CAP];               // solo: what the store keeps
static size_t g_solo_len;
static void later(int get, cd_store_fn got, cd_store_done_fn done, void *ctx) {
    if (g_n_ans == ANSWERS) { broke("more store answers out than the adapter has room for"); return; }
    g_ans[g_n_ans++] = (fake_ans){ get, got, done, ctx };
}
// Delivers the answers the host is owed, in order: the loop's turn before the world's next delivery.
static void drain(void) {
    for (int i = 0; i < g_n_ans; i++) {
        if (!g_ans[i].get) { g_ans[i].done(g_ans[i].ctx, 1); continue; }
        static uint8_t raw[CBC_HOST_SESS_CAP];
        static char text[CBC_CDOM_TEXT_CAP + 1];
        size_t n = g_w ? cbhs_load(g_w, raw, sizeof raw) : g_solo_len;
        if (!g_w) memcpy(raw, g_solo_store, n);
        size_t t = enc(raw, n, text);
        g_store_out = 0;
        g_ans[i].got(g_ans[i].ctx, (const uint8_t *)text, n ? (int32_t)t : -1);
    }
    g_n_ans = 0;
}
static int key_ok(const char *key, uint32_t klen) { return klen == strlen(g_key) && memcmp(key, g_key, klen) == 0; }
void cd_store_get_cb(cd_store_fn fn, void *ctx, const char *key, uint32_t klen, uint32_t deadline_ms) {
    if (!key_ok(key, klen) || deadline_ms != CD_STORE_DEADLINE_MS) broke("a get of another key, or with another deadline");
    g_store_out = 1;
    later(1, fn, NULL, ctx);
}
static void keep(const uint8_t *p, size_t n) {
    if (g_w) cbhs_save(g_w, p, n);
    else { if (n) memcpy(g_solo_store, p, n); g_solo_len = n; }
}
void cd_store_put_cb(cd_store_done_fn fn, void *ctx, const char *key, uint32_t klen, const void *p, uint32_t len) {
    if (!key_ok(key, klen)) broke("a put of another key");
    static uint8_t raw[CBC_CDOM_TEXT_CAP];
    long n = len <= CBC_CDOM_TEXT_CAP ? dec(p, len, raw) : -1;
    if (n <= 0) broke("a put that is not the session's base64");
    else keep(raw, (size_t)n);
    later(0, NULL, fn, ctx);
}
void cd_store_del_cb(cd_store_done_fn fn, void *ctx, const char *key, uint32_t klen) {
    if (!key_ok(key, klen)) broke("a delete of another key");
    keep(NULL, 0);
    later(0, NULL, fn, ctx);
}

// ---- a page's turn between two hosts: the last one stopped, what cdom owed it delivered late ----
static fake_req g_old_req[REQS];
static uint32_t g_n_old_req;
static fake_ans g_old_ans[ANSWERS];
static int g_n_old_ans;
static void stop_host(void) {
    cbhs_world *w = g_w;
    g_w = NULL;   // the old world is gone: the stop's timer stops and close reach cdom only
    cbc_cdom_stop(&g_host);
    g_w = w;
    memcpy(g_old_req, g_req, sizeof g_req);
    g_n_old_req = g_n_req;
    memcpy(g_old_ans, g_ans, sizeof g_ans);
    g_n_old_ans = g_n_ans;
    g_n_req = 0;
    g_n_ans = 0;
    g_store_out = 0;
}
// cdom answers everything the stopped host was still owed: its requests failed, its load a miss,
// its sockets' closes. Every one must be dropped (the new host's world sees nothing of it).
static void owed_late(void) {
    for (uint32_t i = 0; i < g_n_old_req; i++)
        if (!g_old_req[i].answered) { g_old_req[i].answered = 1; g_old_req[i].fn(g_old_req[i].ctx, CD_HTTP_FAILED, NULL, 0); }
    g_n_old_req = 0;
    for (int i = 0; i < g_n_old_ans; i++) {
        if (g_old_ans[i].get) g_old_ans[i].got(g_old_ans[i].ctx, (const uint8_t *)"AAAA", 4);   // a session that is no one's
        else g_old_ans[i].done(g_old_ans[i].ctx, 1);
    }
    g_n_old_ans = 0;
    for (uint32_t i = 0; i < g_n_ids && i < SOCKS; i++)
        if (g_sock[i].live && g_sock[i].ctx == &g_host) deliver(i, CD_WS_CLOSE, NULL, 1006);
}

// ---- the host, as the suite drives it ----
static void on_event(void *ctx, cbc *c, const cbc_event *e, const uint8_t *bytes) {
    if (ctx) cbhs_event(ctx, c, e, bytes);
}
static int g_ready;
static void on_ready(void *ctx, cbc *c) { (void)ctx; (void)c; g_ready++; }

static void *start(cbhs_world *w, const cbc_cfg *cfg) {
    stop_host();
    g_w = w;
    g_ready = 0;
    if (cbc_cdom_start(&g_host, cfg, "app.session", on_event, on_ready, w) != 0) { broke("the start failed"); return &g_host; }
    if (cfg->anonymous ? g_n_ans != 0 : g_ready != 0) broke("an anonymous client asked the store, or a keyed one was ready before its load");
    owed_late();
    drain();
    if (g_ready != 1) broke("ready was not called once by the end of the start");
    return &g_host;
}
static cbc *client(void *h) { return cbc_cdom_client(h); }
static void pump(void *h) { drain(); cbc_cdom_pump(h); }
static void ws_opened(void *h, uint32_t id) { (void)h; drain(); deliver(cid(id), CD_WS_OPEN, NULL, 0); }
static void ws_message(void *h, uint32_t id, const uint8_t *p, size_t n) { (void)h; drain(); deliver(cid(id), CD_WS_MSG, p, (uint32_t)n); }
static void ws_closed(void *h, uint32_t id) { (void)h; drain(); deliver(cid(id), CD_WS_CLOSE, NULL, 1006); }
// cdom has no error event: the browser's says nothing, and its close follows (no_sock_why).
static void ws_failed(void *h, uint32_t id, int why) { (void)h; (void)id; (void)why; drain(); }

static const cbhs_host CDOM_HOST = { "cdom (hosts/cdom/cbc_cdom.c)", start, client, pump, ws_opened, ws_message, ws_closed, ws_failed, 1 };

// ---- the adapter's own cases (solo) ----
static int g_fails;
static void check(int ok, const char *what) {
    printf("%s  cdom: %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) g_fails++;
}
static int g_events, g_done, g_done_status, g_failed, g_failed_why;
static uint32_t g_done_ticket;
static uint8_t g_done_body[16];
static void solo_event(void *ctx, cbc *c, const cbc_event *e, const uint8_t *bytes) {
    (void)ctx;
    (void)c;
    g_events++;
    if (e->kind == CBC_EV_DONE) {
        g_done++;
        g_done_status = e->status;
        g_done_ticket = e->ticket;
        memset(g_done_body, 0, sizeof g_done_body);
        if (bytes && e->len < sizeof g_done_body) memcpy(g_done_body, bytes + e->off, e->len);
    }
    if (e->kind == CBC_EV_SOCKET_FAILED) { g_failed++; g_failed_why = e->why; }
}
// cdom's answer to request i (solo), once.
static void answer_req(uint32_t i, uint32_t status, const uint8_t *body, uint32_t len) {
    if (g_req[i].answered) { broke("a request answered twice"); return; }
    g_req[i].answered = 1;
    g_req[i].fn(g_req[i].ctx, status, body, len);
}
// A solo page: no world, the last host stopped and everything cdom owed it answered (dropped), every
// table empty.
static void solo(void) {
    g_w = NULL;
    cbc_cdom_stop(&g_host);
    for (uint32_t i = 0; i < g_n_req; i++)
        if (!g_req[i].answered) answer_req(i, CD_HTTP_FAILED, NULL, 0);
    drain();
    g_n_req = g_n_ans = 0;
    g_n_old_req = 0;
    g_n_old_ans = 0;
    g_n_st = 0;
    g_store_out = 0;
    g_ready = g_events = g_done = g_failed = 0;
    g_n_opens = 0;
    g_n_sends = g_n_closes = 0;
    g_solo_len = 0;
    g_proto = "http:";
    g_page_host = "app.test";
    g_refuse = 0;
}
static cbc_cfg anon(void) {
    cbc_cfg k;
    cbc_cfg_default(&k);
    k.anonymous = 1;
    return k;
}
// A sign-up's answer, as the server writes it: a session for ANN, a day off.
static size_t session_body(uint8_t *o) {
    size_t n = 0;
    o[n++] = CBW_V;
    o[n++] = CBW_SESSION;
    le_put_u16(o + n, 3); n += 2; memcpy(o + n, "TOK", 3); n += 3;
    o[n++] = 2; memcpy(o + n, "R1", 2); n += 2;
    le_put_u48(o + n, (uint64_t)(g_solo_now + 86400000)); n += 6;
    o[n++] = 3; memcpy(o + n, "u-1", 3); n += 3;
    o[n++] = 3; memcpy(o + n, "ANN", 3); n += 3;
    return n;
}

static uint32_t post(const char *path, const char *body) {
    uint32_t t = cbc_call(cbc_cdom_client(&g_host), CBC_POST, 0, path, (uint32_t)strlen(path), (const uint8_t *)body,
                          (uint32_t)strlen(body), cbc_cdom_now());
    cbc_cdom_pump(&g_host);
    return t;
}

// (a) A request still out at a stop is cdom's to answer later, into a slot the new start's
// requests reuse in the shared host: the answer must reach nothing.
static void a_late_http_answer_after_a_restart_is_dropped(void) {
    solo();
    cbc_cfg k = anon();
    cbc_cdom_start(&g_host, &k, NULL, solo_event, NULL, NULL);
    post("/v1/a", "1");
    uint32_t old = g_n_req;
    cbc_cdom_stop(&g_host);
    cbc_cdom_start(&g_host, &k, NULL, solo_event, NULL, NULL);
    uint32_t t = post("/v1/b", "2");
    int sent = old == 1 && g_n_req == 2;
    answer_req(0, 200, (const uint8_t *)"OLD", 3);   // the stopped start's answer
    int quiet_after = g_done == 0;
    answer_req(1, 200, (const uint8_t *)"NEW", 3);
    check(sent && quiet_after && g_done == 1 && g_done_ticket == t && memcmp(g_done_body, "NEW", 4) == 0,
          "a request's answer after a stop and a new start reaches nothing; the new start's own answer is its call's");
}

// (a) The load is cdom's to answer too: an older start's (a session, here) changes nothing, and
// ready comes once, from the new start's own load.
static void a_late_load_after_a_restart_is_dropped(void) {
    solo();
    snprintf(g_key, sizeof g_key, "app.session");
    cbc_cdom_start(&g_host, NULL, "app.session", solo_event, on_ready, NULL);
    fake_ans first = g_ans[0];
    g_n_ans = 0;
    cbc_cdom_stop(&g_host);
    cbc_cdom_start(&g_host, NULL, "app.session", solo_event, on_ready, NULL);
    // The stopped start's load answers a session (as cbc_session_save writes it, base64), late.
    cbc other;
    cbc_cfg k;
    cbc_cfg_default(&k);
    cbc_init(&other, &k);
    uint8_t b[64], raw[256];
    size_t bn = session_body(b);
    cbc_sign_in(&other, CBW_SIGNUP, "ANN", 3, "secret!!", 8, g_solo_now);
    static uint8_t rq[CBC_HOST_REQ_CAP];
    cbc_req_hdr rh;
    memset(&rh, 0, sizeof rh);
    if (cbc_next_request(&other, rq, sizeof rq, g_solo_now) > 0) memcpy(&rh, rq, sizeof rh);
    cbc_on_response(&other, rh.id, 200, b, bn, g_solo_now);
    char text[CBC_CDOM_TEXT_CAP + 1];
    int rn = cbc_session_save(&other, raw, sizeof raw);
    size_t tn = enc(raw, rn > 0 ? (size_t)rn : 0, text);
    first.got(first.ctx, (const uint8_t *)text, (int32_t)tn);
    int before = g_ready;
    drain();   // the new start's: a miss
    check(rn > 0 && before == 0 && g_ready == 1 && !cbc_user_of(cbc_cdom_client(&g_host))->signed_in,
          "an older start's load answers nothing; ready comes once, from the new start's own");
}

// (a) A stop cancels the tick and reopen timers and closes the socket; the socket's late events
// tell the app nothing and send nothing.
static void a_stop_cancels_the_timers_and_closes_the_socket(void) {
    solo();
    cbc_cfg k = anon();
    cbc_cdom_start(&g_host, &k, NULL, solo_event, NULL, NULL);
    cbc_sub(cbc_cdom_client(&g_host), CBC_ROOM, "feed", 4);
    cbc_cdom_pump(&g_host);
    int opened = g_n_opens == 1;
    uint32_t id = SOCKS;   // the host's socket, by the id cdom handed it
    for (uint32_t i = 0; i < g_n_ids && i < SOCKS; i++)
        if (g_sock[i].live && g_sock[i].ctx == &g_host) id = i;
    deliver(id, CD_WS_OPEN, NULL, 0);   // now open: the HELLO goes out, a timer is armed
    int armed = solo_armed() > 0 && g_n_sends == 1;
    cbc_cdom_stop(&g_host);
    int stopped = solo_armed() == 0 && g_n_closes == 1;
    int ev = g_events, sends = g_n_sends;
    deliver(id, CD_WS_MSG, (const uint8_t *)"\x00", 1);
    deliver(id, CD_WS_CLOSE, NULL, 1000);
    check(opened && armed && stopped && g_events == ev && g_n_sends == sends && solo_armed() == 0,
          "a stop cancels every timer and closes the socket; its late events tell the app nothing and send nothing");
}

// (a) A stop from inside the app's own callback stops the host there: whatever call the callback came
// from (an HTTP answer's DONE, a socket's CBC_EV_SOCKET_FAILED, ready after the load), nothing is
// opened, sent or armed after it, though a subscription still wants the socket.
static int g_stop_on;   // the event kind stop_event stops the host on (0: none)
static void stop_event(void *ctx, cbc *c, const cbc_event *e, const uint8_t *bytes) {
    solo_event(ctx, c, e, bytes);
    if (g_stop_on && ((int)e->kind == g_stop_on || g_stop_on < 0)) cbc_cdom_stop(&g_host);   // -1: on any
}
static void sub_and_stop(void *ctx, cbc *c) {   // ready: a socket wanted and a call made, then the stop
    (void)ctx;
    g_ready++;
    cbc_sub(c, CBC_ROOM, "feed", 4);
    cbc_call(c, CBC_POST, CBC_C_HTTP, "/v1/a", 5, (const uint8_t *)"1", 1, cbc_cdom_now());
    cbc_cdom_stop(&g_host);
}
static uint32_t host_sock(void) {   // the id cdom handed the host's socket (SOCKS: none)
    uint32_t id = SOCKS;
    for (uint32_t i = 0; i < g_n_ids && i < SOCKS; i++)
        if (g_sock[i].live && g_sock[i].ctx == &g_host) id = i;
    return id;
}
static int stopped_quiet(uint32_t opens) { return !g_host.live && g_n_opens == opens && g_n_sends == 0 && solo_armed() == 0; }
static void a_stop_inside_a_callback_stops_the_host(void) {
    cbc_cfg k = anon();
    solo();   // in a call's DONE, the sub made while the call was out
    g_stop_on = CBC_EV_DONE;
    cbc_cdom_start(&g_host, &k, NULL, stop_event, NULL, NULL);
    post("/v1/a", "1");
    cbc_sub(cbc_cdom_client(&g_host), CBC_ROOM, "feed", 4);
    answer_req(0, 200, (const uint8_t *)"ok", 2);
    int done = g_done == 1 && stopped_quiet(0);
    solo();   // in the first of a sign-up answer's events: the rest of the batch is not told
    g_stop_on = -1;
    cbc_cdom_start(&g_host, NULL, "app.session", stop_event, NULL, NULL);
    drain();
    cbc_sign_in(cbc_cdom_client(&g_host), CBW_SIGNUP, "ANN", 3, "secret!!", 8, cbc_cdom_now());
    cbc_cdom_pump(&g_host);
    uint8_t b[64];
    size_t bn = session_body(b);
    answer_req(0, 200, b, (uint32_t)bn);
    int batch = cbc_n_events(cbc_cdom_client(&g_host)) >= 2 && g_events == 1 && stopped_quiet(0);
    solo();   // in the CBC_EV_SOCKET_FAILED of a socket that closed before it opened
    g_stop_on = CBC_EV_SOCKET_FAILED;
    cbc_cdom_start(&g_host, &k, NULL, stop_event, NULL, NULL);
    cbc_sub(cbc_cdom_client(&g_host), CBC_ROOM, "feed", 4);
    cbc_cdom_pump(&g_host);
    deliver(host_sock(), CD_WS_CLOSE, NULL, 1006);
    int closed = g_failed == 1 && stopped_quiet(1);
    solo();   // in the CBC_EV_SOCKET_FAILED a file: page raises from inside the pump, a lost read's retry due
    g_proto = "file:";
    cbc_cdom_start(&g_host, &k, NULL, stop_event, NULL, NULL);
    cbc_call(cbc_cdom_client(&g_host), CBC_GET, CBC_C_IDEMPOTENT, "/v1/r", 5, NULL, 0, cbc_cdom_now());
    cbc_cdom_pump(&g_host);
    answer_req(0, CD_HTTP_FAILED, NULL, 0);
    int due = solo_armed() == 1;
    cbc_sub(cbc_cdom_client(&g_host), CBC_ROOM, "feed", 4);
    cbc_cdom_pump(&g_host);
    int unsupported = due && g_failed == 1 && g_n_req == 1 && stopped_quiet(0);
    g_proto = "http:";
    solo();   // in ready, called inside the start (anonymous)
    g_stop_on = 0;
    cbc_cdom_start(&g_host, &k, NULL, stop_event, sub_and_stop, NULL);
    int ready = g_ready == 1 && g_n_req == 0 && stopped_quiet(0);
    solo();   // in ready, after the load
    cbc_cdom_start(&g_host, NULL, "app.session", stop_event, sub_and_stop, NULL);
    drain();
    int loaded = g_ready == 1 && g_n_req == 0 && stopped_quiet(0);
    check(done && batch && closed && unsupported && ready && loaded,
          "a stop inside DONE, SOCKET_FAILED or ready opens, sends and arms nothing after it");
}

// (a) Starting a live handle traps (it would orphan the first start's socket and timers), and so
// does a start from inside the app's callback, stopped first: the call it came from still runs on
// the handle.
static void restart(void) {
    cbc_cdom_stop(&g_host);
    cbc_cfg k = anon();
    cbc_cdom_start(&g_host, &k, NULL, solo_event, NULL, NULL);
}
static void restart_event(void *ctx, cbc *c, const cbc_event *e, const uint8_t *bytes) {
    solo_event(ctx, c, e, bytes);
    if (e->kind == CBC_EV_DONE) restart();
}
static void restart_ready(void *ctx, cbc *c) { (void)ctx; (void)c; restart(); }
static int traps(int how) {   // a child's start: 0 on a live handle, 1 in DONE, 2 in ready
    fflush(stdout);
    pid_t p = fork();
    if (p == 0) {
        cbc_cfg k = anon();
        cbc_cdom_start(&g_host, &k, NULL, how == 1 ? restart_event : solo_event, how == 2 ? restart_ready : NULL, NULL);
        if (how == 0) cbc_cdom_start(&g_host, &k, NULL, solo_event, NULL, NULL);
        if (how == 1) { post("/v1/a", "1"); answer_req(0, 200, (const uint8_t *)"ok", 2); }
        _exit(0);
    }
    int st = 0;
    waitpid(p, &st, 0);
    return WIFSIGNALED(st) && (WTERMSIG(st) == SIGTRAP || WTERMSIG(st) == SIGILL);
}
static void a_start_on_a_live_handle_traps(void) {
    solo();
    int trapped = traps(0);
    solo();
    int in_done = traps(1);
    solo();
    int in_ready = traps(2);
    solo();
    cbc_cfg k = anon();
    int once = cbc_cdom_start(&g_host, &k, NULL, solo_event, NULL, NULL) == 0;
    cbc_cdom_stop(&g_host);
    int again = cbc_cdom_start(&g_host, &k, NULL, solo_event, NULL, NULL) == 0;
    check(trapped && once && again, "a start on a live handle traps; a stopped one starts again");
    check(in_done && in_ready, "a start from inside DONE or ready traps, stopped first: from the loop only");
}

// (a) Old answers fill the slot table only so far: past it, a request is answered status 0 from a
// 0 ms timer, and is never handed to cdom (its answer would have nowhere to go).
static void a_full_slot_table_answers_status_0_later(void) {
    solo();
    cbc_cfg k = anon();
    for (int gen = 0; gen < 2; gen++) {   // two starts, each leaving every call it can make out
        cbc_cdom_start(&g_host, &k, NULL, solo_event, NULL, NULL);
        for (int i = 0; i < CBC_CALLS_MAX; i++) post("/v1/w", "x");
        cbc_cdom_stop(&g_host);
    }
    uint32_t before = g_n_req;
    int room = CBC_CDOM_OWED_MAX - (int)before;
    cbc_cdom_start(&g_host, &k, NULL, solo_event, NULL, NULL);
    g_done = 0;
    for (int i = 0; i < CBC_CALLS_MAX; i++) post("/v1/w", "x");
    int handed = (int)(g_n_req - before) == room, none_yet = g_done == 0;
    while (solo_fire()) {}
    check(before == 2 * CBC_CALLS_MAX && room > 0 && room < CBC_CALLS_MAX && handed && none_yet &&
          g_done == CBC_CALLS_MAX - room && g_done_status == 0,
          "past the slot table, a request is answered status 0 from a timer and never handed to cdom");
    cbc_cdom_stop(&g_host);
}

// (b) The key is the handle's own copy, up to CBC_CDOM_KEY_MAX; a longer one starts nothing.
static void the_key_is_copied_and_bounded(void) {
    solo();
    char key[CBC_CDOM_KEY_MAX + 2];
    memset(key, 'k', sizeof key - 1);
    key[sizeof key - 1] = 0;   // CBC_CDOM_KEY_MAX + 1 bytes
    int r = cbc_cdom_start(&g_host, NULL, key, solo_event, on_ready, NULL);
    int refused = r == -1 && g_n_ans == 0 && g_ready == 0 && !g_host.live;
    if (r == 0) solo();   // started on a key it should have refused: its get is not this case's
    key[CBC_CDOM_KEY_MAX] = 0;   // CBC_CDOM_KEY_MAX bytes: taken
    memcpy(g_key, key, CBC_CDOM_KEY_MAX + 1);
    int took = cbc_cdom_start(&g_host, NULL, key, solo_event, on_ready, NULL) == 0;
    memset(key, 'x', CBC_CDOM_KEY_MAX);   // the app's buffer changes: the host's key does not
    drain();
    uint32_t t = cbc_sign_in(cbc_cdom_client(&g_host), CBW_SIGNUP, "ANN", 3, "secret!!", 8, cbc_cdom_now());
    cbc_cdom_pump(&g_host);
    int before = g_broken;
    uint8_t b[64];
    size_t n = session_body(b);
    if (t && g_n_req == 1) answer_req(0, 200, b, (uint32_t)n);   // the session: a put, under the host's copy
    check(refused && took && t && g_n_req == 1 && g_broken == before && g_solo_len > 0,
          "a key past CBC_CDOM_KEY_MAX starts nothing (-1); one at it is the handle's copy, the app's buffer free after");
    cbc_cdom_stop(&g_host);
    snprintf(g_key, sizeof g_key, "app.session");
}

// (c) A page that is not http: or https: (file:) makes no socket: the app hears a
// CBC_EV_SOCKET_FAILED (CBC_SOCK_UNSUPPORTED) a try, the next try after the reconnect wait.
static void a_page_that_is_not_http_has_no_socket(void) {
    solo();
    g_proto = "file:";
    cbc_cfg k = anon();
    cbc_cdom_start(&g_host, &k, NULL, solo_event, NULL, NULL);
    cbc_sub(cbc_cdom_client(&g_host), CBC_ROOM, "feed", 4);
    cbc_cdom_pump(&g_host);
    int first = g_failed == 1 && g_failed_why == CBC_SOCK_UNSUPPORTED && g_n_opens == 0;
    for (int i = 0; i < 3; i++) solo_fire();   // the reopen wait (and any tick): a try each
    check(first && g_failed >= 2 && g_failed_why == CBC_SOCK_UNSUPPORTED && g_n_opens == 0,
          "a file: page opens no socket: a CBC_EV_SOCKET_FAILED (CBC_SOCK_UNSUPPORTED) a try, after the reconnect wait");
    cbc_cdom_stop(&g_host);
    g_proto = "http:";
}

// (d) On an https: page the socket is wss:// (the suite's second run holds it on every case).
static void an_https_page_opens_wss(void) {
    solo();
    g_proto = "https:";
    cbc_cfg k = anon();
    cbc_cdom_start(&g_host, &k, NULL, solo_event, NULL, NULL);
    cbc_sub(cbc_cdom_client(&g_host), CBC_ROOM, "feed", 4);
    cbc_cdom_pump(&g_host);
    check(g_n_opens == 1 && strcmp(g_url, "wss://app.test/ws/public") == 0, "an https: page's socket is wss://<host><path>");
    cbc_cdom_stop(&g_host);
    g_proto = "http:";
}

// (d) A host:port of 256 bytes is the page's (no host name is longer); one of 257 to 259 bytes (its
// origin past the adapter's room, or at it) makes no socket rather than one to a host cut short.
static void a_host_past_256_bytes_makes_no_socket(void) {
    static char host[260];
    int ok = 1;
    for (uint32_t len = 256; len <= 259; len++) {
        solo();
        memset(host, 'h', len - 5);
        memcpy(host + len - 5, ":8080", 6);
        g_page_host = host;
        cbc_cfg k = anon();
        cbc_cdom_start(&g_host, &k, NULL, solo_event, NULL, NULL);
        cbc_sub(cbc_cdom_client(&g_host), CBC_ROOM, "feed", 4);
        cbc_cdom_pump(&g_host);
        if (len == 256) ok &= g_n_opens == 1 && g_failed == 0 && strlen(g_url) == 5 + 256 + 10 && memcmp(g_url + 5, host, 256) == 0;
        else ok &= g_n_opens == 0 && g_failed == 1 && g_failed_why == CBC_SOCK_UNSUPPORTED;
        cbc_cdom_stop(&g_host);
    }
    g_page_host = "app.test";
    check(ok, "a host:port of 256 bytes opens ws://<it><path>; one of 257 to 259 bytes opens no socket (CBC_SOCK_UNSUPPORTED)");
}

// (e) A socket the browser refuses to make (its WebSocket constructor throws: a URL it does not take, a
// page's policy) is cdom's CD_WS_REFUSED, then its close, from the loop: the app hears one
// CBC_EV_SOCKET_FAILED (CBC_SOCK_REFUSED), the close after it tells it nothing, and the next try is
// after the reconnect wait, on the id cdom handed out again.
static void a_refused_socket_is_sock_refused(void) {
    solo();
    cbc_cfg k = anon();
    cbc_cdom_start(&g_host, &k, NULL, solo_event, NULL, NULL);
    g_refuse = 1;
    cbc_sub(cbc_cdom_client(&g_host), CBC_ROOM, "feed", 4);
    cbc_cdom_pump(&g_host);
    uint32_t id = SOCKS;
    for (uint32_t i = 0; i < g_n_ids && i < SOCKS; i++)
        if (g_sock[i].live && g_sock[i].ctx == &g_host) id = i;
    int made = g_n_opens == 1 && id < SOCKS && g_failed == 0;   // nothing yet: cdom answers from the loop
    deliver(id, CD_WS_REFUSED, NULL, 0);
    int refused = g_failed == 1 && g_failed_why == CBC_SOCK_REFUSED;
    deliver(id, CD_WS_CLOSE, NULL, 1006);
    int once = g_failed == 1 && g_n_opens == 1 && g_n_sends == 0;
    for (int i = 0; i < 3 && g_n_opens == 1; i++) solo_fire();   // the reconnect wait
    uint32_t again = SOCKS;
    for (uint32_t i = 0; i < g_n_ids && i < SOCKS; i++)
        if (g_sock[i].live && g_sock[i].ctx == &g_host) again = i;
    check(made && refused && once && g_n_opens == 2 && again == id,
          "a refused socket is one CBC_EV_SOCKET_FAILED (CBC_SOCK_REFUSED), its close after tells nothing, the next try after the wait");
    cbc_cdom_stop(&g_host);
}

// The store answers later, so a sign-up the app makes before ready waits in the client, and goes out
// once the load has answered a miss.
static void an_early_call_waits_for_the_load(void) {
    solo();
    int before = g_broken;
    cbc_cdom_start(&g_host, NULL, "app.session", solo_event, on_ready, NULL);
    uint32_t t = cbc_sign_in(cbc_cdom_client(&g_host), CBW_SIGNUP, "ANN", 3, "secret!!", 8, cbc_cdom_now());
    cbc_cdom_pump(&g_host);   // quiet(): a request now is a broken rule
    uint32_t early = g_n_req;
    drain();   // the miss
    check(t != 0 && early == 0 && g_n_req == 1 && g_ready == 1 && g_broken == before,
          "a sign-up made before the session's load answers waits for it, then goes out");
    cbc_cdom_stop(&g_host);
}

// No crypto.getRandomValues: no seed, so nothing starts (-1).
static void no_entropy_starts_nothing(void) {
    solo();
    g_no_crypto = 1;
    int r = cbc_cdom_start(&g_host, NULL, "app.session", solo_event, on_ready, NULL);
    g_no_crypto = 0;
    check(r == -1 && g_n_ans == 0 && !g_host.live, "a page without crypto.getRandomValues starts nothing (-1)");
}

int main(void) {
    int rc = cbhs_run(&CDOM_HOST);
    g_proto = "https:";   // the same cases on an https: page: every socket wss://
    rc |= cbhs_run(&CDOM_HOST);
    a_late_http_answer_after_a_restart_is_dropped();
    a_late_load_after_a_restart_is_dropped();
    a_stop_cancels_the_timers_and_closes_the_socket();
    a_stop_inside_a_callback_stops_the_host();
    a_start_on_a_live_handle_traps();
    a_full_slot_table_answers_status_0_later();
    the_key_is_copied_and_bounded();
    a_page_that_is_not_http_has_no_socket();
    an_https_page_opens_wss();
    a_host_past_256_bytes_makes_no_socket();
    a_refused_socket_is_sock_refused();
    an_early_call_waits_for_the_load();
    no_entropy_starts_nothing();
    if (g_broken) fprintf(stderr, "cdom adapter: %d rule(s) broken\n", g_broken);
    if (g_fails) fprintf(stderr, "cdom adapter: %d case(s) failed\n", g_fails);
    return rc || g_broken || g_fails ? 1 : 0;
}
