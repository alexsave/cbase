// unit_ws.c - the RFC 6455 frame codec, read the way a hostile peer writes it.
//
// PORTED from foolish/server/impls/native/ws_frame_test.c. The native test fed
// frames through a WsConn primed with bytes on a dead fd; the c_base codec is a
// pure function over a buffer, so the same frames go straight in.
//
// The 8-byte ("127") length form is eight bytes of attacker-controlled data.
// Assembled into a SIGNED int64 the last shift can set the sign bit: signed
// overflow, undefined behaviour, which licenses the compiler to delete the
// guard after it. `make check-asan` builds this under UBSan with
// -fno-sanitize-recover, which is what actually distinguishes the two: the
// pre-fix parser still RETURNS -1 in a debug build, so a plain return-value
// assertion would pass against the defect.
#include "t.h"

#include "net/ws.h"
#include "util/err.h"

static int hdr_127(uint8_t *out, unsigned long long len, bool masked) {
    int n = 0;
    out[n++] = 0x82;   // FIN | binary
    out[n++] = (uint8_t)((masked ? 0x80 : 0) | 127);
    for (int i = 7; i >= 0; i--) out[n++] = (uint8_t)((len >> (8 * i)) & 0xFF);
    if (masked) { out[n++] = 1; out[n++] = 2; out[n++] = 3; out[n++] = 4; }
    return n;
}

static void hostile_lengths(void) {
    uint8_t frame[4096];
    cb_ws_frame f;
    int n = hdr_127(frame, 0x8000000000000000ULL, true);
    T_EQ_INT(cb_ws_frame_parse(frame, (size_t)n, 2048, &f), -1);   // the top bit set
    n = hdr_127(frame, 0xFFFFFFFFFFFFFFFFULL, true);
    T_EQ_INT(cb_ws_frame_parse(frame, (size_t)n, 2048, &f), -1);   // every bit set
    n = hdr_127(frame, 0x8000000000000001ULL, true);
    T_EQ_INT(cb_ws_frame_parse(frame, (size_t)n, 2048, &f), -1);   // INT64_MAX + 1
    n = hdr_127(frame, 2049, true);
    T_EQ_INT(cb_ws_frame_parse(frame, (size_t)n, 2048, &f), -1);   // past the caller's limit, before a byte of it
    n = hdr_127(frame, 0x7FFFFFFFFFFFFFFFULL, true);
    T_EQ_INT(cb_ws_frame_parse(frame, (size_t)n, UINT64_MAX, &f), -1);   // too long for the int result
}

static void legal_forms(void) {
    uint8_t frame[4096];
    cb_ws_frame f;
    // The 8-byte form still works for a frame that is merely large.
    int n = hdr_127(frame, 200, true);
    // The payload 0..199, masked with the key 1 2 3 4 hdr_127 wrote.
    for (int i = 0; i < 200; i++) frame[n + i] = (uint8_t)((i & 0xFF) ^ (uint8_t)(i % 4 + 1));
    T_EQ_INT(cb_ws_frame_parse(frame, (size_t)n + 200, 2048, &f), n + 200);
    T_EQ_INT(f.len, 200);
    T_EQ_INT(f.opcode, CB_WS_OP_BIN);
    T_CHECK(f.fin && f.masked);
    cb_ws_unmask(frame + f.header_len, (size_t)f.len, f.mask);
    int same = 1;
    for (int i = 0; i < 200; i++) if (frame[n + i] != (uint8_t)i) same = 0;
    T_CHECK(same);
    // ...and is not complete one byte short.
    T_EQ_INT(cb_ws_frame_parse(frame, (size_t)n + 199, 2048, &f), 0);
    // The short form.
    uint8_t s[] = { 0x81, 3, 'a', 'b', 'c' };
    T_EQ_INT(cb_ws_frame_parse(s, sizeof s, 2048, &f), 5);
    T_EQ_INT(f.opcode, CB_WS_OP_TEXT);
    T_CHECK(!f.masked);
    // The 2-byte form.
    frame[0] = 0x82; frame[1] = 126; frame[2] = 300 >> 8; frame[3] = 300 & 0xFF;
    T_EQ_INT(cb_ws_frame_parse(frame, 4 + 300, 2048, &f), 304);
    T_EQ_INT(f.len, 300);
    // Partial headers ask for more, never error.
    for (size_t k = 0; k < 4; k++) T_EQ_INT(cb_ws_frame_parse(frame, k, 2048, &f), 0);
}

static void protocol_errors(void) {
    cb_ws_frame f;
    uint8_t rsv[] = { 0xC1, 0 };                       // RSV1 set
    T_EQ_INT(cb_ws_frame_parse(rsv, 2, 2048, &f), -1);
    uint8_t op[] = { 0x83, 0 };                        // opcode 3 is reserved
    T_EQ_INT(cb_ws_frame_parse(op, 2, 2048, &f), -1);
    uint8_t ping_frag[] = { 0x09, 0 };                 // a control frame without FIN
    T_EQ_INT(cb_ws_frame_parse(ping_frag, 2, 2048, &f), -1);
    uint8_t ping_long[] = { 0x89, 126, 0, 126 };       // a control frame over 125
    T_EQ_INT(cb_ws_frame_parse(ping_long, 4, 2048, &f), -1);
}

static void header_round_trip(void) {
    static const uint64_t lens[] = { 0, 1, 125, 126, 65535, 65536, 1u << 20 };
    for (size_t i = 0; i < sizeof lens / sizeof lens[0]; i++) {
        uint8_t hdr[14];
        const uint8_t mask[4] = { 9, 8, 7, 6 };
        size_t hn = cb_ws_frame_header(hdr, true, CB_WS_OP_BIN, lens[i], mask);
        T_EQ_INT(hn, (lens[i] <= 125 ? 2 : lens[i] <= 65535 ? 4 : 10) + 4);
        cb_ws_frame f;
        // Header only: the parse asks for the payload, and reports the header it read.
        int r = cb_ws_frame_parse(hdr, hn, UINT32_MAX, &f);
        T_EQ_INT(r, lens[i] == 0 ? (int)hn : 0);
        T_EQ_INT(f.len, lens[i]);
        T_EQ_INT(f.header_len, hn);
        T_EQ_MEM(f.mask, 4, mask, 4);
    }
}

// RFC 6455 section 1.3's worked example.
static void accept_key(void) {
    char out[29];
    T_EQ_INT(cb_ws_accept_key("dGhlIHNhbXBsZSBub25jZQ==", out), CB_OK);
    T_EQ_STR(out, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
    T_EQ_INT(cb_ws_accept_key("short", out), CB_E_INVALID);
}

int main(void) {
    T_RUN(hostile_lengths);
    T_RUN(legal_forms);
    T_RUN(protocol_errors);
    T_RUN(header_round_trip);
    T_RUN(accept_key);
    return T_DONE();
}
