// refuse.c - the platform's refusal (refuse.h).
#include "net/refuse.h"

#include "../../../../shared/c/le_bytes.h"

#include <stdio.h>

int cb_refusal_encode(int why, int32_t code, int32_t detail, uint8_t out[CBW_REFUSAL_BYTES]) {
    if (why <= 0 || why > 255) return -1;
    out[0] = CBW_REFUSAL_FORMAT;
    out[1] = (uint8_t)why;
    le_put_u32(out + 2, (uint32_t)code);
    le_put_u32(out + 6, (uint32_t)detail);
    return CBW_REFUSAL_BYTES;
}

void cb_refuse(cb_http_res *res, int status, int why, int code) {
    uint8_t body[CBW_REFUSAL_BYTES];
    int n = cb_refusal_encode(why, code, 0, body);
    cb_res_header(res, "Cache-Control", "no-store");
    cb_res_send(res, status, "application/octet-stream", body, n > 0 ? (size_t)n : 0);
}

void cb_refuse_busy(cb_http_res *res) {
    cb_res_header(res, "Retry-After", "1");
    cb_refuse(res, 503, CBW_WHY_BUSY, 0);
}

void cb_refuse_limited(cb_http_res *res, int retry_s, bool account) {
    char ra[16];
    snprintf(ra, sizeof ra, "%d", retry_s > 0 ? retry_s : 1);
    cb_res_header(res, "Retry-After", ra);
    if (account) cb_refuse(res, 429, CBW_WHY_ACCOUNT, CBW_ACCOUNT_E_TOO_MANY);
    else cb_refuse(res, 429, CBW_WHY_BUSY, 0);
}
