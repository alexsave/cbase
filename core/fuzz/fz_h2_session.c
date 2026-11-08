// fz_h2_session.c - the HTTP/2 client session (net/h2_session.h) against a
// peer the fuzzer writes. Two requests are submitted (a POST whose body is
// past the default windows, so flow control is in play, and a GET), the
// server's preface SETTINGS arrives, and then the input is the peer's bytes,
// in pieces whose sizes the input gives (a length byte, then that many
// bytes), with a flush after each, as h2.c's I/O thread does it.
//
// The first byte sets what the caller does along the way: refuse the body
// past 100 bytes (bit 0), refuse the fifth header field (bit 1), cancel the
// GET after piece k (bit 2; k is the second byte & 7), close gracefully after
// piece k (bit 3). Bit 4: the server's preface SETTINGS allow one stream at a
// time, so the GET waits its turn (and a GOAWAY or a failure ends it waiting).
//
// Invariants: no crash and no read past a piece; on_end runs exactly once for
// every stream not cancelled, and no callback runs for a stream after it;
// recv answers CB_E_IO exactly when the session is dead, and a dead session
// stays dead, holds no streams, and answers CB_E_IO to every recv after.
#include "fz.h"

#include "net/h2_session.h"
#include "util/err.h"

typedef struct {
    int    ends, headers;
    size_t body;
    bool   refuse_body, refuse_header, cancelled;
} rec;

static int on_header(void *user, bool trailer, const char *name, size_t nl, const char *value, size_t vl) {
    rec *r = user;
    FZ_CHECK(r->ends == 0 && !r->cancelled);
    (void)trailer; (void)name; (void)nl; (void)value; (void)vl;
    if (r->refuse_header && ++r->headers == 5) return CB_E_FULL;
    return CB_OK;
}

static int on_data(void *user, const uint8_t *p, size_t n) {
    rec *r = user;
    FZ_CHECK(r->ends == 0 && !r->cancelled);
    if (n) FZ_CHECK(p != NULL);
    r->body += n;
    if (r->refuse_body && r->body > 100) return CB_E_FULL;
    return CB_OK;
}

static void on_end(void *user, const cb_h2_end *e) {
    rec *r = user;
    FZ_CHECK(!r->cancelled);
    r->ends++;
    FZ_CHECK(r->ends == 1);
    FZ_CHECK(e->ok == (e->kind == CB_H2_END_OK));
    FZ_CHECK(e->ok || e->why != NULL);
}

static const cb_h2_callbacks CB = { on_header, on_data, on_end };

static uint8_t g_body[100000];

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    fz_in in = fz_in_of(data, size);
    uint8_t mode = fz_u8(&in), k = fz_u8(&in) & 7;
    cb_h2_sess *s = cb_h2_sess_new(NULL, &CB);
    FZ_CHECK(s != NULL);
    rec a = { .refuse_body = mode & 1, .refuse_header = (mode & 2) != 0 };
    rec b = a;
    cb_h2_stream *sa = NULL, *sb = NULL;
    cb_h2_field f = { "x-goog-api-key", "secret", 14, 6, true };
    cb_h2_iov body = { g_body, sizeof g_body };
    FZ_CHECK(cb_h2_sess_submit(s, "POST", "https", "example.com", "/a", &f, 1, &body, 1, &a, &sa) == CB_OK);
    FZ_CHECK(cb_h2_sess_submit(s, "GET", "https", "example.com", "/b", NULL, 0, NULL, 0, &b, &sb) == CB_OK);
    FZ_CHECK(cb_h2_sess_streams(s) == 2);
    cb_buf out;
    cb_buf_init(&out);
    cb_h2_sess_flush(s, &out);
    cb_buf preface;
    cb_buf_init(&preface);
    const uint16_t one_id[] = { CB_H2_S_MAX_CONCURRENT_STREAMS };
    const uint32_t one[] = { 1 };
    cb_h2_frame_settings(&preface, one_id, one, (mode & 16) ? 1 : 0, false);
    FZ_CHECK(cb_h2_sess_recv(s, preface.p, preface.len) == CB_OK);
    cb_buf_free(&preface);
    cb_h2_sess_flush(s, &out);
    bool was_dead = false;
    for (int piece = 0; fz_left(&in); piece++) {
        size_t n;
        const uint8_t *p = fz_bytes(&in, fz_u8(&in), &n);
        uint8_t *copy = fz_dup(p, n);   // exact size: a read past the piece is an ASan report
        int rc = cb_h2_sess_recv(s, copy, n);
        free(copy);
        bool dead = cb_h2_sess_dead(s);
        FZ_CHECK((rc == CB_E_IO) == dead);
        FZ_CHECK(!was_dead || dead);
        if (dead) {
            FZ_CHECK(cb_h2_sess_streams(s) == 0);
            FZ_CHECK(!cb_h2_sess_accepting(s));
            FZ_CHECK(a.ends == 1 && (b.cancelled || b.ends == 1));
        }
        was_dead = dead;
        if (piece == k && (mode & 4) && !b.ends && !b.cancelled && !dead) {
            cb_h2_sess_cancel(s, sb);
            b.cancelled = true;
        }
        if (piece == k && (mode & 8)) cb_h2_sess_goaway(s);
        cb_buf_reset(&out);
        cb_h2_sess_flush(s, &out);
        FZ_CHECK(cb_h2_sess_streams(s) <= 2);
    }
    // The end: whatever is in hand fails, and each stream has ended exactly once.
    cb_h2_sess_fail(s, CB_H2_NO_ERROR, "the fuzz input ended");
    FZ_CHECK(cb_h2_sess_dead(s));
    FZ_CHECK(cb_h2_sess_streams(s) == 0);
    FZ_CHECK(a.ends == 1);
    FZ_CHECK(b.cancelled ? b.ends == 0 : b.ends == 1);
    uint8_t ping[17] = { 0, 0, 8, CB_H2_PING, 0, 0, 0, 0, 0 };
    FZ_CHECK(cb_h2_sess_recv(s, ping, sizeof ping) == CB_E_IO);
    FZ_CHECK(!cb_h2_sess_accepting(s));
    rec c = { 0 };
    cb_h2_stream *sc = NULL;
    FZ_CHECK(cb_h2_sess_submit(s, "GET", "https", "example.com", "/c", NULL, 0, NULL, 0, &c, &sc) == CB_E_STATE);
    FZ_CHECK(c.ends == 0);
    cb_buf_reset(&out);
    cb_h2_sess_flush(s, &out);
    cb_buf_free(&out);
    cb_h2_sess_free(s);
    return 0;
}
