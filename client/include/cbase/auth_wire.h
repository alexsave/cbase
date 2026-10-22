// cbase/auth_wire.h - the platform's sessions on the wire: the bodies of /v1/auth/* and the
// refusal every route answers with. They are the bytes the first app's wire already numbers 1-5
// and 20, and its 10-byte refusal (PLATFORM.md 2.6 split that wire by range), stated here for every
// app; when /v1/auth moves into c_base/core (CLIENT_DESIGN.md step 4b) the server reads this file
// too, and the app's own wire header static-asserts its numbers against it. Little-endian; a str8
// is a u8 length then bytes, a str16 a u16 length; a time is u48 epoch ms.
//
//   POST /v1/auth/signup, /v1/auth/signin   [CBW_V][CBW_SIGNUP | CBW_SIGNIN] str8 name, str8 password
//   POST /v1/auth/refresh                   [CBW_V][CBW_REFRESH] str8 refresh token
//   POST /v1/auth/signout (bearer)          [CBW_V][CBW_SIGNOUT] u8 scope
//   the answer to the first three           [CBW_V][CBW_SESSION] str16 access token, str8 refresh
//                                           token, u48 expires_at, str8 user id, str8 username
//   a refusal (any route, any non-2xx)      u8 CBW_REFUSAL_FORMAT, u8 why, i32 code, i32 detail
#ifndef CBASE_AUTH_WIRE_H
#define CBASE_AUTH_WIRE_H

#define CBW_V        1
#define CBW_SIGNUP   1
#define CBW_SIGNIN   2
#define CBW_REFRESH  3
#define CBW_SIGNOUT  4
#define CBW_SESSION  20

#define CBW_SCOPE_LOCAL  0
#define CBW_SCOPE_GLOBAL 1

#define CBW_ACCESS_MAX   2048   // an access token (a JWT)
#define CBW_REFRESH_MAX  64     // a refresh token
#define CBW_ID_MAX       36     // a user id (a canonical UUID)
#define CBW_NAME_MAX     64     // a username, bytes of UTF-8
#define CBW_PASSWORD_MAX 72     // bcrypt's limit

#define CBW_REFUSAL_FORMAT 1
#define CBW_REFUSAL_BYTES  10

// Why (the refusal's second byte). Append-only: the numbers are on the wire.
#define CBW_WHY_AUTH      1    // no token, or one that does not verify
#define CBW_WHY_MALFORMED 2    // the body is not one this route reads
#define CBW_WHY_REFUSED   3    // the app refused (its code)
#define CBW_WHY_NOT_FOUND 4
#define CBW_WHY_INTERNAL  6
#define CBW_WHY_METHOD    7
#define CBW_WHY_ACCOUNT   8    // an account refusal: code is a CBW_ACCOUNT_E_*
#define CBW_WHY_FORBIDDEN 9
#define CBW_WHY_BUSY      10   // over capacity for a moment (503): a read may be sent again

#define CBW_ACCOUNT_E_BAD_LOGIN     1
#define CBW_ACCOUNT_E_EXISTS        2
#define CBW_ACCOUNT_E_WEAK_PASSWORD 3
#define CBW_ACCOUNT_E_LONG_PASSWORD 4
#define CBW_ACCOUNT_E_BAD_NAME      5
#define CBW_ACCOUNT_E_BAD_REFRESH   6   // the refresh token is unknown: the session is over
#define CBW_ACCOUNT_E_REFRESH_USED  7   // used or revoked: the session is over
#define CBW_ACCOUNT_E_SAME_PASSWORD 8
#define CBW_ACCOUNT_E_TOO_MANY      9

#endif
