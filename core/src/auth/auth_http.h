// auth_http.h - the platform's session routes: every app's sign-up, sign-in, refresh, sign-out and
// password change, in the bytes of cbase/auth_wire.h (the server's half: auth/auth_wire.h).
//
//   POST /v1/auth/signup     CBW_SIGNUP -> 200 CBW_SESSION
//   POST /v1/auth/signin     CBW_SIGNIN -> 200 CBW_SESSION
//   POST /v1/auth/refresh    CBW_REFRESH -> 200 CBW_SESSION
//   POST /v1/auth/signout    (bearer) CBW_SIGNOUT -> 204
//   POST /v1/auth/password   (bearer) CBW_PASSWORD (current, new) -> 204; every other session ends
//
// A refusal is the platform's (net/refuse.h): an account refusal is CBW_WHY_ACCOUNT with a
// CBW_ACCOUNT_E_* code (409 for a taken name, 429 for too many attempts, else 400), a missing or
// bad bearer CBW_WHY_AUTH (401), a body this route does not read CBW_WHY_MALFORMED (400). Every
// answer is Cache-Control no-store.
//
// A NAME IS AN ACCOUNT. The client sends the name as typed (upper-cased by the host); the server
// takes its case step (auth/names.h cb_name_case) and finds the account by the email the result
// derives: hex(SHA-256(name))[:16] "@" the app's account domain (cbase/app.h account_domain). An
// app with no account domain answers these name routes 503: it has no accounts by name.
//
// The rate limits come before any work (rate_limits.h): sign-up by address and by the whole
// process, sign-in by address and by account, refresh by address, a password change by account.
#ifndef CB_AUTH_HTTP_H
#define CB_AUTH_HTTP_H

#include "ctx.h"
#include "store/store.h"

#include <stddef.h>

// The routes above, added to ctx->router (handler.c registers them before the app's).
int cb_auth_routes(cb_ctx *ctx);

// The email `name` (n bytes, after cb_name_case) derives under `domain` into out, NUL-terminated:
// its length, or -1 when domain is NULL or empty or the address does not fit.
int cb_auth_name_email(const char *domain, const char *name, size_t n, char out[CB_EMAIL_MAX + 1]);

#endif
