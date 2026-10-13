# As built: auth (src/auth/)

## What was built

`bcrypt.c` embeds Solar Designer's public-domain crypt_blowfish, copied from musl's `src/crypt/crypt_blowfish.c` (the copy under Homebrew's emscripten, sha256 0f9054739e295e3e981e7aa29c8f287a190aef065b84a82a4c6817120d5da05d), with its notice kept.
Four marked changes: `$2a$` runs the correct algorithm (Go's `x/crypto/bcrypt`, which wrote GoTrue's hashes, has no anti-collision tweak), `$2x$` is refused, costs run to 31, and sign-compare casts for `-Werror`.
It hashes `$2b$` at cost 10 and verifies `$2a$`, `$2b$` and `$2y$`, comparing the 31 hash characters with `CRYPTO_memcmp`.
Cost 10 measures 59 ms per verify on this Mac.

`jwt.c` mints HS256 with libcrypto `HMAC()` and the claims `jwt.h` lists (aud, exp, iat, iss, sub, email, role, session_id, user_metadata.username, app_metadata {provider, providers}, is_anonymous).
Verification checks the signature first, as text in constant time, so no second spelling of the signature verifies; then the header's alg must be exactly HS256, aud and role must be "authenticated", sub and session_id must be UUIDs, exp is required, and exp and nbf get 30 s of skew.
Each part must be unpadded base64url.

`auth.c`: sign-up, sign-in, refresh, logout, change password, delete, verify.
Refresh tokens are 32 CSPRNG bytes in base64url, stored as lowercase hex SHA-256.
A refresh consumes its token (`refresh_take`) and issues a new pair in the same session; a consumed or revoked token presented again revokes the whole session (`refresh_revoke_session`).
An unknown email still spends one bcrypt verify against a dummy hash, so a miss is not faster than a wrong password.
Passwords are 6 to 72 characters (bytes over 72 are refused rather than silently cut); usernames are 1 to 64 bytes with no '%' and no control bytes; emails are lowercased and shape-checked.

`auth_http.c`: POST /auth/v1/signup, POST /auth/v1/token (password, refresh_token), GET and PUT /auth/v1/user, POST /auth/v1/logout, all `CB_ROUTE_APIKEY`.
Sessions and users carry every field the brief lists; dates are written `YYYY-MM-DDTHH:MM:SS.ffffffZ`, GoTrue's spelling, because supabase-swift parses with `Date.ISO8601FormatStyle`.

## Where it differs from the brief, and why

- **Error bodies follow the request's API version, as GoTrue does.**
  Both SDKs send `X-Supabase-Api-Version: 2024-01-01`; such a request gets `{"code":"<error_code>","msg":".."}` and the header echoed, and both SDKs read the error code from `code`.
  A request without the header gets the older `{"code":<status>,"error_code":"..","msg":".."}`.
  The brief's single `{"code":400,...}` shape would break supabase-swift: `_RawAPIErrorResponse.code` is a `String?`, so an integer fails the whole decode and every auth error surfaces as "Unexpected error".
  The legacy `error`/`error_description` pair is not sent: auth-js and supabase-swift both read `msg` first.
  A weak password also carries `weak_password.reasons: ["length"]`.
- **Logout scope.** `global` and `local` both revoke the caller's session; `others` revokes nothing (answers 204). store.h has no per-user revoke, so "sign out everywhere" cannot be done; supabase-js's default `signOut()` is `global`.
- **auth.h grew** (my own header, additive): `CB_AUTH_E_REFRESH_USED`, `CB_AUTH_E_SAME_PASSWORD`, `CB_AUTH_E_BAD_EMAIL`, `CB_AUTH_E_LONG_PASSWORD`, and `cb_auth_user_row` (GET and PUT /user answer from the stored row, so a deleted account answers 403 `user_not_found` while its token lives).
- A '%' username is a 422 `validation_failed`; GoTrue answered the seed trigger's raise as a 500. The clients refuse it before sending.
- PUT /user changes only the password; `email` and `data` are ignored (the clients send only a password).
- `iss` is written but not checked on verify, so a change of CBASE_PUBLIC_URL does not sign everyone out.
- With no store (a backend not built yet), every route answers 503; with no `cb_auth`, too. `cb_auth_new` refuses an empty secret (config always supplies one).

## Untested

- A real GoTrue hash from the export: none was available in this worktree. The `$2a$` path is tested with published vectors and an htpasswd cost-10 hash respelled `$2a$`, and Go's algorithm is pinned by the 0xFF vector.
- The real store backends (SQLite, DynamoDB): only the in-RAM fake (`test/lib_auth_fakestore.c`). Concurrent refreshes racing on one token depend on `refresh_take` being atomic in each backend.
- The SDKs themselves against a running cbase (the browser acceptance run does that).
- A C-minted token was checked once by hand against Node's `crypto` HMAC (signature and JSON both read back); that check is not in the suite.

## Mutation checks (file, mutation, the assertion that failed)

1. bcrypt.c, `$2a$` mapped back to the anti-collision variant: unit_bcrypt.c:40 two_a_is_gos_algorithm.
2. bcrypt.c, verify compares no hash bytes: unit_bcrypt.c:32 `cb_bcrypt_verify("wrong", ...) == 0`.
3. bcrypt.c, parse accepts cost 3: unit_bcrypt.c:57 `!cb_bcrypt_parse(bad[i])`.
4. bcrypt.h, cost 12: unit_bcrypt.c:87 cost_ten_budget (prefix, and the 100 ms budget).
5. jwt.c, alg not checked: unit_jwt.c:140 alg none refused.
6. jwt.c, skew 31 s: unit_jwt.c:113 expired at exp + 31.
7. jwt.c, role not checked: unit_jwt.c:155 service_role refused.
8. jwt.c, last signature character not compared: unit_jwt.c:188.
9. auth.c, reuse does not revoke the session: unit_auth.c:135 `revoked == 3`.
10. auth.c, '%' allowed: unit_auth.c:101.
11. auth.c, email not lowercased: unit_auth.c:52.
12. auth.c, verify with a stale clock: unit_auth.c:201 expired token refused.
13. auth.c, dummy hash unparseable (a fast miss): unit_auth.c:83 the miss takes at least 10 ms.
14. auth.c, same password accepted: unit_auth.c:160.
15. auth_http.c, integer `code` to a 2024 client: unit_auth.c:298.
16. auth_http.c, API version header not echoed: unit_auth.c:299.
17. auth.c, dates end "+00:00" instead of "Z": unit_auth.c:275 jdate.
18. auth_http.c, logout revokes nothing: unit_auth.c:438 the refresh after logout is 400.
19. auth_http.c, signup without the anon-key flag: unit_auth.c:358 401.
20. auth_http.c, session without refresh_token: unit_auth.c:269.

## Header changes I would make (not made)

- store.h: `refresh_revoke_user(user_id)`, so logout `global` (supabase-js's default) signs out every device, and a password change can end the other sessions.
- store.h: a `revoked_at` on `cb_refresh_row`, so `refresh_take` can tell a token consumed by rotation seconds ago from one revoked by logout (see the first open defect).

## Open defects

- **Reuse revokes with no grace period.** GoTrue keeps a 10 s reuse interval because a refresh whose response is lost in transit (a phone changing networks) is retried with the old token; here that retry revokes the session and signs the player out. supabase-js serialises refreshes with a navigator lock and supabase-swift with an actor, so concurrent tabs are not the risk; a lost response is. The brief asked for zero-interval reuse detection; a grace window needs the `revoked_at` above and a decision on what to answer (GoTrue returns the active child, which a hash-only store cannot).
- Access tokens outlive logout and account deletion until they expire (up to an hour), as GoTrue's did; only GET /user and PUT /user consult the store.
