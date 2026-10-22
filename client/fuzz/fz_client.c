// fz_client.c - the c_base client (cbase/client.h) under any sequence a host and a server can
// produce: a step program read from the input drives one client through sessions, calls, rooms,
// socket opens and closes, frames and answers of any bytes, and time. Every step checks what the
// client hands out:
//   - every frame cbc_next_out writes parses (stream_frame.h) and is a client's: HELLO, SUB,
//     UNSUB, REQ or PING, and a HELLO comes only on a socket that was just opened;
//   - every request cbc_next_request writes is exactly its head's lengths, with a path that starts
//     with '/', and a body under the arena;
//   - every event's span lies inside the bytes passed to the call that raised it, and no call
//     raises more than CBC_EVENTS_MAX;
//   - a reconnect wait lies in [retry_first_ms, retry_max_ms];
//   - every ticket is finished once at most.
#include "fz.h"

#include "cbase/client.h"
#include "stream_frame.h"

static uint8_t take8(fz_in *in) { return in->at < in->n ? in->p[in->at++] : 0; }
static uint16_t take16(fz_in *in) { return (uint16_t)(take8(in) | take8(in) << 8); }
static const uint8_t *take_bytes(fz_in *in, size_t *n, size_t max) {
    size_t want = take16(in) % (max + 1);
    if (want > in->n - in->at) want = in->n - in->at;
    const uint8_t *p = in->p + in->at;
    in->at += want;
    *n = want;
    return p;
}

#define TICKETS 4096
static uint8_t g_done[TICKETS];

static void check_events(const cbc *c, size_t passed) {
    int n = cbc_n_events(c);
    FZ_CHECK(n >= 0 && n <= CBC_EVENTS_MAX);
    for (int i = 0; i < n; i++) {
        const cbc_event *e = cbc_event_at(c, i);
        FZ_CHECK(e->kind >= CBC_EV_READY && e->kind <= CBC_EV_BAD);
        if (e->kind == CBC_EV_PUSH || (e->kind == CBC_EV_DONE && e->len)) FZ_CHECK((size_t)e->off + e->len <= passed);
        FZ_CHECK(e->room_len <= CBC_ROOM_MAX);
        if (e->kind == CBC_EV_DONE) {
            FZ_CHECK(e->ticket != 0);
            if (e->ticket < TICKETS) { FZ_CHECK(!g_done[e->ticket]); g_done[e->ticket] = 1; }
        }
    }
}

static uint8_t g_out[CBW_ACCESS_MAX + CBC_ARENA + 1024];

static void drain(cbc *c, int64_t now, int *hello_ok) {
    for (int guard = 0; guard < 64; guard++) {
        int n = cbc_next_out(c, g_out, sizeof g_out, now);
        if (n <= 0) break;
        sf_frame f;
        FZ_CHECK(sf_parse(g_out, (size_t)n, &f) == 0);
        FZ_CHECK(f.kind == SF_HELLO || f.kind == SF_SUB || f.kind == SF_UNSUB || f.kind == SF_REQ || f.kind == SF_PING);
        if (f.kind == SF_HELLO) { FZ_CHECK(*hello_ok); *hello_ok = 0; }
    }
    for (int guard = 0; guard < 64; guard++) {
        int n = cbc_next_request(c, g_out, sizeof g_out, now);
        if (n <= 0) break;
        cbc_req_hdr h;
        memcpy(&h, g_out, sizeof h);
        FZ_CHECK((size_t)n == sizeof h + h.path_len + h.auth_len + h.body_len);
        FZ_CHECK(h.path_len > 0 && g_out[sizeof h] == '/');
        FZ_CHECK(h.method == CBC_GET || h.method == CBC_POST);
        FZ_CHECK(h.body_len <= CBC_ARENA);
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    fz_in in = { data, size, 0 };
    memset(g_done, 0, sizeof g_done);
    static cbc c;
    cbc_cfg cfg;
    cbc_cfg_default(&cfg);
    cfg.anonymous = take8(&in) & 1;
    cfg.seed = take16(&in);
    if (take8(&in) & 1) { cfg.retry_first_ms = take16(&in); cfg.retry_max_ms = take16(&in); }
    cbc_init(&c, &cfg);
    int64_t now = 1000000;
    int hello_ok = 0;
    uint32_t last_req = 0;
    for (int step = 0; step < 256 && in.at < in.n; step++) {
        size_t n;
        const uint8_t *p;
        switch (take8(&in) % 14) {
        case 0: now += take16(&in); break;
        case 1: p = take_bytes(&in, &n, 2200); cbc_session_load(&c, p, n); break;
        case 2: { p = take_bytes(&in, &n, 300); size_t k = n / 2;
                  cbc_call(&c, 1 + (take8(&in) & 1), take8(&in) & 7, (const char *)p, k, p + k, n - k, now); break; }
        case 3: { char room[2] = { (char)('a' + (take8(&in) & 7)), 0 }; cbc_sub(&c, take8(&in) & 3, room, 1); break; }
        case 4: { char room[2] = { (char)('a' + (take8(&in) & 7)), 0 }; cbc_unsub(&c, take8(&in) & 3, room, 1); break; }
        case 5: cbc_opened(&c, now); hello_ok = 1; break;
        case 6: { uint32_t w = cbc_closed(&c, now); hello_ok = 0; check_events(&c, 0);
                  FZ_CHECK(w == 0 || (w >= c.cfg.retry_first_ms && w <= c.cfg.retry_max_ms)); break; }
        case 7: p = take_bytes(&in, &n, 600); cbc_in(&c, p, n, now); check_events(&c, n); break;
        case 8: { // a server frame made to fit: READY, a REPLY to a recent id, a push
                  uint8_t f[64]; size_t fn = 2; f[0] = SF_V;
                  switch (take8(&in) % 3) {
                  case 0: f[1] = SF_READY; break;
                  case 1: fn = (size_t)sf_put_reply_head(f, sizeof f, (take8(&in) & 1 ? 0x80000000u : 0) | (take8(&in) & 15),
                                                         (uint16_t)(take8(&in) & 1 ? 200 : 403 + take8(&in))); break;
                  default: f[1] = take8(&in) % SF_HELLO; break;
                  }
                  cbc_in(&c, f, fn, now); check_events(&c, fn); break; }
        case 9: { int n2 = cbc_next_request(&c, g_out, sizeof g_out, now);
                  if (n2 > 0) { cbc_req_hdr h; memcpy(&h, g_out, sizeof h); last_req = h.id; } break; }
        case 10: { p = take_bytes(&in, &n, 300); int st = take8(&in) % 4;
                   int status = st == 0 ? 0 : st == 1 ? 200 : st == 2 ? 401 : 503;
                   cbc_on_response(&c, take8(&in) & 1 ? last_req : take16(&in), status, take8(&in) & 1 ? NULL : p, n, now);
                   check_events(&c, n); break; }
        case 11: cbc_tick(&c, now); check_events(&c, 0); break;
        case 12: { p = take_bytes(&in, &n, 80); cbc_sign_in(&c, 1 + (take8(&in) & 1), (const char *)p, n / 2, (const char *)p + n / 2, n - n / 2, now); break; }
        default: cbc_sign_out(&c, take8(&in) & 1, now); break;
        }
        int due = cbc_due_ms(&c, now);
        FZ_CHECK(due >= -1);
        drain(&c, now, &hello_ok);
        uint8_t blob[CBW_ACCESS_MAX + 256];
        int bn = cbc_session_save(&c, blob, sizeof blob);
        if (bn > 0) { static cbc d; cbc_init(&d, &cfg); FZ_CHECK(cbc_session_load(&d, blob, (size_t)bn) == 0); }
    }
    return 0;
}
