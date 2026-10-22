/* stream_frame_test.c - stream_frame.h: the writers against the parser, the heads as structs
 * against the writers' bytes (what a host generates code from must be what the inline functions
 * put), the bounds, and sf_is_push.
 *
 * Exits 1 on any failure. */
#include <stdint.h>
#include <string.h>
#include "test/check.h"
#include "stream_frame.h"

static void round_trips(void)
{
    TEST("stream_frame: HELLO round trip");
    uint8_t o[4096];
    sf_frame f;
    int n = sf_put_hello(o, sizeof o, "tok-en", 6);
    CHECK(n == 10, "4 + 6 bytes");
    CHECK(sf_parse(o, (size_t)n, &f) == 0 && f.kind == SF_HELLO && f.tok_len == 6 && memcmp(f.tok, "tok-en", 6) == 0, "parsed");
    CHECK(sf_parse(o, (size_t)n - 1, &f) != 0 && sf_parse(o, (size_t)n + 1, &f) != 0, "exact: a byte short or over refuses");
    CHECK(sf_put_hello(o, 9, "tok-en", 6) == -1, "no room");
    TEST("stream_frame: SUB and UNSUB round trip");
    n = sf_put_sub(o, sizeof o, SF_SUB, 0x01020304u, SF_WHAT_ROOM, "room-1", 6);
    CHECK(n == 14, "8 + 6 bytes");
    CHECK(sf_parse(o, (size_t)n, &f) == 0 && f.kind == SF_SUB && f.id == 0x01020304u && f.what == SF_WHAT_ROOM && f.gid_len == 6 &&
          memcmp(f.gid, "room-1", 6) == 0, "parsed");
    n = sf_put_sub(o, sizeof o, SF_UNSUB, 7, 1, "", 0);
    CHECK(n == 8 && sf_parse(o, 8, &f) == 0 && f.kind == SF_UNSUB && f.id == 7 && f.gid_len == 0, "an empty room id");
    char big[SF_GID_MAX + 1];
    memset(big, 'g', sizeof big);
    CHECK(sf_put_sub(o, sizeof o, SF_SUB, 1, 2, big, SF_GID_MAX) == 8 + SF_GID_MAX, "the longest room id");
    CHECK(sf_put_sub(o, sizeof o, SF_SUB, 1, 2, big, SF_GID_MAX + 1) == -1, "one byte over is refused");
    TEST("stream_frame: REQ round trip");
    const uint8_t body[3] = { 9, 8, 7 };
    n = sf_put_req(o, sizeof o, 42, SF_METH_POST, "/v1/x", 5, body, 3);
    CHECK(n == 17, "9 + 5 + 3 bytes");
    CHECK(sf_parse(o, (size_t)n, &f) == 0 && f.kind == SF_REQ && f.id == 42 && f.meth == SF_METH_POST && f.path_len == 5 &&
          memcmp(f.path, "/v1/x", 5) == 0 && f.body_len == 3 && memcmp(f.body, body, 3) == 0, "parsed");
    CHECK(sf_put_req(o, sizeof o, 42, SF_METH_GET, "", 0, NULL, 0) == -1, "a path is required");
    o[6] = 3;   /* a method that is neither */
    CHECK(sf_parse(o, (size_t)n, &f) != 0, "an unknown method refuses");
    TEST("stream_frame: REPLY and the bare ones");
    n = sf_put_reply_head(o, sizeof o, 5, 404);
    o[8] = 'x';
    CHECK(n == 8 && sf_parse(o, 9, &f) == 0 && f.kind == SF_REPLY && f.id == 5 && f.status == 404 && f.body_len == 1 && f.body[0] == 'x', "a reply with a body");
    CHECK(sf_put_bare(o, sizeof o, SF_PING) == 2 && sf_parse(o, 2, &f) == 0 && f.kind == SF_PING, "PING");
    CHECK(sf_parse(o, 3, &f) != 0, "PING with a tail refuses");
    CHECK(sf_put_bare(o, 1, SF_PONG) == -1, "no room");
    o[0] = 2;
    CHECK(sf_parse(o, 2, &f) != 0, "another version refuses");
    TEST("stream_frame: a push is what is not this file's");
    o[0] = SF_V; o[1] = 1;
    CHECK(sf_is_push(o, 2), "kind 1 is a product's");
    o[1] = SF_HELLO;
    CHECK(!sf_is_push(o, 2), "HELLO is not");
    o[1] = SF_PONG + 1;
    CHECK(sf_is_push(o, 2), "past PONG is a product's");
    CHECK(!sf_is_push(o, 1), "one byte is nothing");
}

/* The heads as structs: the bytes the writers put, exactly, so code generated from the structs
 * (a page's SUB writer, its REPLY reader) speaks what the hub parses. */
static void heads(void)
{
    TEST("stream_frame: the heads are the writers' bytes");
    uint8_t o[64];
    sf_hello_hdr h = { .v = SF_V, .kind = SF_HELLO, .n = 6 };   /* designated: a field order swapped in the header must show */
    CHECK(sf_put_hello(o, sizeof o, "tok-en", 6) == 10 && memcmp(o, &h, sizeof h) == 0 && memcmp(o + sizeof h, "tok-en", 6) == 0, "HELLO: head then token");
    sf_sub_hdr s = { .v = SF_V, .kind = SF_SUB, .id = 0x01020304u, .what = SF_WHAT_ROOM, .n = 6 };
    CHECK(sf_put_sub(o, sizeof o, SF_SUB, 0x01020304u, SF_WHAT_ROOM, "room-1", 6) == 14 && memcmp(o, &s, sizeof s) == 0 &&
          memcmp(o + sizeof s, "room-1", 6) == 0, "SUB: head then room id");
    sf_req_hdr q = { .v = SF_V, .kind = SF_REQ, .id = 42, .meth = SF_METH_POST, .n = 5 };
    CHECK(sf_put_req(o, sizeof o, 42, SF_METH_POST, "/v1/x", 5, (const uint8_t *)"abc", 3) == 17 && memcmp(o, &q, sizeof q) == 0 &&
          memcmp(o + sizeof q, "/v1/x", 5) == 0 && memcmp(o + sizeof q + 5, "abc", 3) == 0, "REQ: head, path, body");
    sf_reply_hdr r = { .v = SF_V, .kind = SF_REPLY, .id = 5, .status = 404 };
    CHECK(sf_put_reply_head(o, sizeof o, 5, 404) == 8 && memcmp(o, &r, sizeof r) == 0, "REPLY: the head");
    sf_bare_hdr b = { .v = SF_V, .kind = SF_PONG };
    CHECK(sf_put_bare(o, sizeof o, SF_PONG) == 2 && memcmp(o, &b, sizeof b) == 0, "PONG: the head alone");
    /* ...and the parser reads a head written as a struct. */
    sf_frame f;
    memcpy(o, &s, sizeof s);
    memcpy(o + sizeof s, "room-1", 6);
    CHECK(sf_parse(o, sizeof s + 6, &f) == 0 && f.kind == SF_SUB && f.id == 0x01020304u && f.gid_len == 6, "a struct-written SUB parses");
    /* A host's byte order is the frame's: little-endian in the struct, little-endian on the wire. */
    CHECK(o[2] == 0x04 && o[3] == 0x03 && o[4] == 0x02 && o[5] == 0x01, "the id's bytes are little-endian");
}

int main(void)
{
    round_trips();
    heads();
    return report("stream_frame");
}
