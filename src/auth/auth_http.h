// auth_http.h - the GoTrue routes supabase-js v2 and supabase-swift call.
//
//   POST /auth/v1/signup                         {email, password, data:{username}}
//   POST /auth/v1/token?grant_type=password      {email, password}
//   POST /auth/v1/token?grant_type=refresh_token {refresh_token}
//   GET  /auth/v1/user                           Bearer
//   PUT  /auth/v1/user                           Bearer, {password}
//   POST /auth/v1/logout                         Bearer (204)
//
// All CB_ROUTE_APIKEY. Success bodies are GoTrue's session object
// ({access_token, token_type:"bearer", expires_in, expires_at, refresh_token,
// user}) or user object. Errors carry the 4xx status and GoTrue's body for the
// API version the request names: {"code":"<error code>","msg":".."} plus the
// X-Supabase-Api-Version header for 2024-01-01 (what both SDKs send), else
// {"code":<status>,"error_code":"..","msg":".."} (auth_http.c says why it must
// be both). JSON here is the compatibility tax (DESIGN.md "Where JSON is
// tolerated"); every body is read and written in json_surface.c.
#ifndef CB_AUTH_HTTP_H
#define CB_AUTH_HTTP_H

#include "ctx.h"

// Registers the routes on ctx->router. CB_OK, or CB_E_NOTIMPL from the stub.
int cb_auth_routes(cb_ctx *ctx);

#endif
