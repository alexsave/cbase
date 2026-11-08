// cbc_cdom.c - the C SDK's browser host on cdom (cbc_cdom.h says the contract it implements).
#include "cbc_cdom.h"

#include <string.h>

#include "b64/b64.h"
#include "cdom.h"
#include "cdom_net.h"
#include "cdom_random.h"
#include "cdom_store.h"

int64_t cbc_cdom_now(void) { return (int64_t)cd_epoch_ms(); }

cbc *cbc_cdom_client(cbc_cdom *h) { return &h->c; }

// ---- the session: base64 in cdom's store ----
static void on_stored(void *ctx, uint32_t ok) {
    (void)ctx;
    (void)ok;   // a put or a delete that failed (a quota, a closed database): the session lasts this page
}

static void session_save(cbc_cdom *h) {
    if (!h->key) return;
    int n = cbc_session_save(&h->c, h->sess, sizeof h->sess);
    int t = n > 0 ? b64_encode(h->sess, (size_t)n, h->text, sizeof h->text) : -1;
    if (t > 0) cd_store_put_cb(on_stored, h, h->key, h->klen, h->text, (uint32_t)t);
    else cd_store_del_cb(on_stored, h, h->key, h->klen);   // signed out (0): forget it
}

static void on_loaded(void *ctx, const uint8_t *p, int32_t len) {
    cbc_cdom *h = ctx;
    // A miss, an error or the deadline is len -1; what does not decode is no session either.
    int n = len > 0 ? b64_decode((const char *)p, (size_t)len, h->sess, sizeof h->sess) : -1;
    if (n > 0) cbc_session_load(&h->c, h->sess, (size_t)n);
    h->loading = 0;
    if (h->ready) h->ready(h->ctx, &h->c);
    cbc_cdom_pump(h);
}

// ---- events ----
static void events(cbc_cdom *h, const uint8_t *bytes) {
    int n = cbc_n_events(&h->c), saved = 0;
    for (int i = 0; i < n; i++) {
        const cbc_event *e = cbc_event_at(&h->c, i);
        if (e->kind == CBC_EV_SESSION && !saved) { session_save(h); saved = 1; }
        if (h->fn) h->fn(h->ctx, &h->c, e, bytes);
    }
}

// ---- HTTP ----
static void on_http(void *ctx, uint32_t status, const uint8_t *p, uint32_t len) {
    cbc_cdom_pend *q = ctx;
    cbc_cdom *h = q->h;
    q->used = 0;
    if (status == CD_HTTP_FAILED || status == CD_HTTP_TIMEOUT) status = 0;
    cbc_on_response(&h->c, q->id, (int)status, p, len, cbc_cdom_now());
    events(h, p);
    cbc_cdom_pump(h);
}

static void send_request(cbc_cdom *h, int n) {
    cbc_req_hdr r;
    memcpy(&r, h->req, sizeof r);
    cbc_cdom_pend *q = 0;
    for (int i = 0; i < CBC_CDOM_PEND_MAX && !q; i++)
        if (!h->pend[i].used) q = &h->pend[i];
    if (!q) {   // more out than the client can have: it never arrived
        cbc_on_response(&h->c, r.id, 0, 0, 0, cbc_cdom_now());
        events(h, 0);
        return;
    }
    q->h = h;
    q->id = r.id;
    q->used = 1;
    const char *path = (const char *)h->req + sizeof r;
    const char *auth = path + r.path_len;
    const uint8_t *body = (const uint8_t *)auth + r.auth_len;
    // "Authorization: <auth>\nContent-Type: application/octet-stream\n", written behind the request
    // (cbc_next_request left CBC_CDOM_HDR_CAP for it, more than "Authorization: " and a token take)
    static const char AU[] = "Authorization: ", CT[] = "Content-Type: application/octet-stream\n";
    char *hd = (char *)h->req + n;
    uint32_t hl = 0;
    if (r.auth_len) {
        memcpy(hd, AU, sizeof AU - 1);
        hl = sizeof AU - 1;
        memcpy(hd + hl, auth, r.auth_len);
        hl += r.auth_len;
        hd[hl++] = '\n';
    }
    if (r.body_len) { memcpy(hd + hl, CT, sizeof CT - 1); hl += sizeof CT - 1; }
    int post = r.method == CBC_POST;
    cd_http_cb(on_http, q, post ? "POST" : "GET", post ? 4 : 3, path, r.path_len, hd, hl, body, r.body_len,
               h->c.cfg.call_wait_ms);
}

// ---- the socket and the timers ----
static void on_reopen(void *ctx);

static void on_ws(void *ctx, uint32_t id, uint32_t ev, const uint8_t *p, uint32_t len) {
    cbc_cdom *h = ctx;
    if (id + 1 != h->sock) return;   // not the client's socket (cannot happen: one socket at a time)
    if (ev == CD_WS_OPEN) {
        h->open = 1;
        cbc_opened(&h->c, cbc_cdom_now());
    } else if (ev == CD_WS_MSG) {
        if (!h->open) return;
        cbc_in(&h->c, p, len, cbc_cdom_now());
        events(h, p);
    } else {
        h->sock = 0;
        h->open = h->closing = 0;
        uint32_t wait = cbc_closed(&h->c, cbc_cdom_now());
        events(h, 0);
        if (h->reopen_timer) cd_timer_stop(h->reopen_timer);
        h->reopen_timer = cd_timer_cb(wait, on_reopen, h);
    }
    cbc_cdom_pump(h);
}

static void on_reopen(void *ctx) {
    cbc_cdom *h = ctx;
    h->reopen_timer = 0;
    cbc_cdom_pump(h);
}

static void on_tick(void *ctx) {
    cbc_cdom *h = ctx;
    h->tick_timer = 0;
    cbc_tick(&h->c, cbc_cdom_now());
    events(h, 0);
    cbc_cdom_pump(h);
}

// ws:// or wss:// as the page is http: or https:, the page's host, the client's path.
static void ws_open(cbc_cdom *h) {
    char url[16 + 256 + CBC_PATH_MAX];
    char proto[8];
    uint32_t pl = cd_get_str(CD_WINDOW, CD_LIT("location.protocol"), proto, sizeof proto);
    const char *s = pl == 6 && memcmp(proto, "https:", 6) == 0 ? "wss://" : "ws://";
    uint32_t n = (uint32_t)strlen(s);
    memcpy(url, s, n);
    uint32_t hn = cd_get_str(CD_WINDOW, CD_LIT("location.host"), url + n, 256);
    n += hn < 256 ? hn : 256;
    const char *p = cbc_stream_path(&h->c);
    uint32_t k = (uint32_t)strlen(p);
    memcpy(url + n, p, k);
    h->open = h->closing = 0;
    h->sock = cd_ws_open_cb(on_ws, h, url, n + k) + 1;
}

void cbc_cdom_pump(cbc_cdom *h) {
    if (h->loading) return;   // the session's load answers first, and pumps
    int64_t now = cbc_cdom_now();
    for (int n; (n = cbc_next_request(&h->c, h->req, sizeof h->req - CBC_CDOM_HDR_CAP, now)) > 0;) send_request(h, n);
    if (h->sock && h->open && !h->closing) {
        for (int n; (n = cbc_next_out(&h->c, h->frame, sizeof h->frame, now)) != 0;) {
            if (n < 0) {   // -1: close it now (CBC_E_CAP cannot happen at CBC_CDOM_FRAME_CAP)
                cd_ws_close(h->sock - 1);
                h->closing = 1;
                break;
            }
            cd_ws_send(h->sock - 1, h->frame, (uint32_t)n);
        }
    }
    if (!h->sock && !h->reopen_timer && cbc_stream_wanted(&h->c)) ws_open(h);
    if (h->sock && !h->closing && !cbc_stream_wanted(&h->c)) {
        cd_ws_close(h->sock - 1);
        h->closing = 1;
    }
    if (h->tick_timer) { cd_timer_stop(h->tick_timer); h->tick_timer = 0; }
    // While the socket is closing the client still has it, and answers 0 for what it wants done with it
    // (the close, the next one's HELLO): after this pump nothing else can be due at once, so a 0 then
    // waits for the close's CD_WS_CLOSE, which pumps, instead of spinning the page's loop.
    int due = cbc_due_ms(&h->c, now);
    if (due > 0 || (due == 0 && !h->closing)) h->tick_timer = cd_timer_cb((uint32_t)due, on_tick, h);
}

void cbc_cdom_start(cbc_cdom *h, const cbc_cfg *cfg, const char *session_key, cbc_cdom_event_fn fn,
                    cbc_cdom_ready_fn ready, void *ctx) {
    memset(h, 0, sizeof *h);
    cbc_cfg k;
    if (cfg) k = *cfg;
    else cbc_cfg_default(&k);
    cd_random_bytes(&k.seed, sizeof k.seed);
    cbc_init(&h->c, &k);
    h->fn = fn;
    h->ready = ready;
    h->ctx = ctx;
    if (session_key && !k.anonymous) {
        h->key = session_key;
        h->klen = (uint32_t)strlen(session_key);
        h->loading = 1;
        cd_store_get_cb(on_loaded, h, h->key, h->klen, CD_STORE_DEADLINE_MS);
        return;
    }
    if (ready) ready(ctx, &h->c);
    cbc_cdom_pump(h);
}
