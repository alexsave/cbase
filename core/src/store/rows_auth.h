// rows_auth.h - the auth rows written once, above the item layer (items.h),
// for both backends: a session, a refresh token, an account's failed
// sign-ins. Each backend's cb_store_ops points at these.
//
// THE ITEMS (json_surface.h "THE ITEM LAYOUT" names the rest of the table):
//   SESSION#<id>       SESSION   id, user_id, created_at, revoked, gen; TTL its end
//   REFRESH#<hash>     TOKEN     token_hash, user_id, session_id, created_at,
//                                revoked, revoked_at; gsi1 USERREFRESH#<user>/<hash>;
//                                TTL its session's end, then CB_AUTH_SPENT_TOKEN_MS
//                                after its first take
//   AUTHFAIL#<email>   FAILS     n, last_at, locked_until; TTL an hour after the last write
// A reader treats an item past its expires_ms as gone (TTL deletion lags).
#ifndef CB_ROWS_AUTH_H
#define CB_ROWS_AUTH_H

#include "store.h"

int cb_rows_session_put(cb_store *s, const cb_session_row *r);
int cb_rows_session_get(cb_store *s, const char *id, int64_t now_ms, cb_session_row *out);
int cb_rows_session_revoke(cb_store *s, const char *id, const char *user_id, int64_t expires_at);
int cb_rows_refresh_put(cb_store *s, const cb_refresh_row *r);
int cb_rows_refresh_take(cb_store *s, const char *hash, int64_t now_ms, int64_t spent_ms, cb_refresh_row *out);
int cb_rows_signin_fails_get(cb_store *s, const char *email, int64_t now_ms, cb_signin_fails *out);
int cb_rows_signin_fails_put(cb_store *s, const char *email, const cb_signin_fails *f, int64_t now_ms);
int cb_rows_signin_fails_clear(cb_store *s, const char *email);
// Every operation above, for a backend's cb_store_ops.
#define CB_ROWS_AUTH_OPS \
    .session_put = cb_rows_session_put, \
    .session_get = cb_rows_session_get, \
    .session_revoke = cb_rows_session_revoke, \
    .refresh_put = cb_rows_refresh_put, \
    .refresh_take = cb_rows_refresh_take, \
    .signin_fails_get = cb_rows_signin_fails_get, \
    .signin_fails_put = cb_rows_signin_fails_put, \
    .signin_fails_clear = cb_rows_signin_fails_clear,

#endif
