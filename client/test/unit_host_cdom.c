// unit_host_cdom.c - the host suite (test/host/host_suite.h) on the browser host
// (hosts/cdom/cbc_cdom.c), built natively: this file defines the cdom calls the host makes, each mapped onto
// the suite's world, and hands the world's deliveries to the callbacks the host gave cdom.
//
// Where cdom differs from the world, the mapping says so:
//   - the socket's URL is the page's (ws://app.test + the path): the world gets the path, and a URL
//     on another scheme or host is a broken rule here;
//   - the request's Authorization travels as a header line: the world gets its value, and a body
//     without "Content-Type: application/octet-stream" (or a header line with no body) is broken;
//   - the store holds base64 text and answers later (IndexedDB): the world holds the session's bytes,
//     so a put is decoded and a get encoded here, by this file's own codec (standard alphabet, '='
//     padding, as btoa writes), not the host's; each answer is delivered after the call that asked,
//     before the world's next delivery, and the host must open and send nothing while its load is out.
// Exit 0 when every case passed and no rule of this file was broken.
#include "host_suite.h"

#include <stdio.h>
#include <string.h>

#include "cbc_cdom.h"
#include "cdom.h"
#include "cdom_net.h"
#include "cdom_random.h"
#include "cdom_store.h"

static cbhs_world *g_w;
static cbc_cdom g_host;
static int g_broken;
static void broke(const char *why) { g_broken++; fprintf(stderr, "cdom adapter: %s\n", why); }

// ---- the page: location, and the literal strings it is read through ----
static const char *g_lits[8];
static uint32_t g_n_lits;
cd_str cd_lit_(cd_str *slot, const char *lit, uint32_t len) {
    (void)len;
    if (!*slot) { g_lits[g_n_lits] = lit; *slot = ++g_n_lits; }
    return *slot;
}
uint32_t cd_get_str(cd_id id, cd_str path, char *dst, uint32_t cap) {
    const char *v = "";
    if (id == CD_WINDOW && path && strcmp(g_lits[path - 1], "location.protocol") == 0) v = "http:";
    else if (id == CD_WINDOW && path && strcmp(g_lits[path - 1], "location.host") == 0) v = "app.test";
    else broke("a read of the page the adapter does not know");
    uint32_t n = (uint32_t)strlen(v);
    memcpy(dst, v, n < cap ? n : cap);
    return n;
}

// ---- time and entropy ----
// g_w is NULL in the adapter's own case (early_call_waits_for_the_load, below), which runs on no world.
double cd_epoch_ms(void) { return g_w ? (double)cbhs_now(g_w) : 1700000000000.0; }
uint32_t cd_random_bytes(void *buf, uint32_t len) {
    if (g_w) cbhs_entropy(g_w, buf, len);
    else memset(buf, 7, len);
    return len;
}

// ---- the socket ----
#define SOCKS 16
static struct { cd_ws_fn fn; void *ctx; } g_sock[SOCKS];
static int g_store_out;   // the host's load is not answered yet
static void quiet(const char *what) { if (g_store_out) broke(what); }

uint32_t cd_ws_open_cb(cd_ws_fn fn, void *ctx, const char *url, uint32_t len) {
    quiet("a socket opened while the session's load is out");
    static const char ORIGIN[] = "ws://app.test";
    char path[64];
    uint32_t o = sizeof ORIGIN - 1;
    if (len < o || memcmp(url, ORIGIN, o) != 0 || len - o >= sizeof path) { broke("a socket URL not on the page's host"); o = len = 0; }
    memcpy(path, url + o, len - o);
    path[len - o] = 0;
    if (!g_w) return 1;
    uint32_t id = cbhs_ws_open(g_w, path);
    if (id >= SOCKS) { broke("more sockets than the adapter has room for"); return 0; }
    g_sock[id].fn = fn;
    g_sock[id].ctx = ctx;
    return id;
}
void cd_ws_send(uint32_t id, const void *p, uint32_t len) { cbhs_ws_send(g_w, id, p, len); }
void cd_ws_close(uint32_t id) { cbhs_ws_close(g_w, id); }

// ---- HTTP ----
#define REQS 64
static struct { cd_http_fn fn; void *ctx; } g_req[REQS];
static uint32_t g_n_req;
static void http_done(void *ctx, int status, const uint8_t *body, size_t len) {
    uint32_t i = (uint32_t)(uintptr_t)ctx;
    g_req[i].fn(g_req[i].ctx, status == 0 ? CD_HTTP_FAILED : (uint32_t)status, body, (uint32_t)len);
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
    if (deadline_ms != g_host.c.cfg.call_wait_ms) broke("a request without the call wait as its deadline");
    char m[8] = {0};
    memcpy(m, method, mlen < 7 ? mlen : 7);
    uint32_t al = 0, cl = 0;
    const char *auth = header(headers, hlen, "Authorization: ", &al);
    const char *ct = header(headers, hlen, "Content-Type: ", &cl);
    int typed = ct && cl == 24 && memcmp(ct, "application/octet-stream", 24) == 0;
    if (blen ? !typed : ct != NULL) broke("a body without its octet-stream type, or a type without a body");
    if (g_n_req == REQS) { broke("more requests than the adapter has room for"); return; }
    uint32_t i = g_n_req++;
    g_req[i].fn = fn;
    g_req[i].ctx = ctx;
    if (!g_w) return;
    cbhs_http(g_w, m, url, ulen, auth, al, body, blen, http_done, (void *)(uintptr_t)i);
}

// ---- timers: cdom's tokens are the world's ids ----
static uint32_t g_solo_timer;
uint32_t cd_timer_cb(uint32_t ms, cd_timer_fn fn, void *ctx) { return g_w ? cbhs_timer(g_w, ms, fn, ctx) : ++g_solo_timer; }
void cd_timer_stop(uint32_t token) { if (g_w) cbhs_timer_stop(g_w, token); }

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
static struct { int get; cd_store_fn got; cd_store_done_fn done; void *ctx; } g_ans[ANSWERS];
static int g_n_ans;
static void later(int get, cd_store_fn got, cd_store_done_fn done, void *ctx) {
    if (g_n_ans == ANSWERS) { broke("more store answers out than the adapter has room for"); return; }
    g_ans[g_n_ans].get = get; g_ans[g_n_ans].got = got; g_ans[g_n_ans].done = done; g_ans[g_n_ans].ctx = ctx;
    g_n_ans++;
}
// Delivers the answers the host is owed, in order: the loop's turn before the world's next delivery.
static void drain(void) {
    for (int i = 0; i < g_n_ans; i++) {
        if (!g_ans[i].get) { g_ans[i].done(g_ans[i].ctx, 1); continue; }
        static uint8_t raw[CBC_CDOM_SESS_CAP];
        static char text[CBC_CDOM_TEXT_CAP + 1];
        size_t n = g_w ? cbhs_load(g_w, raw, sizeof raw) : 0, t = enc(raw, n, text);
        g_store_out = 0;
        g_ans[i].got(g_ans[i].ctx, (const uint8_t *)text, n ? (int32_t)t : -1);
    }
    g_n_ans = 0;
}
static int key_ok(const char *key, uint32_t klen) { return klen == 11 && memcmp(key, "app.session", 11) == 0; }
void cd_store_get_cb(cd_store_fn fn, void *ctx, const char *key, uint32_t klen, uint32_t deadline_ms) {
    if (!key_ok(key, klen) || deadline_ms != CD_STORE_DEADLINE_MS) broke("a get of another key, or with another deadline");
    g_store_out = 1;
    later(1, fn, NULL, ctx);
}
void cd_store_put_cb(cd_store_done_fn fn, void *ctx, const char *key, uint32_t klen, const void *p, uint32_t len) {
    if (!key_ok(key, klen)) broke("a put of another key");
    static uint8_t raw[CBC_CDOM_TEXT_CAP];
    long n = len <= CBC_CDOM_TEXT_CAP ? dec(p, len, raw) : -1;
    if (n <= 0) broke("a put that is not the session's base64");
    else cbhs_save(g_w, raw, (size_t)n);
    later(0, NULL, fn, ctx);
}
void cd_store_del_cb(cd_store_done_fn fn, void *ctx, const char *key, uint32_t klen) {
    if (!key_ok(key, klen)) broke("a delete of another key");
    cbhs_save(g_w, NULL, 0);
    later(0, NULL, fn, ctx);
}

// ---- the host, as the suite drives it ----
static void on_event(void *ctx, cbc *c, const cbc_event *e, const uint8_t *bytes) { cbhs_event(ctx, c, e, bytes); }
static int g_ready;
static void on_ready(void *ctx, cbc *c) { (void)ctx; (void)c; g_ready++; }

static void *start(cbhs_world *w, const cbc_cfg *cfg) {
    g_w = w;
    g_n_req = 0;
    g_n_ans = 0;
    g_store_out = 0;
    g_ready = 0;
    cbc_cdom_start(&g_host, cfg, "app.session", on_event, on_ready, w);
    if (cfg->anonymous ? g_n_ans != 0 : g_ready != 0) broke("an anonymous client asked the store, or a keyed one was ready before its load");
    drain();
    if (g_ready != 1) broke("ready was not called once by the end of the start");
    return &g_host;
}
static cbc *client(void *h) { return cbc_cdom_client(h); }
static void pump(void *h) { drain(); cbc_cdom_pump(h); }
static void ws_opened(void *h, uint32_t id) { (void)h; drain(); g_sock[id].fn(g_sock[id].ctx, id, CD_WS_OPEN, NULL, 0); }
static void ws_message(void *h, uint32_t id, const uint8_t *p, size_t n) {
    (void)h;
    drain();
    g_sock[id].fn(g_sock[id].ctx, id, CD_WS_MSG, p, (uint32_t)n);
}
static void ws_closed(void *h, uint32_t id) { (void)h; drain(); g_sock[id].fn(g_sock[id].ctx, id, CD_WS_CLOSE, NULL, 1000); }

static const cbhs_host CDOM_HOST = { "cdom (hosts/cdom/cbc_cdom.c)", start, client, pump, ws_opened, ws_message, ws_closed };

// cdom's own case, which the shared suite cannot make: its store answers later, so a sign-up the app
// makes before ready (between cbc_cdom_start and the load's answer) waits in the client, and goes out
// once the load has answered a miss.
static int early_call_waits_for_the_load(void) {
    g_w = NULL;
    g_n_req = 0;
    g_n_ans = 0;
    int before = g_broken;
    cbc_cdom_start(&g_host, NULL, "app.session", NULL, on_ready, NULL);
    uint32_t t = cbc_sign_in(cbc_cdom_client(&g_host), CBW_SIGNUP, "ANN", 3, "secret!!", 8, cbc_cdom_now());
    cbc_cdom_pump(&g_host);   // quiet(): a request now is a broken rule
    uint32_t early = g_n_req;
    drain();   // the miss
    int ok = t != 0 && early == 0 && g_n_req == 1 && g_broken == before;
    printf("%s  cdom: a sign-up made before the session's load answers waits for it, then goes out\n", ok ? "ok  " : "FAIL");
    return ok ? 0 : 1;
}

int main(void) {
    int rc = cbhs_run(&CDOM_HOST);
    rc |= early_call_waits_for_the_load();
    if (g_broken) fprintf(stderr, "cdom adapter: %d rule(s) broken\n", g_broken);
    return rc || g_broken ? 1 : 0;
}
