// unit_client.c - the c_base client's behaviour (cbase/client.h), driven as a host drives it: bytes
// in, bytes out, time passed in. The server's side of every exchange is written with
// stream_frame.h's own writers and the auth wire's layout, so a frame here is a frame on the wire.
#include "t.h"

#include "cbase/client.h"
#include "stream_frame.h"
#include "le_bytes.h"

#include <string.h>

static uint8_t g_out[8192];

// ---- the host's side ---------------------------------------------------------------------------
static void mk(cbc *c, int anonymous, uint64_t seed) {
    cbc_cfg cfg;
    cbc_cfg_default(&cfg);
    cfg.anonymous = (uint8_t)anonymous;
    cfg.seed = seed;
    cbc_init(c, &cfg);
}

// A CBW_SESSION body.
static size_t session_body(uint8_t *o, const char *access, const char *refresh, int64_t exp, const char *id, const char *name) {
    size_t n = 0, a = strlen(access), r = strlen(refresh), i = strlen(id), m = strlen(name);
    o[n++] = CBW_V; o[n++] = CBW_SESSION;
    le_put_u16(o + n, (uint16_t)a); n += 2; memcpy(o + n, access, a); n += a;
    o[n++] = (uint8_t)r; memcpy(o + n, refresh, r); n += r;
    le_put_u48(o + n, (uint64_t)exp); n += 6;
    o[n++] = (uint8_t)i; memcpy(o + n, id, i); n += i;
    o[n++] = (uint8_t)m; memcpy(o + n, name, m); n += m;
    return n;
}

static void signed_in(cbc *c, const char *access, int64_t exp) {
    uint8_t b[256];
    size_t n = session_body(b, access, "R1", exp, "u-1", "ANN");
    T_EQ_INT(cbc_session_load(c, b, n), 0);
}

static size_t refusal(uint8_t *o, int why, int32_t code) {
    o[0] = CBW_REFUSAL_FORMAT; o[1] = (uint8_t)why;
    le_put_u32(o + 2, (uint32_t)code); le_put_u32(o + 6, 0);
    return CBW_REFUSAL_BYTES;
}

// The next frame the client sends, parsed: its kind, or 0 for none, -1 for "close it".
static sf_frame g_f;
static int out_kind(cbc *c, int64_t now) {
    int n = cbc_next_out(c, g_out, sizeof g_out, now);
    if (n <= 0) return n;
    T_EQ_INT(sf_parse(g_out, (size_t)n, &g_f), 0);
    return g_f.kind;
}

static int feed(cbc *c, const uint8_t *p, size_t n, int64_t now) { return cbc_in(c, p, n, now); }
static int ready(cbc *c, int64_t now) {
    uint8_t b[2];
    sf_put_bare(b, sizeof b, SF_READY);
    return feed(c, b, 2, now);
}
static int reply(cbc *c, uint32_t id, int status, const void *body, size_t bn, int64_t now) {
    uint8_t b[512];
    sf_put_reply_head(b, sizeof b, id, (uint16_t)status);
    if (bn) memcpy(b + 8, body, bn);
    return feed(c, b, 8 + bn, now);
}

// Open the socket and take it to READY (the HELLO checked by the caller when it cares).
static void up(cbc *c, int64_t now) {
    cbc_opened(c, now);
    T_EQ_INT(out_kind(c, now), SF_HELLO);
    T_EQ_INT(ready(c, now), 1);
}

// The next HTTP request: its head into *h and its path, auth and body as strings into the buffers.
static cbc_req_hdr g_h;
static char g_path[300], g_auth[2100];
static uint8_t g_body[600];
static int request(cbc *c, int64_t now) {
    int n = cbc_next_request(c, g_out, sizeof g_out, now);
    if (n <= 0) return n;
    memcpy(&g_h, g_out, sizeof g_h);
    size_t at = sizeof g_h;
    memcpy(g_path, g_out + at, g_h.path_len); g_path[g_h.path_len] = 0; at += g_h.path_len;
    memcpy(g_auth, g_out + at, g_h.auth_len); g_auth[g_h.auth_len] = 0; at += g_h.auth_len;
    memcpy(g_body, g_out + at, g_h.body_len);
    T_EQ_INT((long long)(at + g_h.body_len), n);
    return n;
}

static const cbc_event *ev(const cbc *c, int i) { return cbc_event_at(c, i); }

// ---- the anonymous socket ----------------------------------------------------------------------
static void anonymous_hello_then_sub(void) {
    cbc c; mk(&c, 1, 1);
    T_EQ_STR(cbc_stream_path(&c), "/ws/public");
    T_EQ_INT(cbc_stream_wanted(&c), 0);
    T_EQ_INT(cbc_sub(&c, SF_WHAT_ROOM, "feed", 4), 0);
    T_EQ_INT(cbc_stream_wanted(&c), 1);
    cbc_opened(&c, 1000);
    T_EQ_INT(out_kind(&c, 1000), SF_HELLO);
    T_EQ_INT(g_f.tok_len, 0);
    T_EQ_INT(out_kind(&c, 1000), 0);              // nothing before READY
    T_EQ_INT(ready(&c, 1001), 1);
    T_EQ_INT(ev(&c, 0)->kind, CBC_EV_READY);
    T_EQ_INT(out_kind(&c, 1001), SF_SUB);
    T_CHECK(g_f.id & 0x80000000u);
    T_EQ_INT(g_f.what, SF_WHAT_ROOM);
    T_EQ_MEM(g_f.gid, g_f.gid_len, "feed", 4);
    T_EQ_INT(out_kind(&c, 1001), 0);
    T_EQ_INT(reply(&c, g_f.id, 200, NULL, 0, 1002), 1);
    T_EQ_INT(ev(&c, 0)->kind, CBC_EV_SUB_LIVE);
    T_EQ_MEM(ev(&c, 0)->room, ev(&c, 0)->room_len, "feed", 4);
    // The last hold released: an anonymous socket has nothing left to hear, so it closes (the
    // close frees the room on the server; no UNSUB is needed).
    T_EQ_INT(cbc_unsub(&c, SF_WHAT_ROOM, "feed", 4), 0);
    T_EQ_INT(cbc_stream_wanted(&c), 0);
    T_EQ_INT(out_kind(&c, 1003), -1);
}

static void counted_holds(void) {
    cbc c; mk(&c, 1, 1);
    cbc_sub(&c, 2, "keep", 4);                     // holds the socket open throughout
    up(&c, 0);
    T_EQ_INT(out_kind(&c, 0), SF_SUB);
    reply(&c, g_f.id, 200, NULL, 0, 0);
    cbc_sub(&c, 2, "r", 1);
    cbc_sub(&c, 2, "r", 1);
    T_EQ_INT(out_kind(&c, 0), SF_SUB);
    T_EQ_INT(out_kind(&c, 0), 0);                  // one SUB for two holds
    reply(&c, g_f.id, 200, NULL, 0, 0);
    cbc_unsub(&c, 2, "r", 1);
    T_EQ_INT(out_kind(&c, 1), 0);                  // a hold is left
    cbc_unsub(&c, 2, "r", 1);
    T_EQ_INT(out_kind(&c, 1), SF_UNSUB);
    T_EQ_INT(cbc_unsub(&c, 2, "r", 1), CBC_E_ARG);
}

static void sub_refused_backs_off(void) {
    cbc c; mk(&c, 1, 1);
    cbc_sub(&c, 2, "r", 1);
    up(&c, 0);
    T_EQ_INT(out_kind(&c, 0), SF_SUB);
    T_EQ_INT(reply(&c, g_f.id, 403, NULL, 0, 0), 1);
    T_EQ_INT(ev(&c, 0)->kind, CBC_EV_SUB_REFUSED);
    T_EQ_INT(ev(&c, 0)->status, 403);
    T_EQ_INT(cbc_due_ms(&c, 0), 500);
    T_EQ_INT(out_kind(&c, 499), 0);
    T_EQ_INT(out_kind(&c, 500), SF_SUB);
    reply(&c, g_f.id, 403, NULL, 0, 500);
    T_EQ_INT(out_kind(&c, 1499), 0);               // the wait doubled
    T_EQ_INT(out_kind(&c, 1500), SF_SUB);
    reply(&c, g_f.id, 200, NULL, 0, 1500);
    T_EQ_INT(ev(&c, 0)->kind, CBC_EV_SUB_LIVE);
}

static void unanswered_sub_is_asked_again(void) {
    cbc c; mk(&c, 1, 1);
    cbc_sub(&c, 2, "r", 1);
    up(&c, 0);
    T_EQ_INT(out_kind(&c, 0), SF_SUB);
    uint32_t first = g_f.id;
    T_EQ_INT(out_kind(&c, 9999), 0);
    T_EQ_INT(out_kind(&c, 10000), SF_SUB);
    T_CHECK(g_f.id != first);
    T_EQ_INT(reply(&c, first, 200, NULL, 0, 10001), 0);   // the first one's answer is stale
    T_EQ_INT(reply(&c, g_f.id, 200, NULL, 0, 10001), 1);
}

// ---- liveness and reconnects -------------------------------------------------------------------
static void ping_then_half_open(void) {
    cbc c; mk(&c, 1, 1);
    cbc_sub(&c, 2, "r", 1);
    up(&c, 0);
    T_EQ_INT(out_kind(&c, 0), SF_SUB);
    reply(&c, g_f.id, 200, NULL, 0, 0);
    T_EQ_INT(out_kind(&c, 29999), 0);
    T_EQ_INT(cbc_due_ms(&c, 29999), 1);
    T_EQ_INT(out_kind(&c, 30000), SF_PING);
    T_EQ_INT(out_kind(&c, 39999), 0);
    T_EQ_INT(out_kind(&c, 40000), -1);             // no frame of any kind for pong_ms
    // Any frame answers it.
    cbc d; mk(&d, 1, 1);
    cbc_sub(&d, 2, "r", 1);
    up(&d, 0);
    out_kind(&d, 0);
    reply(&d, g_f.id, 200, NULL, 0, 0);
    T_EQ_INT(out_kind(&d, 30000), SF_PING);
    uint8_t push[3] = { SF_V, 7, 9 };
    feed(&d, push, 3, 35000);
    T_EQ_INT(out_kind(&d, 45000), 0);
}

static void reconnect_is_decorrelated_jitter(void) {
    cbc a; mk(&a, 1, 42);
    cbc b; mk(&b, 1, 42);
    cbc d; mk(&d, 1, 43);
    uint32_t prev = 500;
    int same = 1, differ = 0;
    for (int i = 0; i < 40; i++) {
        uint32_t wa = cbc_closed(&a, i), wb = cbc_closed(&b, i), wd = cbc_closed(&d, i);
        uint32_t hi = prev * 3 < 10000 ? prev * 3 : 10000;
        T_CHECK(wa >= 500 && wa <= hi);
        same &= wa == wb;
        differ |= wa != wd;
        prev = wa;
    }
    T_CHECK(same);     // a seed is a schedule
    T_CHECK(differ);   // and another seed another
    T_CHECK(prev > 1500);   // it grew
    // READY starts it over.
    cbc_sub(&a, 2, "r", 1);
    up(&a, 100);
    T_CHECK(cbc_closed(&a, 200) <= 1500);
}

static void hello_unanswered_is_closed(void) {
    cbc c; mk(&c, 1, 1);
    cbc_sub(&c, 2, "r", 1);
    cbc_opened(&c, 0);
    T_EQ_INT(out_kind(&c, 0), SF_HELLO);
    T_EQ_INT(out_kind(&c, 9999), 0);
    T_EQ_INT(out_kind(&c, 10000), -1);
}

// ---- the session and the socket ----------------------------------------------------------------
static void hello_carries_the_token(void) {
    cbc c; mk(&c, 0, 1);
    T_EQ_STR(cbc_stream_path(&c), "/ws");
    T_EQ_INT(cbc_stream_wanted(&c), 0);
    signed_in(&c, "TOKEN-A", 1000000);
    T_EQ_INT(cbc_stream_wanted(&c), 1);
    cbc_opened(&c, 0);
    T_EQ_INT(out_kind(&c, 0), SF_HELLO);
    T_EQ_MEM(g_f.tok, g_f.tok_len, "TOKEN-A", 7);
}

static void hello_waits_for_the_refresh(void) {
    cbc c; mk(&c, 0, 1);
    signed_in(&c, "OLD", 100000);
    cbc_opened(&c, 50000);                         // within refresh_early_ms of expiry
    T_EQ_INT(out_kind(&c, 50000), 0);
    T_CHECK(request(&c, 50000) > 0);
    T_EQ_STR(g_path, "/v1/auth/refresh");
    T_EQ_INT(g_h.auth_len, 0);
    T_EQ_INT(g_h.flags, CBC_RQ_BODY);
    T_EQ_INT(g_h.body_len, 5);
    T_EQ_MEM(g_body, 5, "\x01\x03\x02R1", 5);
    T_EQ_INT(request(&c, 50000), 0);               // one refresh at a time
    uint8_t b[256];
    size_t n = session_body(b, "NEW", "R2", 2000000, "u-1", "ANN");
    T_EQ_INT(cbc_on_response(&c, g_h.id, 200, b, n, 50001), 1);
    T_EQ_INT(ev(&c, 0)->kind, CBC_EV_SESSION);
    T_EQ_INT(out_kind(&c, 50001), SF_HELLO);
    T_EQ_MEM(g_f.tok, g_f.tok_len, "NEW", 3);
}

static void a_refresh_rolls_the_socket(void) {
    cbc c; mk(&c, 0, 1);
    signed_in(&c, "OLD", 100000);
    up(&c, 0);
    T_EQ_INT(out_kind(&c, 1000), 0);
    T_CHECK(request(&c, 40000) > 0);               // the refresh, early
    uint8_t b[256];
    size_t n = session_body(b, "NEW", "R2", 2000000, "u-1", "ANN");
    cbc_on_response(&c, g_h.id, 200, b, n, 40001);
    T_EQ_INT(out_kind(&c, 40001), -1);             // roll onto the new token
    T_EQ_INT(cbc_closed(&c, 40002), 0);            // at once
    cbc_opened(&c, 40002);
    T_EQ_INT(out_kind(&c, 40002), SF_HELLO);
    T_EQ_MEM(g_f.tok, g_f.tok_len, "NEW", 3);
}

static void a_roll_waits_for_socket_calls(void) {
    cbc c; mk(&c, 0, 1);
    signed_in(&c, "OLD", 100000);
    up(&c, 0);
    cbc_call(&c, CBC_GET, CBC_C_READ, "/x", 2, NULL, 0, 0);
    T_EQ_INT(out_kind(&c, 1), SF_REQ);
    uint32_t id = g_f.id;
    request(&c, 40000);
    uint8_t b[256];
    size_t n = session_body(b, "NEW", "R2", 2000000, "u-1", "ANN");
    cbc_on_response(&c, g_h.id, 200, b, n, 40001);
    T_CHECK(out_kind(&c, 40001) != -1);            // the call rides it still (a PING may go)
    reply(&c, id, 200, "ok", 2, 40002);
    T_EQ_INT(out_kind(&c, 40002), -1);
}

static void a_refused_hello_forces_one_refresh(void) {
    cbc c; mk(&c, 0, 1);
    signed_in(&c, "OLD", 100000000);
    cbc_opened(&c, 0);
    T_EQ_INT(out_kind(&c, 0), SF_HELLO);
    cbc_closed(&c, 10);                            // closed before READY
    T_CHECK(request(&c, 10) > 0);
    T_EQ_STR(g_path, "/v1/auth/refresh");
    uint8_t b[256];
    size_t n = session_body(b, "NEW", "R2", 100000000, "u-1", "ANN");
    cbc_on_response(&c, g_h.id, 200, b, n, 11);
    cbc_opened(&c, 20);
    T_EQ_INT(out_kind(&c, 20), SF_HELLO);
    cbc_closed(&c, 30);                            // refused again, within force_refresh_ms
    T_EQ_INT(request(&c, 30), 0);
    cbc_opened(&c, 700000);
    out_kind(&c, 700000);
    cbc_closed(&c, 700010);                        // and again, later: one more refresh
    T_CHECK(request(&c, 700010) > 0);
}

static void a_dead_refresh_signs_out(void) {
    cbc c; mk(&c, 0, 1);
    signed_in(&c, "OLD", 100000);
    up(&c, 0);
    uint32_t t = cbc_call(&c, CBC_GET, CBC_C_AUTH | CBC_C_HTTP, "/me", 3, NULL, 0, 0);
    T_CHECK(t != 0);
    T_CHECK(request(&c, 40000) > 0);               // the refresh comes before the call
    T_EQ_STR(g_path, "/v1/auth/refresh");
    T_EQ_INT(request(&c, 40000), 0);               // the call waits for it
    uint8_t b[16];
    size_t n = refusal(b, CBW_WHY_ACCOUNT, CBW_ACCOUNT_E_REFRESH_USED);
    T_EQ_INT(cbc_on_response(&c, g_h.id, 401, b, n, 40001), 2);
    T_EQ_INT(ev(&c, 0)->kind, CBC_EV_SESSION);
    T_EQ_INT(ev(&c, 1)->kind, CBC_EV_DONE);
    T_EQ_INT(ev(&c, 1)->ticket, t);
    T_EQ_INT(ev(&c, 1)->why, CBW_WHY_AUTH);
    T_EQ_INT(cbc_user_of(&c)->signed_in, 0);
    T_EQ_INT(cbc_stream_wanted(&c), 0);
    T_EQ_INT(out_kind(&c, 40001), -1);
}

static void a_failing_refresh_backs_off(void) {
    cbc c; mk(&c, 0, 1);
    signed_in(&c, "OLD", 100000);
    T_CHECK(request(&c, 40000) > 0);
    T_EQ_INT(cbc_on_response(&c, g_h.id, 0, NULL, 0, 40000), 0);   // never arrived
    T_EQ_INT(request(&c, 40499), 0);
    T_CHECK(request(&c, 40500) > 0);
    T_EQ_STR(g_path, "/v1/auth/refresh");
    T_EQ_INT(cbc_user_of(&c)->signed_in, 1);
}

// ---- calls -------------------------------------------------------------------------------------
static void calls_ride_the_socket(void) {
    cbc c; mk(&c, 0, 1);
    signed_in(&c, "TOK", 100000000);
    uint32_t t = cbc_call(&c, CBC_POST, 0, "/v1/action", 10, (const uint8_t *)"mv", 2, 0);
    T_CHECK(t != 0);
    T_EQ_INT(request(&c, 0), 0);                   // not HTTP
    up(&c, 1);
    T_EQ_INT(out_kind(&c, 1), SF_REQ);
    T_EQ_INT(g_f.meth, SF_METH_POST);
    T_EQ_MEM(g_f.path, g_f.path_len, "/v1/action", 10);
    T_EQ_MEM(g_f.body, g_f.body_len, "mv", 2);
    T_EQ_INT(reply(&c, g_f.id, 200, "okay", 4, 2), 1);
    const cbc_event *e = ev(&c, 0);
    T_EQ_INT(e->kind, CBC_EV_DONE);
    T_EQ_INT(e->ticket, t);
    T_EQ_INT(e->status, 200);
    T_EQ_INT(e->why, 0);
    T_EQ_INT(e->off, 8);
    T_EQ_INT(e->len, 4);
}

static void a_refused_socket_call(void) {
    cbc c; mk(&c, 0, 1);
    signed_in(&c, "TOK", 100000000);
    up(&c, 0);
    cbc_call(&c, CBC_POST, 0, "/a", 2, NULL, 0, 0);
    out_kind(&c, 0);
    uint8_t b[16];
    size_t n = refusal(b, CBW_WHY_REFUSED, 17);
    reply(&c, g_f.id, 409, b, n, 1);
    T_EQ_INT(ev(&c, 0)->why, CBW_WHY_REFUSED);
    T_EQ_INT(ev(&c, 0)->code, 17);
    T_EQ_INT(ev(&c, 0)->status, 409);
}

static void a_socket_loss(void) {
    cbc c; mk(&c, 0, 1);
    signed_in(&c, "TOK", 100000000);
    up(&c, 0);
    uint32_t rd = cbc_call(&c, CBC_GET, CBC_C_READ, "/r", 2, NULL, 0, 0);
    uint32_t wr = cbc_call(&c, CBC_POST, 0, "/w", 2, NULL, 0, 0);
    T_EQ_INT(out_kind(&c, 0), SF_REQ);
    T_EQ_INT(out_kind(&c, 0), SF_REQ);
    cbc_closed(&c, 5);
    T_EQ_INT(cbc_n_events(&c), 1);                 // the write is over; the read waits
    T_EQ_INT(ev(&c, 0)->ticket, wr);
    T_EQ_INT(ev(&c, 0)->why, CBC_WHY_LOST);
    T_EQ_INT(ev(&c, 0)->status, 0);
    up(&c, 600);
    T_EQ_INT(out_kind(&c, 600), SF_REQ);
    T_EQ_MEM(g_f.path, g_f.path_len, "/r", 2);
    reply(&c, g_f.id, 200, NULL, 0, 601);
    T_EQ_INT(ev(&c, 0)->ticket, rd);
}

static void an_unanswered_socket_call(void) {
    cbc c; mk(&c, 0, 1);
    signed_in(&c, "TOK", 100000000);
    up(&c, 0);
    cbc_call(&c, CBC_GET, CBC_C_READ, "/r", 2, NULL, 0, 0);
    uint32_t wr = cbc_call(&c, CBC_POST, 0, "/w", 2, NULL, 0, 0);
    out_kind(&c, 0);
    out_kind(&c, 0);
    T_EQ_INT(cbc_tick(&c, 14999), 0);
    T_EQ_INT(cbc_tick(&c, 15000), 1);
    T_EQ_INT(ev(&c, 0)->ticket, wr);
    T_EQ_INT(out_kind(&c, 15000), SF_REQ);         // the read goes again
    T_EQ_MEM(g_f.path, g_f.path_len, "/r", 2);
}

static void anonymous_reads_go_by_http(void) {
    cbc c; mk(&c, 1, 1);
    uint32_t t = cbc_call(&c, CBC_GET, CBC_C_READ, "/adv/users", 10, NULL, 0, 0);
    T_CHECK(request(&c, 0) > 0);
    T_EQ_STR(g_path, "/adv/users");
    T_EQ_INT(g_h.method, CBC_GET);
    T_EQ_INT(g_h.auth_len, 0);
    T_EQ_INT(g_h.flags, 0);
    T_EQ_INT(request(&c, 0), 0);
    // Lost three times, sent four times in all, then over.
    for (int i = 0; i < 3; i++) {
        T_EQ_INT(cbc_on_response(&c, g_h.id, 0, NULL, 0, 0), 0);
        int64_t wait = 500LL << i;
        T_EQ_INT(request(&c, wait - 1), 0);
        T_CHECK(request(&c, wait) > 0);
    }
    T_EQ_INT(cbc_on_response(&c, g_h.id, 0, NULL, 0, 9000), 1);
    T_EQ_INT(ev(&c, 0)->ticket, t);
    T_EQ_INT(ev(&c, 0)->why, CBC_WHY_LOST);
    // A write that never arrived is not sent again.
    uint32_t w = cbc_call(&c, CBC_POST, 0, "/x", 2, NULL, 0, 0);
    request(&c, 0);
    T_EQ_INT(cbc_on_response(&c, g_h.id, 0, NULL, 0, 0), 1);
    T_EQ_INT(ev(&c, 0)->ticket, w);
    // An answer: its span is the body the host holds.
    uint32_t ok = cbc_call(&c, CBC_GET, CBC_C_READ, "/y", 2, NULL, 0, 0);
    request(&c, 0);
    T_EQ_INT(cbc_on_response(&c, g_h.id, 200, NULL, 12345, 0), 1);
    T_EQ_INT(ev(&c, 0)->ticket, ok);
    T_EQ_INT(ev(&c, 0)->len, 12345);
}

static void a_busy_read_goes_again(void) {
    cbc c; mk(&c, 1, 1);
    uint32_t t = cbc_call(&c, CBC_GET, CBC_C_READ, "/r", 2, NULL, 0, 0);
    uint8_t b[16];
    size_t n = refusal(b, CBW_WHY_BUSY, 0);
    int64_t now = 0;
    request(&c, now);
    for (int i = 0; i < 3; i++) {
        T_EQ_INT(cbc_on_response(&c, g_h.id, 503, b, n, now), 0);
        T_EQ_INT(request(&c, now + (250LL << i) - 1), 0);
        now += 250LL << i;
        T_CHECK(request(&c, now) > 0);
    }
    T_EQ_INT(cbc_on_response(&c, g_h.id, 503, b, n, now), 1);
    T_EQ_INT(ev(&c, 0)->ticket, t);
    T_EQ_INT(ev(&c, 0)->why, CBW_WHY_BUSY);
    // A write is answered at once.
    uint32_t w = cbc_call(&c, CBC_POST, 0, "/w", 2, NULL, 0, 0);
    request(&c, 0);
    T_EQ_INT(cbc_on_response(&c, g_h.id, 503, b, n, 0), 1);
    T_EQ_INT(ev(&c, 0)->ticket, w);
}

static void a_401_refreshes_and_goes_once_more(void) {
    cbc c; mk(&c, 0, 1);
    signed_in(&c, "OLD", 100000000);
    uint32_t t = cbc_call(&c, CBC_GET, CBC_C_AUTH | CBC_C_HTTP, "/me", 3, NULL, 0, 0);
    T_CHECK(request(&c, 0) > 0);
    T_EQ_STR(g_path, "/me");
    T_EQ_STR(g_auth, "Bearer OLD");
    T_EQ_INT(cbc_on_response(&c, g_h.id, 401, NULL, 0, 1), 0);
    T_CHECK(request(&c, 1) > 0);
    T_EQ_STR(g_path, "/v1/auth/refresh");
    uint8_t b[256];
    size_t n = session_body(b, "NEW", "R2", 100000000, "u-1", "ANN");
    cbc_on_response(&c, g_h.id, 200, b, n, 2);
    T_CHECK(request(&c, 2) > 0);
    T_EQ_STR(g_auth, "Bearer NEW");
    T_EQ_INT(cbc_on_response(&c, g_h.id, 401, NULL, 0, 3), 1);   // once only
    T_EQ_INT(ev(&c, 0)->ticket, t);
    T_EQ_INT(ev(&c, 0)->why, CBW_WHY_AUTH);
}

// ---- sign in and out ---------------------------------------------------------------------------
static void sign_in_then_out(void) {
    cbc c; mk(&c, 0, 1);
    uint32_t t = cbc_sign_in(&c, CBW_SIGNIN, "ANN", 3, "secret!!", 8, 0);
    T_CHECK(t != 0);
    T_CHECK(request(&c, 0) > 0);
    T_EQ_STR(g_path, "/v1/auth/signin");
    T_EQ_INT(g_h.method, CBC_POST);
    T_EQ_INT(g_h.flags, CBC_RQ_BODY);
    T_EQ_INT(g_h.auth_len, 0);
    T_EQ_MEM(g_body, g_h.body_len, "\x01\x02\x03" "ANN" "\x08" "secret!!", 15);
    uint8_t b[256];
    size_t n = session_body(b, "TOK", "R1", 100000000, "u-9", "ANN");
    T_EQ_INT(cbc_on_response(&c, g_h.id, 200, b, n, 1), 2);
    T_EQ_INT(ev(&c, 0)->kind, CBC_EV_SESSION);
    T_EQ_INT(ev(&c, 1)->kind, CBC_EV_DONE);
    T_EQ_INT(ev(&c, 1)->ticket, t);
    T_EQ_INT(ev(&c, 1)->why, 0);
    T_EQ_MEM(cbc_user_of(&c)->user_id, cbc_user_of(&c)->user_id_len, "u-9", 3);
    T_EQ_INT(cbc_user_of(&c)->expires_at, 100000000);
    // A wrong password.
    cbc d; mk(&d, 0, 1);
    cbc_sign_in(&d, CBW_SIGNUP, "ANN", 3, "x", 1, 0);
    request(&d, 0);
    T_EQ_STR(g_path, "/v1/auth/signup");
    uint8_t r[16];
    size_t rn = refusal(r, CBW_WHY_ACCOUNT, CBW_ACCOUNT_E_WEAK_PASSWORD);
    T_EQ_INT(cbc_on_response(&d, g_h.id, 400, r, rn, 1), 1);
    T_EQ_INT(ev(&d, 0)->why, CBW_WHY_ACCOUNT);
    T_EQ_INT(ev(&d, 0)->code, CBW_ACCOUNT_E_WEAK_PASSWORD);
    T_EQ_INT(cbc_user_of(&d)->signed_in, 0);
    // Out: the bearer goes with it, nothing is taken after it, the socket closes.
    up(&c, 2);
    uint32_t o = cbc_sign_out(&c, 1, 3);
    T_CHECK(o != 0);
    T_EQ_INT(cbc_call(&c, CBC_GET, 0, "/x", 2, NULL, 0, 3), 0);
    T_EQ_INT(cbc_error(&c), CBC_E_AUTH);
    T_EQ_INT(out_kind(&c, 3), -1);
    T_CHECK(request(&c, 3) > 0);
    T_EQ_STR(g_path, "/v1/auth/signout");
    T_EQ_STR(g_auth, "Bearer TOK");
    T_EQ_MEM(g_body, g_h.body_len, "\x01\x04\x01", 3);
    T_EQ_INT(cbc_on_response(&c, g_h.id, 500, NULL, 0, 4), 2);   // whatever the answer
    T_EQ_INT(ev(&c, 0)->ticket, o);
    T_EQ_INT(ev(&c, 1)->kind, CBC_EV_SESSION);
    T_EQ_INT(cbc_user_of(&c)->signed_in, 0);
}

static void the_session_blob(void) {
    cbc c; mk(&c, 0, 1);
    uint8_t b[256], s[256];
    size_t n = session_body(b, "TOK", "R1", 123456789, "u-1", "ANN");
    T_EQ_INT(cbc_session_save(&c, s, sizeof s), 0);   // signed out
    T_EQ_INT(cbc_session_load(&c, b, n), 0);
    T_EQ_INT(cbc_session_save(&c, s, sizeof s), (long long)n);
    T_EQ_MEM(s, n, b, n);
    T_EQ_INT(cbc_session_save(&c, s, n - 1), CBC_E_CAP);
    T_EQ_INT(cbc_session_load(&c, b, n - 1), -1);     // a byte short
    T_EQ_INT(cbc_user_of(&c)->signed_in, 0);
    b[n] = 0;
    T_EQ_INT(cbc_session_load(&c, b, n + 1), -1);     // a byte over
    b[1] = 21;
    T_EQ_INT(cbc_session_load(&c, b, n), -1);         // not a session
}

// ---- frames in ---------------------------------------------------------------------------------
static void pushes_and_garbage(void) {
    cbc c; mk(&c, 1, 1);
    cbc_sub(&c, 2, "r", 1);
    up(&c, 0);
    uint8_t p[6] = { SF_V, 5, 1, 2, 3, 4 };
    T_EQ_INT(feed(&c, p, 6, 1), 1);
    T_EQ_INT(ev(&c, 0)->kind, CBC_EV_PUSH);
    T_EQ_INT(ev(&c, 0)->push, 5);
    T_EQ_INT(ev(&c, 0)->off, 0);
    T_EQ_INT(ev(&c, 0)->len, 6);
    uint8_t g[3] = { 9, 9, 9 };
    T_EQ_INT(feed(&c, g, 3, 1), 1);
    T_EQ_INT(ev(&c, 0)->kind, CBC_EV_BAD);
    uint8_t pong[2] = { SF_V, SF_PONG };
    T_EQ_INT(feed(&c, pong, 2, 1), 0);
    uint8_t rdy[2] = { SF_V, SF_READY };
    T_EQ_INT(feed(&c, rdy, 2, 1), 1);                 // a second READY is not this protocol
    T_EQ_INT(ev(&c, 0)->kind, CBC_EV_BAD);
}

// ---- limits ------------------------------------------------------------------------------------
static void limits(void) {
    T_CHECK(sizeof(cbc) <= CBC_SIZE_BUDGET);
    cbc c; mk(&c, 1, 1);
    T_EQ_INT(cbc_call(&c, 3, 0, "/x", 2, NULL, 0, 0), 0);
    T_EQ_INT(cbc_error(&c), CBC_E_ARG);
    T_EQ_INT(cbc_call(&c, CBC_GET, 0, "x", 1, NULL, 0, 0), 0);
    T_EQ_INT(cbc_error(&c), CBC_E_ARG);
    T_EQ_INT(cbc_call(&c, CBC_GET, CBC_C_AUTH, "/x", 2, NULL, 0, 0), 0);
    T_EQ_INT(cbc_error(&c), CBC_E_AUTH);
    for (int i = 0; i < CBC_CALLS_MAX; i++) T_CHECK(cbc_call(&c, CBC_GET, 0, "/x", 2, NULL, 0, 0) != 0);
    T_EQ_INT(cbc_call(&c, CBC_GET, 0, "/x", 2, NULL, 0, 0), 0);
    T_EQ_INT(cbc_error(&c), CBC_E_FULL);
    cbc d; mk(&d, 1, 1);
    static uint8_t big[3000];
    T_CHECK(cbc_call(&d, CBC_POST, 0, "/x", 2, big, sizeof big, 0) != 0);
    T_EQ_INT(cbc_call(&d, CBC_POST, 0, "/x", 2, big, sizeof big, 0), 0);   // the arena is full
    T_EQ_INT(cbc_error(&d), CBC_E_FULL);
    for (int i = 0; i < CBC_SUBS_MAX; i++) { char r[2] = { (char)('a' + i), 0 }; T_EQ_INT(cbc_sub(&d, 2, r, 1), 0); }
    T_EQ_INT(cbc_sub(&d, 2, "z", 1), CBC_E_FULL);
    T_EQ_INT(cbc_sub(&d, 2, "", 0), CBC_E_ARG);
}

// The arena moves the later calls' bytes down when one finishes: each still sends its own.
static void the_arena_keeps_each_call_whole(void) {
    cbc c; mk(&c, 1, 1);
    cbc_call(&c, CBC_POST, 0, "/one", 4, (const uint8_t *)"111", 3, 0);
    cbc_call(&c, CBC_POST, 0, "/two", 4, (const uint8_t *)"22", 2, 0);
    cbc_call(&c, CBC_POST, 0, "/three", 6, (const uint8_t *)"3", 1, 0);
    request(&c, 0);
    T_EQ_STR(g_path, "/one");
    cbc_on_response(&c, g_h.id, 200, NULL, 0, 0);
    request(&c, 0);
    T_EQ_STR(g_path, "/two");
    T_EQ_MEM(g_body, g_h.body_len, "22", 2);
    request(&c, 0);
    T_EQ_STR(g_path, "/three");
    T_EQ_MEM(g_body, g_h.body_len, "3", 1);
    T_EQ_INT(cbc_call(&c, CBC_POST, 0, "/x", 2, (const uint8_t *)"z", 1, 0) != 0, 1);
}

static void due_ms(void) {
    cbc c; mk(&c, 1, 1);
    T_EQ_INT(cbc_due_ms(&c, 0), -1);
    cbc_sub(&c, 2, "r", 1);
    cbc_opened(&c, 0);
    T_EQ_INT(cbc_due_ms(&c, 0), 0);                // the HELLO is due
    out_kind(&c, 0);
    T_EQ_INT(cbc_due_ms(&c, 0), 10000);            // the HELLO's wait
    ready(&c, 0);
    T_EQ_INT(cbc_due_ms(&c, 0), 0);                // the SUB
    out_kind(&c, 0);
    T_EQ_INT(cbc_due_ms(&c, 0), 10000);            // its answer's wait
    reply(&c, g_f.id, 200, NULL, 0, 0);
    T_EQ_INT(cbc_due_ms(&c, 0), 30000);            // the PING
    cbc d; mk(&d, 0, 1);
    signed_in(&d, "T", 1000000);
    T_EQ_INT(cbc_due_ms(&d, 0), 940000);           // the refresh
}

int main(void) {
    T_RUN(anonymous_hello_then_sub);
    T_RUN(counted_holds);
    T_RUN(sub_refused_backs_off);
    T_RUN(unanswered_sub_is_asked_again);
    T_RUN(ping_then_half_open);
    T_RUN(reconnect_is_decorrelated_jitter);
    T_RUN(hello_unanswered_is_closed);
    T_RUN(hello_carries_the_token);
    T_RUN(hello_waits_for_the_refresh);
    T_RUN(a_refresh_rolls_the_socket);
    T_RUN(a_roll_waits_for_socket_calls);
    T_RUN(a_refused_hello_forces_one_refresh);
    T_RUN(a_dead_refresh_signs_out);
    T_RUN(a_failing_refresh_backs_off);
    T_RUN(calls_ride_the_socket);
    T_RUN(a_refused_socket_call);
    T_RUN(a_socket_loss);
    T_RUN(an_unanswered_socket_call);
    T_RUN(anonymous_reads_go_by_http);
    T_RUN(a_busy_read_goes_again);
    T_RUN(a_401_refreshes_and_goes_once_more);
    T_RUN(sign_in_then_out);
    T_RUN(the_session_blob);
    T_RUN(pushes_and_garbage);
    T_RUN(limits);
    T_RUN(the_arena_keeps_each_call_whole);
    T_RUN(due_ms);
    return T_DONE();
}
