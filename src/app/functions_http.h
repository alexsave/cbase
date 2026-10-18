// functions_http.h - POST /v1/{create,action,meta,account/delete}: the game
// writes of the binary surface (DESIGN.md "Binary surface").
//
// Bodies are application/octet-stream both ways and never JSON
// (e2e/validation/no_json_validation.test.ts held the old ones to it):
//   create          no body -> the creator's envelope (cb_table_create)
//   action          table_request_decode body (fmt 2: gid, u32 LE intent,
//                   awire) -> the 7-byte table_action_response; 415 unless
//                   the Content-Type contains application/octet-stream
//   meta            ask_decode body -> the asker's envelope (the spectator's
//                   when unseated); ADD_BOT picks a random unseated bot from
//                   store bots_list (or the named one); BUMP kicks the bots
//                   when the table has bot work; 415 as action
//   delete-account  POST only (else a 405 refusal); table redact (fresh) in
//                   every game the user sits at, rating_set_username(NULL),
//                   cb_auth_delete_user -> 204
// A refusal is the kernel's 10-byte AskRefusal (ask_refusal_encode), with
// wrap400's statuses, which are the TypeScript's and not a per-reason table:
// 400 for every refusal with a reason (AUTH, MALFORMED, REFUSED, NOT_FOUND,
// NO_BOT), 415 for a body that is not octets, 500 INTERNAL for the server's
// own failure (write contention included), 503 INTERNAL when the store is not
// up. create: 400 AUTH, else 500. delete-account: 405 METHOD, 401 AUTH, 500.
// CORS as the router does it. Every method reaches the handler.
#ifndef CB_FUNCTIONS_HTTP_H
#define CB_FUNCTIONS_HTTP_H

#include "auth/auth.h"
#include "cbase/app.h"
#include "ctx.h"

// The same routes as a list, for the app (app/foolish_app.c).
extern const cb_route_list cb_functions_route_list;
int cb_functions_routes(cb_ctx *ctx);

// delete-account's work after the bearer is verified, for the admin console's
// Users panel too: redact the user from every game they sit at (fresh CAS
// commits), clear the leaderboard name, delete the account. CB_OK, or a CB_E_*;
// CB_E_FORBIDDEN with *refused_code (a TABLE_E_*) when a table refused the
// redaction for a reason other than TABLE_E_NOT_SEATED.
int cb_app_delete_account(cb_ctx *ctx, const char *user_id, int *refused_code, int *refused_detail);

// TEST SEAM: how a request's bearer becomes a user. cb_auth_verify_bearer on
// ctx->auth by default; the app's unit tests point it at a table of fake users.
struct cb_http_req;
extern int (*cb_functions_verify)(cb_ctx *ctx, const struct cb_http_req *req, cb_user *out);

#endif
