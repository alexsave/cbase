// fz_json.c - the low-level JSON reader every AWS answer goes through
// (util/json.h: cb_json_valid, cb_json_find, the typed getters, unescape),
// and the json_surface.c readers of bodies a caller sends: the JWT header
// and claims after base64 (fz_jwt reaches them only with a valid HMAC), and
// the admin console's POST bodies. util/json.h is included here, outside
// src/, because the reader itself is what is under test (the containment gate,
// test/unit_json_gate.c, holds src/, bench/ and migrate/).
#include "fz.h"

#include "json_surface.h"
#include "util/b64.h"
#include "util/json.h"

static const char *const PATHS[] = {
    "", "Item", "Items", "a", "a.b", "requestContext.http.method", "headers", "body",
    "Responses.cbase", "CancellationReasons", "__type", "message", "Table.ProvisionedThroughput.ReadCapacityUnits",
};

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 1) return 0;
    const char *path = PATHS[data[0] % (sizeof PATHS / sizeof PATHS[0])];
    size_t n = size - 1;
    const char *json = (const char *)fz_dup(data + 1, n);
    bool valid = cb_json_valid(json, n);
    cb_span v;
    cb_json_type t = cb_json_find(json, n, path, &v);
    if (t > 0) FZ_CHECK(v.p >= json && v.p + v.n <= json + n);
    if (t == CB_JSON_STR) {
        char out[512];
        int k = cb_json_unescape(v, out, sizeof out);
        FZ_CHECK(k < (int)sizeof out);
        if (k >= 0) FZ_CHECK(out[k] == 0);
    }
    if (valid && *path == 0) FZ_CHECK(t > 0);
    char s[128]; int64_t i64; bool b;
    if (cb_json_get_str(json, n, path, s, sizeof s)) FZ_CHECK(strlen(s) < sizeof s);
    (void)cb_json_get_i64(json, n, path, &i64);
    (void)cb_json_get_bool(json, n, path, &b);
    (void)cb_json_array_len(json, n, path);

    (void)cb_js_jwt_header_ok(json, n);
    cb_jwt_claims c; bool has_nbf; int64_t nbf;
    if (cb_js_jwt_claims_decode(json, n, &c, &has_nbf, &nbf))
        FZ_CHECK(memchr(c.sub, 0, sizeof c.sub) && memchr(c.email, 0, sizeof c.email));
    char stmt[256], confirm[64], id[64];
    if (cb_js_admin_query_body(json, n, stmt, sizeof stmt, confirm, sizeof confirm))
        FZ_CHECK(strlen(stmt) < sizeof stmt && strlen(confirm) < sizeof confirm);
    if (cb_js_admin_id_body(json, n, id, sizeof id)) FZ_CHECK(strlen(id) < sizeof id);

    // The base64 writer: the input's bytes as a B value are exactly what the
    // encoding escaped as a string was, valid JSON, and decode back.
    {
        cb_buf ja, jb;
        cb_buf_init(&ja);
        cb_buf_init(&jb);
        cb_jw wa, wb;
        cb_jw_init(&wa, &ja);
        cb_jw_init(&wb, &jb);
        cb_jw_obj(&wa);
        cb_jw_kb64(&wa, "B", data, size);
        cb_jw_end(&wa);
        size_t cap = (size + 2) / 3 * 4 + 1;
        char *enc = malloc(cap);
        FZ_CHECK(enc && cb_b64_encode(data, size, enc, cap) >= 0);
        cb_jw_obj(&wb);
        cb_jw_kstr(&wb, "B", enc);
        cb_jw_end(&wb);
        FZ_CHECK(cb_jw_ok(&wa) && cb_jw_ok(&wb));
        FZ_CHECK(ja.len == jb.len && memcmp(ja.p, jb.p, ja.len) == 0);
        FZ_CHECK(cb_json_valid((const char *)ja.p, ja.len));
        cb_span tok;
        FZ_CHECK(cb_json_find((const char *)ja.p, ja.len, "B", &tok) == CB_JSON_STR);
        uint8_t *back = malloc(size + 3);
        FZ_CHECK(back && cb_b64_decode(tok.p + 1, tok.n - 2, back, size + 3) == (int)size);
        FZ_CHECK(size == 0 || memcmp(back, data, size) == 0);
        free(back);
        free(enc);
        cb_buf_free(&ja);
        cb_buf_free(&jb);
    }
    free((void *)json);
    return 0;
}
