// fz_viewer_addr.c - cb_viewer_addr_parse (net/viewer_addr.h) on an untrusted header.
//
// Input: [cap][text]. The cap is 0 to 70 (the buffer is allocated exactly that size, so a write past it is an
// ASan report); the text is copied to an exact-size C string.
// Invariants: it never reads past the NUL or writes past the cap; on success the output is NUL-terminated
// inside the cap, a canonical address with no port that parses again to itself, and the port in the input was
// 1 to 5 plain digits worth 1 to 65535; on failure the output is empty.
#include "fz.h"

#include "net/viewer_addr.h"

// The port as the grammar says: after the last ':' of the text, past the "]" for a bracketed one.
static bool port_ok(const char *s) {
    const char *colon = strrchr(s, ':');
    if (!colon) return false;
    size_t n = strlen(colon + 1);
    if (n == 0 || n > 5) return false;
    long v = 0;
    for (size_t i = 0; i < n; i++) {
        if (colon[1 + i] < '0' || colon[1 + i] > '9') return false;
        v = v * 10 + (colon[1 + i] - '0');
    }
    return v >= 1 && v <= 65535;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 1) return 0;
    size_t cap = data[0] % 71;
    char *s = fz_cstr(data + 1, size - 1);
    char *out = malloc(cap ? cap : 1);
    if (!out) abort();
    memset(out, 'x', cap ? cap : 1);
    bool ok = cb_viewer_addr_parse(s, out, cap);
    if (ok) {
        FZ_CHECK(cap > 0 && memchr(out, 0, cap) != NULL);          // NUL-terminated inside the cap
        FZ_CHECK(out[0] != 0 && !strchr(out, ']') && !strchr(out, '['));
        FZ_CHECK(port_ok(s));                                      // the port is in the grammar
        // Idempotent: the address it gave, with a port again, gives itself.
        char again[160], back[64];
        if (strchr(out, ':')) snprintf(again, sizeof again, "[%s]:7", out);
        else snprintf(again, sizeof again, "%s:7", out);
        FZ_CHECK(cb_viewer_addr_parse(again, back, sizeof back));
        FZ_CHECK(strcmp(back, out) == 0);
    } else if (cap) {
        FZ_CHECK(out[0] == 0);                                     // a refusal leaves it empty
    }
    free(out);
    free(s);
    return 0;
}
