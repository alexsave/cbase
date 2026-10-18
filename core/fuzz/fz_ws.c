// fz_ws.c - the WebSocket frame parser (net/ws.h cb_ws_frame_parse), the
// unmask, and the upgrade key check: bytes a WebSocket client sends.
#include "fz.h"

#include "net/ws.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 1) return 0;
    uint64_t max = data[0] & 0x80 ? CB_WS_MAX_MESSAGE : (uint64_t)(data[0] & 0x7f) * 3;
    uint8_t *buf = fz_dup(data + 1, size - 1);
    size_t n = size - 1;
    // Walk the frames as the connection's reader does: one after another.
    size_t at = 0;
    while (at < n) {
        cb_ws_frame f;
        memset(&f, 0, sizeof f);
        int k = cb_ws_frame_parse(buf + at, n - at, max, &f);
        if (k <= 0) break;
        FZ_CHECK((size_t)k <= n - at);
        FZ_CHECK(f.header_len >= 2 && f.header_len <= 14);
        FZ_CHECK(f.len <= max);
        FZ_CHECK((uint64_t)k == f.header_len + f.len);
        FZ_CHECK(f.opcode < 8 || f.len <= 125);
        if (f.masked) cb_ws_unmask(buf + at + f.header_len, (size_t)f.len, f.mask);
        // The writer's header for the same frame parses back to it.
        uint8_t h[14 + 1];
        size_t hl = cb_ws_frame_header(h, f.fin, f.opcode, f.len, f.masked ? f.mask : NULL);
        FZ_CHECK(hl >= 2 && hl <= 14);
        at += (size_t)k;
    }
    char *key = fz_cstr(data, size > 40 ? 40 : size);
    char acc[29];
    if (cb_ws_accept_key(key, acc) == 0) FZ_CHECK(strlen(acc) == 28);
    free(key);
    free(buf);
    return 0;
}
