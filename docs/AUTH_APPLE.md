# Sign in with Apple on c_base

Status: built and tested end to end against a fake Apple (`core/test/lib_fake_idp.c`).
It has not met the real Apple yet: that needs the four portal steps below, which only Alex can do.

## 1. What it is

Sign in with Apple is platform auth: one route, `POST /v1/auth/oidc`, that every app on c_base gets.
It answers the same session as a password sign-in (`CBW_SESSION`), so the socket, the refresh and the client do not change.
The design is provider-generic (`CBW_IDP_*` in `client/include/cbase/auth_wire.h`); Apple is provider 1.

The pieces:

| Piece | Where |
|---|---|
| The route and its bytes | `core/src/auth/auth_http.c`, `client/include/cbase/auth_wire.h` (`CBW_OIDC`) |
| The account rules (find, make, link, delete) | `core/src/auth/auth.c` (`cb_auth_oidc`, `cb_auth_oidc_link`, `cb_auth_delete_user`) |
| Apple's side: token check, key cache, code exchange, revocation, client secret | `core/src/auth/oidc.c` |
| Apple's JSON (its only JSON) | `core/src/core_json.c`, the OpenID Connect section |
| Identities and one-time claims | `store.h` (`cb_identity_row`, `once_claim`), SQLite schema 9, `rows_auth.c` items |
| The fake Apple and the tests | `core/test/lib_fake_idp.c`, `core/test/unit_oidc.c` |

## 2. The rules, in one place

An identity token is accepted only when all of these hold (`oidc.h` has the full list):
- it is RS256, signed by a key Apple publishes at `https://appleid.apple.com/auth/keys`;
- `iss` is `https://appleid.apple.com`, and `aud` is one of `CBASE_APPLE_CLIENT_IDS`;
- `exp` and `iat` are within a minute of now;
- `nonce` is the SHA-256 (lowercase hex) of the raw nonce the client sends beside the token;
- it has never been used before (its hash is claimed in the store until it expires).

Apple's keys are fetched once and kept a day.
A key id the server does not hold is fetched again at once, so a rotation is seen at once, but at most once a minute, so made-up key ids cost one fetch a minute.

Accounts:
- An identity is an account's by (provider, subject) alone.
  An email never links two accounts, verified or not, and Apple's private relay addresses are kept as Apple gave them.
- A new identity is answered `CBW_ACCOUNT_E_NEEDS_NAME` until the client sends a name with it.
  That answer spends nothing, so the client asks the person for a name and sends the same token again.
  A name already taken is `CBW_ACCOUNT_E_EXISTS` and also spends nothing.
- The account and its identity are made in one write, with no password.
  A password sign-in by that name refuses as a wrong password does.
- A signed-in account can link an identity (`CBW_OIDC_LINK` with the bearer); one that is another account's is `CBW_ACCOUNT_E_LINKED`.
- Apple sends the person's name only on the first sign-in, and only to the client.
  The server keeps no real name: the client offers it as the suggested username, and the person picks the account's name.
  The email Apple puts in the token is kept on the identity, updated when Apple sends a new one.

Revocation (the App Store rule for apps that offer Sign in with Apple):
- When the client sends Apple's authorization code and the identity holds no grant, the server exchanges it at `/auth/token` for a refresh token.
- That refresh token is sealed (AES-256-GCM, a key derived from `CBASE_JWT_SECRET`) before it is stored.
- When the account is deleted (`cb_auth_delete_user`, which foolish's delete-account and the console's users/delete both reach), every grant is revoked at `/auth/revoke` first.
  A revocation that fails twice is logged as an ERROR and the account is deleted anyway; the grant then lapses at Apple unused.

## 3. The checklist for the real thing (Alex)

Nothing here needs code.
Every step is in the Apple Developer portal or on the server.

### 3.1 The App ID capability

Each iOS app that offers the button needs "Sign in with Apple" on its App ID (for example `cards.foolish.app`), and the entitlement `com.apple.developer.applesignin = [Default]` in the app.
- Portal: Certificates, Identifiers & Profiles, Identifiers, the App ID, tick "Sign in with Apple", Save, then regenerate its provisioning profiles.
- Or through the App Store Connect API key in `~/.appstoreconnect`: `POST /v1/bundleIdCapabilities` with `capabilityType: APPLE_ID_AUTH` for the bundle id's resource id.
  This has not been called; it needs Alex's go-ahead first.
- Remember the memory note: an `xcodegen` run wipes the entitlements file, so check it after one.

### 3.2 The Services ID (the web)

The web button needs a Services ID, which is the web's client id.
- Portal: Identifiers, the "+" button, Services IDs, a name and an id such as `cards.foolish.web`.
- Tick "Sign in with Apple", Configure, choose the primary App ID, and add:
  - the domain the page is served from (for example `staging.foolish.cards`; Apple requires real https, no localhost and no IP addresses);
  - the return URL (for example `https://staging.foolish.cards/auth/apple`), which is also `CBASE_APPLE_REDIRECT_URI`.
- Popup mode still requires a return URL to be registered, even though the page never navigates to it.

### 3.3 The Sign in with Apple key (.p8)

The key signs the client secret that the code exchange and the revocation need.
- Portal: Keys, the "+" button, a name, tick "Sign in with Apple", Configure it with the primary App ID, Continue, Register, Download.
- The `.p8` downloads exactly once.
  Note its Key ID (10 characters) and the Team ID (10 characters, top right of the portal).

### 3.4 Where the secrets go

The c_base secrets live in `~/.config/cbase/ops.env` (mode 0600).
Never print them, never commit them, and never regenerate them on a live stack (`CBASE_JWT_SECRET` especially: it also derives the key the stored grants are sealed with, so a new one makes every stored grant unrevocable).

Add to `ops.env`:

```
CBASE_APPLE_CLIENT_IDS=cards.foolish.app,cards.foolish.web
CBASE_APPLE_TEAM_ID=<the 10-character team id>
CBASE_APPLE_KEY_ID=<the 10-character key id>
CBASE_APPLE_PRIVATE_KEY=<the .p8 file's text, each line break written as \n>
CBASE_APPLE_REDIRECT_URI=https://staging.foolish.cards/auth/apple
```

One way to make the key line without the key passing through a terminal:

```
printf 'CBASE_APPLE_PRIVATE_KEY=%s\n' "$(awk '{printf "%s\\n", $0}' AuthKey_XXXXXXXXXX.p8)" >> ~/.config/cbase/ops.env
```

Then move the `.p8` somewhere safe (or delete it: the portal can make another key, the server only needs the one in `ops.env`).

### 3.5 The settings, all of them

| Setting | Meaning | Secret |
|---|---|---|
| `CBASE_APPLE_CLIENT_IDS` | The audiences a token may carry: every iOS bundle id and the web Services ID, comma-separated. Unset: Sign in with Apple is off. | no |
| `CBASE_APPLE_TEAM_ID` | The client secret's issuer. | no |
| `CBASE_APPLE_KEY_ID` | The client secret's `kid`. | no |
| `CBASE_APPLE_PRIVATE_KEY` | The `.p8` PEM. With the two above, turns on the code exchange and revocation; without them sign-in works but nothing can be revoked. | yes |
| `CBASE_APPLE_REDIRECT_URI` | The web flow's return URL, repeated in the code exchange. | no |
| `CBASE_APPLE_ISSUER` | Default `https://appleid.apple.com`. Only a test sets it. | no |

The server refuses to start when the three key settings are only partly set, when the key is not a P-256 PEM, or when the issuer is not `https://host[:port]`.

### 3.6 Before it is offered to people

- [ ] App ID capability on (3.1) and a build with the entitlement.
- [ ] Services ID with the staging domain and return URL (3.2).
- [ ] Key made and in `ops.env` (3.3, 3.4).
- [ ] One real sign-in on a device and one on the staging web page, then the account deleted, and the server log showing no revocation ERROR.
- [ ] App Store review notes say how a reviewer signs in (the button) and where account deletion is.

## 4. How it was tested without Apple

`core/test/unit_oidc.c` runs everything against `lib_fake_idp.c`, a TLS server on localhost that answers `/auth/keys`, `/auth/token` and `/auth/revoke` in Apple's shapes and mints identity tokens with any claims, signed by keys it publishes or does not.
It checks every client secret it receives the way Apple does (ES256, `kid`, `iss` the team, `sub` the client, `aud` the issuer).
The refusals covered: a bad signature, another key under a published kid, `alg` HS256, RS512 and none, wrong or missing `iss`, wrong or missing `aud`, an `aud` array, expired, issued in the future, wrong or missing nonce, a raw nonce too short or too long, missing `sub`, non-canonical base64url, the other base64 alphabet, padding, two or four parts, an empty signature, an oversized token, a 1024-bit key, an unknown kid with the refetch and its once-a-minute bound, a key set that cannot be fetched, a replayed token.
Every test is mutation-checked (`docs/mutation_checks.md`).
