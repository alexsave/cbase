// refuse.h - the platform's refusal: the 10 bytes a route answers a client with when it does not do
// what was asked (cbase/auth_wire.h, "a refusal"), and the two refusals every route shares (a store
// past its backoff, a rate limit). An app adds its own whys above the platform's (cbase/auth_wire.h
// keeps the platform's numbers; an app's header says its own, and static-asserts that the two agree
// where they meet).
#ifndef CB_REFUSE_H
#define CB_REFUSE_H

#include "net/http.h"

#include "cbase/auth_wire.h"

#include <stdbool.h>
#include <stdint.h>

// The refusal's bytes: CBW_REFUSAL_BYTES, or -1 for why 0 (0 is "no refusal" to every client).
int cb_refusal_encode(int why, int32_t code, int32_t detail, uint8_t out[CBW_REFUSAL_BYTES]);

// Answers `status` with the refusal (why, code, detail 0), Cache-Control no-store.
void cb_refuse(cb_http_res *res, int status, int why, int code);
// 503 CBW_WHY_BUSY with Retry-After 1: the store was throttled past its backoff (CB_E_BUSY). A client
// sends a read again.
void cb_refuse_busy(cb_http_res *res);
// Over a rate limit (rate_limits.h): 429 with Retry-After retry_s (at least 1), refused as
// CBW_WHY_ACCOUNT CBW_ACCOUNT_E_TOO_MANY on an account route (`account`), else as CBW_WHY_BUSY.
void cb_refuse_limited(cb_http_res *res, int retry_s, bool account);

#endif
