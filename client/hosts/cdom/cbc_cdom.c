// cbc_cdom.c - the cdom adapter (cbc_cdom.h says the contract it keeps): the shared host
// (client/hosts/common/cbc_host.c) on cdom's socket, fetch, timers, clock, store and entropy.
#include "cbc_cdom.h"

#include <string.h>

#include "b64/b64.h"
#include "cdom.h"
#include "cdom_net.h"
#include "cdom_random.h"
#include "cdom_store.h"

int64_t cbc_cdom_now(void) { return (int64_t)cd_epoch_ms(); }
cbc *cbc_cdom_client(cbc_cdom *h) { return cbc_host_client(&h->host); }
void cbc_cdom_pump(cbc_cdom *h) { cbc_host_pump(&h->host); }

// ---- what cdom owes: a slot for every answer, stamped with the start it was asked under ----------
static void loaded(cbc_cdom *h, const uint8_t *p, int32_t len);

// A late answer (the slot table full: only while an older start's answers are still out), given
// from a 0 ms timer: a request's status 0, the load's miss.
static void on_late(void *ctx) {
    cbc_cdom *h = ctx;
    void *late[CBC_HOST_PEND_MAX + 1];
    uint32_t n = h->n_late, g = h->gen;
    memcpy(late, h->late, n * sizeof late[0]);
    h->n_late = 0;
    h->late_timer = 0;
    // Each is this start's only while it lasts: a stop inside an answer ends the rest, and so would a
    // new start (which traps inside a callback, so the gen half is held by that trap too).
    for (uint32_t i = 0; i < n && h->live && h->gen == g; i++) {
        if (late[i]) cbc_host_http_answered(late[i], 0, NULL, 0);
        else loaded(h, NULL, -1);
    }
}

// A free slot for an answer of this start (tag: the shared host's, NULL for the load), or NULL when
// the table is full, and then the answer is owed from on_late.
static cbc_cdom_owed *owe(cbc_cdom *h, void *tag) {
    for (int i = 0; i < CBC_CDOM_OWED_MAX; i++) {
        cbc_cdom_owed *o = &h->owed[i];
        if (o->used) continue;
        o->h = h;
        o->tag = tag;
        o->gen = h->gen;
        o->used = 1;
        return o;
    }
    // At most every request of this start and its load are out (CBC_HOST_PEND_MAX + 1), so it fits.
    h->late[h->n_late++] = tag;
    if (!h->late_timer) h->late_timer = cd_timer_cb(0, on_late, h);
    return NULL;
}

// The slot's answer has come: freed, and 1 when it is this start's to hand on.
static int paid(cbc_cdom_owed *o) {
    o->used = 0;
    return o->h->live && o->gen == o->h->gen;
}

// The app's ready, counted as its event callback is (cbc_host_in_app): a start from inside either traps.
static void tell_ready(cbc_cdom *h) {
    if (!h->ready) return;
    h->in_ready++;
    h->ready(h->ctx, cbc_cdom_client(h));
    h->in_ready--;
}

// ---- the session: base64 in cdom's store -------------------------------------------------------
static void on_stored(void *ctx, uint32_t ok) {
    (void)ctx;
    (void)ok;   // a put or a delete that failed (a quota, a closed database): the session lasts this page
}

static void loaded(cbc_cdom *h, const uint8_t *p, int32_t len) {
    // A miss, an error or the deadline is len -1; what does not decode is no session either.
    int n = len > 0 ? b64_decode((const char *)p, (size_t)len, h->host.sess, sizeof h->host.sess) : -1;
    if (n > 0) cbc_session_load(cbc_cdom_client(h), h->host.sess, (size_t)n);
    cbc_host_hold(&h->host, 0);
    tell_ready(h);
    cbc_host_pump(&h->host);
}

static void on_loaded(void *ctx, const uint8_t *p, int32_t len) {
    cbc_cdom_owed *o = ctx;
    if (paid(o)) loaded(o->h, p, len);
}

// ---- the environment: each call one cdom call --------------------------------------------------
static void on_ws(void *ctx, uint32_t id, uint32_t ev, const uint8_t *p, uint32_t len) {
    // A socket a stop closed (its last events) is no longer the shared host's: it ignores them. cdom
    // hands its id out again only after its close is delivered, so no new start's socket shares it.
    // A socket the browser refused to make (its constructor threw) is CD_WS_REFUSED, from the loop, and
    // its close follows, which the shared host ignores after the failure.
    cbc_cdom *h = ctx;
    if (ev == CD_WS_OPEN) cbc_host_ws_opened(&h->host, id);
    else if (ev == CD_WS_MSG) cbc_host_ws_message(&h->host, id, p, len);
    else if (ev == CD_WS_REFUSED) cbc_host_ws_failed(&h->host, id, CBC_SOCK_REFUSED);
    else cbc_host_ws_closed(&h->host, id);   // a close before the open is the shared host's failure
}

// ws:// or wss:// as the page is http: or https:, the page's host, the client's path: the page's origin
// (cd_page_origin, "http://<host>" or "https://<host>") with its "http" made "ws".
#define ORIGIN_MAX (sizeof "https://" - 1 + 256)   // no host name is longer than 256 bytes
static int env_ws_open(void *ud, const char *path, uint32_t *id) {
    cbc_cdom *h = ud;
    char origin[ORIGIN_MAX], url[ORIGIN_MAX + CBC_PATH_MAX];
    uint32_t on = cd_page_origin(origin, sizeof origin);   // its whole length, even past the room
    uint32_t sl = on > 8 && memcmp(origin, "https://", 8) == 0 ? 8 : on > 7 && memcmp(origin, "http://", 7) == 0 ? 7 : 0;
    if (!sl) return CBC_SOCK_UNSUPPORTED;   // file:, blob:, an extension's page: no server to reach
    if (on - sl > 256) return CBC_SOCK_UNSUPPORTED;   // no host name is that long
    memcpy(url, "ws", 2);
    memcpy(url + 2, origin + 4, on - 4);   // "s://<host>" or "://<host>"
    uint32_t n = 2 + on - 4;
    uint32_t k = (uint32_t)strlen(path);
    memcpy(url + n, path, k);
    *id = cd_ws_open_cb(on_ws, h, url, n + k);
    return 0;
}
static void env_ws_send(void *ud, uint32_t id, const uint8_t *frame, size_t len) {
    (void)ud;
    cd_ws_send(id, frame, (uint32_t)len);
}
static void env_ws_close(void *ud, uint32_t id) {
    (void)ud;
    cd_ws_close(id);
}

static void on_http(void *ctx, uint32_t status, const uint8_t *p, uint32_t len) {
    cbc_cdom_owed *o = ctx;
    if (!paid(o)) return;   // an older start's request
    if (status == CD_HTTP_FAILED || status == CD_HTTP_TIMEOUT) status = 0;
    cbc_host_http_answered(o->tag, (int)status, p, len);
}

static void env_http(void *ud, const cbc_host_req *r, void *tag) {
    cbc_cdom *h = ud;
    static const char AU[] = "Authorization: ", CT[] = "Content-Type: application/octet-stream\n";
    uint32_t hl = 0;
    if (r->auth) {   // "Bearer <access>": within CBC_CDOM_HDR_CAP with the type line
        memcpy(h->hdr, AU, sizeof AU - 1);
        hl = sizeof AU - 1;
        memcpy(h->hdr + hl, r->auth, r->auth_len);
        hl += (uint32_t)r->auth_len;
        h->hdr[hl++] = '\n';
    }
    if (r->body_len) { memcpy(h->hdr + hl, CT, sizeof CT - 1); hl += sizeof CT - 1; }
    cbc_cdom_owed *o = owe(h, tag);
    if (!o) return;   // answered status 0 from on_late
    // The body is read during the call: a borrowed one (cbc_call_ref) from the caller's buffer.
    cd_http_cb(on_http, o, r->method, (uint32_t)strlen(r->method), r->path, (uint32_t)r->path_len, h->hdr, hl, r->body,
               (uint32_t)r->body_len, r->wait_ms);
}

static uint32_t env_timer(void *ud, uint32_t ms, cbc_host_fire_fn fire, void *ctx) {
    (void)ud;
    return cd_timer_cb(ms, fire, ctx);
}
static void env_timer_stop(void *ud, uint32_t id) {
    (void)ud;
    cd_timer_stop(id);
}

static void env_save(void *ud, const uint8_t *p, size_t len) {
    cbc_cdom *h = ud;
    if (!h->klen) return;
    int t = len ? b64_encode(p, len, h->text, sizeof h->text) : -1;
    if (t > 0) cd_store_put_cb(on_stored, NULL, h->key, h->klen, h->text, (uint32_t)t);
    else cd_store_del_cb(on_stored, NULL, h->key, h->klen);   // signed out (0): forget it
}

static int64_t env_now(void *ud) {
    (void)ud;
    return cbc_cdom_now();
}

static const cbc_host_env ENV = { env_ws_open, env_ws_send, env_ws_close, env_http, env_timer, env_timer_stop, env_save, env_now };

// ---- start and stop ----------------------------------------------------------------------------
void cbc_cdom_stop(cbc_cdom *h) {
    if (!h->live) return;
    h->live = 0;
    cbc_host_stop(&h->host);   // its timers and socket; nothing more sent or told the app
    if (h->late_timer) cd_timer_stop(h->late_timer);
    h->late_timer = 0;
    h->n_late = 0;
}

int cbc_cdom_start(cbc_cdom *h, const cbc_cfg *cfg, const char *session_key, cbc_cdom_event_fn fn,
                   cbc_cdom_ready_fn ready, void *ctx) {
    if (h->live) __builtin_trap();   // a second start would orphan the first one's socket and timers
    // From the app's callback, the call it came from still runs on the host: from the loop only.
    if (h->in_ready || cbc_host_in_app(&h->host)) __builtin_trap();
    cbc_cfg k;
    if (cfg) k = *cfg;
    else cbc_cfg_default(&k);
    size_t kl = session_key && !k.anonymous ? strlen(session_key) : 0;
    if (kl > CBC_CDOM_KEY_MAX) return -1;
    if (cd_random_bytes(&k.seed, sizeof k.seed) != CD_RANDOM_OK) return -1;
    h->gen++;   // every answer cdom still owes an older start is dropped from here on
    h->live = 1;
    h->ready = ready;
    h->ctx = ctx;
    h->klen = (uint32_t)kl;
    memcpy(h->key, session_key ? session_key : "", kl);
    cbc_host_init(&h->host, &ENV, h, &k, fn, ctx);
    if (kl) {
        cbc_host_hold(&h->host, 1);   // until the store answers: on_loaded pumps
        cbc_cdom_owed *o = owe(h, NULL);
        if (o) cd_store_get_cb(on_loaded, o, h->key, h->klen, CD_STORE_DEADLINE_MS);
        return 0;
    }
    tell_ready(h);
    cbc_host_pump(&h->host);
    return 0;
}
