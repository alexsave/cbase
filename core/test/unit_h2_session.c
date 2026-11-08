// unit_h2_session.c - the HTTP/2 client session (net/h2_session.h) against a
// scripted peer made of the same frame and HPACK pieces: the preface,
// responses with bodies and trailers, CONTINUATION, flow control both ways,
// the peer's stream limit, resets, GOAWAY, PING, and every connection error
// the session must raise.
#include "t.h"

#include "net/h2_session.h"
#include "net/hpack.h"
#include "util/err.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- a request's record --------------------------------------------------------
typedef struct {
    char    headers[1024], trailers[1024];
    cb_buf  body;
    bool    ended;
    cb_h2_end end;
    int     refuse_header_at, refuse_data;   // the callbacks answer an error when set
    int     n_headers;
} req;

static int rq_header(void *user, bool trailer, const char *n, size_t nl, const char *v, size_t vl) {
    req *r = user;
    char *dst = trailer ? r->trailers : r->headers;
    size_t at = strlen(dst);
    snprintf(dst + at, 1024 - at, "%.*s=%.*s;", (int)nl, n, (int)vl, v);
    if (r->refuse_header_at && ++r->n_headers >= r->refuse_header_at) return CB_E_FULL;
    return CB_OK;
}

static int rq_data(void *user, const uint8_t *p, size_t n) {
    req *r = user;
    if (r->refuse_data) return CB_E_FULL;
    cb_buf_append(&r->body, p, n);
    return CB_OK;
}

static void rq_end(void *user, const cb_h2_end *e) {
    req *r = user;
    T_CHECK(!r->ended);   // on_end runs once
    r->ended = true;
    r->end = *e;
}

static const cb_h2_callbacks CB = { rq_header, rq_data, rq_end };

static void rq_init(req *r) {
    memset(r, 0, sizeof *r);
    cb_buf_init(&r->body);
}

// ---- the scripted peer ---------------------------------------------------------
typedef struct {
    uint8_t  type, flags;
    uint32_t stream, len;
    uint8_t  payload[64];   // the first bytes
    char     fields[512];   // HEADERS: the decoded block
} sframe;

typedef struct {
    cb_h2_sess  *c;
    cb_hpack_dec dec;     // the client's header blocks
    cb_buf       wire;    // bytes from the client not yet parsed (after the preface)
    bool         preface_ok;
    sframe       got[256];
    int          n_got;
    cb_buf       block;   // a client header block in pieces
    uint32_t     block_stream;
} peer;

static int sf_field(void *ctx, const char *n, size_t nl, const char *v, size_t vl, bool never) {
    sframe *f = ctx;
    size_t at = strlen(f->fields);
    snprintf(f->fields + at, sizeof f->fields - at, "%.*s=%.*s%s;", (int)nl, n, (int)vl, v, never ? "!" : "");
    return CB_OK;
}

// Flushes the client and reads every frame it sent into p->got.
static void pump(peer *p) {
    cb_buf out;
    cb_buf_init(&out);
    // A flush writes at most its DATA budget: flush until nothing more is ready.
    while (cb_h2_sess_flush(p->c, &out) > 0) {
    }
    cb_buf_append(&p->wire, out.p, out.len);
    cb_buf_free(&out);
    if (!p->preface_ok && p->wire.len >= CB_H2_PREFACE_LEN) {
        T_CHECK(memcmp(p->wire.p, CB_H2_PREFACE, CB_H2_PREFACE_LEN) == 0);
        cb_buf_consume(&p->wire, CB_H2_PREFACE_LEN);
        p->preface_ok = true;
    }
    for (;;) {
        cb_h2_frame f;
        int k = cb_h2_frame_parse(p->wire.p, p->wire.len, CB_H2_MAX_FRAME, &f);
        if (k <= 0) break;
        if (p->n_got == 256) { T_CHECK(!"more frames than the record holds"); break; }
        sframe *s = &p->got[p->n_got++];
        memset(s, 0, sizeof *s);
        s->type = f.type;
        s->flags = f.flags;
        s->stream = f.stream;
        s->len = f.len;
        memcpy(s->payload, p->wire.p + CB_H2_FRAME_HEAD, f.len < 64 ? f.len : 64);
        if (f.type == CB_H2_HEADERS || f.type == CB_H2_CONTINUATION) {
            if (f.type == CB_H2_HEADERS) { cb_buf_reset(&p->block); p->block_stream = f.stream; }
            cb_buf_append(&p->block, p->wire.p + CB_H2_FRAME_HEAD, f.len);
            if (f.flags & CB_H2_F_END_HEADERS)
                T_EQ_INT(cb_hpack_decode(&p->dec, p->block.p, p->block.len, sf_field, s), CB_OK);
        }
        cb_buf_consume(&p->wire, (size_t)k);
    }
}

static void peer_init(peer *p, const cb_h2_limits *lim) {
    memset(p, 0, sizeof *p);
    p->c = cb_h2_sess_new(lim, &CB);
    T_CHECK(p->c != NULL);
    T_EQ_INT(cb_hpack_dec_init(&p->dec, CB_HPACK_TABLE_DEFAULT), CB_OK);
    cb_buf_init(&p->wire);
    cb_buf_init(&p->block);
}

static void peer_free(peer *p) {
    cb_h2_sess_free(p->c);
    cb_hpack_dec_free(&p->dec);
    cb_buf_free(&p->wire);
    cb_buf_free(&p->block);
}

// Sends bytes to the client, in one piece or a byte at a time.
static int deliver(peer *p, const cb_buf *b, bool bytewise) {
    int rc = CB_OK;
    if (!bytewise) return cb_h2_sess_recv(p->c, b->p, b->len);
    for (size_t i = 0; i < b->len && rc == CB_OK; i++) rc = cb_h2_sess_recv(p->c, b->p + i, 1);
    return rc;
}

static void s_settings(cb_buf *b, const uint16_t *ids, const uint32_t *v, int n) { cb_h2_frame_settings(b, ids, v, n, false); }

// A response header block: "name=value;..." pairs, Huffman and indexing as a
// real server's would (incremental indexing, so the client's table fills).
static void s_headers(cb_buf *b, uint32_t stream, const char *fields, bool end_stream, uint32_t max_frame) {
    cb_buf block;
    cb_buf_init(&block);
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", fields);
    for (char *save = NULL, *kv = strtok_r(tmp, ";", &save); kv; kv = strtok_r(NULL, ";", &save)) {
        char *eq = strchr(kv, '=');
        size_t nl = (size_t)(eq - kv), vl = strlen(eq + 1);
        // Literal with incremental indexing, new name (0x40), Huffman strings.
        cb_buf_putc(&block, 0x40);
        cb_hpack_int_encode(&block, 0x80, 7, (uint32_t)cb_hpack_huff_len((const uint8_t *)kv, nl));
        cb_hpack_huff_encode(&block, (const uint8_t *)kv, nl);
        cb_hpack_int_encode(&block, 0x80, 7, (uint32_t)cb_hpack_huff_len((const uint8_t *)eq + 1, vl));
        cb_hpack_huff_encode(&block, (const uint8_t *)eq + 1, vl);
    }
    cb_h2_frame_headers(b, stream, block.p, block.len, end_stream, max_frame);
    cb_buf_free(&block);
}

static void s_data(cb_buf *b, uint32_t stream, const char *s, size_t n, bool end) {
    cb_h2_frame_head(b, (uint32_t)n, CB_H2_DATA, end ? CB_H2_F_END_STREAM : 0, stream);
    cb_buf_append(b, s, n);
}

// The server's preface: an empty SETTINGS (or the given ones).
static void s_preface(peer *p, const uint16_t *ids, const uint32_t *v, int n) {
    cb_buf b;
    cb_buf_init(&b);
    s_settings(&b, ids, v, n);
    T_EQ_INT(deliver(p, &b, false), CB_OK);
    cb_buf_free(&b);
}

static int count(const peer *p, uint8_t type) {
    int n = 0;
    for (int i = 0; i < p->n_got; i++) n += p->got[i].type == type;
    return n;
}

static const sframe *last_of(const peer *p, uint8_t type) {
    for (int i = p->n_got - 1; i >= 0; i--)
        if (p->got[i].type == type) return &p->got[i];
    return NULL;
}

static uint32_t u32_of(const sframe *f, int at) {
    return (uint32_t)f->payload[at] << 24 | (uint32_t)f->payload[at + 1] << 16 | (uint32_t)f->payload[at + 2] << 8 |
           f->payload[at + 3];
}

static cb_h2_stream *get(peer *p, req *r, const char *path, const cb_h2_iov *body, int nb) {
    cb_h2_stream *st = NULL;
    cb_h2_field f[] = { { "content-type", "application/grpc", 12, 16, false },
                        { "x-goog-api-key", "k3y", 14, 3, true } };
    T_EQ_INT(cb_h2_sess_submit(p->c, nb ? "POST" : "GET", "https", "example.com", path, f, 2, body, nb, r, &st),
             CB_OK);
    return st;
}

// ---- the tests ---------------------------------------------------------------------
static void preface(void) {
    peer p;
    cb_h2_limits lim;
    cb_h2_limits_default(&lim);
    peer_init(&p, &lim);
    pump(&p);
    T_CHECK(p.preface_ok);
    T_EQ_INT(p.got[0].type, CB_H2_SETTINGS);
    T_EQ_INT(p.got[0].len, 18);
    // ENABLE_PUSH 0, INITIAL_WINDOW_SIZE, MAX_HEADER_LIST_SIZE.
    T_EQ_INT(p.got[0].payload[1], CB_H2_S_ENABLE_PUSH);
    T_EQ_INT(u32_of(&p.got[0], 2), 0);
    T_EQ_INT(p.got[0].payload[7], CB_H2_S_INITIAL_WINDOW_SIZE);
    T_EQ_INT(u32_of(&p.got[0], 8), lim.stream_window);
    T_EQ_INT(p.got[1].type, CB_H2_WINDOW_UPDATE);
    T_EQ_INT(u32_of(&p.got[1], 0), lim.conn_window - CB_H2_DEFAULT_WINDOW);
    peer_free(&p);
}

// A request, its headers seen by the peer, and a response of headers, body
// and trailers, delivered whole and then a byte at a time.
static void response(void) {
    for (int bytewise = 0; bytewise < 2; bytewise++) {
        peer p;
        peer_init(&p, NULL);
        req r;
        rq_init(&r);
        get(&p, &r, "/v1/x", NULL, 0);
        pump(&p);
        const sframe *h = last_of(&p, CB_H2_HEADERS);
        T_CHECK(h != NULL);
        T_EQ_INT(h->stream, 1);
        T_CHECK(h->flags & CB_H2_F_END_STREAM);
        T_EQ_STR(h->fields,
                 ":method=GET;:scheme=https;:authority=example.com;:path=/v1/x;content-type=application/grpc;"
                 "x-goog-api-key=k3y!;");
        cb_buf b;
        cb_buf_init(&b);
        s_settings(&b, NULL, NULL, 0);
        s_headers(&b, 1, ":status=200;content-type=application/grpc", false, CB_H2_DEFAULT_FRAME);
        s_data(&b, 1, "hello ", 6, false);
        s_data(&b, 1, "world", 5, false);
        s_headers(&b, 1, "grpc-status=0;grpc-message=ok", true, CB_H2_DEFAULT_FRAME);
        T_EQ_INT(deliver(&p, &b, bytewise), CB_OK);
        T_CHECK(r.ended && r.end.ok);
        T_EQ_STR(r.headers, ":status=200;content-type=application/grpc;");
        T_EQ_STR(r.trailers, "grpc-status=0;grpc-message=ok;");
        T_EQ_MEM(r.body.p, r.body.len, "hello world", 11);
        T_EQ_INT(cb_h2_sess_streams(p.c), 0);
        pump(&p);
        T_EQ_INT(count(&p, CB_H2_SETTINGS), 2);   // ours, and the ACK of theirs
        T_CHECK(last_of(&p, CB_H2_SETTINGS)->flags & CB_H2_F_ACK);
        T_EQ_INT(count(&p, CB_H2_RST_STREAM), 0);
        cb_buf_free(&b);
        cb_buf_free(&r.body);
        peer_free(&p);
    }
}

// A trailers-only answer, a 1xx before the real headers, and a header block in
// three pieces (HEADERS + 2 CONTINUATION).
static void header_shapes(void) {
    peer p;
    peer_init(&p, NULL);
    req a, b2, c;
    rq_init(&a);
    rq_init(&b2);
    rq_init(&c);
    get(&p, &a, "/a", NULL, 0);
    get(&p, &b2, "/b", NULL, 0);
    get(&p, &c, "/c", NULL, 0);
    pump(&p);
    cb_buf b;
    cb_buf_init(&b);
    s_settings(&b, NULL, NULL, 0);
    s_headers(&b, 1, ":status=200;grpc-status=14;grpc-message=unavailable", true, CB_H2_DEFAULT_FRAME);
    s_headers(&b, 3, ":status=103;link=x", false, CB_H2_DEFAULT_FRAME);
    s_headers(&b, 3, ":status=204", true, CB_H2_DEFAULT_FRAME);
    s_headers(&b, 5, ":status=200;a-long-header-name=a-long-value-that-spans;another=one", true, 12);
    T_EQ_INT(deliver(&p, &b, false), CB_OK);
    T_CHECK(a.ended && a.end.ok);
    T_EQ_STR(a.headers, ":status=200;grpc-status=14;grpc-message=unavailable;");
    T_EQ_STR(a.trailers, "");
    T_CHECK(b2.ended && b2.end.ok);
    T_EQ_STR(b2.headers, ":status=204;");   // the 103 is dropped whole
    T_CHECK(c.ended && c.end.ok);
    T_EQ_STR(c.headers, ":status=200;a-long-header-name=a-long-value-that-spans;another=one;");
    cb_buf_free(&b);
    cb_buf_free(&a.body);
    cb_buf_free(&b2.body);
    cb_buf_free(&c.body);
    peer_free(&p);
}

// A request body sent within the peer's windows: a stream window of 10, a
// connection window opened by WINDOW_UPDATE, and an initial window change.
static void send_flow_control(void) {
    peer p;
    peer_init(&p, NULL);
    req r;
    rq_init(&r);
    char body[25];
    for (int i = 0; i < 25; i++) body[i] = (char)('a' + i);
    cb_h2_iov iov[2] = { { body, 12 }, { body + 12, 13 } };
    const uint16_t ids[] = { CB_H2_S_INITIAL_WINDOW_SIZE };
    const uint32_t v[] = { 10 };
    s_preface(&p, ids, v, 1);
    get(&p, &r, "/up", iov, 2);
    pump(&p);
    const sframe *h = last_of(&p, CB_H2_HEADERS);
    T_CHECK(!(h->flags & CB_H2_F_END_STREAM));
    T_EQ_INT(count(&p, CB_H2_DATA), 1);
    const sframe *d = last_of(&p, CB_H2_DATA);
    T_EQ_INT(d->len, 10);
    T_EQ_MEM(d->payload, 10, "abcdefghij", 10);
    T_CHECK(!cb_h2_sess_want_flush(p.c));
    // The window opens by 8: 8 more.
    cb_buf b;
    cb_buf_init(&b);
    cb_h2_frame_u32(&b, CB_H2_WINDOW_UPDATE, 1, 8);
    T_EQ_INT(deliver(&p, &b, false), CB_OK);
    pump(&p);
    T_EQ_INT(count(&p, CB_H2_DATA), 2);
    T_EQ_INT(last_of(&p, CB_H2_DATA)->len, 8);
    T_EQ_MEM(last_of(&p, CB_H2_DATA)->payload, 8, "klmnopqr", 8);
    // The initial window rises to 20: the stream's window grows by 10, and the last 7 go, with END_STREAM.
    cb_buf_reset(&b);
    const uint32_t v2[] = { 20 };
    s_settings(&b, ids, v2, 1);
    T_EQ_INT(deliver(&p, &b, false), CB_OK);
    pump(&p);
    T_EQ_INT(count(&p, CB_H2_DATA), 3);
    d = last_of(&p, CB_H2_DATA);
    T_EQ_INT(d->len, 7);
    T_CHECK(d->flags & CB_H2_F_END_STREAM);
    T_EQ_MEM(d->payload, 7, "stuvwxy", 7);
    T_EQ_INT(cb_h2_sess_peer_setting(p.c, CB_H2_S_INITIAL_WINDOW_SIZE), 20);
    cb_buf_free(&b);
    cb_buf_free(&r.body);
    peer_free(&p);
}

// The connection's own send window: 65535 until the peer opens it.
static void send_conn_window(void) {
    peer p;
    peer_init(&p, NULL);
    // The stream's window is a MB, so the connection's 65535 is what binds.
    const uint16_t ids[] = { CB_H2_S_INITIAL_WINDOW_SIZE };
    const uint32_t v[] = { 1u << 20 };
    s_preface(&p, ids, v, 1);
    static char big[200000];
    memset(big, 'z', sizeof big);
    cb_h2_iov iov = { big, sizeof big };
    req r;
    rq_init(&r);
    get(&p, &r, "/big", &iov, 1);
    pump(&p);
    // The connection's window is spent.
    size_t sent = 0;
    for (int i = 0; i < p.n_got; i++) {
        if (p.got[i].type != CB_H2_DATA) continue;
        T_CHECK(p.got[i].len <= CB_H2_DEFAULT_FRAME);
        sent += p.got[i].len;
    }
    T_EQ_INT((int)sent, CB_H2_DEFAULT_WINDOW);
    cb_buf b;
    cb_buf_init(&b);
    cb_h2_frame_u32(&b, CB_H2_WINDOW_UPDATE, 0, 50000);   // the connection opens
    T_EQ_INT(deliver(&p, &b, false), CB_OK);
    pump(&p);
    size_t sent2 = 0;
    for (int i = 0; i < p.n_got; i++)
        if (p.got[i].type == CB_H2_DATA) sent2 += p.got[i].len;
    T_EQ_INT((int)sent2, CB_H2_DEFAULT_WINDOW + 50000);
    cb_buf_reset(&b);
    cb_h2_frame_u32(&b, CB_H2_WINDOW_UPDATE, 0, 100000);
    T_EQ_INT(deliver(&p, &b, false), CB_OK);
    pump(&p);
    sent2 = 0;
    for (int i = 0; i < p.n_got; i++)
        if (p.got[i].type == CB_H2_DATA) sent2 += p.got[i].len;
    T_EQ_INT((int)sent2, (int)sizeof big);
    T_CHECK(last_of(&p, CB_H2_DATA)->flags & CB_H2_F_END_STREAM);
    cb_buf_free(&b);
    cb_buf_free(&r.body);
    peer_free(&p);
}

// One flush writes at most 64 KB of DATA, so the I/O loop reads between
// writes, and says there is more.
static void flush_budget(void) {
    peer p;
    peer_init(&p, NULL);
    // Frames of 20000 do not divide the budget, so the last frame of a flush is cut to fit.
    const uint16_t ids[] = { CB_H2_S_INITIAL_WINDOW_SIZE, CB_H2_S_MAX_FRAME_SIZE };
    const uint32_t v[] = { 1u << 20, 20000 };
    s_preface(&p, ids, v, 2);
    cb_buf b;
    cb_buf_init(&b);
    cb_h2_frame_u32(&b, CB_H2_WINDOW_UPDATE, 0, 1u << 20);
    T_EQ_INT(deliver(&p, &b, false), CB_OK);
    static char big[300000];
    cb_h2_iov iov = { big, sizeof big };
    req r;
    rq_init(&r);
    get(&p, &r, "/big", &iov, 1);
    cb_buf out;
    cb_buf_init(&out);
    cb_h2_sess_flush(p.c, &out);
    size_t data = 0;
    for (size_t at = 0; at < out.len;) {
        if (at == 0 && out.len >= CB_H2_PREFACE_LEN && memcmp(out.p, CB_H2_PREFACE, CB_H2_PREFACE_LEN) == 0) {
            at = CB_H2_PREFACE_LEN;
            continue;
        }
        cb_h2_frame f;
        int k = cb_h2_frame_parse(out.p + at, out.len - at, CB_H2_MAX_FRAME, &f);
        T_CHECK(k > 0);
        if (k <= 0) break;
        if (f.type == CB_H2_DATA) data += f.len;
        at += (size_t)k;
    }
    T_EQ_INT((int)data, 64 << 10);
    T_CHECK(cb_h2_sess_want_flush(p.c));
    cb_buf_free(&out);
    cb_buf_free(&b);
    cb_buf_free(&r.body);
    peer_free(&p);
}

// Receiving: WINDOW_UPDATEs once half a window is consumed, and DATA past a
// stream's window resets it.
static void recv_flow_control(void) {
    cb_h2_limits lim;
    cb_h2_limits_default(&lim);
    lim.stream_window = CB_H2_DEFAULT_WINDOW;
    lim.conn_window = 200000;
    peer p;
    peer_init(&p, &lim);
    s_preface(&p, NULL, NULL, 0);
    req r, r2;
    rq_init(&r);
    rq_init(&r2);
    get(&p, &r, "/dl", NULL, 0);
    get(&p, &r2, "/dl2", NULL, 0);
    pump(&p);
    int wu0 = count(&p, CB_H2_WINDOW_UPDATE);   // the preface's
    cb_buf b;
    cb_buf_init(&b);
    static char chunk[16000];
    memset(chunk, 'q', sizeof chunk);
    s_headers(&b, 1, ":status=200", false, CB_H2_DEFAULT_FRAME);
    s_headers(&b, 3, ":status=200", false, CB_H2_DEFAULT_FRAME);
    s_data(&b, 1, chunk, sizeof chunk, false);
    T_EQ_INT(deliver(&p, &b, false), CB_OK);
    pump(&p);
    T_EQ_INT(count(&p, CB_H2_WINDOW_UPDATE), wu0);   // 16000 is under half a window: nothing given back yet
    cb_buf_reset(&b);
    for (int i = 0; i < 2; i++) s_data(&b, 1, chunk, sizeof chunk, false);   // 48000 >= 65535 / 2
    T_EQ_INT(deliver(&p, &b, false), CB_OK);
    pump(&p);
    const sframe *wu = last_of(&p, CB_H2_WINDOW_UPDATE);
    T_EQ_INT(count(&p, CB_H2_WINDOW_UPDATE), wu0 + 1);
    T_EQ_INT(wu->stream, 1);
    T_EQ_INT(u32_of(wu, 0), 48000);
    // Stream 3: 65535 and one more byte, in one batch, is past its window.
    cb_buf_reset(&b);
    for (int i = 0; i < 4; i++) s_data(&b, 3, chunk, sizeof chunk, false);
    s_data(&b, 3, chunk, 1536, false);
    T_EQ_INT(deliver(&p, &b, false), CB_OK);
    T_CHECK(r2.ended && !r2.end.ok);
    T_EQ_INT(r2.end.h2_code, CB_H2_FLOW_CONTROL_ERROR);
    pump(&p);
    const sframe *rst = last_of(&p, CB_H2_RST_STREAM);
    T_EQ_INT(rst->stream, 3);
    T_EQ_INT(u32_of(rst, 0), CB_H2_FLOW_CONTROL_ERROR);
    T_CHECK(!cb_h2_sess_dead(p.c));
    // The connection: 48000 + 65536 consumed, over half of 200000, was given back in one update.
    bool conn_wu = false;
    for (int i = 0; i < p.n_got; i++)
        if (p.got[i].type == CB_H2_WINDOW_UPDATE && p.got[i].stream == 0 && u32_of(&p.got[i], 0) == 113536)
            conn_wu = true;
    T_CHECK(conn_wu);
    cb_buf_free(&b);
    cb_buf_free(&r.body);
    cb_buf_free(&r2.body);
    peer_free(&p);

    // Past the connection's window is a connection error.
    lim.conn_window = CB_H2_DEFAULT_WINDOW;
    lim.stream_window = 1 << 20;
    peer_init(&p, &lim);
    s_preface(&p, NULL, NULL, 0);
    rq_init(&r);
    get(&p, &r, "/dl", NULL, 0);
    pump(&p);
    cb_buf_init(&b);
    s_headers(&b, 1, ":status=200", false, CB_H2_DEFAULT_FRAME);
    for (int i = 0; i < 4; i++) s_data(&b, 1, chunk, sizeof chunk, false);
    s_data(&b, 1, chunk, 1536, false);
    T_EQ_INT(deliver(&p, &b, false), CB_E_IO);
    T_CHECK(cb_h2_sess_dead(p.c));
    T_CHECK(r.ended && r.end.h2_code == CB_H2_FLOW_CONTROL_ERROR);
    pump(&p);
    T_EQ_INT(u32_of(last_of(&p, CB_H2_GOAWAY), 4), CB_H2_FLOW_CONTROL_ERROR);
    cb_buf_free(&b);
    cb_buf_free(&r.body);
    peer_free(&p);
}

// MAX_CONCURRENT_STREAMS 2: five requests open two at a time, in order.
static void concurrency(void) {
    peer p;
    peer_init(&p, NULL);
    const uint16_t ids[] = { CB_H2_S_MAX_CONCURRENT_STREAMS };
    const uint32_t v[] = { 2 };
    s_preface(&p, ids, v, 1);
    req r[5];
    for (int i = 0; i < 5; i++) {
        rq_init(&r[i]);
        get(&p, &r[i], "/n", NULL, 0);
    }
    pump(&p);
    T_EQ_INT(count(&p, CB_H2_HEADERS), 2);
    T_EQ_INT(cb_h2_sess_streams(p.c), 5);
    cb_buf b;
    cb_buf_init(&b);
    s_headers(&b, 3, ":status=200", true, CB_H2_DEFAULT_FRAME);   // the second ends first
    T_EQ_INT(deliver(&p, &b, false), CB_OK);
    T_CHECK(r[1].ended && !r[0].ended);
    pump(&p);
    T_EQ_INT(count(&p, CB_H2_HEADERS), 3);
    T_EQ_INT(last_of(&p, CB_H2_HEADERS)->stream, 5);
    cb_buf_reset(&b);
    s_headers(&b, 1, ":status=200", true, CB_H2_DEFAULT_FRAME);
    s_headers(&b, 5, ":status=200", true, CB_H2_DEFAULT_FRAME);
    T_EQ_INT(deliver(&p, &b, false), CB_OK);
    pump(&p);
    T_EQ_INT(count(&p, CB_H2_HEADERS), 5);
    T_EQ_INT(last_of(&p, CB_H2_HEADERS)->stream, 9);
    for (int i = 0; i < 5; i++) cb_buf_free(&r[i].body);
    cb_buf_free(&b);
    peer_free(&p);
}

// Resets, GOAWAY and PING.
static void resets_goaway_ping(void) {
    peer p;
    peer_init(&p, NULL);
    s_preface(&p, NULL, NULL, 0);
    req a, b2, c;
    rq_init(&a);
    rq_init(&b2);
    rq_init(&c);
    get(&p, &a, "/a", NULL, 0);
    get(&p, &b2, "/b", NULL, 0);
    get(&p, &c, "/c", NULL, 0);
    pump(&p);
    cb_buf b;
    cb_buf_init(&b);
    cb_h2_frame_u32(&b, CB_H2_RST_STREAM, 1, CB_H2_REFUSED_STREAM);
    uint8_t opaque[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    cb_h2_frame_ping(&b, opaque, false);
    cb_h2_frame_goaway(&b, 3, CB_H2_NO_ERROR);   // 5 was never processed
    T_EQ_INT(deliver(&p, &b, false), CB_OK);
    T_CHECK(a.ended && !a.end.ok && a.end.by_peer && a.end.unprocessed);
    T_EQ_INT(a.end.h2_code, CB_H2_REFUSED_STREAM);
    T_CHECK(c.ended && c.end.unprocessed && c.end.by_peer);
    T_CHECK(!b2.ended);
    T_CHECK(!cb_h2_sess_accepting(p.c));
    req d;
    rq_init(&d);
    cb_h2_stream *st;
    T_EQ_INT(cb_h2_sess_submit(p.c, "GET", "https", "x", "/", NULL, 0, NULL, 0, &d, &st), CB_E_STATE);
    pump(&p);
    const sframe *pong = last_of(&p, CB_H2_PING);
    T_CHECK(pong && (pong->flags & CB_H2_F_ACK));
    T_EQ_MEM(pong->payload, 8, opaque, 8);
    // Stream 3 still finishes.
    cb_buf_reset(&b);
    s_headers(&b, 3, ":status=200", true, CB_H2_DEFAULT_FRAME);
    T_EQ_INT(deliver(&p, &b, false), CB_OK);
    T_CHECK(b2.ended && b2.end.ok);
    T_EQ_INT(cb_h2_sess_streams(p.c), 0);
    // A reset of an ordinary kind is not "unprocessed".
    cb_buf_free(&b);
    cb_buf_free(&a.body);
    cb_buf_free(&b2.body);
    cb_buf_free(&c.body);
    cb_buf_free(&d.body);
    peer_free(&p);
}

// Cancel: a RST_STREAM goes out, later frames for the stream are dropped, the
// connection window still counts them, and the HPACK table stays in step.
static void cancel(void) {
    peer p;
    peer_init(&p, NULL);
    s_preface(&p, NULL, NULL, 0);
    req a, b2;
    rq_init(&a);
    rq_init(&b2);
    cb_h2_stream *sa = get(&p, &a, "/a", NULL, 0);
    get(&p, &b2, "/b", NULL, 0);
    pump(&p);
    cb_h2_sess_cancel(p.c, sa);
    pump(&p);
    const sframe *rst = last_of(&p, CB_H2_RST_STREAM);
    T_EQ_INT(rst->stream, 1);
    T_EQ_INT(u32_of(rst, 0), CB_H2_CANCEL);
    cb_buf b;
    cb_buf_init(&b);
    // The peer had already answered 1, indexing "x-new=v1"; 3's block names it by index 62.
    s_headers(&b, 1, ":status=200;x-new=v1", false, CB_H2_DEFAULT_FRAME);
    s_data(&b, 1, "dropped", 7, true);
    cb_buf block;
    cb_buf_init(&block);
    cb_buf_putc(&block, (char)0x88);   // :status 200 (static 8)
    cb_buf_putc(&block, (char)0xbe);   // 62: the newest entry, "x-new: v1"
    cb_h2_frame_headers(&b, 3, block.p, block.len, true, CB_H2_DEFAULT_FRAME);
    T_EQ_INT(deliver(&p, &b, false), CB_OK);
    T_CHECK(!a.ended);   // no callback after cancel
    T_CHECK(b2.ended && b2.end.ok);
    T_EQ_STR(b2.headers, ":status=200;x-new=v1;");
    cb_buf_free(&block);
    cb_buf_free(&b);
    cb_buf_free(&a.body);
    cb_buf_free(&b2.body);
    peer_free(&p);
}

// Stream errors: each resets one stream, and the connection lives on.
static void stream_errors(void) {
    static const struct {
        const char *fields;
        bool        trailers_first;   // send ":status=200" first, then fields as trailers
        uint32_t    code;
    } cases[] = {
        { ":status=200;Upper=x", false, CB_H2_PROTOCOL_ERROR },
        { "content-type=x", false, CB_H2_PROTOCOL_ERROR },              // no :status
        { ":status=200;:path=/", false, CB_H2_PROTOCOL_ERROR },         // a request's pseudo-header
        { "x=1;:status=200", false, CB_H2_PROTOCOL_ERROR },             // a pseudo-header after a field
        { ":status=101", false, CB_H2_PROTOCOL_ERROR },
        { ":status=2000", false, CB_H2_PROTOCOL_ERROR },
        { ":status=200;connection=close", false, CB_H2_PROTOCOL_ERROR },
        { ":status=200", true, CB_H2_PROTOCOL_ERROR },                  // a pseudo-header in trailers
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        peer p;
        peer_init(&p, NULL);
        s_preface(&p, NULL, NULL, 0);
        req r;
        rq_init(&r);
        get(&p, &r, "/", NULL, 0);
        pump(&p);
        cb_buf b;
        cb_buf_init(&b);
        if (cases[i].trailers_first) s_headers(&b, 1, ":status=200", false, CB_H2_DEFAULT_FRAME);
        s_headers(&b, 1, cases[i].fields, true, CB_H2_DEFAULT_FRAME);
        T_EQ_INT(deliver(&p, &b, false), CB_OK);
        T_CHECK(r.ended && !r.end.ok && !r.end.by_peer);
        T_EQ_INT(r.end.h2_code, cases[i].code);
        if (cases[i].fields[0] != ':') T_EQ_STR(r.headers, "");   // no field reaches the caller before :status
        T_CHECK(!cb_h2_sess_dead(p.c));
        pump(&p);
        T_EQ_INT(u32_of(last_of(&p, CB_H2_RST_STREAM), 0), cases[i].code);
        cb_buf_free(&b);
        cb_buf_free(&r.body);
        peer_free(&p);
    }
    // DATA before HEADERS; trailers without END_STREAM; the caller refusing a
    // header or the body; a header list over the limit.
    for (int k = 0; k < 5; k++) {
        cb_h2_limits lim;
        cb_h2_limits_default(&lim);
        lim.max_header_list = 100;
        peer p;
        peer_init(&p, &lim);
        s_preface(&p, NULL, NULL, 0);
        req r;
        rq_init(&r);
        if (k == 2) r.refuse_header_at = 1;
        if (k == 3) r.refuse_data = 1;
        get(&p, &r, "/", NULL, 0);
        pump(&p);
        cb_buf b;
        cb_buf_init(&b);
        if (k == 0) s_data(&b, 1, "x", 1, true);
        if (k == 1) {
            s_headers(&b, 1, ":status=200", false, CB_H2_DEFAULT_FRAME);
            s_headers(&b, 1, "grpc-status=0", false, CB_H2_DEFAULT_FRAME);
        }
        if (k == 2) s_headers(&b, 1, ":status=200", true, CB_H2_DEFAULT_FRAME);
        if (k == 3) { s_headers(&b, 1, ":status=200", false, CB_H2_DEFAULT_FRAME); s_data(&b, 1, "x", 1, true); }
        if (k == 4)
            s_headers(&b, 1, ":status=200;x-a=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa;x-b=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", true,
                      CB_H2_DEFAULT_FRAME);
        T_EQ_INT(deliver(&p, &b, false), CB_OK);
        T_CHECK(r.ended && !r.end.ok);
        T_EQ_INT(r.end.h2_code, k < 2 ? CB_H2_PROTOCOL_ERROR : CB_H2_CANCEL);
        T_CHECK(!cb_h2_sess_dead(p.c));
        cb_buf_free(&b);
        cb_buf_free(&r.body);
        peer_free(&p);
    }
}

// The response ends while the request is still going out: the rest is cancelled.
static void early_response(void) {
    peer p;
    peer_init(&p, NULL);
    const uint16_t ids[] = { CB_H2_S_INITIAL_WINDOW_SIZE };
    const uint32_t v[] = { 4 };
    s_preface(&p, ids, v, 1);
    req r;
    rq_init(&r);
    cb_h2_iov iov = { "0123456789", 10 };
    get(&p, &r, "/", &iov, 1);
    pump(&p);
    cb_buf b;
    cb_buf_init(&b);
    s_headers(&b, 1, ":status=413", true, CB_H2_DEFAULT_FRAME);
    T_EQ_INT(deliver(&p, &b, false), CB_OK);
    T_CHECK(r.ended && r.end.ok);
    pump(&p);
    const sframe *rst = last_of(&p, CB_H2_RST_STREAM);
    T_CHECK(rst && rst->stream == 1);
    T_EQ_INT(u32_of(rst, 0), CB_H2_CANCEL);
    cb_buf_free(&b);
    cb_buf_free(&r.body);
    peer_free(&p);
}

// Connection errors: each kills the session, ends the stream, and queues a
// GOAWAY with the code.
typedef void (*build_fn)(cb_buf *b);
static void ce_not_settings(cb_buf *b) { cb_h2_frame_u32(b, CB_H2_WINDOW_UPDATE, 0, 1); }
static void ce_push(cb_buf *b) { s_settings(b, NULL, NULL, 0); cb_h2_frame_head(b, 4, CB_H2_PUSH_PROMISE, CB_H2_F_END_HEADERS, 1); cb_buf_append(b, "\0\0\0\2", 4); }
static void ce_big(cb_buf *b) { s_settings(b, NULL, NULL, 0); cb_h2_frame_head(b, CB_H2_DEFAULT_FRAME + 1, CB_H2_DATA, 0, 1); }
static void ce_idle(cb_buf *b) { s_settings(b, NULL, NULL, 0); s_headers(b, 7, ":status=200", true, CB_H2_DEFAULT_FRAME); }
static void ce_even(cb_buf *b) { s_settings(b, NULL, NULL, 0); s_headers(b, 2, ":status=200", true, CB_H2_DEFAULT_FRAME); }
// Stream 2 is below the last opened (3), so only its evenness makes it one this side never opened.
static void ce_even_low(cb_buf *b) { s_settings(b, NULL, NULL, 0); cb_h2_frame_u32(b, CB_H2_WINDOW_UPDATE, 2, 1); }
static void ce_hpack(cb_buf *b) { s_settings(b, NULL, NULL, 0); cb_h2_frame_head(b, 1, CB_H2_HEADERS, CB_H2_F_END_HEADERS, 1); cb_buf_putc(b, (char)0x80); }
static void ce_enable_push(cb_buf *b) { const uint16_t i[] = { CB_H2_S_ENABLE_PUSH }; const uint32_t v[] = { 1 }; s_settings(b, i, v, 1); }
static void ce_window(cb_buf *b) { const uint16_t i[] = { CB_H2_S_INITIAL_WINDOW_SIZE }; const uint32_t v[] = { 0x80000000u }; s_settings(b, i, v, 1); }
static void ce_frame_size(cb_buf *b) { const uint16_t i[] = { CB_H2_S_MAX_FRAME_SIZE }; const uint32_t v[] = { 100 }; s_settings(b, i, v, 1); }
static void ce_settings_len(cb_buf *b) { cb_h2_frame_head(b, 5, CB_H2_SETTINGS, 0, 0); cb_buf_append(b, "\0\0\0\0\0", 5); }
static void ce_cont(cb_buf *b) { s_settings(b, NULL, NULL, 0); cb_h2_frame_head(b, 1, CB_H2_CONTINUATION, CB_H2_F_END_HEADERS, 1); cb_buf_putc(b, (char)0x88); }
static void ce_interleave(cb_buf *b) {
    s_settings(b, NULL, NULL, 0);
    cb_h2_frame_head(b, 1, CB_H2_HEADERS, 0, 1);
    cb_buf_putc(b, (char)0x88);
    uint8_t o[8] = { 0 };
    cb_h2_frame_ping(b, o, false);
}
static void ce_wu_zero(cb_buf *b) { s_settings(b, NULL, NULL, 0); cb_h2_frame_u32(b, CB_H2_WINDOW_UPDATE, 0, 0); }
static void ce_wu_over(cb_buf *b) { s_settings(b, NULL, NULL, 0); cb_h2_frame_u32(b, CB_H2_WINDOW_UPDATE, 0, 0x7fffffffu); }
static void ce_ping_len(cb_buf *b) { s_settings(b, NULL, NULL, 0); cb_h2_frame_head(b, 4, CB_H2_PING, 0, 0); cb_buf_append(b, "1234", 4); }
static void ce_rst_zero(cb_buf *b) { s_settings(b, NULL, NULL, 0); cb_h2_frame_u32(b, CB_H2_RST_STREAM, 0, 1); }
static void ce_pad(cb_buf *b) { s_settings(b, NULL, NULL, 0); s_headers(b, 1, ":status=200", false, CB_H2_DEFAULT_FRAME); cb_h2_frame_head(b, 2, CB_H2_DATA, CB_H2_F_PADDED, 1); cb_buf_append(b, "\x05x", 2); }

static void connection_errors(void) {
    static const struct {
        build_fn f;
        uint32_t code;
    } cases[] = {
        { ce_not_settings, CB_H2_PROTOCOL_ERROR }, { ce_push, CB_H2_PROTOCOL_ERROR },
        { ce_big, CB_H2_FRAME_SIZE_ERROR },        { ce_idle, CB_H2_PROTOCOL_ERROR },
        { ce_even, CB_H2_PROTOCOL_ERROR },         { ce_hpack, CB_H2_COMPRESSION_ERROR },
        { ce_enable_push, CB_H2_PROTOCOL_ERROR },  { ce_window, CB_H2_FLOW_CONTROL_ERROR },
        { ce_frame_size, CB_H2_PROTOCOL_ERROR },   { ce_settings_len, CB_H2_FRAME_SIZE_ERROR },
        { ce_cont, CB_H2_PROTOCOL_ERROR },         { ce_interleave, CB_H2_PROTOCOL_ERROR },
        { ce_wu_zero, CB_H2_PROTOCOL_ERROR },      { ce_wu_over, CB_H2_FLOW_CONTROL_ERROR },
        { ce_ping_len, CB_H2_FRAME_SIZE_ERROR },   { ce_rst_zero, CB_H2_PROTOCOL_ERROR },
        { ce_pad, CB_H2_PROTOCOL_ERROR },          { ce_even_low, CB_H2_PROTOCOL_ERROR },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        peer p;
        peer_init(&p, NULL);
        req r, r3;
        rq_init(&r);
        rq_init(&r3);
        get(&p, &r, "/", NULL, 0);
        get(&p, &r3, "/3", NULL, 0);   // stream 3, so 2 is below the last opened
        pump(&p);
        cb_buf b;
        cb_buf_init(&b);
        cases[i].f(&b);
        t_current = "connection_errors";
        T_EQ_INT(deliver(&p, &b, false), CB_E_IO);
        T_CHECK(cb_h2_sess_dead(p.c));
        T_CHECK(r.ended && !r.end.ok);
        T_EQ_INT(r.end.h2_code, cases[i].code);
        pump(&p);
        const sframe *g = last_of(&p, CB_H2_GOAWAY);
        T_CHECK(g != NULL);
        if (g) T_EQ_INT(u32_of(g, 4), cases[i].code);
        if (r.end.h2_code != cases[i].code) fprintf(stderr, "     case %zu\n", i);
        T_EQ_INT(cb_h2_sess_recv(p.c, (const uint8_t *)"x", 1), CB_E_IO);
        cb_buf_free(&b);
        cb_buf_free(&r.body);
        cb_buf_free(&r3.body);
        peer_free(&p);
    }
}

// Submit's refusals, and fail() from this side.
static void submit_and_fail(void) {
    cb_h2_limits lim;
    cb_h2_limits_default(&lim);
    lim.max_streams = 2;
    peer p;
    peer_init(&p, &lim);
    req r[3];
    cb_h2_stream *st;
    for (int i = 0; i < 3; i++) rq_init(&r[i]);
    cb_h2_field upper = { "X-Key", "v", 5, 1, false };
    T_EQ_INT(cb_h2_sess_submit(p.c, "GET", "https", "x", "/", &upper, 1, NULL, 0, &r[0], &st), CB_E_INVALID);
    cb_h2_field conn = { "connection", "close", 10, 5, false };
    T_EQ_INT(cb_h2_sess_submit(p.c, "GET", "https", "x", "/", &conn, 1, NULL, 0, &r[0], &st), CB_E_INVALID);
    cb_h2_field te = { "te", "gzip", 2, 4, false };
    T_EQ_INT(cb_h2_sess_submit(p.c, "GET", "https", "x", "/", &te, 1, NULL, 0, &r[0], &st), CB_E_INVALID);
    cb_h2_field crlf = { "x-a", "a\r\nb", 3, 4, false };
    T_EQ_INT(cb_h2_sess_submit(p.c, "GET", "https", "x", "/", &crlf, 1, NULL, 0, &r[0], &st), CB_E_INVALID);
    T_EQ_INT(cb_h2_sess_submit(p.c, "GET", "https", "x", "/", NULL, 0, NULL, 0, &r[0], &st), CB_OK);
    pump(&p);   // r[0] opens
    T_EQ_INT(cb_h2_sess_submit(p.c, "GET", "https", "x", "/", NULL, 0, NULL, 0, &r[1], &st), CB_OK);
    T_EQ_INT(cb_h2_sess_submit(p.c, "GET", "https", "x", "/", NULL, 0, NULL, 0, &r[2], &st), CB_E_FULL);
    // r[1] has not been flushed: it is unprocessed; r[0] went out: it is not.
    cb_h2_sess_fail(p.c, CB_H2_INTERNAL_ERROR, "the socket closed");
    T_CHECK(r[0].ended && !r[0].end.unprocessed && !r[0].end.by_peer);
    T_CHECK(r[1].ended && r[1].end.unprocessed);
    T_EQ_STR(r[1].end.why, "the socket closed");
    T_CHECK(cb_h2_sess_dead(p.c));
    pump(&p);
    T_EQ_INT(u32_of(last_of(&p, CB_H2_GOAWAY), 4), CB_H2_INTERNAL_ERROR);
    for (int i = 0; i < 3; i++) cb_buf_free(&r[i].body);
    peer_free(&p);
}

int main(void) {
    T_RUN(preface);
    T_RUN(response);
    T_RUN(header_shapes);
    T_RUN(send_flow_control);
    T_RUN(send_conn_window);
    T_RUN(flush_budget);
    T_RUN(recv_flow_control);
    T_RUN(concurrency);
    T_RUN(resets_goaway_ping);
    T_RUN(cancel);
    T_RUN(stream_errors);
    T_RUN(early_response);
    T_RUN(connection_errors);
    T_RUN(submit_and_fail);
    return T_DONE();
}
