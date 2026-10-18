// v1_http.h - the binary surface's HTTP routes (DESIGN.md "Binary surface";
// the bytes are the kernel's netwire.h).
//
//   POST /v1/auth/signup     NW_SIGNUP -> 200 NW_SESSION
//   POST /v1/auth/signin     NW_SIGNIN -> 200 NW_SESSION
//   POST /v1/auth/refresh    NW_REFRESH -> 200 NW_SESSION
//   POST /v1/auth/signout    (bearer) NW_SIGNOUT -> 204
//   POST /v1/auth/password   (bearer) NW_PASSWORD (current, new) -> 204; every other session ends
//   GET  /v1/games           (bearer) -> NW_GAMES, the caller's views, newest first
//   GET  /v1/games/<id>      (bearer) -> NW_VIEW, the caller's view, else the spectators'
//   GET  /v1/chat/<id>       (bearer) -> NW_CHATS, the last 100, oldest first (none for a non-member)
//   POST /v1/chat/<id>       (bearer, member) NW_CHAT_SEND -> 204
//   GET  /v1/bots            -> NW_BOTS, ladder order (public: cached at the edge, a day)
//   GET  /v1/leaderboard     -> NW_BOARD: the top 100 humans and the top 100 bots who have played
//                               (public: cached at the edge, an hour; invalidated by a rated game)
//   GET  /v1/history         (bearer) -> NW_HISTORY: the caller's rating and last 50 replays
//   GET  /v1/replay/<id>     (bearer) -> NW_REPLAY: the newest replay of a game the caller played
//
// The writes (create, action, meta, account/delete) are functions_http.c's,
// mounted under /v1/ too. Every refusal is the kernel's AskRefusal; an account
// refusal is ASK_WHY_ACCOUNT with an NW_ACCOUNT_E_* code. No route takes the
// anon key.
#ifndef CB_V1_HTTP_H
#define CB_V1_HTTP_H

#include "cbase/app.h"
#include "ctx.h"

#include <stddef.h>

// The same routes as a list, for the app (app/foolish_app.c).
extern const cb_route_list cb_v1_route_list;
int cb_v1_routes(cb_ctx *ctx);

// The account email a username names: hex(SHA-256(name))[:16] "@foolish.cards",
// the derivation both clients made before the server took it over (`name` is
// upper-cased by the client). `out` holds at least CB_V1_EMAIL_BYTES.
#include <stdbool.h>

#define CB_V1_EMAIL_BYTES 32

// 503 ASK_WHY_BUSY with Retry-After: the store was throttled past its backoff
// (CB_E_BUSY). A client sends a read again (netclient.h nc_retry_ms).
struct cb_http_res;
void cb_v1_busy(struct cb_http_res *res);
// Over a rate limit (limits.h): 429 with Retry-After retry_s, refused as
// ACCOUNT TOO_MANY on an account route (`account`), else as BUSY.
void cb_v1_limited(struct cb_http_res *res, int retry_s, bool account);
void cb_v1_email_of(const char *name, size_t len, char out[CB_V1_EMAIL_BYTES]);

#endif
