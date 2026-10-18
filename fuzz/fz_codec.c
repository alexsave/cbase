// fz_codec.c - the small text decoders every boundary leans on: base64 (both
// alphabets), hex and Postgres's \x bytea, UUIDs, ISO-8601 times, URL
// percent-decoding.
// Each decode that succeeds must round-trip through its own encoder.
#include "fz.h"

#include "../../shared/c/b64/b64.h"
#include "util/clock.h"
#include "../../shared/c/hex/hex.h"
#include "util/bytea.h"
#include "util/str.h"
#include "util/uuid.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 1) return 0;
    int which = data[0] % 7;
    size_t n = size - 1;
    char *s = fz_cstr(data + 1, n);
    size_t cap = n + 8;
    uint8_t *out = malloc(cap);
    char *txt = malloc(2 * cap + 16);
    if (!out || !txt) abort();
    switch (which) {
    case 0: case 1: {
        size_t c = which ? n : n / 2;   // a short cap too
        int k = b64_decode(s, n, out, c);
        if (k >= 0) {
            FZ_CHECK((size_t)k <= c);
            int e = b64url_encode(out, (size_t)k, txt, 2 * cap + 16);
            FZ_CHECK(e >= 0);
            uint8_t *back = malloc((size_t)k + 1);
            int k2 = b64_decode(txt, (size_t)e, back, (size_t)k + 1);
            FZ_CHECK(k2 == k && memcmp(back, out, (size_t)k) == 0);
            free(back);
        }
        break;
    }
    case 2: {
        int k = cb_bytea_decode(s, n, out, cap);
        if (k >= 0) {
            int e = cb_bytea_encode(out, (size_t)k, txt, 2 * cap + 16);
            FZ_CHECK(e == 2 + 2 * k);
            uint8_t *back = malloc((size_t)k + 1);
            FZ_CHECK(cb_bytea_decode(txt, (size_t)e, back, (size_t)k + 1) == k && memcmp(back, out, (size_t)k) == 0);
            free(back);
        }
        break;
    }
    case 3:
        if (cb_uuid_valid(s, n)) FZ_CHECK(n == CB_UUID_LEN);
        break;
    case 4: {
        int64_t ms = 0;
        if (cb_iso8601_parse(s, n, &ms) == 0 && ms >= 0 && ms < 253402300800000LL) {
            char iso[64];
            int e = cb_iso8601(ms, iso, sizeof iso);
            if (e >= 0) {
                int64_t back = -1;
                FZ_CHECK(cb_iso8601_parse(iso, strlen(iso), &back) == 0 && back == ms);
            }
        }
        break;
    }
    case 5: case 6: {
        int k = cb_url_decode(s, n, txt, n + 1, which == 6);
        if (k >= 0) FZ_CHECK((size_t)k <= n && txt[k] == 0);
        break;
    }
    }
    free(txt);
    free(out);
    free(s);
    return 0;
}
