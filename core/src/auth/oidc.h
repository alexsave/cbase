// oidc.h - an identity provider's side of a sign-in (OpenID Connect): its ID token verified, its
// authorization code exchanged for the refresh token that later revokes the grant, and that
// revocation. Provider-generic by id (cbase/auth_wire.h CBW_IDP_*); Apple is the first, configured
// by config.h's CBASE_APPLE_* (docs/AUTH_APPLE.md). The account rules over it are auth.h's
// cb_auth_oidc; every JSON shape is core_json.c's.
//
// AN ID TOKEN IS ACCEPTED when, in this order:
//   * it is three base64url parts, each canonical (the one spelling of its bytes, so a token has one
//     text and the replay key below is the token's), at most CB_OIDC_TOKEN_MAX bytes;
//   * its header says alg RS256 (nothing else: not none, not an HMAC) and a kid the provider
//     publishes. The keys are fetched from the provider over HTTPS (<issuer>/auth/keys), kept
//     CB_OIDC_KEYS_TTL_MS, and fetched again at once for a kid they do not hold, at most once every
//     CB_OIDC_REFETCH_MS (a rotation is seen at once, and a flood of made-up kids costs one fetch a
//     minute). A key is RSA of 2048 bits or more;
//   * the signature verifies (RSASSA-PKCS1-v1_5 over SHA-256);
//   * iss is the issuer exactly, aud names one of the configured clients, exp is after now and iat
//     not after now, each within CB_OIDC_SKEW_S, and sub is not empty;
//   * nonce is the lowercase hex SHA-256 of the raw nonce the client kept (CB_OIDC_NONCE_MIN to
//     CB_OIDC_NONCE_MAX bytes), so a token is good for the one sign-in that asked for it.
// What is not checked here: that the token is used once. The caller claims its key (out->key) in
// the store (store.h once_claim) before it acts on it.
//
// THE CLIENT SECRET (Apple's own scheme): a JWT signed ES256 with the developer's key, header
// {alg ES256, kid <key id>}, claims {iss <team id>, iat, exp iat + CB_OIDC_SECRET_S, aud <issuer>,
// sub <client id>}, made fresh for each call to the token and revocation endpoints.
//
// THE STORED REFRESH TOKEN is sealed before it reaches the store (cb_oidc_seal): AES-256-GCM under a
// key derived from the JWT secret, so a copy of the table alone cannot revoke or use a grant.
#ifndef CB_OIDC_H
#define CB_OIDC_H

#include "config.h"
#include "core_json.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CB_OIDC_TOKEN_MAX    8192
#define CB_OIDC_NONCE_MIN    16
#define CB_OIDC_NONCE_MAX    128
#define CB_OIDC_SKEW_S       60
#define CB_OIDC_KEYS_TTL_MS  (24LL * 3600 * 1000)
#define CB_OIDC_REFETCH_MS   (60LL * 1000)
#define CB_OIDC_RETRY_MS     1000   // with no keys held, a failed fetch is not tried again sooner
#define CB_OIDC_SECRET_S     300
#define CB_OIDC_TIMEOUT_MS   5000
#define CB_OIDC_REFRESH_MAX  512    // a provider's refresh token, bytes
#define CB_OIDC_SEALED_MAX   (2 * (12 + CB_OIDC_REFRESH_MAX + 16) + 1)
#define CB_OIDC_CODE_MAX     1024   // an authorization code, bytes
#define CB_OIDC_CLIENTS_MAX  8
#define CB_OIDC_JWKS_MAX     16     // keys held per provider

typedef struct cb_oidc cb_oidc;

// The providers the configuration turns on. CB_OK and *out (with none on, when nothing is
// configured), CB_E_INVALID with a sentence in err for a private key that does not load as P-256,
// or CB_E_NOMEM.
int  cb_oidc_new(const cb_config *cfg, cb_oidc **out, char *err, size_t cap);
void cb_oidc_free(cb_oidc *o);

// Is provider idp on (it has a client)? And can it exchange codes and revoke (it has a key)?
bool cb_oidc_on(const cb_oidc *o, int idp);
bool cb_oidc_can_exchange(const cb_oidc *o, int idp);

// What a verified ID token says.
typedef struct {
    char    sub[CB_OIDC_SUB_MAX + 1];
    char    client_id[CB_OIDC_STR_MAX + 1];   // the configured client the token's aud named
    char    email[CB_EMAIL_MAX + 1];          // "" when the token carries none
    bool    email_verified, private_email;
    int64_t exp;                              // seconds since the epoch
    char    key[65];                          // lowercase hex SHA-256 of the token's text: the replay key
} cb_oidc_identity;

// Verifies an ID token of provider idp for the raw nonce the client kept (above). CB_OK and *out;
// CB_E_AUTH for a token that is not one the provider issued to one of our clients for this nonce
// now; CB_E_IO when the provider's keys could not be had (none held, or a kid no fetch found and
// the fetch failed); CB_E_NOT_FOUND for a provider that is off; CB_E_NOMEM.
int cb_oidc_verify(cb_oidc *o, int idp, const char *token, size_t n, const char *nonce, size_t nonce_len, int64_t now_ms,
                   cb_oidc_identity *out);

// The authorization code exchanged at <issuer>/auth/token for the grant's refresh token, into
// refresh (NUL-terminated), for the client the sign-in's token named. The ID token the exchange
// answers with must verify as above (without a nonce: the code's own) and name the same subject,
// `sub`. CB_OK; CB_E_STATE when the provider cannot exchange (no key); CB_E_AUTH when the provider
// refused the code or answered for another subject; CB_E_IO when it could not be reached or
// answered nothing readable.
int cb_oidc_exchange(cb_oidc *o, int idp, const char *client_id, const char *sub, const char *code, size_t code_len,
                     int64_t now_ms, char *refresh, size_t cap);

// The grant behind refresh_token revoked at <issuer>/auth/revoke, for the client it was made to
// (configured now or not: a client retired from the configuration still has its grants revoked). CB_OK (the provider answered 200,
// which it also answers for a token already revoked); CB_E_STATE without a key; CB_E_AUTH when the
// provider refused (its 400: a client it does not know, a bad secret); CB_E_IO otherwise.
int cb_oidc_revoke(cb_oidc *o, int idp, const char *client_id, const char *refresh_token, int64_t now_ms);

// A refresh token sealed for the store (lowercase hex of nonce, ciphertext and tag) and opened
// again. Each: the text's length, or -1 (a token over CB_OIDC_REFRESH_MAX, an out too small, a
// sealed text that does not open under this key: tampered, or another secret's).
int cb_oidc_seal(const cb_oidc *o, const char *plain, char *out, size_t cap);
int cb_oidc_open(const cb_oidc *o, const char *sealed, char *out, size_t cap);

// The client secret for client_id at now_s (above), NUL-terminated: its length, or CB_E_STATE
// without a key, CB_E_NOMEM, or CB_E_FULL when out is too small. For the calls above and the tests.
int cb_oidc_client_secret(cb_oidc *o, int idp, const char *client_id, int64_t now_s, char *out, size_t cap);

// A key set's text installed as if a fetch had just answered it (fetched and tried at now_ms): the
// keys held, 0 when none is usable (the held ones stay), -1 for a provider that is off or a document
// that is not a key set. For the fuzz target and the tests, which have no network; a server fetches.
int cb_oidc_keys_load(cb_oidc *o, int idp, const char *json, size_t n, int64_t now_ms);

// Key fetches made so far (the tests: a rotation fetches once, a flood of unknown kids once a minute).
int cb_oidc_key_fetches(const cb_oidc *o, int idp);

#endif
