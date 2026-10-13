// functions_http.h - POST /functions/v1/{create,action,meta,delete-account}:
// the four edge functions, byte for byte.
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
// A refusal is the kernel's 10-byte AskRefusal (ask_refusal_encode), status
// 400, or 401 for ASK_WHY_AUTH, 404 for NOT_FOUND, 405 for METHOD, 500 for
// INTERNAL. CORS as the router does it. All CB_ROUTE_APIKEY.
#ifndef CB_FUNCTIONS_HTTP_H
#define CB_FUNCTIONS_HTTP_H

#include "ctx.h"

int cb_functions_routes(cb_ctx *ctx);

#endif
