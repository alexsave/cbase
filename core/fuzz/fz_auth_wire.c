// fz_auth_wire.c - the session routes' request bodies (auth/auth_wire.h cb_aw_request_decode), the
// bytes anyone can POST to /v1/auth/*. The first input byte picks the kind the route reads (a
// route reads one kind); the rest is the body. A body that decodes must be exactly the bytes its
// fields write back (nothing skipped, nothing invented), every length within its field; one that
// does not leaves the request zeroed. The session answer is encoded from the fields too, and read
// back by hand.
#include "fz.h"

#include "auth/auth_wire.h"
#include "../../../shared/c/le_bytes.h"

static size_t put_str8(uint8_t *o, size_t at, const char *s, size_t n) {
    o[at++] = (uint8_t)n;
    memcpy(o + at, s, n);
    return at + n;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 1 || size > 1024) return 0;
    uint8_t kind = (uint8_t)(data[0] % 7);   // 0 and 6 are no request kind: always refused
    const uint8_t *p = data + 1;
    size_t n = size - 1;
    uint8_t *copy = malloc(n ? n : 1);   // its own allocation, so a read past the body is ASan's
    if (!copy) abort();
    memcpy(copy, p, n);
    cb_aw_request q;
    int rc = cb_aw_request_decode(copy, n, kind, &q);
    if (rc != 0) {
        static const cb_aw_request zero;
        FZ_CHECK(rc == -1 && memcmp(&q, &zero, sizeof q) == 0);
        free(copy);
        return 0;
    }
    FZ_CHECK(kind >= CBW_SIGNUP && kind <= CBW_PASSWORD && q.kind == kind);
    FZ_CHECK(q.name_len <= CBW_NAME_MAX && q.password_len <= CBW_PASSWORD_MAX && q.new_password_len <= CBW_PASSWORD_MAX &&
             q.refresh_len <= CBW_REFRESH_MAX);
    uint8_t back[512];
    size_t at = 0;
    back[at++] = CBW_V;
    back[at++] = kind;
    switch (kind) {
    case CBW_SIGNUP:
    case CBW_SIGNIN:
        at = put_str8(back, at, q.name, q.name_len);
        at = put_str8(back, at, q.password, q.password_len);
        break;
    case CBW_REFRESH: at = put_str8(back, at, q.refresh, q.refresh_len); break;
    case CBW_SIGNOUT:
        FZ_CHECK(q.scope == CBW_SCOPE_LOCAL || q.scope == CBW_SCOPE_GLOBAL);
        back[at++] = q.scope;
        break;
    case CBW_PASSWORD:
        at = put_str8(back, at, q.password, q.password_len);
        at = put_str8(back, at, q.new_password, q.new_password_len);
        break;
    }
    FZ_CHECK(at == n && memcmp(back, copy, n) == 0);
    // The session answer from the decoded fields: its layout, read back.
    uint8_t s[CB_AW_SESSION_MAX];
    int sn = cb_aw_session_encode(q.password, q.password_len, q.refresh, q.refresh_len, n, q.name, q.name_len > CBW_ID_MAX ? CBW_ID_MAX : q.name_len,
                                  q.new_password, q.new_password_len > CBW_NAME_MAX ? CBW_NAME_MAX : q.new_password_len, s, sizeof s);
    FZ_CHECK(sn > 0 && s[0] == CBW_V && s[1] == CBW_SESSION && le_get_u16(s + 2) == q.password_len);
    FZ_CHECK(s[4 + q.password_len] == q.refresh_len && le_get_u48(s + 5 + q.password_len + q.refresh_len) == n);
    // One byte short of the answer is refused, never cut.
    FZ_CHECK(cb_aw_session_encode(q.password, q.password_len, q.refresh, q.refresh_len, n, "", 0, "", 0, s,
                                  (size_t)(4 + q.password_len + 1 + q.refresh_len + 6 + 2 - 1)) == -1);
    free(copy);
    return 0;
}
