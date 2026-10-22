// auth_wire.c - the server's half of the platform's session bytes (auth_wire.h).
#include "auth/auth_wire.h"

#include "../../../../shared/c/le_bytes.h"

#include <string.h>

typedef struct {
    const uint8_t *p;
    size_t         len, at;
    int            bad;
} rd;

static const uint8_t *take(rd *r, size_t n) {
    if (r->bad || n > r->len - r->at) { r->bad = 1; return NULL; }
    const uint8_t *q = r->p + r->at;
    r->at += n;
    return q;
}

static uint8_t u8(rd *r) {
    const uint8_t *q = take(r, 1);
    return q ? q[0] : 0;
}

// A str8 into a field of `cap` bytes; longer is malformed.
static void str8(rd *r, char *dst, size_t cap, uint8_t *len_out) {
    uint8_t n = u8(r);
    if (n > cap) { r->bad = 1; return; }
    const uint8_t *q = take(r, n);
    if (!q) return;
    memcpy(dst, q, n);
    *len_out = n;
}

int cb_aw_request_decode(const uint8_t *p, size_t len, uint8_t kind, cb_aw_request *out) {
    memset(out, 0, sizeof *out);
    rd r = { p, p ? len : 0, 0, 0 };
    if (u8(&r) != CBW_V || u8(&r) != kind) r.bad = 1;
    out->kind = kind;
    switch (kind) {
    case CBW_SIGNUP:
    case CBW_SIGNIN:
        str8(&r, out->name, sizeof out->name, &out->name_len);
        str8(&r, out->password, sizeof out->password, &out->password_len);
        break;
    case CBW_REFRESH: str8(&r, out->refresh, sizeof out->refresh, &out->refresh_len); break;
    case CBW_SIGNOUT:
        out->scope = u8(&r);
        if (out->scope != CBW_SCOPE_LOCAL && out->scope != CBW_SCOPE_GLOBAL) r.bad = 1;
        break;
    case CBW_PASSWORD:
        str8(&r, out->password, sizeof out->password, &out->password_len);
        str8(&r, out->new_password, sizeof out->new_password, &out->new_password_len);
        break;
    default: r.bad = 1;
    }
    if (r.bad || r.at != r.len) {
        memset(out, 0, sizeof *out);
        return -1;
    }
    return 0;
}

typedef struct {
    uint8_t *p;
    size_t   cap, n;
    int      bad;
} wr;

static void put(wr *w, const void *src, size_t n) {
    if (w->bad || n > w->cap - w->n) { w->bad = 1; return; }
    if (n) memcpy(w->p + w->n, src, n);
    w->n += n;
}

static void put_str8(wr *w, const char *s, size_t n) {
    uint8_t b = (uint8_t)n;
    put(w, &b, 1);
    put(w, s, n);
}

int cb_aw_session_encode(const char *access, size_t access_len, const char *refresh, size_t refresh_len,
                         uint64_t expires_at_ms, const char *user_id, size_t user_id_len, const char *name,
                         size_t name_len, uint8_t *out, size_t cap) {
    if (access_len > CBW_ACCESS_MAX || refresh_len > CBW_REFRESH_MAX || user_id_len > CBW_ID_MAX ||
        name_len > CBW_NAME_MAX || expires_at_ms > LE_U48_MAX)
        return -1;
    wr w = { out, out ? cap : 0, 0, 0 };
    uint8_t head[4] = { CBW_V, CBW_SESSION, 0, 0 };
    le_put_u16(head + 2, (uint16_t)access_len);
    put(&w, head, sizeof head);
    put(&w, access, access_len);
    put_str8(&w, refresh, refresh_len);
    uint8_t exp[6];
    le_put_u48(exp, expires_at_ms);
    put(&w, exp, sizeof exp);
    put_str8(&w, user_id, user_id_len);
    put_str8(&w, name, name_len);
    return w.bad ? -1 : (int)w.n;
}
