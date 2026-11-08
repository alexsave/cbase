// h2_session.c - one HTTP/2 client connection, sans I/O: see h2_session.h.
#include "net/h2_session.h"

#include "net/hpack.h"
#include "util/err.h"

#include <stdlib.h>
#include <string.h>

#define MAX_IOV 4
// The most DATA one flush writes, so the I/O loop reads between writes.
#define FLUSH_DATA_BUDGET (64u << 10)

enum { ST_AWAIT_HEADERS, ST_BODY };

struct cb_h2_stream {
    uint32_t     id;          // 0 while it waits to open
    void        *user;
    cb_buf       block;       // the request's header block, until sent
    cb_h2_iov    body[MAX_IOV];
    int          n_body, iov_at;
    size_t       iov_off;
    uint64_t     body_left;
    bool         end_sent;    // END_STREAM went out
    int64_t      send_window, recv_window;
    uint32_t     recv_unacked;
    int          state;       // ST_*
    cb_h2_stream *next;       // the open list, or the waiting queue
};

struct cb_h2_sess {
    cb_h2_limits    lim;
    cb_h2_callbacks cb;
    cb_hpack_dec    dec;
    cb_buf          in;    // the peer's bytes not yet a whole frame
    cb_buf          ctl;   // frames queued by recv and the preface: flushed first
    bool            dead, got_settings, goaway_recv, goaway_sent, ids_out;
    uint32_t        next_id;
    uint32_t        peer[7];   // the peer's settings, by id
    int64_t         conn_send_window, conn_recv_window;
    uint32_t        conn_recv_unacked;
    cb_h2_stream   *open;      // opened and not ended
    cb_h2_stream   *wait_head, *wait_tail;
    int             n_open, n_wait;
    // A header block arriving in pieces (HEADERS without END_HEADERS).
    bool            in_block;
    uint32_t        block_stream;
    uint8_t         block_flags;
    cb_buf          block;
};

void cb_h2_limits_default(cb_h2_limits *l) {
    l->stream_window = 4u << 20;
    l->conn_window = 16u << 20;
    l->max_header_list = 64u << 10;
    l->max_header_block = 256u << 10;
    l->max_streams = 256;
}

// ---- frames -------------------------------------------------------------------
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static void put32(cb_buf *b, uint32_t v) {
    uint8_t x[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
    cb_buf_append(b, x, 4);
}

int cb_h2_frame_parse(const uint8_t *p, size_t n, uint32_t max_frame, cb_h2_frame *f) {
    if (n < CB_H2_FRAME_HEAD) return 0;
    f->len = (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
    f->type = p[3];
    f->flags = p[4];
    f->stream = rd32(p + 5) & 0x7fffffffu;   // the reserved bit is ignored (RFC 9113 4.1)
    if (f->len > max_frame) return CB_E_INVALID;
    if (n < CB_H2_FRAME_HEAD + (size_t)f->len) return 0;
    return CB_H2_FRAME_HEAD + (int)f->len;
}

void cb_h2_frame_head(cb_buf *out, uint32_t len, uint8_t type, uint8_t flags, uint32_t stream) {
    uint8_t h[CB_H2_FRAME_HEAD] = { (uint8_t)(len >> 16), (uint8_t)(len >> 8), (uint8_t)len, type, flags,
                                    (uint8_t)(stream >> 24 & 0x7f), (uint8_t)(stream >> 16), (uint8_t)(stream >> 8),
                                    (uint8_t)stream };
    cb_buf_append(out, h, sizeof h);
}

void cb_h2_frame_headers(cb_buf *out, uint32_t stream, const uint8_t *block, size_t n, bool end_stream,
                         uint32_t max_frame) {
    size_t at = 0;
    bool first = true;
    do {
        size_t k = n - at < max_frame ? n - at : max_frame;
        bool last = at + k == n;
        uint8_t flags = (uint8_t)((last ? CB_H2_F_END_HEADERS : 0) | (first && end_stream ? CB_H2_F_END_STREAM : 0));
        cb_h2_frame_head(out, (uint32_t)k, first ? CB_H2_HEADERS : CB_H2_CONTINUATION, flags, stream);
        cb_buf_append(out, block + at, k);
        at += k;
        first = false;
    } while (at < n);
}

void cb_h2_frame_settings(cb_buf *out, const uint16_t *ids, const uint32_t *values, int n, bool ack) {
    cb_h2_frame_head(out, ack ? 0 : (uint32_t)n * 6, CB_H2_SETTINGS, ack ? CB_H2_F_ACK : 0, 0);
    if (ack) return;
    for (int i = 0; i < n; i++) {
        uint8_t id[2] = { (uint8_t)(ids[i] >> 8), (uint8_t)ids[i] };
        cb_buf_append(out, id, 2);
        put32(out, values[i]);
    }
}

void cb_h2_frame_u32(cb_buf *out, uint8_t type, uint32_t stream, uint32_t v) {
    cb_h2_frame_head(out, 4, type, 0, stream);
    put32(out, v);
}

void cb_h2_frame_goaway(cb_buf *out, uint32_t last_stream, uint32_t code) {
    cb_h2_frame_head(out, 8, CB_H2_GOAWAY, 0, 0);
    put32(out, last_stream & 0x7fffffffu);
    put32(out, code);
}

void cb_h2_frame_ping(cb_buf *out, const uint8_t opaque[8], bool ack) {
    cb_h2_frame_head(out, 8, CB_H2_PING, ack ? CB_H2_F_ACK : 0, 0);
    cb_buf_append(out, opaque, 8);
}

int cb_h2_frame_unpad(const cb_h2_frame *f, const uint8_t *payload, const uint8_t **out, size_t *out_len) {
    size_t n = f->len, at = 0, pad = 0;
    if (f->flags & CB_H2_F_PADDED) {
        if (n < 1) return CB_E_INVALID;
        pad = payload[0];
        at = 1;
    }
    if (f->type == CB_H2_HEADERS && (f->flags & CB_H2_F_PRIORITY)) at += 5;
    if (at + pad > n) return CB_E_INVALID;
    *out = payload + at;
    *out_len = n - at - pad;
    return CB_OK;
}

// ---- streams --------------------------------------------------------------------
static uint32_t last_opened(const cb_h2_sess *s) { return s->next_id > 1 ? s->next_id - 2 : 0; }

// An id this side never opened: an even one (the peer may open none: no
// push), or an odd one past the last opened.
static bool idle_id(const cb_h2_sess *s, uint32_t id) { return (id & 1) == 0 || id > last_opened(s); }

static cb_h2_stream *find(const cb_h2_sess *s, uint32_t id) {
    for (cb_h2_stream *st = s->open; st; st = st->next)
        if (st->id == id) return st;
    return NULL;
}

static void unlink_open(cb_h2_sess *s, cb_h2_stream *st) {
    for (cb_h2_stream **pp = &s->open; *pp; pp = &(*pp)->next)
        if (*pp == st) { *pp = st->next; s->n_open--; return; }
}

static void unlink_wait(cb_h2_sess *s, cb_h2_stream *st) {
    cb_h2_stream *prev = NULL;
    for (cb_h2_stream *p = s->wait_head; p; prev = p, p = p->next) {
        if (p != st) continue;
        if (prev) prev->next = p->next;
        else s->wait_head = p->next;
        if (s->wait_tail == p) s->wait_tail = prev;
        s->n_wait--;
        return;
    }
}

static void stream_free(cb_h2_stream *st) {
    cb_buf_free(&st->block);
    free(st);
}

// Ends an open stream: unlinked, its on_end called, freed.
static void end_open(cb_h2_sess *s, cb_h2_stream *st, const cb_h2_end *e) {
    unlink_open(s, st);
    if (s->cb.on_end) s->cb.on_end(st->user, e);
    stream_free(st);
}

static void end_waiting_all(cb_h2_sess *s, cb_h2_end_kind kind, uint32_t code, bool by_peer, const char *why) {
    while (s->wait_head) {
        cb_h2_stream *st = s->wait_head;
        unlink_wait(s, st);
        cb_h2_end e = { kind, false, code, by_peer, true, why };
        if (s->cb.on_end) s->cb.on_end(st->user, &e);
        stream_free(st);
    }
}

// A stream error: RST_STREAM with code, and the stream ends.
static void reset_stream(cb_h2_sess *s, cb_h2_stream *st, uint32_t code, const char *why) {
    cb_h2_frame_u32(&s->ctl, CB_H2_RST_STREAM, st->id, code);
    cb_h2_end e = { CB_H2_END_STREAM_ERROR, false, code, false, false, why };
    end_open(s, st, &e);
}

static void fail(cb_h2_sess *s, cb_h2_end_kind kind, uint32_t code, bool by_peer, const char *why) {
    if (s->dead) return;
    s->dead = true;
    if (!s->goaway_sent) {
        // A client opens no stream the peer could name, so its last id is 0.
        cb_h2_frame_goaway(&s->ctl, 0, code);
        s->goaway_sent = true;
    }
    while (s->open) {
        cb_h2_end e = { kind, false, code, by_peer, false, why };
        end_open(s, s->open, &e);
    }
    end_waiting_all(s, kind, code, by_peer, why);
}

// A connection error found in what the peer sent.
static int conn_error(cb_h2_sess *s, uint32_t code, const char *why) {
    fail(s, CB_H2_END_CONN_ERROR, code, false, why);
    return CB_E_IO;
}

// ---- the session ----------------------------------------------------------------
cb_h2_sess *cb_h2_sess_new(const cb_h2_limits *limits, const cb_h2_callbacks *cb) {
    cb_h2_sess *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    if (limits) s->lim = *limits;
    else cb_h2_limits_default(&s->lim);
    // A smaller window than the default would refuse data the peer may send
    // before it has read these settings.
    if (s->lim.stream_window < CB_H2_DEFAULT_WINDOW) s->lim.stream_window = CB_H2_DEFAULT_WINDOW;
    if (s->lim.stream_window > CB_H2_MAX_WINDOW) s->lim.stream_window = CB_H2_MAX_WINDOW;
    if (s->lim.conn_window < CB_H2_DEFAULT_WINDOW) s->lim.conn_window = CB_H2_DEFAULT_WINDOW;
    if (s->lim.conn_window > CB_H2_MAX_WINDOW) s->lim.conn_window = CB_H2_MAX_WINDOW;
    if (s->lim.max_streams < 1) s->lim.max_streams = 1;
    s->cb = *cb;
    if (cb_hpack_dec_init(&s->dec, CB_HPACK_TABLE_DEFAULT) != CB_OK) { free(s); return NULL; }
    cb_buf_init(&s->in);
    cb_buf_init(&s->ctl);
    cb_buf_init(&s->block);
    s->next_id = 1;
    s->peer[CB_H2_S_HEADER_TABLE_SIZE] = CB_HPACK_TABLE_DEFAULT;
    s->peer[CB_H2_S_ENABLE_PUSH] = 1;
    s->peer[CB_H2_S_MAX_CONCURRENT_STREAMS] = UINT32_MAX;   // unlimited until the peer says
    s->peer[CB_H2_S_INITIAL_WINDOW_SIZE] = CB_H2_DEFAULT_WINDOW;
    s->peer[CB_H2_S_MAX_FRAME_SIZE] = CB_H2_DEFAULT_FRAME;
    s->peer[CB_H2_S_MAX_HEADER_LIST_SIZE] = UINT32_MAX;
    s->conn_send_window = CB_H2_DEFAULT_WINDOW;
    s->conn_recv_window = s->lim.conn_window;
    // The preface: the magic, SETTINGS, and the connection window raised.
    cb_buf_append(&s->ctl, CB_H2_PREFACE, CB_H2_PREFACE_LEN);
    const uint16_t ids[] = { CB_H2_S_ENABLE_PUSH, CB_H2_S_INITIAL_WINDOW_SIZE, CB_H2_S_MAX_HEADER_LIST_SIZE };
    const uint32_t vals[] = { 0, s->lim.stream_window, s->lim.max_header_list };
    cb_h2_frame_settings(&s->ctl, ids, vals, 3, false);
    if (s->lim.conn_window > CB_H2_DEFAULT_WINDOW)
        cb_h2_frame_u32(&s->ctl, CB_H2_WINDOW_UPDATE, 0, s->lim.conn_window - CB_H2_DEFAULT_WINDOW);
    return s;
}

void cb_h2_sess_free(cb_h2_sess *s) {
    if (!s) return;
    while (s->open) {
        cb_h2_stream *st = s->open;
        s->open = st->next;
        stream_free(st);
    }
    while (s->wait_head) {
        cb_h2_stream *st = s->wait_head;
        s->wait_head = st->next;
        stream_free(st);
    }
    cb_hpack_dec_free(&s->dec);
    cb_buf_free(&s->in);
    cb_buf_free(&s->ctl);
    cb_buf_free(&s->block);
    free(s);
}

bool cb_h2_sess_dead(const cb_h2_sess *s) { return s->dead; }
bool cb_h2_sess_accepting(const cb_h2_sess *s) {
    return !s->dead && !s->goaway_recv && !s->goaway_sent && !s->ids_out;
}
int cb_h2_sess_streams(const cb_h2_sess *s) { return s->n_open + s->n_wait; }
uint32_t cb_h2_sess_peer_setting(const cb_h2_sess *s, int id) { return id >= 1 && id <= 6 ? s->peer[id] : 0; }

void cb_h2_sess_fail(cb_h2_sess *s, uint32_t code, const char *why) { fail(s, CB_H2_END_FAILED, code, false, why); }

void cb_h2_sess_goaway(cb_h2_sess *s) {
    if (s->dead || s->goaway_sent) return;
    cb_h2_frame_goaway(&s->ctl, 0, CB_H2_NO_ERROR);
    s->goaway_sent = true;
    end_waiting_all(s, CB_H2_END_FAILED, CB_H2_NO_ERROR, false, "the connection is closing");
}

// RFC 9113 8.2.2: fields that name the connection, not the request.
static bool connection_specific(const char *n, size_t len) {
    static const char *const bad[] = { "connection", "keep-alive", "proxy-connection", "transfer-encoding",
                                       "upgrade", "host" };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
        if (strlen(bad[i]) == len && memcmp(bad[i], n, len) == 0) return true;
    return false;
}

// RFC 9113 8.2.1: no NUL, CR or LF in a value, nor space or tab at its ends.
static bool value_ok(const char *v, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (v[i] == 0 || v[i] == '\r' || v[i] == '\n') return false;
    if (n && (v[0] == ' ' || v[0] == '\t' || v[n - 1] == ' ' || v[n - 1] == '\t')) return false;
    return true;
}

// A field name: lowercase visible ASCII, no ':' but a pseudo-header's first.
static bool name_ok(const char *n, size_t len) {
    if (len == 0) return false;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)n[i];
        if (c <= 0x20 || c >= 0x7f || (c >= 'A' && c <= 'Z')) return false;
        if (c == ':' && i > 0) return false;
    }
    return true;
}

int cb_h2_sess_submit(cb_h2_sess *s, const char *method, const char *scheme, const char *authority, const char *path,
                      const cb_h2_field *fields, int n_fields, const cb_h2_iov *body, int n_body, void *user,
                      cb_h2_stream **out) {
    if (!cb_h2_sess_accepting(s)) return CB_E_STATE;
    if (n_body < 0 || n_body > MAX_IOV || n_fields < 0) return CB_E_INVALID;
    // Ids left: each waiting stream takes one.
    if ((uint64_t)s->next_id + 2ull * (uint64_t)s->n_wait > 0x7fffffffull) return CB_E_STATE;
    if ((uint32_t)(s->n_open + s->n_wait) >= s->lim.max_streams) return CB_E_FULL;
    for (int i = 0; i < n_fields; i++) {
        const cb_h2_field *f = &fields[i];
        if (!name_ok(f->name, f->name_len) || f->name[0] == ':') return CB_E_INVALID;
        if (connection_specific(f->name, f->name_len) || !value_ok(f->value, f->value_len)) return CB_E_INVALID;
        if (f->name_len == 2 && memcmp(f->name, "te", 2) == 0 &&
            !(f->value_len == 8 && memcmp(f->value, "trailers", 8) == 0))
            return CB_E_INVALID;
    }
    cb_h2_stream *st = calloc(1, sizeof *st);
    if (!st) return CB_E_NOMEM;
    cb_buf_init(&st->block);
    cb_hpack_encode(&st->block, ":method", 7, method, strlen(method), false);
    cb_hpack_encode(&st->block, ":scheme", 7, scheme, strlen(scheme), false);
    cb_hpack_encode(&st->block, ":authority", 10, authority, strlen(authority), false);
    cb_hpack_encode(&st->block, ":path", 5, path, strlen(path), false);
    for (int i = 0; i < n_fields; i++)
        cb_hpack_encode(&st->block, fields[i].name, fields[i].name_len, fields[i].value, fields[i].value_len,
                        fields[i].never);
    for (int i = 0; i < n_body; i++) {
        if (!body[i].n) continue;
        st->body[st->n_body++] = body[i];
        st->body_left += body[i].n;
    }
    st->user = user;
    st->state = ST_AWAIT_HEADERS;
    if (s->wait_tail) s->wait_tail->next = st;
    else s->wait_head = st;
    s->wait_tail = st;
    s->n_wait++;
    *out = st;
    return CB_OK;
}

void cb_h2_sess_cancel(cb_h2_sess *s, cb_h2_stream *st) {
    if (st->id) {
        cb_h2_frame_u32(&s->ctl, CB_H2_RST_STREAM, st->id, CB_H2_CANCEL);
        unlink_open(s, st);
    } else {
        unlink_wait(s, st);
    }
    stream_free(st);
}

// ---- receiving --------------------------------------------------------------------
// What a header block's fields do, field by field.
typedef struct {
    cb_h2_sess   *s;
    cb_h2_stream *st;        // NULL: a stream already gone; decode for the table, drop the fields
    bool          trailer;
    bool          seen_regular, seen_status, informational;
    bool          malformed, overflow, aborted;
    uint32_t      list_size;
} block_ctx;

static int on_field(void *ctx, const char *name, size_t nl, const char *value, size_t vl, bool never) {
    block_ctx *b = ctx;
    if (!b->st) return CB_OK;
    b->list_size += (uint32_t)(nl + vl + 32);
    if (b->list_size > b->s->lim.max_header_list) b->overflow = true;
    if (b->malformed || b->overflow || b->aborted) return CB_OK;   // go on, for the table's sake
    if (!name_ok(name, nl) || !value_ok(value, vl)) { b->malformed = true; return CB_OK; }
    if (name[0] == ':') {
        // A response's one pseudo-header, before any regular field, once; none in trailers.
        if (b->trailer || b->seen_regular || b->seen_status || nl != 7 || memcmp(name, ":status", 7) != 0) {
            b->malformed = true;
            return CB_OK;
        }
        b->seen_status = true;
        if (vl != 3 || value[0] < '1' || value[0] > '5' || value[1] < '0' || value[1] > '9' || value[2] < '0' ||
            value[2] > '9') {
            b->malformed = true;
            return CB_OK;
        }
        if (value[0] == '1') {
            // 101 has no place in HTTP/2; other 1xx blocks are dropped whole.
            if (value[1] == '0' && value[2] == '1') b->malformed = true;
            b->informational = true;
            return CB_OK;
        }
    } else {
        b->seen_regular = true;
        if (!b->trailer && !b->seen_status) { b->malformed = true; return CB_OK; }
        if (connection_specific(name, nl) && !(nl == 4 && memcmp(name, "host", 4) == 0)) {
            b->malformed = true;
            return CB_OK;
        }
    }
    if (b->informational) return CB_OK;
    if (b->s->cb.on_header && b->s->cb.on_header(b->st->user, b->trailer, name, nl, value, vl) != CB_OK)
        b->aborted = true;
    return CB_OK;
}

// The response is whole: if the request is still going out, it stops (CANCEL).
static void complete(cb_h2_sess *s, cb_h2_stream *st) {
    if (!st->end_sent) cb_h2_frame_u32(&s->ctl, CB_H2_RST_STREAM, st->id, CB_H2_CANCEL);
    cb_h2_end e = { CB_H2_END_OK, true, CB_H2_NO_ERROR, false, false, NULL };
    end_open(s, st, &e);
}

static int header_block(cb_h2_sess *s, uint32_t id, uint8_t flags, const uint8_t *p, size_t n) {
    block_ctx b = { .s = s, .st = find(s, id) };
    if (b.st) b.trailer = b.st->state == ST_BODY;
    if (cb_hpack_decode(&s->dec, p, n, on_field, &b) != CB_OK)
        return conn_error(s, CB_H2_COMPRESSION_ERROR, "the peer's header block does not decode");
    cb_h2_stream *st = b.st;
    if (!st) return CB_OK;
    bool end = flags & CB_H2_F_END_STREAM;
    if (!b.trailer && !b.seen_status) b.malformed = true;
    if (b.trailer && !end) b.malformed = true;               // trailers end the stream
    if (b.informational && end) b.malformed = true;          // a 1xx is never the last block
    if (b.malformed) { reset_stream(s, st, CB_H2_PROTOCOL_ERROR, "the response's header block is malformed"); return CB_OK; }
    if (b.overflow) { reset_stream(s, st, CB_H2_CANCEL, "the response's header list is over the limit"); return CB_OK; }
    if (b.aborted) { reset_stream(s, st, CB_H2_CANCEL, "the caller refused a header"); return CB_OK; }
    if (b.informational) return CB_OK;
    st->state = ST_BODY;
    if (end) complete(s, st);
    return CB_OK;
}

static int on_headers(cb_h2_sess *s, const cb_h2_frame *f, const uint8_t *payload) {
    if (f->stream == 0 || idle_id(s, f->stream))
        return conn_error(s, CB_H2_PROTOCOL_ERROR, "HEADERS on a stream this side never opened");
    const uint8_t *frag;
    size_t n;
    if (cb_h2_frame_unpad(f, payload, &frag, &n) != CB_OK)
        return conn_error(s, CB_H2_PROTOCOL_ERROR, "HEADERS padded past its end");
    if (f->flags & CB_H2_F_END_HEADERS) return header_block(s, f->stream, f->flags, frag, n);
    if (n > s->lim.max_header_block) return conn_error(s, CB_H2_ENHANCE_YOUR_CALM, "a header block over the limit");
    s->in_block = true;
    s->block_stream = f->stream;
    s->block_flags = f->flags;
    cb_buf_reset(&s->block);
    cb_buf_append(&s->block, frag, n);
    return CB_OK;
}

static int on_continuation(cb_h2_sess *s, const cb_h2_frame *f, const uint8_t *payload) {
    if (!s->in_block || f->stream != s->block_stream)
        return conn_error(s, CB_H2_PROTOCOL_ERROR, "CONTINUATION out of place");
    if (s->block.len + f->len > s->lim.max_header_block)
        return conn_error(s, CB_H2_ENHANCE_YOUR_CALM, "a header block over the limit");
    cb_buf_append(&s->block, payload, f->len);
    if (!(f->flags & CB_H2_F_END_HEADERS)) return CB_OK;
    s->in_block = false;
    return header_block(s, s->block_stream, s->block_flags, s->block.p, s->block.len);
}

static int on_data(cb_h2_sess *s, const cb_h2_frame *f, const uint8_t *payload) {
    if (f->stream == 0 || idle_id(s, f->stream))
        return conn_error(s, CB_H2_PROTOCOL_ERROR, "DATA on a stream this side never opened");
    // The whole payload, padding too, counts against both windows (RFC 9113 6.9.1).
    s->conn_recv_window -= f->len;
    if (s->conn_recv_window < 0) return conn_error(s, CB_H2_FLOW_CONTROL_ERROR, "DATA past the connection window");
    s->conn_recv_unacked += f->len;
    cb_h2_stream *st = find(s, f->stream);
    if (!st) return CB_OK;   // a stream this side ended: only the connection's window cares
    st->recv_window -= f->len;
    if (st->recv_window < 0) {
        reset_stream(s, st, CB_H2_FLOW_CONTROL_ERROR, "DATA past the stream window");
        return CB_OK;
    }
    if (st->state != ST_BODY) {
        reset_stream(s, st, CB_H2_PROTOCOL_ERROR, "DATA before the response's headers");
        return CB_OK;
    }
    const uint8_t *d;
    size_t n;
    if (cb_h2_frame_unpad(f, payload, &d, &n) != CB_OK)
        return conn_error(s, CB_H2_PROTOCOL_ERROR, "DATA padded past its end");
    if (n && s->cb.on_data && s->cb.on_data(st->user, d, n) != CB_OK) {
        reset_stream(s, st, CB_H2_CANCEL, "the caller refused the body");
        return CB_OK;
    }
    if (f->flags & CB_H2_F_END_STREAM) { complete(s, st); return CB_OK; }
    st->recv_unacked += f->len;   // given back at the end of this batch (replenish)
    return CB_OK;
}

static int on_settings(cb_h2_sess *s, const cb_h2_frame *f, const uint8_t *p) {
    if (f->stream != 0) return conn_error(s, CB_H2_PROTOCOL_ERROR, "SETTINGS on a stream");
    if (f->flags & CB_H2_F_ACK) {
        if (f->len != 0) return conn_error(s, CB_H2_FRAME_SIZE_ERROR, "a SETTINGS ACK with a payload");
        return CB_OK;
    }
    if (f->len % 6) return conn_error(s, CB_H2_FRAME_SIZE_ERROR, "SETTINGS of a length not a multiple of 6");
    for (uint32_t at = 0; at < f->len; at += 6) {
        uint16_t id = (uint16_t)(p[at] << 8 | p[at + 1]);
        uint32_t v = rd32(p + at + 2);
        switch (id) {
        case CB_H2_S_ENABLE_PUSH:
            if (v != 0) return conn_error(s, CB_H2_PROTOCOL_ERROR, "a server's SETTINGS_ENABLE_PUSH is not 0");
            break;
        case CB_H2_S_INITIAL_WINDOW_SIZE: {
            if (v > CB_H2_MAX_WINDOW) return conn_error(s, CB_H2_FLOW_CONTROL_ERROR, "an initial window over 2^31-1");
            int64_t delta = (int64_t)v - (int64_t)s->peer[id];
            for (cb_h2_stream *st = s->open; st; st = st->next) {
                st->send_window += delta;
                if (st->send_window > CB_H2_MAX_WINDOW)
                    return conn_error(s, CB_H2_FLOW_CONTROL_ERROR, "a new initial window overflows a stream's");
            }
            break;
        }
        case CB_H2_S_MAX_FRAME_SIZE:
            if (v < CB_H2_DEFAULT_FRAME || v > CB_H2_MAX_FRAME)
                return conn_error(s, CB_H2_PROTOCOL_ERROR, "a max frame size out of range");
            break;
        default:
            break;
        }
        if (id >= 1 && id <= 6) s->peer[id] = v;   // others are unknown, and ignored
    }
    s->got_settings = true;
    cb_h2_frame_settings(&s->ctl, NULL, NULL, 0, true);
    return CB_OK;
}

static int on_goaway(cb_h2_sess *s, const cb_h2_frame *f, const uint8_t *p) {
    if (f->stream != 0) return conn_error(s, CB_H2_PROTOCOL_ERROR, "GOAWAY on a stream");
    if (f->len < 8) return conn_error(s, CB_H2_FRAME_SIZE_ERROR, "a GOAWAY under 8 bytes");
    uint32_t last = rd32(p) & 0x7fffffffu, code = rd32(p + 4);
    s->goaway_recv = true;
    // Streams past last were never processed: safe to send again elsewhere.
    for (cb_h2_stream *st = s->open, *next; st; st = next) {
        next = st->next;
        if (st->id > last) {
            cb_h2_end e = { CB_H2_END_GOAWAY, false, code, true, true, "the peer went away before it processed the request" };
            end_open(s, st, &e);
        }
    }
    end_waiting_all(s, CB_H2_END_GOAWAY, code, true, "the peer went away before the request was sent");
    return CB_OK;
}

static int on_window_update(cb_h2_sess *s, const cb_h2_frame *f, const uint8_t *p) {
    if (f->len != 4) return conn_error(s, CB_H2_FRAME_SIZE_ERROR, "a WINDOW_UPDATE not 4 bytes");
    uint32_t inc = rd32(p) & 0x7fffffffu;
    if (f->stream == 0) {
        if (inc == 0) return conn_error(s, CB_H2_PROTOCOL_ERROR, "a WINDOW_UPDATE of 0 on the connection");
        s->conn_send_window += inc;
        if (s->conn_send_window > CB_H2_MAX_WINDOW)
            return conn_error(s, CB_H2_FLOW_CONTROL_ERROR, "the connection window over 2^31-1");
        return CB_OK;
    }
    if (idle_id(s, f->stream)) return conn_error(s, CB_H2_PROTOCOL_ERROR, "WINDOW_UPDATE on a stream never opened");
    cb_h2_stream *st = find(s, f->stream);
    if (!st) return CB_OK;
    if (inc == 0) { reset_stream(s, st, CB_H2_PROTOCOL_ERROR, "a WINDOW_UPDATE of 0"); return CB_OK; }
    st->send_window += inc;
    if (st->send_window > CB_H2_MAX_WINDOW) reset_stream(s, st, CB_H2_FLOW_CONTROL_ERROR, "a stream window over 2^31-1");
    return CB_OK;
}

static int on_frame(cb_h2_sess *s, const cb_h2_frame *f, const uint8_t *payload) {
    if (!s->got_settings && !(f->type == CB_H2_SETTINGS && !(f->flags & CB_H2_F_ACK)))
        return conn_error(s, CB_H2_PROTOCOL_ERROR, "the server's preface is not a SETTINGS frame");
    if (s->in_block && f->type != CB_H2_CONTINUATION)
        return conn_error(s, CB_H2_PROTOCOL_ERROR, "a frame inside a header block");
    switch (f->type) {
    case CB_H2_DATA: return on_data(s, f, payload);
    case CB_H2_HEADERS: return on_headers(s, f, payload);
    case CB_H2_CONTINUATION: return on_continuation(s, f, payload);
    case CB_H2_SETTINGS: return on_settings(s, f, payload);
    case CB_H2_GOAWAY: return on_goaway(s, f, payload);
    case CB_H2_WINDOW_UPDATE: return on_window_update(s, f, payload);
    case CB_H2_PUSH_PROMISE: return conn_error(s, CB_H2_PROTOCOL_ERROR, "a PUSH_PROMISE, with push turned off");
    case CB_H2_PING:
        if (f->stream != 0) return conn_error(s, CB_H2_PROTOCOL_ERROR, "PING on a stream");
        if (f->len != 8) return conn_error(s, CB_H2_FRAME_SIZE_ERROR, "a PING not 8 bytes");
        if (!(f->flags & CB_H2_F_ACK)) cb_h2_frame_ping(&s->ctl, payload, true);
        return CB_OK;
    case CB_H2_RST_STREAM: {
        if (f->stream == 0 || idle_id(s, f->stream))
            return conn_error(s, CB_H2_PROTOCOL_ERROR, "RST_STREAM on a stream never opened");
        if (f->len != 4) return conn_error(s, CB_H2_FRAME_SIZE_ERROR, "a RST_STREAM not 4 bytes");
        cb_h2_stream *st = find(s, f->stream);
        if (!st) return CB_OK;
        uint32_t code = rd32(payload);
        cb_h2_end e = { CB_H2_END_PEER_RESET, false, code, true, code == CB_H2_REFUSED_STREAM, "the peer reset the stream" };
        end_open(s, st, &e);
        return CB_OK;
    }
    case CB_H2_PRIORITY:
        if (f->stream == 0) return conn_error(s, CB_H2_PROTOCOL_ERROR, "PRIORITY on stream 0");
        if (f->len != 5) {
            cb_h2_stream *st = find(s, f->stream);
            if (st) reset_stream(s, st, CB_H2_FRAME_SIZE_ERROR, "a PRIORITY not 5 bytes");
        }
        return CB_OK;
    default:
        return CB_OK;   // an unknown type is ignored (RFC 9113 4.1)
    }
}

// Credit for what a batch of the peer's bytes consumed, once half a window is
// owed: given only here, at the end of a recv, so a window counts what the
// peer could have known, and DATA past it within one batch is caught.
static void replenish(cb_h2_sess *s) {
    if (s->dead) return;
    for (cb_h2_stream *st = s->open; st; st = st->next) {
        if (st->recv_unacked < s->lim.stream_window / 2) continue;
        cb_h2_frame_u32(&s->ctl, CB_H2_WINDOW_UPDATE, st->id, st->recv_unacked);
        st->recv_window += st->recv_unacked;
        st->recv_unacked = 0;
    }
    if (s->conn_recv_unacked < s->lim.conn_window / 2) return;
    cb_h2_frame_u32(&s->ctl, CB_H2_WINDOW_UPDATE, 0, s->conn_recv_unacked);
    s->conn_recv_window += s->conn_recv_unacked;
    s->conn_recv_unacked = 0;
}

int cb_h2_sess_recv(cb_h2_sess *s, const uint8_t *p, size_t n) {
    if (s->dead) return CB_E_IO;
    cb_buf_append(&s->in, p, n);
    size_t at = 0;
    int rc = CB_OK;
    while (!s->dead) {
        cb_h2_frame f;
        int k = cb_h2_frame_parse(s->in.p + at, s->in.len - at, CB_H2_DEFAULT_FRAME, &f);
        if (k == 0) break;
        if (k < 0) { rc = conn_error(s, CB_H2_FRAME_SIZE_ERROR, "a frame over the max frame size"); break; }
        rc = on_frame(s, &f, s->in.p + at + CB_H2_FRAME_HEAD);
        at += (size_t)k;
        if (rc != CB_OK) break;
    }
    cb_buf_consume(&s->in, at);
    replenish(s);
    return s->dead ? CB_E_IO : CB_OK;
}

// ---- sending ----------------------------------------------------------------------
// Streams open only once the peer's SETTINGS are in: its MAX_CONCURRENT_STREAMS
// counts as unlimited until then, and a burst opened before it arrives would be
// refused past the limit. A server's preface is its first frame, so this waits
// on bytes already on their way.
static bool may_open(const cb_h2_sess *s) {
    return s->got_settings && s->wait_head && !s->goaway_recv && !s->goaway_sent &&
           (uint32_t)s->n_open < s->peer[CB_H2_S_MAX_CONCURRENT_STREAMS];
}

static void open_waiting(cb_h2_sess *s, cb_buf *out) {
    while (s->got_settings && s->wait_head && !s->goaway_recv && !s->goaway_sent &&
           (uint32_t)s->n_open < s->peer[CB_H2_S_MAX_CONCURRENT_STREAMS]) {
        cb_h2_stream *st = s->wait_head;
        if (s->next_id > 0x7fffffffu) {
            s->ids_out = true;
            end_waiting_all(s, CB_H2_END_FAILED, CB_H2_NO_ERROR, false, "the connection is out of stream ids");
            return;
        }
        unlink_wait(s, st);
        st->id = s->next_id;
        s->next_id += 2;
        st->next = s->open;
        s->open = st;
        s->n_open++;
        st->send_window = s->peer[CB_H2_S_INITIAL_WINDOW_SIZE];
        st->recv_window = s->lim.stream_window;
        st->end_sent = st->body_left == 0;
        cb_h2_frame_headers(out, st->id, st->block.p, st->block.len, st->end_sent,
                            s->peer[CB_H2_S_MAX_FRAME_SIZE]);
        cb_buf_free(&st->block);
    }
}

// Writes up to `max` bytes of st's body as one DATA frame.
static size_t send_data(cb_buf *out, cb_h2_stream *st, size_t max) {
    size_t k = st->body_left < max ? (size_t)st->body_left : max;
    bool last = k == st->body_left;
    cb_h2_frame_head(out, (uint32_t)k, CB_H2_DATA, last ? CB_H2_F_END_STREAM : 0, st->id);
    size_t left = k;
    while (left) {
        const cb_h2_iov *v = &st->body[st->iov_at];
        size_t take = v->n - st->iov_off < left ? v->n - st->iov_off : left;
        cb_buf_append(out, (const uint8_t *)v->p + st->iov_off, take);
        st->iov_off += take;
        left -= take;
        if (st->iov_off == v->n) { st->iov_at++; st->iov_off = 0; }
    }
    st->body_left -= k;
    st->send_window -= (int64_t)k;
    if (last) st->end_sent = true;
    return k;
}

size_t cb_h2_sess_flush(cb_h2_sess *s, cb_buf *out) {
    size_t before = out->len;
    cb_buf_append(out, s->ctl.p, s->ctl.len);
    cb_buf_reset(&s->ctl);
    if (s->dead) return out->len - before;
    open_waiting(s, out);
    // DATA, round robin over the streams with body left, one frame each pass.
    size_t budget = FLUSH_DATA_BUDGET;
    bool progress = true;
    while (progress && budget && s->conn_send_window > 0) {
        progress = false;
        for (cb_h2_stream *st = s->open; st && budget && s->conn_send_window > 0; st = st->next) {
            if (st->end_sent || st->send_window <= 0) continue;
            size_t max = s->peer[CB_H2_S_MAX_FRAME_SIZE];
            if ((int64_t)max > st->send_window) max = (size_t)st->send_window;
            if ((int64_t)max > s->conn_send_window) max = (size_t)s->conn_send_window;
            if (max > budget) max = budget;
            if (max == 0) continue;
            size_t k = send_data(out, st, max);
            s->conn_send_window -= (int64_t)k;
            budget -= k;
            progress = true;
        }
    }
    return out->len - before;
}

bool cb_h2_sess_want_flush(const cb_h2_sess *s) {
    if (s->ctl.len) return true;
    if (s->dead) return false;
    if (may_open(s)) return true;
    if (s->conn_send_window <= 0) return false;
    for (const cb_h2_stream *st = s->open; st; st = st->next)
        if (!st->end_sent && st->send_window > 0) return true;
    return false;
}
