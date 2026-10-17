// fz_table_wire.c - the kernel's request bodies the functions take
// (table.h table_request_decode for /v1/action, ask.h ask_decode for
// /v1/meta), the action wire inside a request (awire.h), and the two small
// answers a client reads back (table_response_decode, ask_refusal_decode).
// Each body that decodes must re-encode to the same bytes.
#include "fz.h"

#include "ask.h"
#include "awire.h"
#include "table.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size > 4096) return 0;
    const uint8_t *p = fz_dup(data, size);
    int len = (int)size;

    TableRequest q;
    if (table_request_decode(p, len, &q) == 0) {
        FZ_CHECK(q.gid_off >= 0 && q.wire_off >= 0 && q.wire_len >= 0);
        FZ_CHECK(q.gid_off + q.gid_len <= len && q.wire_off + q.wire_len <= len);
        // The client's encoder writes the same request around the same bytes.
        uint8_t *buf = malloc(size + 16);
        memcpy(buf, p + q.gid_off, q.gid_len);
        memcpy(buf + q.gid_len, p + q.wire_off, (size_t)q.wire_len);
        int n = table_request_encode(buf, (int)size + 16, q.gid_len, q.wire_len, q.has_intent, q.intent);
        FZ_CHECK(n > 0);
        TableRequest q2;
        FZ_CHECK(table_request_decode(buf, n, &q2) == 0);
        FZ_CHECK(q2.gid_len == q.gid_len && q2.wire_len == q.wire_len && q2.has_intent == q.has_intent);
        FZ_CHECK(memcmp(buf + q2.gid_off, p + q.gid_off, q.gid_len) == 0);
        FZ_CHECK(memcmp(buf + q2.wire_off, p + q.wire_off, (size_t)q.wire_len) == 0);
        free(buf);
        AwireAction a;
        if (awire_decode(p + q.wire_off, q.wire_len, &a)) FZ_CHECK(a.n >= 0 && a.n <= AWIRE_MAX_CARDS);
    }

    AwireAction a;
    if (awire_decode(p, len, &a)) {
        FZ_CHECK(a.n >= 0 && a.n <= AWIRE_MAX_CARDS);
        uint8_t out[2 + 2 * AWIRE_MAX_CARDS];
        int n = awire_encode(&a, out, sizeof out);
        FZ_CHECK(n == len);
        AwireAction b;
        FZ_CHECK(awire_decode(out, n, &b) == 1 && b.kind == a.kind && b.n == a.n);
    }
    int fl = awire_frame_len(p, len);
    FZ_CHECK(fl >= 0 && fl <= len);

    TableAsk ask;
    if (ask_decode(p, len, &ask) == ASK_OK) {
        FZ_CHECK(ask.gid_len <= ROSTER_GAME_ID_MAX && ask.bot_len <= ROSTER_ID_MAX && ask.player_len <= ROSTER_ID_MAX);
        FZ_CHECK(ask.n_idx <= MAX_HAND_SIZE && ask.n_order <= MAX_PLAYERS && ask.title_len <= ASK_TITLE_MAX);
        uint8_t out[ASK_BYTES_MAX];
        int n = ask_encode(&ask, out, sizeof out);
        if (n > 0) {
            TableAsk b;
            FZ_CHECK(ask_decode(out, n, &b) == ASK_OK);
            FZ_CHECK(memcmp(&b, &ask, sizeof b) == 0);
        }
    }

    TableResponse r;
    if (table_response_decode(p, len, &r) == 0) FZ_CHECK(len >= TABLE_RESP_BYTES);
    AskRefusal ar;
    if (ask_refusal_decode(p, len, &ar) == ASK_OK) {
        uint8_t out[ASK_REFUSAL_BYTES];
        FZ_CHECK(ask_refusal_encode(&ar, out, sizeof out) == ASK_REFUSAL_BYTES && memcmp(out, p, ASK_REFUSAL_BYTES) == 0);
    }
    free((void *)p);
    return 0;
}
